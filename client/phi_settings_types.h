// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_SETTINGS_TYPES_H_
#define PHIBROWSER_WIN_CLIENT_PHI_SETTINGS_TYPES_H_

#include <array>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "phibrowser-win/client/phi_app_model.h"

// The rows the settings panes read (docs/architecture-review.md §3.5, lane 1
// §1.3 and §5 D7). Every one of these used to cross PhiSettingsHost as a
// TAB-SEPARATED STRING that both sides split apart again by position, so a
// field added at one end silently misaligned at the other. They are structs
// now; the tab form survives ONLY where it is genuinely a wire or file
// format, and each of those has its writer and its reader side by side here
// so they cannot drift (test/model/settings_rows_test.cc round-trips them).
//
// HOST-CLEAN ON PURPOSE: STL plus phi_app_model.h — no windows.h and no
// base/ — so every service header can take it and the encodings are
// host-tested.

namespace phi_client {

// --- The rows ----------------------------------------------------------------

// One Space, as the Spaces / Navigation / Shortcuts panes read it.
struct PhiSpaceSummary {
  std::string id;
  std::string name;
  // The icon's DISPLAY text (a catalog glyph or a colour emoji), already
  // resolved from the stored value — the panes draw it, never decode it.
  std::string icon_text;
  std::string profile_id;
  std::string theme_id;   // Empty: the Space follows the app-wide theme.
  int saturation = -1;    // -1: the Space follows the app-wide slider.
  // The icon's STORAGE value ("phi:<id>" / "emoji:<id>"), what the picker
  // preselects.
  std::string icon;
};

// One Chromium profile, as the Profiles pane and the Spaces list read it.
struct PhiProfileSummary {
  std::string id;
  std::string name;
};

// One search engine of a profile. Exactly one row of a non-empty list is
// the default; an EMPTY list means the profile is not loaded yet, which is
// what greys the row out.
struct PhiSearchEngineRow {
  std::string keyword;
  std::string name;
  bool is_default = false;
};

// One Space URL routing rule with its WIRE ID. The id is the only handle a
// caller may keep: ids regenerate on every rule write (the Mac contract),
// so a row's POSITION in the list is not its identity — an index into a
// list the caller re-read is the bug class lane 1 §5 D7 names.
struct PhiUrlRuleRow {
  std::string id;
  model::UrlRule rule;
};

// One row of the Shortcuts pane, in display order.
struct PhiShortcutRow {
  std::string group;
  std::string label;
  std::string keys;
  // The stable rebind name. EMPTY marks a FIXED, informational row: the
  // hook special-cases its chord (or other machinery advertises it), so its
  // keycap is inert and it is never customised. The one place that rule is
  // written down.
  std::string name;
  bool customized = false;
  bool rebindable() const { return !name.empty(); }
};

// One line of the credential approval audit trail, newest first.
struct PhiCredentialAuditRow {
  std::string stamp;  // "YYYY-MM-DD HH:MM", local time.
  model::CredentialAccessKind kind = model::CredentialAccessKind::kFill;
  std::string scope;
  std::string decision;
};

// --- The encodings that still cross a boundary --------------------------------
// Three records genuinely leave the client as text: the search-engine rows
// agentSpace.settings.get answers verbatim, the rule rows the urlRules.*
// verbs build their JSON from, and the audit lines the consent FILE stores.
// Each gets a writer and a parser here, together, so the two halves are one
// piece of code.

// "a\tb\tc" -> {"a","b","c"}. Keeps empty fields so positions stay stable.
// The only splitter left: everything that is not one of the three formats
// below travels as a struct.
inline std::vector<std::string> PhiSplitTabRow(std::string_view value) {
  std::vector<std::string> parts;
  size_t start = 0;
  for (;;) {
    const size_t tab = value.find('\t', start);
    if (tab == std::string_view::npos) {
      parts.emplace_back(value.substr(start));
      return parts;
    }
    parts.emplace_back(value.substr(start, tab - start));
    start = tab + 1;
  }
}

// The persisted / wire spelling of a credential exposure.
inline const char* PhiCredentialKindName(model::CredentialAccessKind kind) {
  return kind == model::CredentialAccessKind::kReveal ? "reveal" : "fill";
}
inline model::CredentialAccessKind PhiCredentialKindFromName(
    std::string_view name) {
  // Anything but the literal "reveal" is a fill: a fill never leaves the
  // secret with the agent, so an unreadable value must not widen it.
  return name == "reveal" ? model::CredentialAccessKind::kReveal
                          : model::CredentialAccessKind::kFill;
}

// "<keyword>\t<name>\t<0|1 default>" — agentSpace.settings.get answers the
// `searchEngines` list as these strings, so the format is part of the
// control-plane contract and cannot change without the verb's callers.
inline std::string PhiSearchEngineWireRow(const PhiSearchEngineRow& row) {
  return row.keyword + "\t" + row.name + "\t" + (row.is_default ? "1" : "0");
}
inline bool PhiParseSearchEngineWireRow(std::string_view value,
                                        PhiSearchEngineRow* out) {
  const std::vector<std::string> parts = PhiSplitTabRow(value);
  if (parts.size() < 3 || parts[0].empty()) {
    return false;
  }
  out->keyword = parts[0];
  out->name = parts[1];
  out->is_default = parts[2] == "1";
  return true;
}

// "<spaceId>\t<host>\t<pathPrefix>\t<0|1 ask>" — what the urlRules.* verbs
// read. `host` is the wire form ("github.com" exact, "*.foo.com" suffix,
// "*needle*" contains).
inline std::string PhiUrlRuleWireRow(const model::UrlRule& rule) {
  return rule.space_id + "\t" + rule.host + "\t" + rule.path_prefix + "\t" +
         (rule.ask ? "1" : "0");
}
inline bool PhiParseUrlRuleWireRow(std::string_view value,
                                   model::UrlRule* out) {
  const std::vector<std::string> parts = PhiSplitTabRow(value);
  if (parts.size() < 4 || parts[0].empty() || parts[1].empty()) {
    return false;
  }
  out->space_id = parts[0];
  out->host = parts[1];
  out->path_prefix = parts[2];
  out->ask = parts[3] == "1";
  return true;
}

// "<stamp>\t<fill|reveal>\t<scope>\t<decision>" — the consent FILE's line
// format, so this one has to survive a restart unchanged.
inline std::string PhiCredentialAuditWireRow(const PhiCredentialAuditRow& row) {
  return row.stamp + "\t" + PhiCredentialKindName(row.kind) + "\t" +
         row.scope + "\t" + row.decision;
}
inline bool PhiParseCredentialAuditWireRow(std::string_view value,
                                           PhiCredentialAuditRow* out) {
  const std::vector<std::string> parts = PhiSplitTabRow(value);
  if (parts.size() < 4) {
    return false;
  }
  out->stamp = parts[0];
  out->kind = PhiCredentialKindFromName(parts[1]);
  out->scope = parts[2];
  out->decision = parts[3];
  return true;
}

// --- The Shortcuts pane's row order ------------------------------------------

// The pane's groups in Mac order — ASCII ids, which the pane spells as
// PhiStr("settings.shortcuts.group.<id, lower-cased>") so every heading is
// translated. constexpr string_views: no allocation, and nothing to destroy
// at exit.
inline constexpr std::array<std::string_view, 9> kPhiShortcutGroups = {
    "App",       "File",   "Edit", "View",  "History",
    "Bookmarks", "Window", "Tab",  "Spaces"};

// Assembles the Shortcuts pane's rows: for each group, its REBINDABLE rows
// first (yielded by `append_rebindable`, which appends the group's rows in
// table order), then that group's fixed, informational ones, whose titles
// `label` resolves from the Mac's string keys (this header has no catalog
// of its own). The order and the fixed rows live HERE rather than inside
// the preferences service so the pane's shape is one piece of data instead
// of two lists that can drift; the service supplies only the chords it owns.
inline std::vector<PhiShortcutRow> PhiBuildShortcutRows(
    const std::function<void(std::string_view group,
                             std::vector<PhiShortcutRow>* out)>&
        append_rebindable,
    const std::function<std::string(const char* key)>& label) {
  const auto fixed = [&](std::vector<PhiShortcutRow>* out, const char* group,
                         const char* label_key, const char* keys) {
    out->push_back({group, label(label_key), keys, std::string(), false});
  };
  std::vector<PhiShortcutRow> rows;
  for (const std::string_view group : kPhiShortcutGroups) {
    append_rebindable(group, &rows);
    if (group == "App") {
      // Quit is the hook's own (the Mac locks ⌘Q the same way); shown so
      // the pane is not lying about what the chord does, never rebound.
      fixed(&rows, "App", "settings.shortcuts.command.quitApp",
            "Ctrl+Shift+Q");
    } else if (group == "Tab") {
      // The MRU switcher's chords, handled in the hook (PhiTabSwitcher).
      fixed(&rows, "Tab", "settings.shortcuts.command.selectNextTab",
            "Ctrl+Tab");
      fixed(&rows, "Tab", "settings.shortcuts.command.selectPreviousTab",
            "Ctrl+Shift+Tab");
    }
  }
  return rows;
}

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_SETTINGS_TYPES_H_
