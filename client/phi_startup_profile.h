// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_STARTUP_PROFILE_H_
#define PHIBROWSER_WIN_CLIENT_PHI_STARTUP_PROFILE_H_

#include <string>
#include <string_view>

namespace phi_client {

// The profile Chromium starts in (its --profile-directory). The startup
// browser becomes the DEFAULT Space's window, so it has to open in that
// Space's profile — which is not always "Default": the default role passes
// to another Space when its holder is deleted, and a synced account may
// have no Space in "Default" at all. The client records the default Space's
// profile in this file (one line, the on-disk basename) whenever it
// changes; the exe reads it back before Chromium starts
// (PhiClientStartupProfile, asked by ChromeMainDelegate::BasicStartupComplete).
inline constexpr wchar_t kPhiStartupProfileFile[] = L"PhiStartupProfile";

// A basename that can only name a folder directly under the user data dir:
// non-empty, short, no separators, not "." or "..".
bool PhiIsProfileBasename(std::string_view name);

// The recorded startup profile, or "" when the file is missing, unreadable,
// malformed, or names a folder that no longer exists (Chromium would create
// a fresh profile for an unknown name) — the caller then starts "Default".
std::string PhiReadStartupProfile(const std::wstring& user_data_dir);

// Records `profile_id` (atomic replace). False when it is not a valid
// basename or the write failed.
bool PhiWriteStartupProfile(const std::wstring& user_data_dir,
                            const std::string& profile_id);

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_STARTUP_PROFILE_H_
