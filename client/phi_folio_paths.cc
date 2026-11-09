// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "client/phi_folio_paths.h"

#include <algorithm>
#include <cctype>

namespace phi_client {

namespace {

bool EndsWith(std::string_view text, std::string_view suffix) {
  return text.size() >= suffix.size() &&
         text.substr(text.size() - suffix.size()) == suffix;
}

bool StartsWithNoCase(std::string_view text, std::string_view prefix) {
  if (text.size() < prefix.size()) {
    return false;
  }
  for (size_t i = 0; i < prefix.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(text[i])) != prefix[i]) {
      return false;
    }
  }
  return true;
}

}  // namespace

bool PhiFolioBrokerFileName(std::string_view name) {
  if (name.empty() || name.front() == '.') {
    return false;
  }
  for (const char c : name) {
    if (c == '/' || c == '\\' || c == '\0' || c == ':') {
      return false;
    }
  }
  return EndsWith(name, ".md") || EndsWith(name, ".mhtml");
}

std::string PhiFolioFrontmatterPrefix(std::string_view data) {
  constexpr std::string_view kOpening = "---\n";
  constexpr std::string_view kClosing = "\n---\n";
  if (data.substr(0, kOpening.size()) != kOpening) {
    return std::string();
  }
  // The closing line may follow the opening one directly ("---\n---\n").
  const size_t close = data.find(kClosing, kOpening.size() - 1);
  if (close == std::string_view::npos) {
    return std::string();
  }
  return std::string(data.substr(0, close + kClosing.size()));
}

std::string PhiFolioTrimToUtf8Boundary(std::string_view data) {
  // A sequence is at most four bytes, so only the last three can belong to
  // an incomplete one. Find the lead byte of the final sequence and drop it
  // when fewer bytes follow than it announces.
  const size_t size = data.size();
  for (size_t back = 1; back <= std::min<size_t>(4, size); ++back) {
    const unsigned char byte = static_cast<unsigned char>(data[size - back]);
    if ((byte & 0xC0) == 0x80) {
      continue;  // A continuation byte: keep looking for the lead.
    }
    size_t expected = 1;
    if ((byte & 0xE0) == 0xC0) {
      expected = 2;
    } else if ((byte & 0xF0) == 0xE0) {
      expected = 3;
    } else if ((byte & 0xF8) == 0xF0) {
      expected = 4;
    }
    if (expected > back) {
      return std::string(data.substr(0, size - back));
    }
    break;
  }
  return std::string(data);
}

std::string PhiFolioFirstFreeBasename(
    const std::string& desired,
    const std::function<bool(const std::string&)>& taken) {
  std::string candidate = desired;
  for (int counter = 2; taken(candidate); ++counter) {
    candidate = desired + " " + std::to_string(counter);
  }
  return candidate;
}

bool PhiFolioHasUserHighlights(std::string_view content) {
  constexpr std::string_view kSection = "\n## Highlights\n";
  const size_t section = content.rfind(kSection);
  if (section == std::string_view::npos) {
    return false;
  }
  return content.find("### Highlight", section + kSection.size()) !=
         std::string_view::npos;
}

bool PhiFolioIsSaveableUrl(std::string_view url) {
  return StartsWithNoCase(url, "http://") || StartsWithNoCase(url, "https://");
}

}  // namespace phi_client
