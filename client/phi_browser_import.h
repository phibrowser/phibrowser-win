// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_BROWSER_IMPORT_H_
#define PHIBROWSER_WIN_CLIENT_PHI_BROWSER_IMPORT_H_

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/values.h"

// The model half of "Import from Another Browser…" — the Mac's
// BrowserDataImporter, ImportDataType and ArcDataParserTool
// (Sources/UserInterface/Onboarding/Importer). The window
// (phi_import_window.h) is the view; the coordinator runs the import.
//
// Windows' sources are the Chromium family the Mac reads plus the two
// Chromium browsers Windows users have instead of Safari and Dia: Chrome,
// Microsoft Edge, Brave and Arc for Windows, each read by the fork's
// ImporterProxy, then a user-picked file. Arc's bookmarks are its Space's
// pinned tree from StorableSidebar.json, parsed here as the Mac parses it;
// every other source's bookmarks are staged in the window profile's
// CHROMIUM bookmark model and copied into the client store afterwards.

namespace phi_client {

class PhiTabStore;

// The import window's rows, in display order.
enum class PhiImportSource { kChrome, kEdge, kBrave, kArc, kFile };

inline constexpr PhiImportSource kPhiImportSources[] = {
    PhiImportSource::kChrome, PhiImportSource::kEdge, PhiImportSource::kBrave,
    PhiImportSource::kArc, PhiImportSource::kFile};

// The framework's / analytics' name: "chrome", "edge", "brave", "arc",
// "file".
const char* PhiImportSourceKey(PhiImportSource source);
std::optional<PhiImportSource> PhiImportSourceFromKey(std::string_view key);
// The browser's name in status lines ("Chrome", "Edge", …; a product name,
// not localized — the Mac's browserName(for:)).
const wchar_t* PhiImportSourceName(PhiImportSource source);
bool PhiImportSourceIsChromium(PhiImportSource source);

// The Mac's ImportDataType.
enum class PhiImportDataType { kBookmarks, kHistory, kCookies, kExtensions };
// The Chromium bridge's word ("favorites", "history", "cookies",
// "extensions").
const char* PhiImportDataTypeWire(PhiImportDataType type);
// The analytics' normalised word ("bookmarks", …).
const char* PhiImportDataTypeAnalytics(PhiImportDataType type);
// The toggle label's string key.
const char* PhiImportDataTypeLabelKey(PhiImportDataType type);
// The toggles a source's row offers; the file row has none (it is a
// picker, and Chromium decides what the file holds).
std::vector<PhiImportDataType> PhiImportAvailableTypes(PhiImportSource source);

// One profile of a Chromium-family install, from its Local State.
struct PhiChromiumProfile {
  std::string directory;  // "Default", "Profile 1"
  std::string name;       // The display name; the directory when empty.
  std::string email;      // The signed-in account, when there is one.
};
// profile.profiles_order with each entry's info_cache name and user_name
// (the Mac's loadChromiumProfiles). Empty for unreadable JSON.
std::vector<PhiChromiumProfile> PhiParseChromiumProfiles(
    std::string_view local_state_json);

// A bookmark tree the client lands (the Mac's ArcDataParserTool.Bookmark).
struct PhiImportBookmark {
  std::string title;
  std::string url;  // Empty for a folder.
  bool folder = false;
  // A split entry: the second page and Phi's layout word ("vertical" /
  // "horizontal"; empty = the default).
  std::string secondary_url;
  std::string secondary_title;
  std::string layout;
  std::vector<PhiImportBookmark> children;
};

// One Arc Space: its title, the Chromium profile directory its data lives
// in (nullopt when Arc's record is unreadable — then only its bookmarks
// may be imported, never Default's data) and its pinned tree.
struct PhiArcSpace {
  std::string id;
  std::string title;
  std::optional<std::string> profile_directory;
  PhiImportBookmark root;
};
// StorableSidebar.json → the Spaces in Arc's sidebar order (the Mac's
// ArcDataParserTool.parse, without the Favorites and theme the Migration
// wizard reads). `untitled` names a Space with no title. Nullopt when the
// file is not Arc's shape.
std::optional<std::vector<PhiArcSpace>> PhiParseArcSidebar(
    std::string_view json,
    const std::string& untitled);

// --- This machine -----------------------------------------------------------

// The source's Chromium user-data folder (holding Local State), or empty
// when the browser is not installed; always empty for kFile. Mirrors the
// folders the fork's importers read.
std::wstring PhiImportUserDataDir(PhiImportSource source);
// Arc's StorableSidebar.json, beside its User Data.
std::wstring PhiArcSidebarPath();
std::vector<PhiChromiumProfile> PhiLoadChromiumProfiles(PhiImportSource source);
// Whether the source seals its cookies with an app-bound key
// (os_crypt.app_bound_encrypted_key in its Local State: Chrome 127+, current
// Edge). Only the browser's own elevation service can unwrap that key, so
// its cookies cannot be imported and the row offers no Cookies toggle.
bool PhiImportUsesAppBoundKey(PhiImportSource source);
// PhiImportAvailableTypes for this machine's install of `source`.
std::vector<PhiImportDataType> PhiImportOfferedTypes(PhiImportSource source);
// Arc's Spaces sorted by title, as the Mac's picker lists them.
std::vector<PhiArcSpace> PhiLoadArcSpaces(const std::string& untitled);

// --- Landing in the client store ---------------------------------------------

// The window profile's Chromium bookmark tree (PhiFramework::
// AllBookmarksJson) after an import: the bookmark bar's children — the
// "Imported From <browser>" folders the fork's ProfileWriter filed each
// source under — go to the Space's root, Chromium-sourced (the Mac's
// saveChromiumBookmarksToLocalStore). Returns the bookmarks written
// (folders not counted), or -1 when the tree has no bookmark bar.
int PhiLandChromiumBookmarks(PhiTabStore& store,
                             const std::string& profile_id,
                             const std::string& space_id,
                             const base::ListValue& tree);
// One Arc Space's tree under "Imported From Arc" ▸ <Space> (the Mac's
// saveArcBookmarksToLocalStore). Returns the bookmarks written.
int PhiLandArcSpace(PhiTabStore& store,
                    const std::string& profile_id,
                    const std::string& space_id,
                    const PhiArcSpace& space,
                    const std::string& arc_folder_title);
// The Space root's order after an import: other folders, then the
// imported-browser folders in the window's row order, then the rest (the
// Mac's reorderImportedBrowserFolders).
void PhiReorderImportedFolders(PhiTabStore& store,
                               const std::string& profile_id,
                               const std::string& space_id,
                               const std::string& arc_folder_title);

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_BROWSER_IMPORT_H_
