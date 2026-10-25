// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_browser_import.h"

#include <windows.h>

#include <shlobj.h>

#include <algorithm>
#include <map>
#include <set>
#include <utility>

#include "base/json/json_reader.h"
#include "base/logging.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "phibrowser-win/client/phi_file_io.h"
#include "phibrowser-win/client/phi_local_store.h"
#include "phibrowser-win/client/phi_tab_store.h"

namespace phi_client {

namespace {

// --- Sources and types ---------------------------------------------------------

struct SourceInfo {
  PhiImportSource source;
  const char* key;
  const wchar_t* name;
};

constexpr SourceInfo kSourceInfo[] = {
    {PhiImportSource::kChrome, "chrome", L"Chrome"},
    {PhiImportSource::kEdge, "edge", L"Edge"},
    {PhiImportSource::kBrave, "brave", L"Brave"},
    {PhiImportSource::kArc, "arc", L"Arc"},
    {PhiImportSource::kFile, "file", L"File"},
};

const SourceInfo& InfoOf(PhiImportSource source) {
  for (const SourceInfo& info : kSourceInfo) {
    if (info.source == source) {
      return info;
    }
  }
  return kSourceInfo[0];
}

// --- Arc sidebar ----------------------------------------------------------------

// One sidebar item, flattened (ArcDataParserTool.SidebarItem + its
// Bookmark, before the tree is linked).
struct ArcItem {
  std::string title;
  std::string url;
  bool folder = false;
  bool split = false;
  std::string split_orientation;
  // The Space a container item holds pins for (itemContainer.containerType.
  // spaceItems._0); empty otherwise.
  std::string container_space;
  bool container = false;
  std::vector<std::string> children;
};

// Arc's id-then-object arrays: a bare string names the object that follows
// (which may also carry its own id); an object is either {"value": item}
// or the item itself. `visit(id, object)` is called once per object.
template <typename Visit>
void WalkIdObjectList(const base::ListValue& list, Visit visit) {
  std::string pending_id;
  for (const base::Value& entry : list) {
    if (entry.is_string()) {
      pending_id = entry.GetString();
      continue;
    }
    const base::DictValue* dict = entry.GetIfDict();
    if (!dict) {
      pending_id.clear();
      continue;
    }
    if (const base::DictValue* value = dict->FindDict("value")) {
      dict = value;
    }
    const std::string* own = dict->FindString("id");
    std::string id = !pending_id.empty() ? pending_id
                                         : (own ? *own : std::string());
    pending_id.clear();
    if (!id.empty()) {
      visit(id, *dict);
    }
  }
}

void ReadArcItems(const base::ListValue& list,
                  std::map<std::string, ArcItem>* items) {
  WalkIdObjectList(list, [&](const std::string& id, const base::DictValue& d) {
    const base::DictValue* data = d.FindDict("data");
    if (!data) {
      return;  // Not an item this parser reads.
    }
    ArcItem item;
    const std::string* title = d.FindString("title");
    if (const base::DictValue* tab = data->FindDict("tab")) {
      const std::string* url = tab->FindString("savedURL");
      if (!url) {
        return;
      }
      const std::string* saved_title = tab->FindString("savedTitle");
      item.url = *url;
      item.title = saved_title ? *saved_title : (title ? *title : "");
    } else if (data->contains("splitView")) {
      item.folder = true;
      item.split = true;
      if (const base::DictValue* split = data->FindDict("splitView")) {
        if (const std::string* o = split->FindString("layoutOrientation")) {
          item.split_orientation = *o;
        }
      }
    } else {
      // A list (folder) or a container; both land as folders.
      item.folder = true;
      item.title = title ? *title : "Untitled";
      if (!data->contains("list")) {
        item.container = true;
        if (const std::string* space = data->FindStringByDottedPath(
                "itemContainer.containerType.spaceItems._0")) {
          item.container_space = *space;
        }
      }
    }
    if (const base::ListValue* children = d.FindList("childrenIds")) {
      for (const base::Value& child : *children) {
        if (child.is_string()) {
          item.children.push_back(child.GetString());
        }
      }
    }
    (*items)[id] = std::move(item);
  });
}

// A profile directory must be one non-empty path component: anything else
// (empty, a separator, a traversal) reads as unknown, so it never reaches
// the importer — which would map "" to Default or escape the folder.
std::optional<std::string> ArcProfileDirectory(const base::DictValue& space) {
  const base::Value* profile = space.Find("profile");
  if (!profile) {
    return "Default";
  }
  const base::DictValue* dict = profile->GetIfDict();
  if (!dict) {
    return std::nullopt;
  }
  if (dict->FindBool("default").value_or(false)) {
    return "Default";
  }
  const std::string* basename =
      dict->FindStringByDottedPath("custom._0.directoryBasename");
  if (!basename) {
    return std::nullopt;
  }
  const std::string trimmed(
      base::TrimWhitespaceASCII(*basename, base::TRIM_ALL));
  if (trimmed.empty() || trimmed == "." || trimmed == ".." ||
      trimmed.find_first_of("/\\") != std::string::npos) {
    return std::nullopt;
  }
  return trimmed;
}

struct ArcSpaceModel {
  std::string id;
  std::string title;
  std::vector<std::string> container_ids;
  std::optional<std::string> profile_directory;
};

// File order; a repeated id keeps its first position and its last value.
std::vector<ArcSpaceModel> ReadArcSpaceModels(
    const std::vector<const base::ListValue*>& lists) {
  std::vector<ArcSpaceModel> order;
  for (const base::ListValue* list : lists) {
    WalkIdObjectList(*list, [&](const std::string& id,
                                const base::DictValue& d) {
      const std::string* own = d.FindString("id");
      ArcSpaceModel model;
      model.id = own && !own->empty() ? *own : id;
      if (const std::string* title = d.FindString("title")) {
        model.title = *title;
      }
      if (const base::ListValue* ids = d.FindList("containerIDs")) {
        for (const base::Value& v : *ids) {
          if (v.is_string()) {
            model.container_ids.push_back(v.GetString());
          }
        }
      }
      model.profile_directory = ArcProfileDirectory(d);
      for (ArcSpaceModel& existing : order) {
        if (existing.id == model.id) {
          existing = std::move(model);
          return;
        }
      }
      order.push_back(std::move(model));
    });
  }
  return order;
}

std::string SplitLayoutFromArc(const std::string& orientation) {
  // Arc names the axis the panes run along; Phi the divider between them.
  if (orientation == "horizontal") {
    return "vertical";
  }
  if (orientation == "vertical") {
    return "horizontal";
  }
  return std::string();
}

// The Arc tree, materialized from the flat items: a split of two plain tabs
// becomes one split entry; any other split is replaced by its tabs in place.
using ArcItems = std::map<std::string, ArcItem>;

void AppendArcEntry(const ArcItems& items,
                    const std::string& id,
                    std::set<std::string>* path,
                    std::vector<PhiImportBookmark>* out);

std::vector<PhiImportBookmark> ArcChildren(const ArcItems& items,
                                           const std::string& id,
                                           std::set<std::string>* path) {
  std::vector<PhiImportBookmark> out;
  auto it = items.find(id);
  if (it == items.end() || !path->insert(id).second) {
    return out;  // Unknown, or a loop Arc never writes.
  }
  for (const std::string& child_id : it->second.children) {
    AppendArcEntry(items, child_id, path, &out);
  }
  path->erase(id);
  return out;
}

std::optional<PhiImportBookmark> ArcSplitPair(const ArcItems& items,
                                              const ArcItem& split) {
  if (split.children.size() != 2) {
    return std::nullopt;
  }
  auto first = items.find(split.children.front());
  auto second = items.find(split.children.back());
  for (auto pane : {first, second}) {
    if (pane == items.end() || pane->second.folder || pane->second.url.empty()) {
      return std::nullopt;
    }
  }
  PhiImportBookmark entry;
  entry.title = first->second.title;
  entry.url = first->second.url;
  entry.secondary_url = second->second.url;
  entry.secondary_title = second->second.title;
  entry.layout = SplitLayoutFromArc(split.split_orientation);
  return entry;
}

void AppendArcEntry(const ArcItems& items,
                    const std::string& id,
                    std::set<std::string>* path,
                    std::vector<PhiImportBookmark>* out) {
  auto it = items.find(id);
  if (it == items.end()) {
    return;
  }
  const ArcItem& item = it->second;
  if (!item.split) {
    PhiImportBookmark node;
    node.title = item.title;
    node.url = item.url;
    node.folder = item.folder;
    if (item.folder) {
      node.children = ArcChildren(items, id, path);
    }
    out->push_back(std::move(node));
    return;
  }
  if (std::optional<PhiImportBookmark> pair = ArcSplitPair(items, item)) {
    out->push_back(std::move(*pair));
    return;
  }
  for (PhiImportBookmark& tab : ArcChildren(items, id, path)) {
    out->push_back(std::move(tab));
  }
}

bool IsEmptyPlaceholderFolder(const PhiImportBookmark& node) {
  if (!node.folder || !node.children.empty()) {
    return false;
  }
  const std::string title(base::TrimWhitespaceASCII(node.title, base::TRIM_ALL));
  return title.empty() || title == "Folder";
}

// The pinned containers a Space's containerIDs name, with Arc's section
// markers: everything after "unpinned" is the Space's open tabs, not pins.
std::vector<std::string> PinnedContainerIds(
    const ArcSpaceModel& space,
    const std::map<std::string, ArcItem>& items) {
  std::vector<std::string> out;
  bool unpinned = false;
  for (const std::string& id : space.container_ids) {
    if (id == "pinned") {
      unpinned = false;
      continue;
    }
    if (id == "unpinned") {
      unpinned = true;
      continue;
    }
    if (unpinned) {
      continue;
    }
    auto it = items.find(id);
    if (it != items.end() && it->second.container &&
        it->second.container_space == space.id) {
      out.push_back(id);
    }
  }
  if (!out.empty()) {
    return out;
  }
  // No usable model list: any container of this Space that is not the
  // unpinned one.
  for (const auto& [id, item] : items) {
    if (item.container && item.container_space == space.id &&
        base::ToLowerASCII(id).find("unpinned") == std::string::npos) {
      out.push_back(id);
    }
  }
  return out;
}

// --- Folders on this machine -------------------------------------------------------

std::wstring LocalAppData() {
  PWSTR path = nullptr;
  std::wstring out;
  if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr,
                                       &path)) &&
      path) {
    out = path;
  }
  ::CoTaskMemFree(path);
  return out;
}

