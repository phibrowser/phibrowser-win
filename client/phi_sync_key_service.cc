// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_sync_key_service.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/uuid.h"
#include "base/values.h"
#include "phibrowser-win/client/phi_auth.h"
#include "phibrowser-win/client/phi_favicon_fetch.h"
#include "phibrowser-win/client/phi_key_api_win.h"
#include "phibrowser-win/client/phi_local_store.h"
#include "phibrowser-win/client/phi_sync_cleanup.h"
#include "phibrowser-win/client/phi_sync_invalidation_win.h"
#include "phibrowser-win/client/phi_sync_merge.h"
#include "phibrowser-win/client/phi_sync_proto.h"
#include "phibrowser-win/client/phi_sync_protocol.h"
#include "phibrowser-win/client/phi_sync_settings_win.h"
#include "phibrowser-win/client/phi_sync_spaces_win.h"
#include "phibrowser-win/client/phi_sync_store_win.h"

namespace phi_client {

namespace {

using keys::KeyErrorKind;
using keys::KeyStatus;

// --- The road back to the UI thread ------------------------------------------
// One process-wide message-only window, created on the UI thread by the
// first service; a worker posts a heap closure, the window runs and frees
// it. Closures carry a weak reference to their service's UI state, so one
// arriving after the service is gone does nothing.

constexpr UINT kRunClosure = WM_APP + 1;
constexpr wchar_t kSinkClass[] = L"PhiSyncKeyServiceSink";

LRESULT CALLBACK SinkProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  if (msg == kRunClosure) {
    std::unique_ptr<std::function<void()>> closure(
        reinterpret_cast<std::function<void()>*>(lparam));
    if (closure && *closure) {
      (*closure)();
    }
    return 0;
  }
  return ::DefWindowProcW(hwnd, msg, wparam, lparam);
}

HWND SinkWindow() {
  static HWND sink = [] {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &SinkProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = kSinkClass;
    ::RegisterClassExW(&wc);
    return ::CreateWindowExW(0, kSinkClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE,
                             nullptr, wc.hInstance, nullptr);
  }();
  return sink;
}

void PostToUi(HWND sink, std::function<void()> closure) {
  auto* heap = new std::function<void()>(std::move(closure));
  if (!::PostMessageW(sink, kRunClosure, 0, reinterpret_cast<LPARAM>(heap))) {
    delete heap;
  }
}

PhiSyncKeyService::Unlock UnlockFor(const KeyStatus& status,
                                    keys::UnlockResult result) {
  if (!status.ok()) {
    return PhiSyncKeyService::Unlock::kFailed;
  }
  switch (result) {
    case keys::UnlockResult::kUnlocked:
      return PhiSyncKeyService::Unlock::kUnlocked;
    case keys::UnlockResult::kNeedsJoin:
      return PhiSyncKeyService::Unlock::kNeedsJoin;
    case keys::UnlockResult::kNotSignedIn:
      return PhiSyncKeyService::Unlock::kNotSignedIn;
  }
  return PhiSyncKeyService::Unlock::kFailed;
}

}  // namespace

// --- The worker ------------------------------------------------------------------

struct PhiSyncKeyService::UiState {
  Snapshot snapshot;
  std::function<void()> on_changed;
};

struct PhiSyncKeyService::Worker {
  Worker(const std::wstring& account_dir,
         std::string subject,
         PhiAuth* auth,
         NativeSyncConfig native)
      : config(std::move(native)),
        marker_store(config.marker_path),
        state_store(config.state_path),
        sidecars(config.sidecar_path),
        space_table(config.space_table_path),
        bookmark_table(config.bookmarks_table_path),
        pin_table(config.pins_table_path),
        rule_table(config.rules_table_path),
        transport(PhiAuth::SyncBaseUrl(),
                  [this, auth, subject] {
                    return retired ? std::string()
                                   : auth->AccessTokenBlockingFor(subject);
                  },
                  [this, auth] {
                    if (!retired) {
                      auth->InvalidateAccessToken();
                    }
                  }),
        api(PhiAuth::SyncBaseUrl(),
            // Bound to the account this layer was built for; nothing once
            // retired (the coordinator may already be tearing PhiAuth down).
            [this, auth, subject] {
              return retired ? std::string()
                             : auth->AccessTokenBlockingFor(subject);
            },
            [this, auth] {
              if (!retired) {
                auth->InvalidateAccessToken();
              }
            }),
        device_keys(account_dir + L"\\sync\\device_key.dat"),
        state(account_dir + L"\\sync\\key_state.json"),
        manager(api, device_keys, state, PhiThisDeviceIdentity()),
        approvals(api, manager),
        profiles(api, manager, state, codec),
        core(manager, profiles, state),
        domain(api, manager, codec),
        domain_keys(domain),
        stream_auth(auth),
        stream_subject(subject) {
    core.set_warning_sink(
        [](const std::string& line) { LOG(WARNING) << line; });
  }

