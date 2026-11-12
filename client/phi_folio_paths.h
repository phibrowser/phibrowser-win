// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_FOLIO_PATHS_H_
#define PHIBROWSER_WIN_CLIENT_PHI_FOLIO_PATHS_H_

#include <functional>
#include <string>
#include <string_view>

namespace phi_client {

// Folio's pure rules (docs/folio-port.md), STL only so the host tests run
// them: the one file-name rule every broker path asks, the frontmatter cut
// a listing carries, the UTF-8 boundary trim, and the free-basename bump.

// A broker file name is a bare `<basename>.md|.mhtml` inside the folder;
// anything that could escape it is refused (nullopt-as-false). Traversal is
// separators plus a leading dot, not ".." wherever it appears: a title
// ending in a period ("… worth knowing.") makes "… worth knowing..md",
// which is an ordinary file. Windows adds ':' (an alternate data stream or
// a drive) to the Mac's '/' '\\' NUL.
bool PhiFolioBrokerFileName(std::string_view name);

// The leading `---` frontmatter block of a Markdown prefix, through its
// closing `---` line; empty when the prefix holds no complete block.
std::string PhiFolioFrontmatterPrefix(std::string_view data);

// `data` cut back to its last complete UTF-8 sequence, so a fixed byte
// prefix never ends mid-character. Data that is invalid for another reason
// comes back unchanged.
std::string PhiFolioTrimToUtf8Boundary(std::string_view data);

// The first basename whose pair is free — `desired`, then "desired 2",
// "desired 3", … — asking `taken(name)` whether a pair exists.
std::string PhiFolioFirstFreeBasename(
    const std::string& desired,
    const std::function<bool(const std::string&)>& taken);

// True when Markdown `content` carries the user's own highlights: Folio's
// section is the LAST "## Highlights" heading holding "### Highlight"
// entries (an article's own heading of that name does not count). Undo of
// an automatic save yields to them.
bool PhiFolioHasUserHighlights(std::string_view content);

// True for a page Folio can save: an http(s) URL.
bool PhiFolioIsSaveableUrl(std::string_view url);

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_FOLIO_PATHS_H_