bool DirectoryExists(const std::wstring& path) {
  const DWORD attributes = ::GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES &&
         (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

// Arc for Windows is an MSIX package: its %LOCALAPPDATA% lands in the
// package's LocalCache (the fork's GetArcUserDataFolder reads the same).
std::wstring ArcDataFolder() {
  const std::wstring local = LocalAppData();
  if (local.empty()) {
    return std::wstring();
  }
  WIN32_FIND_DATAW found = {};
  const std::wstring pattern = local + L"\\Packages\\TheBrowserCompany.Arc_*";
  HANDLE find = ::FindFirstFileW(pattern.c_str(), &found);
  if (find == INVALID_HANDLE_VALUE) {
    return std::wstring();
  }
  std::wstring out;
  do {
    if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
      continue;
    }
    const std::wstring arc = local + L"\\Packages\\" + found.cFileName +
                             L"\\LocalCache\\Local\\Arc";
    if (DirectoryExists(arc + L"\\User Data")) {
      out = arc;
      break;
    }
  } while (::FindNextFileW(find, &found));
  ::FindClose(find);
  return out;
}

// --- Landing --------------------------------------------------------------------------

int LandTree(PhiTabStore& store,
             const std::string& profile_id,
             const std::string& space_id,
             const std::string& parent,
             const PhiImportBookmark& node,
             store::Source source) {
  if (node.folder) {
    const std::string guid = store.AddBookmark(
        profile_id, space_id, parent,
        node.title.empty() ? "Untitled" : node.title, std::string(),
        /*folder=*/true, std::string(), 0, 0, source);
    if (guid.empty()) {
      return 0;
    }
    int written = 0;
    for (const PhiImportBookmark& child : node.children) {
      written += LandTree(store, profile_id, space_id, guid, child, source);
    }
    return written;
  }
  if (node.url.empty()) {
    return 0;
  }
  const std::string guid = store.AddBookmark(
      profile_id, space_id, parent, node.title.empty() ? node.url : node.title,
      node.url, /*folder=*/false, std::string(), 0, 0, source);
  if (guid.empty()) {
    return 0;
  }
  if (!node.secondary_url.empty()) {
    store.SetRowSecondary(guid, node.secondary_url, node.secondary_title);
    if (!node.layout.empty()) {
      store.SetRowLayout(guid, node.layout);
    }
  }
  return 1;
}

PhiImportBookmark FromChromiumJson(const base::DictValue& d) {
  PhiImportBookmark node;
  if (const std::string* title = d.FindString("title")) {
    node.title = *title;
  }
  node.folder = d.FindBool("folder").value_or(false);
  if (const std::string* url = d.FindString("url"); url && !node.folder) {
    node.url = *url;
  }
  if (const base::ListValue* children = d.FindList("children")) {
    for (const base::Value& child : *children) {
      if (const base::DictValue* c = child.GetIfDict()) {
        node.children.push_back(FromChromiumJson(*c));
      }
    }
  }
  return node;
}

// Where an imported-browser folder sorts among its kind at the Space root:
// the window's row order. Nullopt for any other folder. Matched on the
// title as the Mac does ("Imported From Chrome" carries the browser's
// name in every language Chromium ships).
std::optional<int> ImportedFolderRank(const std::string& title,
                                      const std::string& arc_title) {
  if (title == arc_title) {
    return 3;
  }
  const std::string lower = base::ToLowerASCII(title);
  if (lower.find("chrome") != std::string::npos) {
    return 0;
  }
  if (lower.find("edge") != std::string::npos) {
    return 1;
  }
  if (lower.find("brave") != std::string::npos) {
    return 2;
  }
  return std::nullopt;
}

}  // namespace

