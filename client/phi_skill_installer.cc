// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "client/phi_skill_installer.h"

#include <windows.h>

#include <shellapi.h>
#include <shlobj.h>
#include <winioctl.h>

#include <cstdint>
#include <iterator>
#include <vector>

#include "base/compiler_specific.h"
#include "base/no_destructor.h"
#include "client/phi_i18n.h"
#include "client/phi_util.h"

namespace phi_client {

namespace {

constexpr wchar_t kLinkName[] = L"phi-browser";

// Every agent the skill can be linked into, in the Mac's order. Grok Bot
// (the desktop app) has no folder of its own and skips third-party folders
// (Grok Build's, Claude Code's, Codex's) unless those are enabled; it always
// reads Cursor's and the common folder. Its row therefore links the common
// folder: one link serves it and "Common agents", and both rows tick
// together (the Mac's `1e890904`).
// "Common agents" is the cross-agent folder of the agentskills.io
// convention (Deep Code, Kimi Code, Cline, DeepSeek Harness, …).
const std::vector<PhiSkillTarget>& Targets() {
  static const base::NoDestructor<std::vector<PhiSkillTarget>> targets({
      {"claude", L"Claude Code", "claude", L".claude\\skills", nullptr},
      {"codex", L"Codex", "openai", L".codex\\skills", nullptr},
      {"cursor", L"Cursor", "cursor", L".cursor\\skills", nullptr},
      {"hermes", L"Hermes", "hermes", L".hermes\\skills", nullptr},
      {"openclaw", L"OpenClaw", "openclaw", L".openclaw\\skills", nullptr},
      {"pi", L"Pi", "pi", L".pi\\agent\\skills", L".pi\\agent\\extensions"},
      {"grok", L"Grok Build", "grok", L".grok\\skills", nullptr},
      {"grokbot", L"Grok Bot", "grokbot", L".agents\\skills", nullptr},
      // Antigravity's "shared" folder, which Gemini CLI reads too.
      {"antigravity", L"Antigravity", "antigravity", L".gemini\\skills",
       nullptr},
      {"copilot", L"GitHub Copilot", "copilot", L".copilot\\skills", nullptr},
      {"opencode", L"OpenCode", "opencode", L".config\\opencode\\skills",
       nullptr},
      {"qwen", L"Qwen Code", "qwen", L".qwen\\skills", nullptr},
      {"codebuddy", L"CodeBuddy", "codebuddy", L".codebuddy\\skills",
       nullptr},
      // No single brand owns the cross-agent folder: the generic terminal.
      {"agents", L"Common agents", "generic", L".agents\\skills", nullptr},
  });
  return *targets;
}

// The user's profile folder, no trailing separator.
std::wstring HomeDir() {
  static const base::NoDestructor<std::wstring> home([] {
    std::wstring result;
    PWSTR path = nullptr;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr,
                                         &path))) {
      result = path;
    }
    if (path) {
      ::CoTaskMemFree(path);
    }
    if (result.empty()) {
      wchar_t buffer[MAX_PATH] = {};
      if (::GetEnvironmentVariableW(L"USERPROFILE", buffer, MAX_PATH)) {
        result = buffer;
      }
    }
    while (!result.empty() && result.back() == L'\\') {
      result.pop_back();
    }
    return result;
  }());
  return *home;
}

std::wstring Join(const std::wstring& dir, const wchar_t* leaf) {
  return dir + L"\\" + leaf;
}