  // Runs `work` on the UI thread and waits (up to 10 s). The closure owns
  // everything it touches: one that runs after a timeout finds nothing
  // dangling. A hop that times out before the UI took it is withdrawn, so
  // false means `work` never runs — a late write (a Space create) cannot
  // land after the caller parked and retried it.
  bool RunOnUiAndWait(const std::function<void()>& work) {
    if (retired) {
      return false;
    }
    struct Event {
      Event() : handle(::CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
      ~Event() {
        if (handle) {
          ::CloseHandle(handle);
        }
      }
      HANDLE handle;
    };
    auto event = std::make_shared<Event>();
    if (!event->handle) {
      return false;
    }
    std::weak_ptr<UiState> ui_state = ui;
    // kQueued → kTaken (the UI runs it) or kWithdrawn (the worker gave up).
    enum : int { kQueued, kTaken, kWithdrawn };
    auto hop = std::make_shared<std::atomic<int>>(kQueued);
    PostToUi(sink, [work, event, ui_state, hop] {
      int queued = kQueued;
      if (hop->compare_exchange_strong(queued, kTaken) &&
          !ui_state.expired()) {
        work();
      }
      ::SetEvent(event->handle);
    });
    if (::WaitForSingleObject(event->handle, 10000) != WAIT_OBJECT_0) {
      int queued = kQueued;
      if (hop->compare_exchange_strong(queued, kWithdrawn)) {
        return false;  // never runs
      }
      // The UI thread is running it now: its effects land, so wait them out.
      ::WaitForSingleObject(event->handle, INFINITE);
    }
    return !ui.expired();
  }

  // The engine runs only while unlocked AND paired (all native rounds
  // require enrollment). Worker thread.
  sync::PhiSyncEngine* EnsureEngine() {
    if (retired || NeedsReconfiguration() || !manager.current_ark() ||
        !core.IsPaired()) {
      return nullptr;
    }
    if (engine) {
      return engine.get();
    }
    std::string device_id;
    manager.DeviceKeyId(&device_id);
    sync_client = std::make_unique<sync::SyncProtocolClient>(
        transport, [this] {
          std::string id;
          manager.DeviceKeyId(&id);
          return id;
        });
    sync::EngineDeps deps;
    deps.client = sync_client.get();
    deps.domain_keys = &domain_keys;
    deps.marker_store = &marker_store;
    deps.state = &state_store;
    deps.sidecars = &sidecars;
    if (config.make_settings) {
      SeedDefaultSpaceRegister();
      deps.settings = config.make_settings(
          [this](const std::function<void()>& work) {
            return RunOnUiAndWait(work);
          },
          &sidecars,
          [this](const std::string& uuid) { return LocalSpaceIdOf(uuid); });
    }
    deps.now = [] {
      return static_cast<int64_t>(
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::system_clock::now().time_since_epoch())
              .count());
    };
    deps.build_identity = config.build_identity;
    deps.request_follow_up = [this] {
      std::lock_guard<std::mutex> hold(lock);
      follow_up_due = true;
    };
    deps.applying_remote = [this](bool applying) {
      applying_remote = applying;
    };
    // The Space kind, when the host hands out the Space list.
    if (config.spaces && !config.space_table_path.empty()) {
      PhiSyncSpaceAccess::ProfileHooks profile_hooks;
      profile_hooks.local_profiles = local_profiles_fn;
      profile_hooks.create_profile = config.create_profile;
      profile_hooks.mappings_changed = [this] {
        auto locals = std::make_shared<std::vector<keys::LocalProfile>>();
        std::function<std::vector<keys::LocalProfile>()> list = local_profiles_fn;
        if (list && RunOnUiAndWait([locals, list] { *locals = list(); })) {
          core.ResolveMappings(*locals);
        }
      };
      space_access = std::make_unique<PhiSyncSpaceAccess>(
          state, profiles,
          [this](const std::function<void()>& work) {
            return RunOnUiAndWait(work);
          },
          config.spaces, config.space_store_unavailable, config.known_profiles,
          std::move(profile_hooks));
      space_table.set_on_saved([this](const sync::SpaceSyncTable& table) {
        PublishHiddenSpaces(table);
      });
      deps.space_access = space_access.get();
      deps.space_store = &space_table;
      deps.new_space_id = [] { return store::NewUppercaseUuid(); };
    }
    // Bookmarks and pins ride the Space section.
    if (space_access && config.owned.store &&
        !config.bookmarks_table_path.empty() && !config.pins_table_path.empty()) {
      auto run_on_ui = [this](const std::function<void()>& work) {
        return RunOnUiAndWait(work);
      };
      bookmark_access =
          std::make_unique<PhiSyncBookmarkAccess>(run_on_ui, config.owned);
      pin_access = std::make_unique<PhiSyncPinAccess>(run_on_ui, config.owned);
      bookmark_kind = std::make_unique<sync::BookmarkOwnedKind>(
          *bookmark_access, bookmark_table, [] { return keys::NewLowercaseUuid(); },
          [] { return store::NewUppercaseUuid(); });
      pin_kind = std::make_unique<sync::PinOwnedKind>(
          *pin_access, pin_table, [] { return store::NewUppercaseUuid(); });
      deps.owned_kinds = {bookmark_kind.get(), pin_kind.get()};
      if (!config.rules_table_path.empty()) {
        rule_access =
            std::make_unique<PhiSyncUrlRuleAccess>(run_on_ui, config.owned);
        rule_kind =
            std::make_unique<sync::UrlRuleOwnedKind>(*rule_access, rule_table);
        deps.owned_kinds.push_back(rule_kind.get());
      }
    }
    engine = std::make_unique<sync::PhiSyncEngine>(std::move(deps));
    LOG(INFO) << "[PhiSync] engine started";
    StartInvalidationStream();
    if (space_access) {
      // The gate: account bound, key unlocked, pairing done — exactly when
      // this engine exists. Opening it arms the replay when needed.
      PublishHiddenSpaces(space_table.Load());
      engine->SetSpaceSyncEnabled(true);
      engine->RetentionSweep();
    }
    return engine.get();
  }

  // The Space identity mapping (sync.spaceGlobalUuids), both ways; the
  // default Space is a constant. Worker thread.
  std::optional<std::string> LocalSpaceIdOf(const std::string& uuid) {
    if (uuid == sync::kDefaultSpaceUuid) {
      return std::string(sync::kDefaultSpaceUuid);
    }
    for (const auto& [space_id, mapped] : state.SpaceMappings()) {
      if (mapped == uuid) {
        return space_id;
      }
    }
    return std::nullopt;
  }
  std::optional<std::string> SyncUuidOf(const std::string& space_id) {
    if (space_id == sync::kDefaultSpaceUuid) {
      return std::string(sync::kDefaultSpaceUuid);
    }
    const std::map<std::string, std::string> mappings = state.SpaceMappings();
    const auto it = mappings.find(space_id);
    return it == mappings.end() ? std::nullopt
                                : std::optional<std::string>(it->second);
  }

  // PhiDefaultSpaceMirror.reseed: a device with no register seeds it from
  // its own role holder (or the well-known default), stamped 0 so any real
  // hand-off wins.
  void SeedDefaultSpaceRegister() {
    const std::optional<std::string> mirrored =
        sidecars.Extra(kDefaultSpaceMirrorKey);
    if (mirrored && !mirrored->empty()) {
      return;
    }
    auto holder = std::make_shared<std::string>();
    std::function<std::string()> read = config.default_space_id;
    if (read) {
      RunOnUiAndWait([holder, read] { *holder = read(); });
    }
    const std::string uuid =
        SyncUuidOf(*holder).value_or(std::string(sync::kDefaultSpaceUuid));
    sidecars.SetExtra(kDefaultSpaceMirrorKey, uuid);
    sync::SettingValue value;
    value.SetString(uuid);
    sidecars.Set("PhiDefaultSpaceUuid", 0, sync::ValueSignature(value));
  }

  // A pull as soon as the loop can run one (coalesced: one per wake).
  void RequestPull() {
    {
      std::lock_guard<std::mutex> hold(lock);
      follow_up_due = true;
    }
    wake.notify_one();
  }

