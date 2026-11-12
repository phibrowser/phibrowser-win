// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_FOLIO_H_
#define PHIBROWSER_WIN_CLIENT_PHI_FOLIO_H_

#include <windows.h>

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "base/memory/raw_ptr_exclusion.h"

namespace phi_client {

class PhiPreferences;

// Documents\PhiFolio — the Mac's default Folio folder. UTF-8.
std::string PhiFolioDefaultFolder();

// Folio, the Mac's Save for Later (docs/folio-port.md). The Mirage extension
// owns the save end to end — extraction, the Markdown document, the MHTML
// capture, names and highlights — and writes through the jailed file broker
// here. This side owns the gates, the folder each save lands in, the save
// RPC, and the window chrome: toasts, the Add Note prompt, Reveal.
//
// Every broker request is answered (an error reply, never silence) and
// every file name it touches passes PhiFolioBrokerFileName. File I/O runs
// on one worker thread in arrival order, so a stream's chunks stay in
// sequence; replies come back to the UI thread through a message-only
// window before they reach the framework.
class PhiFolio {
 public:
  struct TabInfo {
    int64_t window_id = 0;
    std::string url;
    std::string title;
  };

  // What the coordinator lends. Everything is called on the UI thread.
  class Host {
   public:
    virtual ~Host() = default;
    virtual void RespondToExtension(const std::string& request_id,
                                    const std::string& json) = 0;
    // To Mirage in the profile owning `payload`'s "windowId" (the active
    // window's profile when there is none). Extensions only — never the
    // agent relay: a save carries user content.
    virtual bool BroadcastToExtensions(const std::string& type,
                                       const std::string& payload_json) = 0;
    virtual std::optional<TabInfo> FolioFindTab(int tab_id) const = 0;
    // The window the user is in, or 0.
    virtual int64_t FolioActiveWindow() const = 0;
    // The profile owning a browser window ("" when unknown).
    virtual std::string FolioProfileOfWindow(int64_t window_id) const = 0;
    // One open browser window per loaded profile: the armed state reaches
    // every profile's Mirage through these.
    virtual std::vector<int64_t> FolioWindowPerProfile() const = 0;
    // The shell window showing `window_id`'s browser (toast / prompt owner),
    // or null.
    virtual HWND FolioShellForWindow(int64_t window_id) const = 0;
    // A transient toast over `window_id`'s shell; `action_label` non-empty
    // adds one button that runs `action`.
    virtual void FolioShowToast(int64_t window_id,
                                const std::wstring& title,
                                const std::wstring& message,
                                int duration_ms,
                                const std::wstring& action_label,
                                std::function<void()> action) = 0;
    // A new foreground tab in the active window.
    virtual void FolioOpenTab(const std::string& url) = 0;
    virtual PhiPreferences& FolioPreferences() = 0;
  };

  explicit PhiFolio(Host* host);
  ~PhiFolio();
  PhiFolio(const PhiFolio&) = delete;
  PhiFolio& operator=(const PhiFolio&) = delete;

  // The master switch (kPhiFolioEnabled).
  static bool Enabled();
  // Folio on && the auto-trigger switch && the user's opt-in.
  bool AutoTriggerArmed() const;

  // Save to Folio is offered for this tab: Folio on, an http(s) page, and
  // no save of it in flight.
  bool CanSave(int tab_id) const;
  // Save to Folio (menu / Ctrl+Alt+S). Hands the save to Mirage.
  void Save(int tab_id);
  // Open Folio: Mirage's library page in a new tab of the active window.
  void OpenLibrary();
  // Settings ▸ General ▸ Folio ▸ Choose sites…: the extension's per-site
  // auto-save switches.
  void OpenAutoSaveSites();
  // Tells every profile's Mirage the armed state (and the master flag as
  // `enabled`). After the opt-in flips, and when a profile loads.
  void BroadcastArmedState();