const char* PhiImportSourceKey(PhiImportSource source) {
  return InfoOf(source).key;
}

std::optional<PhiImportSource> PhiImportSourceFromKey(std::string_view key) {
  for (const SourceInfo& info : kSourceInfo) {
    if (key == info.key) {
      return info.source;
    }
  }
  return std::nullopt;
}

const wchar_t* PhiImportSourceName(PhiImportSource source) {
  return InfoOf(source).name;
}

bool PhiImportSourceIsChromium(PhiImportSource source) {
  return source != PhiImportSource::kFile;
}

const char* PhiImportDataTypeWire(PhiImportDataType type) {
  switch (type) {
    case PhiImportDataType::kBookmarks:
      return "favorites";  // Chromium's word.
    case PhiImportDataType::kHistory:
      return "history";
    case PhiImportDataType::kCookies:
      return "cookies";
    case PhiImportDataType::kExtensions:
      return "extensions";
  }
  return "";
}

const char* PhiImportDataTypeAnalytics(PhiImportDataType type) {
  return type == PhiImportDataType::kBookmarks ? "bookmarks"
                                               : PhiImportDataTypeWire(type);
}

const char* PhiImportDataTypeLabelKey(PhiImportDataType type) {
  switch (type) {
    case PhiImportDataType::kBookmarks:
      return "oobe.importDataType.bookmarksToggle";
    case PhiImportDataType::kHistory:
      return "oobe.importDataType.browsingHistoryToggle";
    case PhiImportDataType::kCookies:
      return "oobe.importDataType.cookiesToggle";
    case PhiImportDataType::kExtensions:
      return "oobe.importDataType.extensionsToggle";
  }
  return "";
}

