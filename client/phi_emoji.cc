// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "client/phi_emoji.h"

#include <windows.h>

#include <map>

#include "base/files/file_path.h"
#include "base/json/json_reader.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/path_service.h"
#include "base/values.h"
#include "client/phi_icons.h"

namespace phi_client {

namespace {

struct Catalog {
  std::vector<PhiEmojiGroup> groups;
  std::map<std::string, std::string> text_by_id;
};

// Reads a whole file with plain Win32 I/O. The catalog loads LAZILY, on the
// browser UI thread, from whichever surface first needs an emoji (the icon
// picker, agentSpace.emoji.*) — and that thread disallows blocking calls once
// the main loop runs, so base::ReadFileToString there is a DCHECK crash in
// the dev build (thread_restrictions.cc:62, seen on the box). The client's
// other stores read this way for the same reason; the catalog is ~200 KB.
bool ReadWholeFile(const std::wstring& path, std::string* out) {
  HANDLE f = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
  if (f == INVALID_HANDLE_VALUE) {
    return false;
  }
  char buf[16384];
  DWORD read = 0;
  while (::ReadFile(f, buf, sizeof(buf), &read, nullptr) && read > 0) {
    out->append(buf, read);
  }
  ::CloseHandle(f);
  return true;
}

// Reads the catalog the Mac bundles, shipped beside the exe by the build.
Catalog LoadCatalog() {
  Catalog catalog;
  base::FilePath dir;
  if (!base::PathService::Get(base::DIR_EXE, &dir)) {
    return catalog;
  }
  const base::FilePath path =
      dir.AppendASCII("resources").AppendASCII("emoji-catalog.json");
  std::string json;
  if (!ReadWholeFile(path.value(), &json)) {
    LOG(ERROR) << "[PhiClient] emoji catalog missing: " << path;
    return catalog;
  }
  std::optional<base::DictValue> root =
      base::JSONReader::ReadDict(json, base::JSON_PARSE_RFC);
  if (!root) {
    LOG(ERROR) << "[PhiClient] emoji catalog is not valid JSON";
    return catalog;
  }
  const base::ListValue* groups = root->FindList("groups");
  if (!groups) {
    return catalog;
  }
  for (const base::Value& group_value : *groups) {
    if (!group_value.is_dict()) {
      continue;
    }
    const base::DictValue& group_dict = group_value.GetDict();
    PhiEmojiGroup group;
    if (const std::string* name = group_dict.FindString("name")) {
      group.name = *name;
    }
    const base::ListValue* items = group_dict.FindList("items");
    if (!items) {
      continue;
    }
    for (const base::Value& item_value : *items) {
      if (!item_value.is_dict()) {
        continue;
      }
      const base::DictValue& item = item_value.GetDict();
      const std::string* id = item.FindString("id");
      const std::string* text = item.FindString("text");
      if (!id || !text) {
        continue;
      }
      PhiEmoji emoji;
      emoji.id = *id;
      emoji.text = *text;
      if (const std::string* name = item.FindString("name")) {
        emoji.name = *name;
      }
      catalog.text_by_id[emoji.id] = emoji.text;
      // Skin-tone variants are addressable by their own ids, exactly as on Mac
      // (EmojiCatalog.text(for:) searches variants too).
      if (const base::ListValue* variants = item.FindList("skinVariants")) {
        for (const base::Value& variant_value : *variants) {
          if (!variant_value.is_dict()) {
            continue;
          }
          const base::DictValue& variant = variant_value.GetDict();
          const std::string* variant_id = variant.FindString("id");
          const std::string* variant_text = variant.FindString("text");
          if (variant_id && variant_text) {
            catalog.text_by_id[*variant_id] = *variant_text;
          }
        }
      }
      group.items.push_back(std::move(emoji));
    }
    catalog.groups.push_back(std::move(group));
  }
  LOG(INFO) << "[PhiClient] emoji catalog: " << catalog.groups.size()
            << " groups, " << catalog.text_by_id.size() << " ids";
  return catalog;
}

const Catalog& GetCatalog() {
  static const base::NoDestructor<Catalog> catalog(LoadCatalog());
  return *catalog;
}

bool StartsWith(const std::string& value, const char* prefix) {
  const std::string p(prefix);
  return value.size() >= p.size() && value.compare(0, p.size(), p) == 0;
}

}  // namespace

const std::vector<PhiEmojiGroup>& PhiEmojiGroups() {
  return GetCatalog().groups;
}

std::string PhiEmojiTextForId(const std::string& id) {
  const auto& by_id = GetCatalog().text_by_id;
  auto it = by_id.find(id);
  return it == by_id.end() ? std::string() : it->second;
}

std::string PhiIconDisplayText(const std::string& storage_value) {
  if (StartsWith(storage_value, kPhiEmojiPrefix)) {
    return PhiEmojiTextForId(
        storage_value.substr(std::string(kPhiEmojiPrefix).size()));
  }
  if (StartsWith(storage_value, kPhiIconPrefix)) {
    // The Mac's Phi icon catalog, stood in by MDL2 glyphs / characters.
    return PhiIconCatalogGlyph(
        storage_value.substr(std::string(kPhiIconPrefix).size()));
  }
  return std::string();
}

bool PhiIconIsValid(const std::string& storage_value) {
  if (StartsWith(storage_value, kPhiIconPrefix)) {
    // In the catalog, or unknown-but-Mac-shaped (an id a NEWER Mac catalog
    // may know); either way it round-trips unharmed.
    return storage_value.size() > std::string(kPhiIconPrefix).size();
  }
  return !PhiIconDisplayText(storage_value).empty();
}

namespace {

// U+FE0E / U+FE0F (EF B8 8E / EF B8 8F in UTF-8) dropped.
std::string StripVariationSelectors(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size(); ++i) {
    if (i + 2 < text.size() && static_cast<unsigned char>(text[i]) == 0xEF &&
        static_cast<unsigned char>(text[i + 1]) == 0xB8 &&
        (static_cast<unsigned char>(text[i + 2]) == 0x8E ||
         static_cast<unsigned char>(text[i + 2]) == 0x8F)) {
      i += 2;
      continue;
    }
    out.push_back(text[i]);
  }
  return out;
}

// A catalog id ("phi-icon-mail") or a legacy numbered one ("phi-icon-22")
// as the canonical catalog id, or empty. The Mac matched legacy numbers by
// shape: the first 47 pictograms count down from 47, the map is 48.
std::string CanonicalIconId(const std::string& id) {
  const std::vector<PhiCatalogIcon>& icons = PhiIconCatalogAll();
  for (const PhiCatalogIcon& icon : icons) {
    if (id == icon.id) {
      return icon.id;
    }
  }
  constexpr char kPrefix[] = "phi-icon-";
  if (!StartsWith(id, kPrefix)) {
    return std::string();
  }
  const std::string number = id.substr(sizeof(kPrefix) - 1);
  if (number.empty() || number.size() > 2 ||
      number.find_first_not_of("0123456789") != std::string::npos) {
    return std::string();
  }
  const int legacy = std::stoi(number);
  const int index = legacy == 48 ? 47 : 47 - legacy;
  if (legacy < 1 || legacy > 48 || index >= static_cast<int>(icons.size())) {
    return std::string();
  }
  return icons[index].id;
}

std::string IconIdForName(const std::string& name) {
  for (const PhiCatalogIcon& icon : PhiIconCatalogAll()) {
    if (name == icon.name) {
      return icon.id;
    }
  }
  return std::string();
}

std::string UpperAscii(std::string value) {
  for (char& c : value) {
    if (c >= 'a' && c <= 'z') {
      c = static_cast<char>(c - 'a' + 'A');
    }
  }
  return value;
}

}  // namespace