  // True when `type` is Folio's: answered (possibly with an error).
  bool HandleExtensionMessage(const std::string& request_id,
                              const std::string& sender_id,
                              const std::string& type,
                              const std::string& payload_json);

 private:
  enum class Trigger { kManual, kSiteAction };
  struct PendingSave {
    int tab_id = 0;
    int64_t window_id = 0;
    std::string profile_id;
    ULONGLONG deadline = 0;
  };
  struct BrokerWrite {
    HANDLE handle = INVALID_HANDLE_VALUE;
    std::wstring part;
    std::wstring final_path;
    ULONGLONG touched = 0;
  };
  struct RenamedArchive {
    std::wstring to;
    ULONGLONG at = 0;
  };

  void StartSave(int tab_id, const TabInfo& tab, Trigger trigger,
                 const std::string& action);
  void SettleSave(const std::string& save_id, bool accepted);

  // Folder resolution (the Mac's libraryFolder(forTab:)).
  std::wstring FolderForProfile(const std::string& profile_id) const;
  std::wstring FolderForTab(std::optional<int> tab_id) const;
  void PinFolder(int tab_id, const std::string& profile_id);

  // The handlers, one per message type.
  void OnGetArmed(const std::string& request_id);
  void OnTrigger(const std::string& request_id, const std::string& payload);
  void OnSaveResult(const std::string& request_id, const std::string& payload);
  void OnToast(const std::string& request_id, const std::string& payload);
  void OnList(const std::string& request_id, const std::string& payload);
  void OnRead(const std::string& request_id, const std::string& payload);
  void OnWriteText(const std::string& request_id, const std::string& payload);
  void OnWriteBegin(const std::string& request_id, const std::string& payload);
  void OnWriteChunk(const std::string& request_id, const std::string& payload);
  void OnWriteEnd(const std::string& request_id, const std::string& payload);
  void OnRename(const std::string& request_id, const std::string& payload);
  void OnDelete(const std::string& request_id, const std::string& payload);
  void OnReveal(const std::string& request_id, const std::string& payload);
  void OnLegacyHighlight(const std::string& request_id);

  // Toasts in the app's language, from the outcome Mirage names.
  void ShowToast(int64_t window_id,
                 const std::wstring& title,
                 const std::wstring& message);
  void PromptNote(int64_t window_id,
                  const std::string& basename,
                  const std::string& block,
                  std::optional<int> tab_id);
  int64_t WindowForTab(std::optional<int> tab_id) const;

  // Renames that happened while a webpage copy was still streaming.
  std::wstring CurrentArchivePath(const std::wstring& path) const;
  void RecordArchiveRename(const std::wstring& from, const std::wstring& to);

  // Worker / UI marshalling.
  void RunOnWorker(std::function<void()> job);
  void PostToUi(std::function<void()> task);
  void DrainUi();
  void WorkerLoop();
  void UpdateTimer();
  void OnTick();
  static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

  RAW_PTR_EXCLUSION Host* host_;
  HWND msg_hwnd_ = nullptr;
  bool timer_running_ = false;

  // UI-thread state.
  std::set<int> in_flight_tabs_;
  std::map<std::string, PendingSave> pending_saves_;  // by save request id
  std::map<int, std::pair<std::wstring, ULONGLONG>> pinned_folders_;
  std::map<std::string, ULONGLONG> recent_auto_saves_;  // url -> when
  std::map<std::string, BrokerWrite> broker_writes_;    // by token
  std::map<std::wstring, RenamedArchive> renamed_archives_;
  std::set<std::wstring> finishing_archives_;

  // Worker thread + its queue; results come back through ui_tasks_.
  std::mutex mutex_;
  std::deque<std::function<void()>> worker_jobs_;
  std::deque<std::function<void()>> ui_tasks_;
  bool stopping_ = false;
  HANDLE worker_wake_ = nullptr;
  HANDLE worker_thread_ = nullptr;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_FOLIO_H_