std::wstring Parent(const std::wstring& path) {
  const size_t slash = path.find_last_of(L'\\');
  return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

std::wstring CompanionLinkPath(const PhiSkillTarget& target) {
  return Join(Join(HomeDir(), target.extensions_subdir), kLinkName);
}

std::wstring BundledCompanionDir() {
  return PhiBundledSkillDir() + L"\\extensions\\pi";
}

bool IsDirectory(const std::wstring& path) {
  const DWORD attrs = ::GetFileAttributesW(path.c_str());
  return attrs != INVALID_FILE_ATTRIBUTES &&
         (attrs & FILE_ATTRIBUTE_DIRECTORY);
}

// The path `path` finally names, following any junction or symlink on the
// way, in the form GetFinalPathNameByHandle returns minus its \\?\ prefix
// and lower-cased (NTFS does not care about case, and neither may we).
// Empty when nothing is there — a dangling link resolves to nothing, which
// reads as "not installed", exactly as a dangling symlink does on the Mac.
std::wstring Canonical(const std::wstring& path) {
  HANDLE handle =
      ::CreateFileW(path.c_str(), 0,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS,
                    nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return std::wstring();
  }
  wchar_t buffer[4096] = {};
  const DWORD length = ::GetFinalPathNameByHandleW(
      handle, buffer, static_cast<DWORD>(std::size(buffer)),
      FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  ::CloseHandle(handle);
  if (length == 0 || length >= std::size(buffer)) {
    return std::wstring();
  }
  std::wstring result(buffer, length);
  if (result.rfind(L"\\\\?\\UNC\\", 0) == 0) {
    result = L"\\\\" + result.substr(8);
  } else if (result.rfind(L"\\\\?\\", 0) == 0) {
    result.erase(0, 4);
  }
  return PhiLower(result);
}

// True only when `link` is a reparse point (junction or symlink) resolving
// to `bundled`, so a real folder someone made by hand never reads as ours.
bool IsLinked(const std::wstring& link, const std::wstring& bundled) {
  const DWORD attrs = ::GetFileAttributesW(link.c_str());
  if (attrs == INVALID_FILE_ATTRIBUTES ||
      !(attrs & FILE_ATTRIBUTE_REPARSE_POINT)) {
    return false;
  }
  const std::wstring target = Canonical(link);
  return !target.empty() && target == Canonical(bundled);
}

std::wstring SystemMessage(DWORD error) {
  wchar_t* text = nullptr;
  const DWORD length = ::FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, error, 0, reinterpret_cast<LPWSTR>(&text), 0, nullptr);
  std::wstring result = length && text ? std::wstring(text, length)
                                       : L"error " + std::to_wstring(error);
  if (text) {
    ::LocalFree(text);
  }
  return PhiTrim(result);
}

// Whatever sits at `path` goes: a junction or directory symlink is removed
// as the link it is (its target is untouched), a file symlink or file is
// deleted, and a REAL directory — which only gets here after the user said
// "Replace" — is removed with its contents, as the Mac's removeItem does.
bool RemoveExisting(const std::wstring& path, DWORD* error) {
  const DWORD attrs = ::GetFileAttributesW(path.c_str());
  if (attrs == INVALID_FILE_ATTRIBUTES) {
    return true;
  }
  if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) {
    const bool ok = (attrs & FILE_ATTRIBUTE_DIRECTORY)
                        ? ::RemoveDirectoryW(path.c_str())
                        : ::DeleteFileW(path.c_str());
    *error = ok ? ERROR_SUCCESS : ::GetLastError();
    return ok;
  }
  if (!(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
    if (attrs & FILE_ATTRIBUTE_READONLY) {
      ::SetFileAttributesW(path.c_str(), attrs & ~FILE_ATTRIBUTE_READONLY);
    }
    const bool ok = ::DeleteFileW(path.c_str());
    *error = ok ? ERROR_SUCCESS : ::GetLastError();
    return ok;
  }
  // SHFileOperation wants a double-NUL-terminated list.
  std::wstring from = path;
  from.push_back(L'\0');
  SHFILEOPSTRUCTW op = {};
  op.wFunc = FO_DELETE;
  op.pFrom = from.c_str();
  op.fFlags = FOF_NO_UI | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
  const int result = ::SHFileOperationW(&op);
  *error = result == 0 ? ERROR_SUCCESS : ERROR_ACCESS_DENIED;
  return result == 0 && !op.fAnyOperationsAborted;
}

// mklink /J: an empty directory turned into a junction whose substitute
// name is the NT form of `target` and whose print name is the Win32 one.
// The reparse buffer is a mount-point REPARSE_DATA_BUFFER — the SDK keeps
// that struct in ntifs.h, a driver header — laid out byte by byte: the
// 8-byte reparse header (tag, data length, reserved), the two name
// descriptors (offset + length each), then both names NUL-terminated.
void AppendU16(std::vector<uint8_t>* out, size_t value) {
  out->push_back(static_cast<uint8_t>(value & 0xFF));
  out->push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
}

void AppendU32(std::vector<uint8_t>* out, uint32_t value) {
  AppendU16(out, value & 0xFFFF);
  AppendU16(out, (value >> 16) & 0xFFFF);
}

void AppendWide(std::vector<uint8_t>* out, const std::wstring& text) {
  for (const wchar_t c : text) {
    AppendU16(out, static_cast<uint16_t>(c));
  }
}

bool CreateJunction(const std::wstring& link,
                    const std::wstring& target,
                    DWORD* error) {
  if (!::CreateDirectoryW(link.c_str(), nullptr)) {
    *error = ::GetLastError();
    return false;
  }
  const std::wstring substitute = L"\\??\\" + target;
  const size_t substitute_bytes = substitute.size() * sizeof(wchar_t);
  const size_t print_bytes = target.size() * sizeof(wchar_t);
  // Descriptors (4 x USHORT) + both names with their NULs.
  const size_t data_length =
      8 + substitute_bytes + sizeof(wchar_t) + print_bytes + sizeof(wchar_t);
  std::vector<uint8_t> raw;
  raw.reserve(8 + data_length);
  AppendU32(&raw, IO_REPARSE_TAG_MOUNT_POINT);
  AppendU16(&raw, data_length);
  AppendU16(&raw, 0);  // Reserved.
  AppendU16(&raw, 0);  // SubstituteNameOffset.
  AppendU16(&raw, substitute_bytes);
  AppendU16(&raw, substitute_bytes + sizeof(wchar_t));  // PrintNameOffset.
  AppendU16(&raw, print_bytes);
  AppendWide(&raw, substitute);
  AppendU16(&raw, 0);
  AppendWide(&raw, target);
  AppendU16(&raw, 0);

  HANDLE handle = ::CreateFileW(
      link.c_str(), GENERIC_WRITE,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
      nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    *error = ::GetLastError();
    ::RemoveDirectoryW(link.c_str());
    return false;
  }
  DWORD returned = 0;
  // The buffer is exactly raw.size() bytes; the kernel reads no further.
  const bool ok = UNSAFE_BUFFERS(::DeviceIoControl(
      handle, FSCTL_SET_REPARSE_POINT, raw.data(),
      static_cast<DWORD>(raw.size()), nullptr, 0, &returned, nullptr));
  *error = ok ? ERROR_SUCCESS : ::GetLastError();
  ::CloseHandle(handle);
  if (!ok) {
    ::RemoveDirectoryW(link.c_str());
  }
  return ok;
}

}  // namespace