  // The notification stream runs while the engine does (the Mac's
  // PhiSyncInvalidationCoordinator): `ready` catches every kind up, a Phi
  // hint from another device pulls, a Chromium hint goes to the framework.
  void StartInvalidationStream() {
    if (stream || retired) {
      return;
    }
    std::string device_id;
    manager.DeviceKeyId(&device_id);
    PhiSyncInvalidationStream::Callbacks callbacks;
    callbacks.token = [this] {
      return retired ? std::string()
                     : stream_auth->AccessTokenBlockingFor(stream_subject);
    };
    callbacks.invalidate_token = [this] {
      if (!retired) {
        stream_auth->InvalidateAccessToken();
      }
    };
    callbacks.device_id = [device_id] { return device_id; };
    callbacks.on_ready = [this] {
      if (retired) {
        return;
      }
      RequestPull();
      ForwardChromiumHint(std::string(), {}, std::string());
    };
    callbacks.on_hint = [this, device_id](const sync::InvalidationHint& hint) {
      if (retired) {
        return;
      }
      if (hint.name_space == "chromium:phi") {
        const bool native = std::find(hint.data_types.begin(), hint.data_types.end(),
                                      2000) != hint.data_types.end();
        if (native && (hint.source_client_id.empty() ||
                       hint.source_client_id != device_id)) {
          RequestPull();
        }
        return;
      }
      if (const std::optional<std::string> uuid = hint.profile_uuid()) {
        ForwardChromiumHint(*uuid, hint.data_types, hint.source_client_id);
      }
    };
    callbacks.on_health = [this](bool healthy) { stream_healthy = healthy; };
    stream = std::make_unique<PhiSyncInvalidationStream>(PhiAuth::SyncBaseUrl(),
                                                         std::move(callbacks));
    stream->Start();
  }

  void ForwardChromiumHint(const std::string& uuid,
                           const std::vector<int>& types,
                           const std::string& excluding) {
    auto forward = config.chromium_invalidation;
    if (!forward) {
      return;
    }
    std::weak_ptr<UiState> ui_state = ui;
    PostToUi(sink, [ui_state, forward, uuid, types, excluding] {
      if (!ui_state.expired()) {
        forward(uuid, types, excluding);
      }
    });
  }

  // Sign-out, removal, reconfiguration: the engine writes nothing more.
  void StopEngine() {
    stream.reset();  // joins the stream thread
    stream_healthy = false;
    if (engine) {
      engine->Stop();
      engine.reset();
      sync_client.reset();
    }
    space_table.set_on_saved(nullptr);
    bookmark_kind.reset();
    pin_kind.reset();
    rule_kind.reset();
    rule_access.reset();
    bookmark_access.reset();
    pin_access.reset();
    space_access.reset();
  }

  // True while confirmed cleanup is unfinished. A journaled account reset
  // counts: its resetOperationId is the durable mark of a reset whose
  // outcome this PC has not recorded yet, so it must resume through Set up
  // sync again (ReconfigureSync) and never be replayed against an account
  // set up afresh. While the request is in flight the flag is true as well,
  // which keeps the engine and the pairing flow out.
  bool NeedsReconfiguration() {
    const sync::MarkerFile journal = marker_store.Load();
    return cleanup_pending || journal.requires_reconfiguration ||
           journal.reset_operation_id.has_value() ||
           (engine && engine->requires_reconfiguration());
  }

  // Records the account reset's operation UUID before the request goes out
  // (the Mac's journalResetOperation), so a crash, timeout or lost response
  // retries the same operation. The engine writes marker.json from its own
  // mirror and would save over the journal, so it is stopped first; it does
  // not start again while the UUID is set (NeedsReconfiguration).
  KeyStatus JournalResetOperation(const std::string& operation_id) {
    StopEngine();
    sync::MarkerFile journal = marker_store.Load();
    if (journal.reset_operation_id == operation_id) {
      return KeyStatus::Ok();
    }
    journal.reset_operation_id = operation_id;
    return marker_store.Save(journal)
               ? KeyStatus::Ok()
               : KeyStatus::Error(KeyErrorKind::kStorage, "reset journal");
  }

  // Sends the journaled reset. The server's two refusals deleted nothing, so
  // the journal forgets the operation before the refusal propagates; every
  // other error leaves it for the retry.
  KeyStatus RequestAccountReset(const std::string& operation_id) {
    const KeyStatus status = core.ResetAccountFromThisDevice(operation_id);
    if (status.ok()) {
      LOG(INFO) << "[PhiSync] account reset accepted by the server";
    } else if (status.kind == KeyErrorKind::kNotLastDevice ||
               status.kind == KeyErrorKind::kDeviceNotFound) {
      LOG(INFO) << "[PhiSync] account reset refused ("
                << keys::KeyStatusName(status) << ")";
      sync::MarkerFile journal = marker_store.Load();
      journal.reset_operation_id.reset();
      // A failed save leaves a UUID the server will refuse again; nothing
      // depends on it.
      marker_store.Save(journal);
    } else {
      LOG(WARNING) << "[PhiSync] account reset failed ("
                   << keys::KeyStatusName(status) << ")";
    }
    return status;
  }

  KeyStatus ClearLocalSyncState(bool removing_device) {
    cleanup_pending = true;
    removal_pending = removal_pending || removing_device ||
                      marker_store.Load().removal_pending;
    StopEngine();
    core.ClearResolved();
    domain.Clear();
    sync::CleanupActions actions;
    actions.clear_keys = [this] {
      return !retired && core.ClearLocalSyncState(removal_pending).ok();
    };
    actions.remove_owned_cursors = [this] {
      return !retired && bookmark_table.Remove() && pin_table.Remove() &&
             rule_table.Remove();
    };
    actions.clear_bookmark_ids = [this] {
      auto cleared = std::make_shared<bool>(false);
      PhiOwnedSyncHost host = config.owned;
      return host.store && RunOnUiAndWait([host, cleared] {
        if (store::LocalStore* s = host.store()) {
          host.applying(true);
          *cleared = s->ClearBookmarkSyncIds();
          host.applying(false);
        }
      }) && *cleared;
    };
    actions.clear_engine_state = [this] {
      return !retired && state_store.ClearSyncState() &&
             sidecars.ClearSidecars();
    };
    actions.clear_spaces = [this] {
      return !retired && state.SetSpaceMappings({}) && space_table.Remove();
    };
    actions.remove_marker = [this] {
      return !retired && marker_store.Remove();
    };
    if (!sync::CleanupSyncState(marker_store, removal_pending, actions)) {
      return KeyStatus::Error(KeyErrorKind::kStorage, "sync cleanup");
    }
    cleanup_pending = false;
    removal_pending = false;
    unlock = Unlock::kNeedsJoin;
    if (config.set_hidden_spaces) {
      RunOnUiAndWait([apply = config.set_hidden_spaces] { apply({}); });
    }
    return KeyStatus::Ok();
  }