std::vector<PhiImportDataType> PhiImportAvailableTypes(PhiImportSource source) {
  if (source == PhiImportSource::kFile) {
    return {};
  }
  return {PhiImportDataType::kBookmarks, PhiImportDataType::kHistory,
          PhiImportDataType::kCookies, PhiImportDataType::kExtensions};
}

std::vector<PhiChromiumProfile> PhiParseChromiumProfiles(
    std::string_view local_state_json) {
  std::vector<PhiChromiumProfile> out;
  std::optional<base::DictValue> root = base::JSONReader::ReadDict(
      local_state_json, base::JSON_PARSE_CHROMIUM_EXTENSIONS);
  const base::DictValue* profile = root ? root->FindDict("profile") : nullptr;
  const base::ListValue* order =
      profile ? profile->FindList("profiles_order") : nullptr;
  const base::DictValue* cache =
      profile ? profile->FindDict("info_cache") : nullptr;
  if (!order || !cache) {
    return out;
  }
  for (const base::Value& entry : *order) {
    if (!entry.is_string()) {
      continue;
    }
    const base::DictValue* info = cache->FindDict(entry.GetString());
    if (!info) {
      continue;
    }
    PhiChromiumProfile p;
    p.directory = entry.GetString();
    const std::string* name = info->FindString("name");
    p.name = name && !name->empty() ? *name : p.directory;
    if (const std::string* email = info->FindString("user_name")) {
      p.email = *email;
    }
    out.push_back(std::move(p));
  }
  return out;
}