const std::vector<PhiSkillTarget>& PhiSkillTargets() {
  return Targets();
}

std::wstring PhiSkillLinkPath(const PhiSkillTarget& target) {
  return Join(Join(HomeDir(), target.skills_subdir), kLinkName);
}

std::wstring PhiSkillDisplayPath(const PhiSkillTarget& target) {
  return Join(L"~", target.skills_subdir);
}

std::wstring PhiBundledSkillDir() {
  wchar_t exe[MAX_PATH] = {};
  const DWORD length = ::GetModuleFileNameW(nullptr, exe, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) {
    return std::wstring();
  }
  const std::wstring dir = Parent(std::wstring(exe, length));
  return dir.empty() ? dir : dir + L"\\resources\\phi-browser-skill";
}

std::set<std::string> PhiInstalledSkillTargetIds() {
  std::set<std::string> installed;
  const std::wstring bundled = PhiBundledSkillDir();
  if (bundled.empty() || !IsDirectory(bundled)) {
    return installed;
  }
  for (const PhiSkillTarget& target : Targets()) {
    if (!IsLinked(PhiSkillLinkPath(target), bundled)) {
      continue;
    }
    if (target.extensions_subdir &&
        !IsLinked(CompanionLinkPath(target), BundledCompanionDir())) {
      continue;
    }
    installed.insert(target.id);
  }
  return installed;
}

PhiSkillInstallResult PhiInstallSkill(
    const PhiSkillTarget& target,
    const std::function<bool(const std::wstring& path)>& confirm_overwrite) {
  PhiSkillInstallResult result;
  const std::wstring bundled = PhiBundledSkillDir();
  if (bundled.empty() || !IsDirectory(bundled)) {
    result.error =
        PhiStr("settings.developer.skillInstall.missingResourcesError");
    return result;
  }
  struct Link {
    std::wstring link;
    std::wstring source;
  };
  std::vector<Link> links = {{PhiSkillLinkPath(target), bundled}};
  if (target.extensions_subdir) {
    const std::wstring companion = BundledCompanionDir();
    if (!IsDirectory(companion)) {
      result.error =
          PhiStr("settings.developer.skillInstall.missingCompanionError");
      return result;
    }
    links.push_back({CompanionLinkPath(target), companion});
  }

  // Ask about every real thing in the way before changing anything.
  for (const Link& entry : links) {
    const int created =
        ::SHCreateDirectoryExW(nullptr, Parent(entry.link).c_str(), nullptr);
    if (created != ERROR_SUCCESS && created != ERROR_ALREADY_EXISTS &&
        created != ERROR_FILE_EXISTS) {
      result.error = PhiStrF("settings.developer.skillInstall.linkError",
                             {Parent(entry.link),
                              SystemMessage(static_cast<DWORD>(created))});
      return result;
    }
    const DWORD attrs = ::GetFileAttributesW(entry.link.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES &&
        !(attrs & FILE_ATTRIBUTE_REPARSE_POINT) &&
        !confirm_overwrite(entry.link)) {
      result.outcome = PhiSkillInstallOutcome::kCancelled;
      return result;
    }
  }

  for (const Link& entry : links) {
    DWORD error = ERROR_SUCCESS;
    if (!RemoveExisting(entry.link, &error) ||
        !CreateJunction(entry.link, entry.source, &error)) {
      result.error = PhiStrF("settings.developer.skillInstall.linkError",
                             {entry.link, SystemMessage(error)});
      return result;
    }
  }
  result.outcome = PhiSkillInstallOutcome::kSuccess;
  return result;
}

}  // namespace phi_client