  // The table's hidden cursors, as local ids, to the Space list (async).
  // Worker thread.
  void PublishHiddenSpaces(const sync::SpaceSyncTable& table) {
    if (!space_access || !config.set_hidden_spaces || retired) {
      return;
    }
    std::set<std::string> hidden = space_access->HiddenLocalIds(table);
    std::weak_ptr<UiState> ui_state = ui;
    std::function<void(std::set<std::string>)> apply = config.set_hidden_spaces;
    PostToUi(sink, [ui_state, apply, hidden] {
      if (!ui_state.expired()) {
        apply(hidden);
      }
    });
  }

  void RunRound(void (sync::PhiSyncEngine::*round)()) {
    if (sync::PhiSyncEngine* e = EnsureEngine()) {
      (e->*round)();
      LOG(INFO) << "[PhiSync] round outcome="
                << sync::RoundOutcomeName(e->last_outcome())
                << " pages=" << e->last_pages()
                << " marker_advanced=" << e->last_marker_advanced();
      const auto& owned = e->last_owned_counters();
      bool landed_rows = false;
      for (const char* label : {"bookmarks", "pins"}) {
        const auto it = owned.find(label);
        landed_rows = landed_rows || (it != owned.end() && it->second.applied > 0);
      }
      // Dormant copies parked for another pinned-tab scope are excluded
      // from status; their own line keeps the round line unchanged.
      for (const auto& [label, counters] : owned) {
        if (counters.parked_for_scope > 0) {
          LOG(INFO) << "[PhiSync] " << label << " parked for another scope="
                    << counters.parked_for_scope;
        }
      }
      // Also once per session: rows an earlier run landed.
      if (landed_rows || !favicon_startup_done) {
        favicon_startup_done = true;
        BackfillFavicons();
      }
    }
  }

  // The favicon backfill (PhiFaviconBackfillQueue): a synced bookmark or
  // pin lands with no icon; up to 20 per round get https://<host>/
  // favicon.ico, each row tried once per session. (The sidebar also asks
  // Chromium's history cache at paint time; this covers never-visited
  // pages.) Worker thread.
  void BackfillFavicons() {
    PhiOwnedSyncHost host = config.owned;
    if (!host.store || retired) {
      return;
    }
    struct Candidate {
      std::string guid;
      std::string url;
    };
    auto candidates = std::make_shared<std::vector<Candidate>>();
    std::set<std::string> attempted = favicon_attempted;
    if (!RunOnUiAndWait([host, candidates, attempted] {
          store::LocalStore* s = host.store();
          if (!s) {
            return;
          }
          for (const auto& [guid, tab] : s->tabs()) {
            if (candidates->size() >= 20) {
              break;
            }
            const bool kind = tab.type == store::RowType::kBookmark ||
                              (tab.type == store::RowType::kPinnedTab &&
                               !tab.pin_dormant);
            if (kind && tab.favicon.empty() && !attempted.count(guid) &&
                PhiFaviconHostAllowed(tab.url)) {
              candidates->push_back({guid, tab.url});
            }
          }
        })) {
      return;
    }
    std::map<std::string, std::string> by_url;  // one fetch per page url
    std::vector<std::pair<std::string, std::string>> writes;
    for (const Candidate& candidate : *candidates) {
      if (retired) {
        return;
      }
      favicon_attempted.insert(candidate.guid);
      auto it = by_url.find(candidate.url);
      if (it == by_url.end()) {
        std::string bgra;
        PhiFetchSiteFavicon(candidate.url, kFaviconPx, &bgra);
        it = by_url.emplace(candidate.url, std::move(bgra)).first;
      }
      if (!it->second.empty()) {
        writes.push_back({candidate.guid, it->second});
      }
    }
    LOG(INFO) << "[PhiSync] favicon backfill: queued=" << candidates->size()
              << " fetched=" << writes.size();
    if (writes.empty()) {
      return;
    }
    RunOnUiAndWait([host, writes] {
      store::LocalStore* s = host.store();
      if (!s) {
        return;
      }
      for (const auto& [guid, bgra] : writes) {
        s->SetFavicon(guid, /*pane_b=*/false, bgra, kFaviconPx, kFaviconPx);
      }
      if (host.landed) {
        host.landed();
      }
    });
  }

  // Worker thread.
  Snapshot MakeSnapshot() {
    Snapshot snapshot;
    snapshot.unlock = unlock;
    snapshot.paired = core.IsPaired();
    snapshot.needs_pairing = core.needs_pairing();
    snapshot.needs_pairing_actionable = core.needs_pairing_actionable();
    manager.DeviceKeyId(&snapshot.device_key_id);
    snapshot.exposed = core.ExposedKeys();
    snapshot.last_error = last_error;
    snapshot.native_sync_active = engine != nullptr;
    if (engine) {
      snapshot.native_sync_outcome =
          sync::RoundOutcomeName(engine->last_outcome());
      snapshot.native_sync_pages = engine->last_pages();
      snapshot.native_sync_needs_reconfiguration =
          engine->requires_reconfiguration();
      snapshot.native_sync_spaces_enabled = engine->space_section_enabled();
      snapshot.native_sync_spaces = engine->last_space_counters();
      snapshot.native_sync_owned = engine->last_owned_counters();
      snapshot.invalidation_stream_healthy = stream_healthy;
      snapshot.native_sync_status = engine->status();
    }
    if (NeedsReconfiguration()) {
      snapshot.native_sync_needs_reconfiguration = true;
      snapshot.native_sync_status.phase = sync::SyncPhase::kNeedsAttention;
      const sync::MarkerFile journal = marker_store.Load();
      snapshot.reset_resume_pending = journal.reset_operation_id.has_value();
      // A journaled account reset means this PC is leaving the account, like
      // a pending removal: nothing of its keys reaches Chromium meanwhile.
      if (cleanup_pending || journal.removal_pending ||
          snapshot.reset_resume_pending) {
        snapshot.exposed.clear();
        snapshot.paired = false;
      }
    }
    return snapshot;
  }

  void Enqueue(std::function<void()> job) {
    {
      std::lock_guard<std::mutex> hold(lock);
      if (stop) {
        return;
      }
      jobs.push_back(std::move(job));
    }
    wake.notify_one();
  }

  void Stop() {
    retired = true;
    core.Retire();
    if (engine) {
      engine->Stop();  // the round in flight writes nothing more
    }
    {
      std::lock_guard<std::mutex> hold(lock);
      stop = true;
      jobs.clear();  // a retired layer runs nothing more
    }
    wake.notify_one();
  }

