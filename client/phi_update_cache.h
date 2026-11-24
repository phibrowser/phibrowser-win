// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_UPDATE_CACHE_H_
#define PHIBROWSER_WIN_CLIENT_PHI_UPDATE_CACHE_H_

#include <windows.h>

#include <cstdint>
#include <optional>
#include <string>

namespace phi_client {

// The downloaded update installer, kept between attempts (PHI-2199).
//
// WinSparkle downloads the full installer into a fresh %TEMP%\Update-<uuid>
// folder on every "Install update" and never looks at it again, so a
// cancelled UAC prompt, a failed launch, a second click on the Update pill
// or a restart all meant the whole package again. PhiUpdater copies the
// file WinSparkle verified into this cache, with the version and the EdDSA
// signature the feed gave for it, and installs from here next time. Every
// run re-verifies the file against that signature and the build's feed key,
// so a file changed on disk is never started: it is deleted instead.

// One <enclosure> of the update feed.
struct PhiUpdateEnclosure {
  std::string version;    // sparkle:version
  std::string url;
  std::string signature;  // sparkle:edSignature, base64
  uint64_t length = 0;
};

// The enclosure WinSparkle would pick: the newest version among the items
// that carry one for `os_tag` ("windows-x64" / "windows-arm64"), preferring
// that tag over the generic "windows" and an untagged one, as WinSparkle
// 0.9.4 does. Empty when the feed has none.
std::optional<PhiUpdateEnclosure> PhiParseAppcast(const std::string& xml,
                                                  const char* os_tag);

// Dotted numeric versions ("0.8.0.23"): <0, 0, >0. A missing part is 0, a
// non-numeric part compares as text.
int PhiCompareUpdateVersions(const std::string& a, const std::string& b);

// The cache folder's record of the one installer it holds.
struct PhiCachedUpdate {
  std::string version;
  std::string signature;  // base64, from the feed
  uint64_t length = 0;
  std::wstring file;      // the installer's full path
};

class PhiUpdateCache {
 public:
  explicit PhiUpdateCache(std::wstring dir);

  // The record, when one exists and names a file that is there with the
  // recorded length. NOT verified: see OpenVerified.
  std::optional<PhiCachedUpdate> Read() const;

  // Replaces whatever is cached with a copy of `source` under `enclosure`'s
  // version and signature. False (and nothing cached) on any failure.
  bool Store(const std::wstring& source, const PhiUpdateEnclosure& enclosure);

  // Removes every file in the folder, then the folder. Best effort: a file
  // still open elsewhere stays until the next Clear.
  void Clear() const;

  const std::wstring& dir() const { return dir_; }

 private:
  std::wstring dir_;
};

// Opens `file` so nobody can write, rename or delete it while it is open,
// checks its length and the Ed25519 `signature_b64` over its bytes against
// `public_key_b64`, and hands back the open handle: keep it open until the
// installer has started, so the bytes that start are the bytes that were
// checked. INVALID_HANDLE_VALUE when the file cannot be opened or does not
// verify.
HANDLE PhiOpenVerifiedUpdate(const std::wstring& file,
                             uint64_t length,
                             const std::string& signature_b64,
                             const std::string& public_key_b64);

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_UPDATE_CACHE_H_
