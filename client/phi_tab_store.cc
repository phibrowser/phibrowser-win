// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#include "client/phi_tab_store.h"

#include <climits>

#include "base/base64.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/values.h"

namespace phi_client {

namespace {

std::string Encode(const std::string& bytes) {
  return bytes.empty() ? std::string() : base::Base64Encode(bytes);
}

std::string Decode(const std::string& b64) {
  std::string out;
  if (!b64.empty() && !base::Base64Decode(b64, &out)) {
    out.clear();
  }
  return out;
}

// One pane's favicon in the form the reader asked for. A raw buffer whose
// size disagrees with its dimensions (a damaged row, a bad sync payload) is
// dropped with them, so no painter reads past it.
void FillFavicon(const std::string& bytes,
                 int width,
                 int height,
                 PhiFaviconForm form,
                 std::string* base64,
                 std::string* raw,
                 int* out_width,
                 int* out_height) {
  switch (form) {
    case PhiFaviconForm::kNone:
      return;
    case PhiFaviconForm::kBase64:
      *base64 = Encode(bytes);
      *out_width = width;
      *out_height = height;
      return;
    case PhiFaviconForm::kRaw:
      if (bytes.empty() || width <= 0 || height <= 0 ||
          bytes.size() != static_cast<size_t>(width) *
                              static_cast<size_t>(height) * 4) {
        return;
      }
      *raw = bytes;
      *out_width = width;
      *out_height = height;
      return;
  }
}


// The panes after the second of a split cell, out of the row's JSON text:
// [{"url":"…","title":"…","favicon":"<base64 BGRA>","faviconWidth":16,
// "faviconHeight":16}]. `title` and the favicon keys are optional; a Mac
// record has url and title only. Unreadable text, or an entry with no url,
// reads as none, so the cell falls back to a pair.
std::vector<PhiRow::ExtraPane> DecodeExtraPanes(
    const std::optional<std::string>& json,
    PhiFaviconForm favicons) {
  std::vector<PhiRow::ExtraPane> out;
  if (!json || json->empty()) {
    return out;
  }
  const std::optional<base::Value> parsed =
      base::JSONReader::Read(*json, base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_list()) {
    return out;
  }
  for (const base::Value& entry : parsed->GetList()) {
    const base::DictValue* dict = entry.GetIfDict();
    const std::string* url = dict ? dict->FindString("url") : nullptr;
    if (!url || url->empty()) {
      return {};
    }
    PhiRow::ExtraPane pane;
    pane.url = *url;
    if (const std::string* title = dict->FindString("title")) {
      pane.title = *title;
    }
    if (const std::string* icon = dict->FindString("favicon")) {
      FillFavicon(Decode(*icon), dict->FindInt("faviconWidth").value_or(0),
                  dict->FindInt("faviconHeight").value_or(0), favicons,
                  &pane.favicon_base64, &pane.favicon_raw,
                  &pane.favicon_width, &pane.favicon_height);
    }
    out.push_back(std::move(pane));
    if (out.size() == 2) {
      break;  // A split holds four panes.
    }
  }
  return out;
}

std::string EncodeExtraPanes(const std::vector<PhiRow::ExtraPane>& panes) {
  base::ListValue list;
  for (const PhiRow::ExtraPane& pane : panes) {
    if (pane.url.empty() || list.size() == 2) {
      continue;
    }
    base::DictValue entry;
    entry.Set("url", pane.url);
    if (!pane.title.empty()) {
      entry.Set("title", pane.title);
    }
    if (!pane.favicon_base64.empty() && pane.favicon_width > 0 &&
        pane.favicon_height > 0) {
      entry.Set("favicon", pane.favicon_base64);
      entry.Set("faviconWidth", pane.favicon_width);
      entry.Set("faviconHeight", pane.favicon_height);
    }
    list.Append(std::move(entry));
  }
  std::string out;
  if (!list.empty()) {
    base::JSONWriter::Write(list, &out);
  }
  return out;
}

}  // namespace

const char* PhiPinScopeName(PhiPinScope scope) {
  return store::PinScopeName(scope);
}

bool PhiPinScopeFromName(const std::string& name, PhiPinScope* out) {
  return store::PinScopeFromName(name, out);
}

PhiTabStore::PhiTabStore(store::LocalStore& store) : store_(store) {}
PhiTabStore::~PhiTabStore() = default;

bool PhiTabStore::SetPinScope(PhiPinScope scope,
                              const std::string& preferred_profile_id,
                              const std::string& preferred_space_id) {
  return store_.ChangePinScope(scope, preferred_profile_id,
                               preferred_space_id);
}

PhiRow PhiTabStore::CellFromPin(const store::Tab& a,
                                const store::Tab* b,
                                int index,
                                PhiFaviconForm favicons) const {
  PhiRow row;
  row.guid = a.guid;
  row.type = PhiRowType::kPinnedTab;
  row.title = a.title;
  row.url = a.url;
  row.index = index;
  row.profile_id = a.profile_id.value_or(std::string());
  row.space_id = a.space_id.value_or(std::string());
  FillFavicon(a.favicon, a.favicon_width, a.favicon_height, favicons,
              &row.favicon_base64, &row.favicon_raw, &row.favicon_width,
              &row.favicon_height);
  row.lineage_id = a.pin_lineage_id.value_or(a.guid);
  row.layout = a.layout.value_or(std::string());
  if (b) {
    row.url_b = b->url;
    row.title_b = b->title;
    FillFavicon(b->favicon, b->favicon_width, b->favicon_height, favicons,
                &row.favicon_b_base64, &row.favicon_b_raw,
                &row.favicon_b_width, &row.favicon_b_height);
    row.partner_guid = b->guid;
    if (row.layout.empty()) {
      row.layout = b->layout.value_or(std::string());
    }
    // The later panes ride the first row; a swap of the pair can leave
    // them on the second.
    row.more = DecodeExtraPanes(a.split_extra_panes, favicons);
    if (row.more.empty()) {
      row.more = DecodeExtraPanes(b->split_extra_panes, favicons);
    }
  }
  return row;
}

std::string PhiTabStore::ParentForUi(const store::Tab& row) const {
  if (!row.parent_guid) {
    return std::string();
  }
  return store_.IsBookmarkRoot(*row.parent_guid) ? std::string()
                                                 : *row.parent_guid;
}

std::optional<std::string> PhiTabStore::ParentForStore(
    const std::string& profile_id,
    const std::string& space_id,
    const std::string& parent) const {
  (void)profile_id;
  (void)space_id;
  if (parent.empty()) {
    return std::nullopt;  // The store resolves nil to the Space's root.
  }
  return parent;
}

PhiRow PhiTabStore::CellFromBookmark(const store::Tab& t,
                                     PhiFaviconForm favicons) const {
  PhiRow row;
  row.guid = t.guid;
  row.type = t.type == store::RowType::kBookmarkFolder
                 ? PhiRowType::kBookmarkFolder
                 : PhiRowType::kBookmark;
  row.title = t.title;
  // Folders carry the placeholder url in the store; the UI never showed one.
  row.url = row.type == PhiRowType::kBookmarkFolder ? std::string() : t.url;
  row.index = t.index;
  row.profile_id = t.profile_id.value_or(std::string());
  row.space_id = t.space_id.value_or(std::string());
  row.parent_guid = ParentForUi(t);
  FillFavicon(t.favicon, t.favicon_width, t.favicon_height, favicons,
              &row.favicon_base64, &row.favicon_raw, &row.favicon_width,
              &row.favicon_height);
  row.url_b = t.secondary_url.value_or(std::string());
  row.title_b = t.secondary_title.value_or(std::string());
  FillFavicon(t.secondary_favicon, t.secondary_favicon_width,
              t.secondary_favicon_height, favicons, &row.favicon_b_base64,
              &row.favicon_b_raw, &row.favicon_b_width,
              &row.favicon_b_height);
  row.icon = t.icon == store::kDefaultFolderIcon ? std::string() : t.icon;
  row.layout = t.layout.value_or(std::string());
  if (!row.url_b.empty()) {
    row.more = DecodeExtraPanes(t.split_extra_panes, favicons);
  }
  return row;
}

std::vector<PhiRow> PhiTabStore::PinnedFor(const std::string& profile_id,
                                           const std::string& space_id,
                                           PhiFaviconForm favicons) const {
  std::vector<PhiRow> out;
  int index = 0;
  for (const store::LocalStore::PinUnit& unit :
       store_.PinUnits(profile_id, space_id)) {
    out.push_back(
        CellFromPin(*unit.primary, unit.secondary, index++, favicons));
  }
  return out;
}

std::string PhiTabStore::AddPinned(const std::string& profile_id,
                                   const std::string& space_id,
                                   const std::string& title,
                                   const std::string& url,
                                   const std::string& favicon_base64,
                                   int favicon_width,
                                   int favicon_height) {
  store::LocalStore::PinCreate in;
  in.url = url;
  in.title = title;
  in.profile_id = profile_id;
  in.space_id = space_id;
  in.favicon = Decode(favicon_base64);
  in.favicon_width = in.favicon.empty() ? 0 : favicon_width;
  in.favicon_height = in.favicon.empty() ? 0 : favicon_height;
  return store_.CreatePin(in);
}

std::vector<PhiRow> PhiTabStore::BookmarksFor(
    const std::string& profile_id,
    const std::string& space_id,
    const std::string& parent_guid,
    PhiFaviconForm favicons) const {
  const std::string parent =
      parent_guid.empty() ? store_.ExistingBookmarkRoot(profile_id, space_id)
                          : parent_guid;
  std::vector<PhiRow> out;
  if (parent.empty()) {
    return out;
  }
  for (const store::Tab* t : store_.Children(parent)) {
    if (t->is_bookmark_kind() && t->profile_id == profile_id &&
        t->space_id == space_id) {
      out.push_back(CellFromBookmark(*t, favicons));
    }
  }
  return out;
}

std::vector<PhiRow> PhiTabStore::BookmarkTree(
    const std::string& profile_id,
    const std::string& space_id,
    PhiFaviconForm favicons) const {
  std::vector<PhiRow> out;
  for (const auto& [guid, t] : store_.tabs()) {
    if (t.is_bookmark_kind() && t.profile_id == profile_id &&
        t.space_id == space_id && !store_.IsBookmarkRoot(guid) &&
        t.parent_guid) {
      out.push_back(CellFromBookmark(t, favicons));
    }
  }
  return out;
}

std::string PhiTabStore::AddBookmark(const std::string& profile_id,
                                     const std::string& space_id,
                                     const std::string& parent_guid,
                                     const std::string& title,
                                     const std::string& url,
                                     bool folder,
                                     const std::string& favicon_base64,
                                     int favicon_width,
                                     int favicon_height,
                                     store::Source source) {
  const std::optional<std::string> parent =
      ParentForStore(profile_id, space_id, parent_guid);
  if (folder) {
    return store_.CreateFolder(profile_id, space_id, parent, std::nullopt,
                               title, std::string(), static_cast<int>(source));
  }
  store::LocalStore::BookmarkCreate in;
  in.profile_id = profile_id;
  in.space_id = space_id;
  in.parent_guid = parent;
  in.title = title;
  in.url = url;
  in.favicon = Decode(favicon_base64);
  in.favicon_width = in.favicon.empty() ? 0 : favicon_width;
  in.favicon_height = in.favicon.empty() ? 0 : favicon_height;
  in.source = static_cast<int>(source);
  return store_.CreateBookmark(in);
}

bool PhiTabStore::Remove(const std::string& guid) {
  const store::Tab* t = store_.FindTab(guid);
  if (!t) {
    return false;
  }
  if (t->type == store::RowType::kPinnedTab) {
    return store_.RemovePinUnit(guid);
  }
  return store_.DeleteBookmark(guid);
}

bool PhiTabStore::Contains(const std::string& guid) const {
  const store::Tab* t = store_.FindTab(guid);
  return t && !store_.IsBookmarkRoot(guid);
}

bool PhiTabStore::SetRowSecondary(const std::string& guid,
                                  const std::string& url_b,
                                  const std::string& title_b,
                                  const std::string& favicon_b_base64,
                                  int favicon_b_width,
                                  int favicon_b_height) {
  const store::Tab* t = store_.FindTab(guid);
  if (!t) {
    return false;
  }
  if (t->type != store::RowType::kPinnedTab) {
    store::LocalStore::BookmarkUpdate update;
    if (url_b.empty()) {
      update.secondary_url = std::optional<std::string>();
    } else {
      update.secondary_url = std::optional<std::string>(url_b);
      update.secondary_title = std::optional<std::string>(title_b);
    }
    if (!store_.UpdateBookmark(guid, update)) {
      return false;
    }
    if (!url_b.empty()) {
      store_.SetFavicon(guid, /*pane_b=*/true, Decode(favicon_b_base64),
                        favicon_b_width, favicon_b_height);
    }
    return true;
  }
  // A pin's pane B is its partner row.
  const std::string partner = t->split_partner_guid.value_or(std::string());
  const store::Tab* partner_row =
      partner.empty() ? nullptr : store_.FindTab(partner);
  if (url_b.empty()) {
    if (!partner_row) {
      return store_.ClearPinSplitPartner(guid);
    }
    // Unsplitting keeps pane A as a plain pin; pane B's row goes.
    const std::string profile = t->profile_id.value_or(std::string());
    const std::string space = t->space_id.value_or(std::string());
    return store_.RemovePin(partner, profile, space) &&
           store_.ClearPinSplitPartner(guid);
  }
  const std::string favicon = Decode(favicon_b_base64);
  if (partner_row) {
    bool ok = store_.UpdatePin(partner, url_b, title_b);
    store_.SetFavicon(partner, /*pane_b=*/false, favicon, favicon_b_width,
                      favicon_b_height);
    return ok;
  }
  // A new pair: the partner lands right after pane A in the same
  // collection, with a lineage of its own.
  int position = 0;
  const std::string profile = t->profile_id.value_or(std::string());
  const std::string space = t->space_id.value_or(std::string());
  std::string layout = t->layout.value_or("vertical");
  for (const store::Tab* pin : store_.ActivePins(profile, space)) {
    ++position;
    if (pin->guid == guid) {
      break;
    }
  }
  store::LocalStore::PinCreate in;
  in.url = url_b;
  in.title = title_b;
  in.profile_id = profile;
  in.space_id = space;
  in.index = position;
  in.favicon = favicon;
  in.favicon_width = favicon.empty() ? 0 : favicon_b_width;
  in.favicon_height = favicon.empty() ? 0 : favicon_b_height;
  const std::string created = store_.CreatePin(in);
  return !created.empty() && store_.SetPinSplitPair(guid, created, layout);
}

bool PhiTabStore::SetRowExtraPanes(
    const std::string& guid,
    const std::vector<PhiRow::ExtraPane>& panes) {
  const store::Tab* t = store_.FindTab(guid);
  if (!t) {
    return false;
  }
  const bool pin = t->type == store::RowType::kPinnedTab;
  const bool split = pin ? t->split_partner_guid.has_value()
                         : t->secondary_url.has_value();
  if (!split) {
    return panes.empty();
  }
  if (pin) {
    // One copy, on the row the cell is named by.
    store_.SetSplitExtraPanes(*t->split_partner_guid, std::nullopt);
  }
  const std::string json = EncodeExtraPanes(panes);
  return store_.SetSplitExtraPanes(
      guid, json.empty() ? std::nullopt : std::optional<std::string>(json));
}

bool PhiTabStore::RenameRow(const std::string& guid, const std::string& title) {
  const store::Tab* t = store_.FindTab(guid);
  if (!t) {
    return false;
  }
  if (t->type == store::RowType::kPinnedTab) {
    return store_.UpdatePin(guid, std::nullopt, title);
  }
  store::LocalStore::BookmarkUpdate update;
  update.title = title;
  // A folder may be renamed to nothing only in the UI's own sense; the
  // store substitutes the url for an empty BOOKMARK title, as on the Mac.
  return store_.UpdateBookmark(guid, update);
}

bool PhiTabStore::SetRowIcon(const std::string& guid, const std::string& icon) {
  const store::Tab* t = store_.FindTab(guid);
  if (!t || t->type != store::RowType::kBookmarkFolder) {
    return false;
  }
  const std::string stored = icon.empty() ? store::kDefaultFolderIcon : icon;
  if (t->icon == stored) {
    return false;
  }
  store::LocalStore::BookmarkUpdate update;
  update.icon = stored;
  return store_.UpdateBookmark(guid, update);
}

bool PhiTabStore::SetRowLayout(const std::string& guid,
                               const std::string& layout) {
  const store::Tab* t = store_.FindTab(guid);
  if (!t) {
    return false;
  }
  if (t->type == store::RowType::kPinnedTab) {
    if (!t->split_partner_guid || t->layout == layout) {
      return false;
    }
    return store_.SetPinSplitLayout(guid, *t->split_partner_guid, layout);
  }
  if (t->layout == layout) {
    return false;
  }
  return store_.SetBookmarkSplitLayout(guid, layout);
}

std::string PhiTabStore::SwapPanes(const std::string& guid) {
  const store::Tab* t = store_.FindTab(guid);
  if (!t) {
    return std::string();
  }
  if (t->type != store::RowType::kPinnedTab) {
    return store_.SwapBookmarkPanes(guid) ? guid : std::string();
  }
  if (!store_.SwapPinSplitOrder(guid)) {
    return std::string();
  }
  // The cell is now named by whichever row sorts first.
  if (std::optional<PhiRow> cell = Find(guid)) {
    return cell->guid;
  }
  return std::string();
}

bool PhiTabStore::RemoveForSpace(const std::string& space_id) {
  return store_.DeleteSpaceCascade(space_id, /*user_intent=*/true);
}

bool PhiTabStore::RehomeSpace(const std::string& space_id,
                              const std::string& profile_id) {
  return store_.ChangeSpaceProfile(space_id, profile_id);
}

bool PhiTabStore::Reorder(const std::string& guid, int new_index) {
  const store::Tab* t = store_.FindTab(guid);
  if (!t) {
    return false;
  }
  if (t->type == store::RowType::kPinnedTab) {
    return store_.MovePinUnit(guid, new_index);
  }
  return store_.MoveBookmark(guid, t->profile_id.value_or(std::string()),
                             t->parent_guid, new_index);
}

std::optional<PhiRow> PhiTabStore::Find(const std::string& guid) const {
  const store::Tab* t = store_.FindTab(guid);
  if (!t || store_.IsBookmarkRoot(guid)) {
    return std::nullopt;
  }
  if (t->type != store::RowType::kPinnedTab) {
    return CellFromBookmark(*t, PhiFaviconForm::kBase64);
  }
  int index = 0;
  for (const store::LocalStore::PinUnit& unit :
       store_.PinUnits(t->profile_id.value_or(std::string()),
                       t->space_id.value_or(std::string()))) {
    if (unit.primary->guid == guid ||
        (unit.secondary && unit.secondary->guid == guid)) {
      return CellFromPin(*unit.primary, unit.secondary, index,
                         PhiFaviconForm::kBase64);
    }
    ++index;
  }
  // An inactive-scope backup: shown as itself.
  return CellFromPin(*t, nullptr, t->index, PhiFaviconForm::kBase64);
}

bool PhiTabStore::SetRowUrl(const std::string& guid, const std::string& url) {
  const store::Tab* t = store_.FindTab(guid);
  if (!t || url.empty() || t->type == store::RowType::kBookmarkFolder) {
    return false;
  }
  bool ok = false;
  if (t->type == store::RowType::kPinnedTab) {
    ok = store_.UpdatePin(guid, url, std::nullopt);
  } else {
    store::LocalStore::BookmarkUpdate update;
    update.url = url;
    ok = store_.UpdateBookmark(guid, update);
  }
  if (ok) {
    store_.SetFavicon(guid, /*pane_b=*/false, std::string(), 0, 0);
  }
  return ok;
}

bool PhiTabStore::Move(const std::string& guid,
                       const std::string& new_parent,
                       int new_index) {
  const store::Tab* t = store_.FindTab(guid);
  if (!t || !t->is_bookmark_kind()) {
    return false;
  }
  const std::string profile = t->profile_id.value_or(std::string());
  const std::string space = t->space_id.value_or(std::string());
  if (!new_parent.empty()) {
    const store::Tab* parent = store_.FindTab(new_parent);
    if (!parent || parent->type != store::RowType::kBookmarkFolder ||
        parent->profile_id != t->profile_id ||
        parent->space_id != t->space_id) {
      return false;
    }
  }
  return store_.MoveBookmark(guid, profile,
                             ParentForStore(profile, space, new_parent),
                             new_index >= 0 ? new_index : INT_MAX);
}

bool PhiTabStore::SetFavicon(const std::string& guid,
                             bool pane_b,
                             const std::string& favicon_base64,
                             int width,
                             int height) {
  const store::Tab* t = store_.FindTab(guid);
  if (!t) {
    return false;
  }
  if (pane_b && t->type == store::RowType::kPinnedTab) {
    if (!t->split_partner_guid) {
      return false;
    }
    return store_.SetFavicon(*t->split_partner_guid, false,
                             Decode(favicon_base64), width, height);
  }
  return store_.SetFavicon(guid, pane_b, Decode(favicon_base64), width,
                           height);
}

}  // namespace phi_client