  // The worker loop: queued jobs first; between them, Phi native sync's two
  // timed rounds — a debounced local change and the periodic pull (60 s:
  // the Mac's cadence while it has no invalidation stream).
  static void Run(std::shared_ptr<Worker> self) {
    using Clock = std::chrono::steady_clock;
    // 60 s, or 300 s while the notification stream is healthy.
    auto pull_interval = [&self] {
      return std::chrono::seconds(self->stream_healthy ? 300 : 60);
    };
    Clock::time_point next_pull = Clock::now() + pull_interval();
    for (;;) {
      std::function<void()> job;
      bool local_change = false;
      bool settings_changed = false;
      bool spaces_changed = false;
      bool owned_changed = false;
      bool periodic = false;
      {
        std::unique_lock<std::mutex> hold(self->lock);
        Clock::time_point due = next_pull;
        if (self->local_change_due && *self->local_change_due < due) {
          due = *self->local_change_due;
        }
        self->wake.wait_until(hold, due, [&] {
          return self->stop || !self->jobs.empty() || self->follow_up_due ||
                 self->deadline_moved;
        });
        if (self->stop) {
          return;
        }
        if (self->deadline_moved) {
          // A debounced local change set a new deadline: wait for it.
          self->deadline_moved = false;
          continue;
        }
        const Clock::time_point now = Clock::now();
        if (!self->jobs.empty()) {
          job = std::move(self->jobs.front());
          self->jobs.pop_front();
        } else if (self->local_change_due && *self->local_change_due <= now) {
          self->local_change_due.reset();
          local_change = true;
          settings_changed = self->settings_changed;
          spaces_changed = self->spaces_changed;
          owned_changed = self->owned_changed;
          self->settings_changed = false;
          self->spaces_changed = false;
          self->owned_changed = false;
        } else if (self->follow_up_due || now >= next_pull) {
          self->follow_up_due = false;
          next_pull = now + pull_interval();
          periodic = true;
        }
      }
      if (job) {
        job();
      } else if (local_change) {
        if (settings_changed) {
          self->RunTimedRound(&sync::PhiSyncEngine::HandleLocalSettingsChange);
        }
        if (spaces_changed) {
          self->RunTimedRound(&sync::PhiSyncEngine::HandleLocalSpaceChange);
        }
        if (owned_changed) {
          self->RunTimedRound(&sync::PhiSyncEngine::HandleLocalOwnedChange);
        }
      } else if (periodic) {
        self->RunTimedRound(&sync::PhiSyncEngine::PullOnce);
      }
    }
  }

  // A timed round, reported to the UI like a job.
  void RunTimedRound(void (sync::PhiSyncEngine::*round)()) {
    if (!engine && !core.IsPaired()) {
      return;
    }
    RunRound(round);
    if (retired) {
      return;
    }
    Snapshot snapshot = MakeSnapshot();
    std::weak_ptr<UiState> ui_state = ui;
    PostToUi(sink, [ui_state, snapshot] {
      std::shared_ptr<UiState> state_ptr = ui_state.lock();
      if (!state_ptr) {
        return;
      }
      state_ptr->snapshot = snapshot;
      if (state_ptr->on_changed) {
        state_ptr->on_changed();
      }
    });
  }

  std::atomic<bool> retired{false};
  bool cleanup_pending = false;
  bool removal_pending = false;
  NativeSyncConfig config;
  PhiSyncMarkerFile marker_store;
  PhiSyncStateFile state_store;
  PhiSyncSidecarFile sidecars;
  PhiSpaceTableFile space_table;
  PhiOwnedTableFile bookmark_table;
  PhiOwnedTableFile pin_table;
  PhiOwnedTableFile rule_table;
  PhiSyncHttpTransport transport;
  PhiKeyApiClient api;
  PhiDpapiDeviceKeyStore device_keys;
  PhiKeyStateFile state;
  PhiProfilePayloadJson codec;
  keys::AccountKeyManager manager;
  keys::DeviceApprovalService approvals;
  keys::ProfileKeyManager profiles;
  keys::SyncKeyCore core;
  keys::DomainKeyManager domain;
  struct DomainKeys : sync::DomainKeyProvider {
    explicit DomainKeys(keys::DomainKeyManager& manager) : manager(manager) {}
    bool DomainKey(keys::Bytes* out) override {
      return manager.DomainKey(out).ok();
    }
    RAW_PTR_EXCLUSION keys::DomainKeyManager& manager;
  } domain_keys;

  // Phi native sync (worker thread).
  std::unique_ptr<sync::SyncProtocolClient> sync_client;
  std::unique_ptr<PhiSyncSpaceAccess> space_access;
  std::unique_ptr<PhiSyncBookmarkAccess> bookmark_access;
  std::unique_ptr<PhiSyncPinAccess> pin_access;
  std::unique_ptr<sync::BookmarkOwnedKind> bookmark_kind;
  std::unique_ptr<sync::PinOwnedKind> pin_kind;
  std::unique_ptr<PhiSyncUrlRuleAccess> rule_access;
  std::unique_ptr<sync::UrlRuleOwnedKind> rule_kind;
  std::unique_ptr<sync::PhiSyncEngine> engine;
  std::atomic<bool> applying_remote{false};
  // Under `lock`: the debounced local change and a page-budget follow-up.
  std::optional<std::chrono::steady_clock::time_point> local_change_due;
  bool settings_changed = false;
  bool spaces_changed = false;
  bool owned_changed = false;
  bool follow_up_due = false;
  bool deadline_moved = false;
  // The UI side, for RunOnUiAndWait and the timed rounds' reports.
  std::weak_ptr<UiState> ui;
  HWND sink = nullptr;

  // Worker-thread state.
  Unlock unlock = Unlock::kUnknown;
  std::string last_error;
  // Bumped (UI thread) by CancelJoin: a request or poll started under an
  // older generation cannot consume a late approval.
  std::atomic<uint32_t> join_generation{0};

  std::mutex lock;
  std::condition_variable wake;
  std::deque<std::function<void()>> jobs;
  bool stop = false;

  // Rows the favicon backfill already tried this session.
  static constexpr int kFaviconPx = 32;
  std::set<std::string> favicon_attempted;
  bool favicon_startup_done = false;

  // The user's profiles (UI thread), for the account-profile refresh.
  std::function<std::vector<keys::LocalProfile>()> local_profiles_fn;

  // The notification stream: last, so it stops before anything its thread
  // touches goes.
  RAW_PTR_EXCLUSION PhiAuth* stream_auth = nullptr;
  std::string stream_subject;
  std::atomic<bool> stream_healthy{false};
  std::unique_ptr<PhiSyncInvalidationStream> stream;
};

// --- PhiSyncKeyService -----------------------------------------------------------