std::optional<std::vector<PhiArcSpace>> PhiParseArcSidebar(
    std::string_view json,
    const std::string& untitled) {
  std::optional<base::DictValue> root =
      base::JSONReader::ReadDict(json, base::JSON_PARSE_RFC);
  const base::DictValue* sync =
      root ? root->FindDict("sidebarSyncState") : nullptr;
  const base::DictValue* sidebar = root ? root->FindDict("sidebar") : nullptr;
  const base::ListValue* sync_items = sync ? sync->FindList("items") : nullptr;
  const base::ListValue* containers =
      sidebar ? sidebar->FindList("containers") : nullptr;
  if (!sync_items || !containers) {
    return std::nullopt;
  }
  // The synced items, then the live sidebar's own on top of them.
  std::map<std::string, ArcItem> items;
  ReadArcItems(*sync_items, &items);
  std::vector<const base::ListValue*> container_spaces;
  for (const base::Value& c : *containers) {
    const base::DictValue* container = c.GetIfDict();
    if (!container) {
      continue;
    }
    if (const base::ListValue* own = container->FindList("items")) {
      ReadArcItems(*own, &items);
    }
    if (const base::ListValue* spaces = container->FindList("spaces")) {
      container_spaces.push_back(spaces);
    }
  }
  std::vector<ArcSpaceModel> models = ReadArcSpaceModels(container_spaces);
  if (models.empty()) {
    if (const base::ListValue* synced = sync->FindList("spaceModels")) {
      models = ReadArcSpaceModels({synced});
    }
  }

  std::vector<PhiArcSpace> out;
  for (const ArcSpaceModel& model : models) {
    PhiArcSpace space;
    space.id = model.id;
    space.title =
        std::string(base::TrimWhitespaceASCII(model.title, base::TRIM_ALL));
    if (space.title.empty()) {
      space.title = untitled;
    }
    space.profile_directory = model.profile_directory;
    space.root.folder = true;
    space.root.title = space.title;
    // A pin reachable from two of the Space's containers lands once.
    std::set<std::string> seen;
    for (const std::string& container : PinnedContainerIds(model, items)) {
      for (const std::string& child_id : items.at(container).children) {
        if (!seen.insert(child_id).second) {
          continue;
        }
        std::set<std::string> path = {container};
        std::vector<PhiImportBookmark> entries;
        AppendArcEntry(items, child_id, &path, &entries);
        for (PhiImportBookmark& entry : entries) {
          if (!IsEmptyPlaceholderFolder(entry)) {
            space.root.children.push_back(std::move(entry));
          }
        }
      }
    }
    out.push_back(std::move(space));
  }
  return out;
}

std::wstring PhiImportUserDataDir(PhiImportSource source) {
  std::wstring path;
  switch (source) {
    case PhiImportSource::kChrome:
      path = LocalAppData() + L"\\Google\\Chrome\\User Data";
      break;
    case PhiImportSource::kEdge:
      path = LocalAppData() + L"\\Microsoft\\Edge\\User Data";
      break;
    case PhiImportSource::kBrave:
      path = LocalAppData() + L"\\BraveSoftware\\Brave-Browser\\User Data";
      break;
    case PhiImportSource::kArc: {
      const std::wstring arc = ArcDataFolder();
      if (arc.empty()) {
        return std::wstring();
      }
      path = arc + L"\\User Data";
      break;
    }
    case PhiImportSource::kFile:
      return std::wstring();
  }
  return DirectoryExists(path) ? path : std::wstring();
}

std::wstring PhiArcSidebarPath() {
  const std::wstring arc = ArcDataFolder();
  return arc.empty() ? std::wstring() : arc + L"\\StorableSidebar.json";
}

std::vector<PhiChromiumProfile> PhiLoadChromiumProfiles(PhiImportSource source) {
  const std::wstring dir = PhiImportUserDataDir(source);
  std::string bytes;
  if (dir.empty() || !PhiReadFileBytes(dir + L"\\Local State", &bytes)) {
    return {};
  }
  return PhiParseChromiumProfiles(bytes);
}

bool PhiImportUsesAppBoundKey(PhiImportSource source) {
  const std::wstring dir = PhiImportUserDataDir(source);
  std::string bytes;
  if (dir.empty() || !PhiReadFileBytes(dir + L"\\Local State", &bytes)) {
    return false;
  }
  std::optional<base::DictValue> root =
      base::JSONReader::ReadDict(bytes, base::JSON_PARSE_CHROMIUM_EXTENSIONS);
  const std::string* key =
      root ? root->FindStringByDottedPath("os_crypt.app_bound_encrypted_key")
           : nullptr;
  return key && !key->empty();
}