std::string PhiEmojiIdForText(const std::string& text) {
  const std::string key = StripVariationSelectors(text);
  if (key.empty()) {
    return std::string();
  }
  for (const auto& [id, glyph] : GetCatalog().text_by_id) {
    if (StripVariationSelectors(glyph) == key) {
      return id;
    }
  }
  return std::string();
}

std::string PhiNormalizeIconName(const std::string& raw) {
  const size_t first = raw.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) {
    return std::string();
  }
  const std::string value =
      raw.substr(first, raw.find_last_not_of(" \t\r\n") - first + 1);
  // The storage forms, as the picker stores them.
  if (StartsWith(value, kPhiIconPrefix)) {
    const std::string id =
        CanonicalIconId(value.substr(std::string(kPhiIconPrefix).size()));
    if (!id.empty()) {
      return kPhiIconPrefix + id;
    }
  } else if (StartsWith(value, kPhiEmojiPrefix) &&
             !PhiIconDisplayText(value).empty()) {
    return value;
  }
  std::string body = value;
  for (const char* prefix : {kPhiIconPrefix, kPhiEmojiPrefix}) {
    if (StartsWith(body, prefix)) {
      body = body.substr(std::string(prefix).size());
    }
  }
  std::string id = CanonicalIconId(body);
  if (id.empty()) {
    id = IconIdForName(body);
  }
  if (id.empty()) {
    id = CanonicalIconId("phi-icon-" + body);
  }
  if (!id.empty()) {
    return kPhiIconPrefix + id;
  }
  if (!PhiEmojiTextForId(UpperAscii(body)).empty()) {
    return kPhiEmojiPrefix + UpperAscii(body);
  }
  if (const std::string emoji = PhiEmojiIdForText(body); !emoji.empty()) {
    return kPhiEmojiPrefix + emoji;
  }
  return std::string();
}

}  // namespace phi_client