PhiSyncKeyService::PhiSyncKeyService(
    std::wstring account_dir,
    std::string subject,
    PhiAuth* auth,
    std::function<std::vector<keys::LocalProfile>()> local_profiles,
    std::function<void()> on_changed,
    NativeSyncConfig native_sync)
    : subject_(subject),
      local_profiles_(std::move(local_profiles)),
      ui_(std::make_shared<UiState>()),
      worker_(std::make_shared<Worker>(account_dir, subject, auth,
                                       std::move(native_sync))) {
  ui_->on_changed = std::move(on_changed);
  worker_->ui = ui_;
  worker_->local_profiles_fn = local_profiles_;
  worker_->sink = SinkWindow();  // created here, on the UI thread
  std::thread(&Worker::Run, worker_).detach();
}

PhiSyncKeyService::~PhiSyncKeyService() {
  // The worker keeps its own reference and exits once any call parked in
  // the network returns; the retired layer writes nothing after it.
  worker_->Stop();
}

const PhiSyncKeyService::Snapshot& PhiSyncKeyService::snapshot() const {
  return ui_->snapshot;
}

void PhiSyncKeyService::Post(Job job) {
  std::weak_ptr<UiState> ui = ui_;
  const HWND sink = SinkWindow();
  Worker* worker = worker_.get();
  worker_->Enqueue([worker, job = std::move(job), ui, sink] {
    std::function<void()> reply = job(*worker);
    if (worker->retired) {
      return;
    }
    Snapshot snapshot = worker->MakeSnapshot();
    PostToUi(sink, [ui, snapshot = std::move(snapshot),
                    reply = std::move(reply)] {
      std::shared_ptr<UiState> state = ui.lock();
      if (!state) {
        return;
      }
      state->snapshot = snapshot;
      if (reply) {
        reply();
      }
      if (state->on_changed) {
        state->on_changed();
      }
    });
  });
}

void PhiSyncKeyService::SilentUnlockAndResolve() {
  if (ui_->snapshot.unlock == Unlock::kUnknown) {
    ui_->snapshot.unlock = Unlock::kWorking;
  }
  Post([locals = local_profiles_()](Worker& w) -> std::function<void()> {
    if (w.NeedsReconfiguration()) {
      return nullptr;
    }
    keys::UnlockResult result = keys::UnlockResult::kNeedsJoin;
    KeyStatus status;
    w.core.SilentUnlockAndResolve(locals, &result, &status);
    w.unlock = UnlockFor(status, result);
    w.last_error = status.ok() ? std::string() : keys::KeyStatusName(status);
    LOG(INFO) << "[PhiSyncKeys] silent unlock: "
              << (status.ok() ? static_cast<int>(result) : -1) << " "
              << keys::KeyStatusName(status) << " resolved="
              << w.core.resolved().size();
    // Unlocked and paired: Phi native sync's first round.
    w.RunRound(&sync::PhiSyncEngine::PullOnce);
    return nullptr;
  });
}

void PhiSyncKeyService::ResolveMappings() {
  Post([locals = local_profiles_()](Worker& w) -> std::function<void()> {
    if (!w.NeedsReconfiguration() && w.manager.current_ark()) {
      w.core.ResolveMappings(locals);
    }
    return nullptr;
  });
}

void PhiSyncKeyService::BeginSetup(
    std::function<void(SetupRoute, KeyStatus)> done) {
  Post([locals = local_profiles_(),
        done = std::move(done)](Worker& w) -> std::function<void()> {
    if (w.NeedsReconfiguration()) {
      return [done] {
        done(SetupRoute::kNeedsReconfiguration, KeyStatus::Ok());
      };
    }
    keys::UnlockResult result = keys::UnlockResult::kNeedsJoin;
    KeyStatus status = w.manager.UnlockAtStartup(&result);
    w.unlock = UnlockFor(status, result);
    SetupRoute route = SetupRoute::kError;
    if (status.ok()) {
      switch (result) {
        case keys::UnlockResult::kUnlocked:
          if (w.core.IsPaired()) {
            w.core.ResolveMappings(locals);
            route = SetupRoute::kDone;
          } else {
            status = w.core.BeginEnrollment();
            w.StopEngine();  // enrollment withdrawn: native rounds pause
            route = status.ok() ? SetupRoute::kReadyToPair : SetupRoute::kError;
          }
          break;
        case keys::UnlockResult::kNotSignedIn:
          route = SetupRoute::kNotSignedIn;
          break;
        case keys::UnlockResult::kNeedsJoin: {
          bool exists = false;
          status = w.manager.AccountExists(&exists);
          route = !status.ok() ? SetupRoute::kError
                  : exists     ? SetupRoute::kChooseJoin
                               : SetupRoute::kIntroduction;
          break;
        }
      }
    }
    return [done, route, status] { done(route, status); };
  });
}

void PhiSyncKeyService::Bootstrap(
    std::function<void(KeyStatus, std::string)> done) {
  Post([done = std::move(done)](Worker& w) -> std::function<void()> {
    std::string code;
    KeyStatus status = w.core.BeginEnrollment();
    if (status.ok()) {
      status = w.manager.Bootstrap(&code);
    }
    if (status.ok()) {
      w.unlock = Unlock::kUnlocked;
    }
    return [done, status, code] { done(status, code); };
  });
}

void PhiSyncKeyService::RequestJoin(
    std::function<void(KeyStatus, keys::JoinTicket)> done) {
  const uint32_t generation = worker_->join_generation;
  Post([done = std::move(done),
        generation](Worker& w) -> std::function<void()> {
    keys::JoinTicket ticket;
    KeyStatus status = w.core.BeginEnrollment();
    if (status.ok()) {
      status = w.manager.RequestJoinApproval(&ticket);
    }
    if (status.ok() && w.join_generation != generation) {
      // Cancelled while the POST was in flight: withdraw exactly this one.
      w.manager.CancelJoinApproval(ticket.request_id);
      status = KeyStatus::Error(KeyErrorKind::kCancelled);
    }
    return [done, status, ticket] { done(status, ticket); };
  });
}

void PhiSyncKeyService::PollJoin(
    const std::string& request_id,
    std::function<void(KeyStatus, keys::JoinPoll)> done) {
  const uint32_t generation = worker_->join_generation;
  Post([done = std::move(done), request_id,
        generation](Worker& w) -> std::function<void()> {
    keys::JoinPoll poll;
    const KeyStatus status = w.manager.PollJoin(
        request_id, [&w, generation] { return w.join_generation != generation; },
        &poll);
    if (status.ok() && poll.state == keys::JoinPollState::kApproved) {
      w.unlock = Unlock::kUnlocked;
    }
    return [done, status, poll] { done(status, poll); };
  });
}

void PhiSyncKeyService::CancelJoin(const std::string& request_id) {
  ++worker_->join_generation;
  if (request_id.empty()) {
    return;
  }
  Post([request_id](Worker& w) -> std::function<void()> {
    w.manager.CancelJoinApproval(request_id);
    return nullptr;
  });
}

