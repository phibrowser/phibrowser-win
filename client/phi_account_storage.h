// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_ACCOUNT_STORAGE_H_
#define PHIBROWSER_WIN_CLIENT_PHI_ACCOUNT_STORAGE_H_

#include <map>
#include <string>
#include <string_view>

// Where an account's data lives — the Mac's Account.userDataStorage
// (docs/user-data-parity.md §4):
//
//   <UserData>\Phi\users\<account dir>\
//     localDB\LocalStore.sqlite, LocalStoreCompatibility.json
//     defaults\account_defaults.json
//     PhiRestore.json
//
// Chromium's profiles stay global, beside Phi\, as on the Mac. The account
// id is the Auth0 subject, or kGuestAccountId for Guest Mode (and, until a
// session names one, the pre-login placeholder).
//
// The PURE half (the id rules) is host-tested; the directory creation and
// the defaults file are Windows-only.

namespace phi_client {

// Account.defaultUid: Guest Mode's account, and every build without auth.
inline constexpr char kGuestAccountId[] = "default-account-id";

// The account's directory NAME. The Mac uses the raw Auth0 `sub`, which
// holds '|' (e.g. "google-oauth2|1234") — illegal in a Windows file name —
// so the characters Windows refuses become '_', the rule Phi Sentinel
// already uses for the same subject (ServiceBrokerSocketPath). The raw
// subject stays the account's identity everywhere else.
std::string PhiAccountDirName(std::string_view account_id);

// Which account's data a launch opens — the Mac's localDataAccount over the
// same inputs ResolveInitialAccess reads: a finished signed-in account, the
// persisted Guest choice, and otherwise the session's account while its
// onboarding runs (or the guest placeholder with no session at all).
std::string PhiDataAccountFor(bool has_session,
                              std::string_view subject,
                              int onboarding_phase,
                              bool guest_mode_enabled);

}  // namespace phi_client

#if defined(_WIN32)

#include "base/values.h"

namespace phi_client {

// <UserData>\Phi\users\<PhiAccountDirName(account_id)>, without creating it.
std::wstring PhiAccountDirPath(const std::wstring& user_data_dir,
                               const std::string& account_id);

// <UserData>\Phi\users\<PhiAccountDirName(account_id)>, created on demand
// with its localDB\ and defaults\ subdirectories. Empty on failure.
std::wstring PhiEnsureAccountDir(const std::wstring& user_data_dir,
                                 const std::string& account_id);

// The account's defaults\account_defaults.json: the Mac's per-account
// AccountUserDefaults keys, spelled as on the Mac (defaultSpaceId,
// spaceThemeIds, …). A missing or unreadable file reads as empty; every Set
// writes the file at once (write-temp-then-replace).
class PhiAccountDefaults {
 public:
  PhiAccountDefaults();
  ~PhiAccountDefaults();

  void Load(const std::wstring& account_dir);
  void Clear();

  std::string GetString(std::string_view key) const;
  void SetString(std::string_view key, const std::string& value);
  // A {key: string} map (spaceThemeIds).
  std::map<std::string, std::string> GetStringMap(std::string_view key) const;
  void SetStringMap(std::string_view key,
                    const std::map<std::string, std::string>& value);
  // Scalars. A missing or wrongly typed key reads as `fallback`.
  int GetInt(std::string_view key, int fallback) const;
  void SetInt(std::string_view key, int value);
  double GetDouble(std::string_view key, double fallback) const;
  void SetDouble(std::string_view key, double value);
  const std::wstring& dir() const { return dir_; }
  // A nested dictionary (spaceOverlayOpacities); null when absent.
  const base::DictValue* GetDict(std::string_view key) const;
  void SetDict(std::string_view key, base::DictValue value);
  // A {key: int} map (the Windows-only per-Space saturation).
  std::map<std::string, int> GetIntMap(std::string_view key) const;
  void SetIntMap(std::string_view key, const std::map<std::string, int>& value);

 private:
  void Save();

  std::wstring dir_;
  std::wstring path_;
  base::DictValue values_;
  // The file was there but unreadable at Load: never overwritten.
  bool save_blocked_ = false;
};

// The Mac's key names.
inline constexpr char kDefaultsDefaultSpaceId[] = "defaultSpaceId";
inline constexpr char kDefaultsSpaceThemeIds[] = "spaceThemeIds";
// {spaceId: {"light": Double, "dark": Double}}: the Mac's per-Space overlay
// opacities, carried for sync (Windows paints with its saturation).
inline constexpr char kDefaultsSpaceOverlayOpacities[] =
    "spaceOverlayOpacities";
inline constexpr char kDefaultsLoginPhase[] = "loginPhase";
// The membership card's {palette, texture, ornament} (HolographicCardStyle).
inline constexpr char kDefaultsProfileCardStyle[] = "profileCardStyle";
inline constexpr char kDefaultsSidebarWidth[] = "lastKnownSidebarWidth";
// Windows-only: the per-Space saturation (0..100). The Mac's
// spaceThemeSaturations has a different shape and meaning, so it is not
// reused under its name (docs/user-data-parity.md §6, themes).
inline constexpr char kDefaultsWinSpaceSaturations[] = "winSpaceSaturations";
// {spaceId: {version, hue, saturation, brightness, colorCount, harmony}}:
// the Mac's per-Space linked palettes (ThemePalette's own coding keys), a
// LOCAL record on both clients — never on the Space sync payload.
inline constexpr char kDefaultsSpaceThemePalettes[] = "spaceThemePalettes";

}  // namespace phi_client

#endif  // defined(_WIN32)

#endif  // PHIBROWSER_WIN_CLIENT_PHI_ACCOUNT_STORAGE_H_