std::vector<PhiImportDataType> PhiImportOfferedTypes(PhiImportSource source) {
  std::vector<PhiImportDataType> types = PhiImportAvailableTypes(source);
  if (PhiImportUsesAppBoundKey(source)) {
    std::erase(types, PhiImportDataType::kCookies);
  }
  return types;
}

std::vector<PhiArcSpace> PhiLoadArcSpaces(const std::string& untitled) {
  const std::wstring path = PhiArcSidebarPath();
  std::string bytes;
  if (path.empty() || !PhiReadFileBytes(path, &bytes)) {
    return {};
  }
  std::optional<std::vector<PhiArcSpace>> spaces =
      PhiParseArcSidebar(bytes, untitled);
  if (!spaces) {
    LOG(WARNING) << "[Import] StorableSidebar.json is not Arc's shape";
    return {};
  }
  std::stable_sort(spaces->begin(), spaces->end(),
                   [](const PhiArcSpace& a, const PhiArcSpace& b) {
                     return base::CompareCaseInsensitiveASCII(a.title,
                                                              b.title) < 0;
                   });
  return std::move(*spaces);
}

int PhiLandChromiumBookmarks(PhiTabStore& store,
                             const std::string& profile_id,
                             const std::string& space_id,
                             const base::ListValue& tree) {
  // The bookmark bar is the model root's first child (AllBookmarksJson
  // keeps the model's order); its title is in the UI language, so it is
  // found by position, never by name.
  const base::DictValue* bar = tree.empty() ? nullptr : tree[0].GetIfDict();
  if (!bar || !bar->FindBool("folder").value_or(false)) {
    LOG(ERROR) << "[Import] no bookmark bar in the Chromium tree ("
               << tree.size() << " root nodes)";
    return -1;
  }
  int written = 0;
  const base::ListValue* children = bar->FindList("children");
  if (!children) {
    return 0;
  }
  for (const base::Value& child : *children) {
    if (const base::DictValue* d = child.GetIfDict()) {
      written += LandTree(store, profile_id, space_id, std::string(),
                          FromChromiumJson(*d), store::Source::kChromium);
    }
  }
  return written;
}

int PhiLandArcSpace(PhiTabStore& store,
                    const std::string& profile_id,
                    const std::string& space_id,
                    const PhiArcSpace& space,
                    const std::string& arc_folder_title) {
  if (space.root.children.empty()) {
    return 0;  // No empty Space-named folder.
  }
  const std::string import_root = store.AddBookmark(
      profile_id, space_id, std::string(), arc_folder_title, std::string(),
      /*folder=*/true, std::string(), 0, 0, store::Source::kArc);
  if (import_root.empty()) {
    return 0;
  }
  const std::string landing = store.AddBookmark(
      profile_id, space_id, import_root, space.title, std::string(),
      /*folder=*/true, std::string(), 0, 0, store::Source::kArc);
  if (landing.empty()) {
    return 0;
  }
  int written = 0;
  for (const PhiImportBookmark& child : space.root.children) {
    written += LandTree(store, profile_id, space_id, landing, child,
                        store::Source::kArc);
  }
  return written;
}

void PhiReorderImportedFolders(PhiTabStore& store,
                               const std::string& profile_id,
                               const std::string& space_id,
                               const std::string& arc_folder_title) {
  const std::vector<PhiRow> rows = store.BookmarksFor(
      profile_id, space_id, std::string(), PhiFaviconForm::kNone);
  std::vector<std::pair<std::pair<int, int>, std::string>> imported;
  std::vector<std::string> folders;
  std::vector<std::string> others;
  for (size_t i = 0; i < rows.size(); ++i) {
    const PhiRow& row = rows[i];
    if (row.type != PhiRowType::kBookmarkFolder) {
      others.push_back(row.guid);
      continue;
    }
    if (std::optional<int> rank =
            ImportedFolderRank(row.title, arc_folder_title)) {
      imported.push_back({{*rank, static_cast<int>(i)}, row.guid});
    } else {
      folders.push_back(row.guid);
    }
  }
  if (imported.empty()) {
    return;
  }
  std::stable_sort(imported.begin(), imported.end());
  std::vector<std::string> order = folders;
  for (const auto& [rank, guid] : imported) {
    order.push_back(guid);
  }
  order.insert(order.end(), others.begin(), others.end());
  for (size_t i = 0; i < order.size(); ++i) {
    store.Reorder(order[i], static_cast<int>(i));
  }
}

}  // namespace phi_client