void PhiSyncKeyService::JoinWithRecoveryCode(
    const std::string& code,
    std::function<void(KeyStatus)> done) {
  Post([done = std::move(done), code](Worker& w) -> std::function<void()> {
    KeyStatus status = w.core.BeginEnrollment();
    if (status.ok()) {
      status = w.manager.JoinWithRecoveryCode(code);
    }
    if (status.ok()) {
      w.unlock = Unlock::kUnlocked;
    }
    return [done, status] { done(status); };
  });
}

void PhiSyncKeyService::StartPairing(
    std::function<void(KeyStatus, keys::PairingCandidates)> done) {
  Post([locals = local_profiles_(),
        done = std::move(done)](Worker& w) -> std::function<void()> {
    keys::PairingCandidates candidates;
    const KeyStatus status = w.NeedsReconfiguration()
        ? KeyStatus::Error(KeyErrorKind::kStorage, "sync cleanup required")
        : w.core.StartPairing(locals, &candidates);
    return [done, status, candidates] { done(status, candidates); };
  });
}

void PhiSyncKeyService::ApplyDecisions(
    std::vector<keys::PairingDecision> decisions,
    std::function<void(KeyStatus)> done) {
  Post([locals = local_profiles_(), decisions = std::move(decisions),
        done = std::move(done)](Worker& w) -> std::function<void()> {
    const KeyStatus status = w.NeedsReconfiguration()
        ? KeyStatus::Error(KeyErrorKind::kStorage, "sync cleanup required")
        : w.core.ApplyDecisions(decisions, locals);
    if (status.ok()) {
      w.RunRound(&sync::PhiSyncEngine::PullOnce);
    }
    return [done, status] { done(status); };
  });
}

void PhiSyncKeyService::Refresh(std::function<void(DeviceList)> done) {
  Post([done = std::move(done)](Worker& w) -> std::function<void()> {
    DeviceList list;
    list.status = w.approvals.ListPendingApprovals(&list.pending);
    if (list.status.ok()) {
      list.status = w.approvals.ListDevices(&list.devices);
    }
    return [done, list] { done(list); };
  });
}

void PhiSyncKeyService::Approve(const keys::PendingApproval& approval,
                                std::function<void(KeyStatus)> done) {
  Post([done = std::move(done), approval](Worker& w) -> std::function<void()> {
    const KeyStatus status = w.approvals.Approve(approval);
    return [done, status] { done(status); };
  });
}

void PhiSyncKeyService::Deny(const std::string& request_id,
                             std::function<void(KeyStatus)> done) {
  Post([done = std::move(done), request_id](Worker& w) -> std::function<void()> {
    const KeyStatus status = w.approvals.Deny(request_id);
    return [done, status] { done(status); };
  });
}

void PhiSyncKeyService::SyncNow() {
  Post([](Worker& w) -> std::function<void()> {
    w.RunRound(&sync::PhiSyncEngine::PullOnce);
    return nullptr;
  });
}

void PhiSyncKeyService::NoteLocalSettingsChanged() {
  if (applying_remote()) {
    return;
  }
  {
    std::lock_guard<std::mutex> hold(worker_->lock);
    worker_->local_change_due =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    worker_->settings_changed = true;
    worker_->deadline_moved = true;
  }
  worker_->wake.notify_one();
}

bool PhiSyncKeyService::applying_remote() const {
  return worker_->applying_remote;
}

void PhiSyncKeyService::NoteLocalSpacesChanged() {
  {
    std::lock_guard<std::mutex> hold(worker_->lock);
    worker_->local_change_due =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    worker_->spaces_changed = true;
    worker_->deadline_moved = true;
  }
  worker_->wake.notify_one();
}

void PhiSyncKeyService::NoteDefaultSpaceHandOff(const std::string& space_id) {
  Post([space_id](Worker& w) -> std::function<void()> {
    const std::optional<std::string> uuid = w.SyncUuidOf(space_id);
    if (!uuid) {
      return nullptr;  // no account identity: nothing to publish
    }
    w.sidecars.SetExtra(kDefaultSpaceMirrorKey, *uuid);
    {
      std::lock_guard<std::mutex> hold(w.lock);
      w.local_change_due =
          std::chrono::steady_clock::now() + std::chrono::seconds(2);
      w.settings_changed = true;
      w.deadline_moved = true;
    }
    w.wake.notify_one();
    return nullptr;
  });
}

void PhiSyncKeyService::NoteLocalOwnedChanged() {
  {
    std::lock_guard<std::mutex> hold(worker_->lock);
    worker_->local_change_due =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    worker_->owned_changed = true;
    worker_->deadline_moved = true;
  }
  worker_->wake.notify_one();
}

void PhiSyncKeyService::NoteLocalSpaceDeleted(const std::string& space_id) {
  Post([space_id](Worker& w) -> std::function<void()> {
    if (w.engine) {
      w.engine->RecordLocalSpaceDeletion(space_id);
    }
    return nullptr;
  });
}

void PhiSyncKeyService::PeekPhiData(std::function<void(std::string)> done) {
  Post([done = std::move(done)](Worker& w) -> std::function<void()> {
    base::DictValue out;
    auto finish = [&out, &done]() -> std::function<void()> {
      std::string json;
      base::JSONWriter::Write(out, &json);
      return [done, json] { done(json); };
    };
    const std::optional<keys::Bytes> ark = w.manager.current_ark();
    if (!ark) {
      out.Set("error", "not_unlocked");
      return finish();
    }
    std::optional<keys::Bytes> envelope;
    KeyStatus status =
        w.api.GetDomainKey(keys::DomainKeyManager::kDomain, &envelope);
    if (!status.ok() || !envelope) {
      out.Set("error", status.ok() ? std::string("no_domain_key")
                                   : keys::KeyStatusName(status));
      return finish();
    }
    keys::Bytes domain_key;
    std::string ignored_name;
    if (!keys::ProfileKeyManager::OpenPayload(w.codec, *envelope, *ark,
                                              &domain_key, &ignored_name)) {
      out.Set("error", "domain_key_unopenable");
      return finish();
    }
    std::string device_id;
    w.manager.DeviceKeyId(&device_id);
    sync::SyncProtocolClient client(w.transport,
                                    [device_id] { return device_id; });
    std::string marker;
    std::string birthday;
    int pages = 0;
    int tombstones = 0;
    int undecryptable = 0;
    std::map<std::string, int> kinds;
    base::ListValue spaces;
    base::ListValue bookmarks;
    base::ListValue pins;
    base::ListValue rules;
    base::ListValue settings;
    base::ListValue setting_values;
    for (bool more = true; more && pages < 40 && !w.retired; ++pages) {
      sync::GetUpdatesResult page;
      const sync::ProtocolStatus pulled =
          client.GetUpdates(marker, birthday, &page);
      if (!pulled.ok()) {
        out.Set("error", std::string("get_updates_") +
                             sync::ProtocolErrorName(pulled.error) + "_" +
                             std::to_string(pulled.http_status));
        break;
      }
      marker = page.new_marker;
      birthday = page.store_birthday;
      more = page.changes_remaining;
      for (const sync::RemoteEntity& entity : page.entities) {
        if (entity.deleted) {
          ++tombstones;
          continue;
        }
        const std::optional<keys::Bytes> plain = keys::OpenWithSymmetric(
            keys::Bytes(entity.ciphertext.begin(), entity.ciphertext.end()),
            domain_key);
        sync::PhiEntity decoded;
        if (!plain ||
            !decoded.ParseFrom(std::string(plain->begin(), plain->end()))) {
          ++undecryptable;
          continue;
        }
        switch (decoded.kind) {
          case sync::PhiEntity::Kind::kSetting:
            ++kinds["setting"];
            for (const auto& [key, value] : decoded.setting.values) {
              settings.Append(key);
              // The value too: settings are not private data.
              base::DictValue row;
              row.Set("key", key);
              row.Set("updatedAtMs", static_cast<double>(value.updated_at_ms));
              if (value.kind == sync::SettingValue::Kind::kString) {
                row.Set("value", value.string_value);
              } else if (value.kind == sync::SettingValue::Kind::kBool) {
                row.Set("value", value.bool_value);
              } else if (value.kind == sync::SettingValue::Kind::kInt) {
                row.Set("value", static_cast<double>(value.int_value));
              }
              setting_values.Append(std::move(row));
            }
            break;
          case sync::PhiEntity::Kind::kSpace: {
            ++kinds["space"];
            base::DictValue row;
            row.Set("uuid", decoded.space.space_uuid);
            row.Set("name", decoded.space.name.get().str());
            row.Set("rank", decoded.space.rank.get().str());
            spaces.Append(std::move(row));
            break;
          }
          case sync::PhiEntity::Kind::kBookmark:
            ++kinds["bookmark"];
            if (bookmarks.size() < 20) {
              bookmarks.Append(decoded.bookmark.title.get().str());
            }
            break;
          case sync::PhiEntity::Kind::kPinTab:
            ++kinds["pin"];
            if (pins.size() < 20) {
              pins.Append(decoded.pin_tab.title.get().str());
            }
            break;
          case sync::PhiEntity::Kind::kUrlRule:
            ++kinds["urlRule"];
            if (rules.size() < 20) {
              rules.Append(decoded.url_rule.host.get().str());
            }
            break;
          case sync::PhiEntity::Kind::kNone:
            ++kinds["unknown"];
            break;
        }
      }
    }
    out.Set("pages", pages);
    out.Set("tombstones", tombstones);
    out.Set("undecryptable", undecryptable);
    base::DictValue counts;
    for (const auto& [kind, count] : kinds) {
      counts.Set(kind, count);
    }
    out.Set("kinds", std::move(counts));
    out.Set("spaces", std::move(spaces));
    out.Set("bookmarkTitles", std::move(bookmarks));
    out.Set("pinTitles", std::move(pins));
    out.Set("urlRuleHosts", std::move(rules));
    out.Set("settingKeys", std::move(settings));
    out.Set("settingValues", std::move(setting_values));
    out.Set("hasBirthday", !birthday.empty());
    return finish();
  });
}

void PhiSyncKeyService::RemoveThisDevice(std::function<void(KeyStatus)> done) {
  Post([done = std::move(done)](Worker& w) -> std::function<void()> {
    // A retry after partial removal must not revoke the newly rotated key.
    KeyStatus status =
        w.removal_pending || w.marker_store.Load().removal_pending
            ? KeyStatus::Ok()
            : w.core.RevokeThisDevice();
    if (status.ok() && !w.retired) {
      status = w.ClearLocalSyncState(/*removing_device=*/true);
    }
    return [done, status] { done(status); };
  });
}

void PhiSyncKeyService::ResetSyncAndRemoveThisDevice(
    std::function<void(KeyStatus)> done) {
  Post([done = std::move(done)](Worker& w) -> std::function<void()> {
    // Server first, like RemoveThisDevice: journal the operation (an earlier
    // attempt's UUID is reused so the server answers the same operation),
    // let the server delete the account's sync data, then the shared
    // cleanup. A refusal or a transport failure leaves local state as it was.
    const std::string operation_id =
        w.marker_store.Load().reset_operation_id.value_or(
            base::Uuid::GenerateRandomV4().AsLowercaseString());
    KeyStatus status = w.JournalResetOperation(operation_id);
    if (status.ok()) {
      status = w.RequestAccountReset(operation_id);
      if (!w.retired && (status.kind == KeyErrorKind::kNotLastDevice ||
                         status.kind == KeyErrorKind::kDeviceNotFound)) {
        // Refused and forgotten: the account still syncs, so the engine the
        // journal step stopped comes back now rather than at the next
        // periodic round.
        w.RunRound(&sync::PhiSyncEngine::PullOnce);
      }
    }
    if (status.ok() && !w.retired) {
      status = w.ClearLocalSyncState(/*removing_device=*/true);
    }
    return [done, status] { done(status); };
  });
}

void PhiSyncKeyService::ReconfigureSync(std::function<void(KeyStatus)> done) {
  Post([done = std::move(done)](Worker& w) -> std::function<void()> {
    if (!w.NeedsReconfiguration()) {
      return [done] { done(KeyStatus::Error(KeyErrorKind::kCancelled)); };
    }
    // An account reset whose outcome this PC never recorded (a lost
    // response, or a crash between the server's answer and the end of
    // cleanup) is retried first with the same UUID. The server answers a
    // finished reset with `reset` again; a refusal turns this back into an
    // ordinary reconfiguration; any other failure is reported and retried
    // by the next Set up sync again.
    bool removing_device = false;
    if (const std::optional<std::string> operation_id =
            w.marker_store.Load().reset_operation_id) {
      w.StopEngine();
      const KeyStatus reset = w.RequestAccountReset(*operation_id);
      if (reset.ok()) {
        removing_device = true;
      } else if (reset.kind == KeyErrorKind::kNotLastDevice ||
                 reset.kind == KeyErrorKind::kDeviceNotFound) {
        LOG(INFO) << "[PhiSync] pending account reset was refused; "
                     "reconfiguring without it";
      } else {
        return [done, reset] { done(reset); };
      }
    }
    const KeyStatus status =
        w.retired ? KeyStatus::Error(KeyErrorKind::kCancelled)
                  : w.ClearLocalSyncState(removing_device);
    return [done, status] { done(status); };
  });
}

}  // namespace phi_client
