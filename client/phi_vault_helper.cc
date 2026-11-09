// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "client/phi_vault_helper.h"

#include <algorithm>
#include <vector>

#include "base/compiler_specific.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"

namespace phi_client {

namespace {

// Matches the helper's MAX_FRAME. A peer that announces more is refused
// outright rather than allowed to drive an unbounded allocation.
constexpr uint32_t kMaxFrame = 4 * 1024 * 1024;

constexpr wchar_t kHelperName[] = L"PhiBitwardenHelper.exe";

// PhiBitwardenHelper.exe beside our own module. Resolving from the module path
// (never PATH, never the working directory) is what makes binary planting a
// non-issue: an attacker would already need write access to our install dir.
std::wstring DefaultHelperPath() {
  std::vector<wchar_t> buffer(MAX_PATH);
  for (;;) {
    const DWORD written = ::GetModuleFileNameW(nullptr, buffer.data(),
                                               static_cast<DWORD>(buffer.size()));
    if (written == 0) {
      return std::wstring();
    }
    if (written < buffer.size() - 1) {
      break;
    }
    buffer.resize(buffer.size() * 2);  // Truncated — grow and retry.
  }
  std::wstring path(buffer.data());
  const size_t slash = path.find_last_of(L'\\');
  if (slash == std::wstring::npos) {
    return std::wstring();
  }
  path.resize(slash + 1);
  return path + kHelperName;
}

// Blocking read of exactly `len` bytes, honouring a deadline. The pipe is
// synchronous, so the deadline is enforced by peeking before each blocking
// read — enough to stop a wedged helper from hanging the UI thread forever.
bool ReadExact(HANDLE pipe, void* out, DWORD len, DWORD timeout_ms) {
  auto* cursor = static_cast<uint8_t*>(out);
  DWORD remaining = len;
  const ULONGLONG deadline = ::GetTickCount64() + timeout_ms;
  while (remaining > 0) {
    DWORD available = 0;
    if (!::PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) {
      return false;  // Peer gone.
    }
    if (available == 0) {
      if (::GetTickCount64() > deadline) {
        return false;
      }
      ::Sleep(5);
      continue;
    }
    DWORD read = 0;
    const DWORD want = std::min(available, remaining);
    if (!::ReadFile(pipe, cursor, want, &read, nullptr) || read == 0) {
      return false;
    }
    UNSAFE_BUFFERS(cursor += read);
    remaining -= read;
  }
  return true;
}

bool WriteAll(HANDLE pipe, const void* data, size_t len) {
  const auto* cursor = static_cast<const uint8_t*>(data);
  size_t remaining = len;
  while (remaining > 0) {
    DWORD wrote = 0;
    if (!::WriteFile(pipe, cursor, static_cast<DWORD>(remaining), &wrote,
                     nullptr) ||
        wrote == 0) {
      return false;
    }
    UNSAFE_BUFFERS(cursor += wrote);
    remaining -= wrote;
  }
  return true;
}

}  // namespace

PhiVaultHelper::PhiVaultHelper() = default;

PhiVaultHelper::~PhiVaultHelper() {
  Stop();
}

bool PhiVaultHelper::running() const {
  if (!handshaked_ || !process_) {
    return false;
  }
  return ::WaitForSingleObject(process_, 0) == WAIT_TIMEOUT;
}

bool PhiVaultHelper::Start(const std::wstring& helper_path) {
  if (running()) {
    return true;
  }
  Stop();  // Clear any half-built state from a previous attempt.

  const std::wstring path =
      helper_path.empty() ? DefaultHelperPath() : helper_path;
  if (path.empty() || ::GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
    // No helper installed — the caller falls back to the dev vault.
    return false;
  }
  path_ = path;

  // Two ANONYMOUS pipes: no name means no third party can connect. Only the
  // ends the child needs are marked inheritable; our own ends never are.
  SECURITY_ATTRIBUTES inheritable = {sizeof(SECURITY_ATTRIBUTES), nullptr,
                                     TRUE};
  HANDLE child_stdin_read = nullptr;   // child reads our requests here
  HANDLE our_write = nullptr;
  HANDLE our_read = nullptr;
  HANDLE child_stdout_write = nullptr;  // child writes replies here
  if (!::CreatePipe(&child_stdin_read, &our_write, &inheritable, 0)) {
    return false;
  }
  if (!::CreatePipe(&our_read, &child_stdout_write, &inheritable, 0)) {
    ::CloseHandle(child_stdin_read);
    ::CloseHandle(our_write);
    return false;
  }
  // OUR ends must not be inheritable: the child has no business holding them.
  ::SetHandleInformation(our_write, HANDLE_FLAG_INHERIT, 0);
  ::SetHandleInformation(our_read, HANDLE_FLAG_INHERIT, 0);

  // Inherit EXACTLY the two pipe ends — not every inheritable handle in the
  // browser process.
  SIZE_T attribute_size = 0;
  ::InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_size);
  std::vector<uint8_t> attribute_storage(attribute_size);
  auto* attributes =
      reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attribute_storage.data());
  HANDLE inherit[2] = {child_stdin_read, child_stdout_write};
  bool spawned = false;
  PROCESS_INFORMATION process_info = {};
  if (::InitializeProcThreadAttributeList(attributes, 1, 0, &attribute_size) &&
      ::UpdateProcThreadAttribute(attributes, 0,
                                  PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit,
                                  sizeof(inherit), nullptr, nullptr)) {
    STARTUPINFOEXW startup = {};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = child_stdin_read;
    startup.StartupInfo.hStdOutput = child_stdout_write;
    // stderr stays ours (diagnostics only — the helper never logs secrets).
    startup.StartupInfo.hStdError = ::GetStdHandle(STD_ERROR_HANDLE);
    startup.lpAttributeList = attributes;
    // lpApplicationName carries the absolute path, so the command line is
    // never parsed for a program to run. No arguments: nothing to inject, and
    // no secret could ever appear in the (world-readable) command line.
    spawned = ::CreateProcessW(path.c_str(), nullptr, nullptr, nullptr,
                               /*bInheritHandles=*/TRUE,
                               EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW,
                               nullptr, nullptr, &startup.StartupInfo,
                               &process_info) != 0;
    ::DeleteProcThreadAttributeList(attributes);
  }
  // The child owns its ends now; ours would otherwise keep the pipes from
  // ever reaching EOF.
  ::CloseHandle(child_stdin_read);
  ::CloseHandle(child_stdout_write);
  if (!spawned) {
    ::CloseHandle(our_write);
    ::CloseHandle(our_read);
    LOG(ERROR) << "[PhiVault] helper spawn failed err=" << ::GetLastError();
    return false;
  }
  ::CloseHandle(process_info.hThread);
  process_ = process_info.hProcess;
  write_ = our_write;
  read_ = our_read;

  // A job object with KILL_ON_JOB_CLOSE guarantees the helper — which may hold
  // a DECRYPTED VAULT in memory — cannot outlive us, including on a crash.
  job_ = ::CreateJobObjectW(nullptr, nullptr);
  if (job_) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
    limits.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    ::SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limits,
                              sizeof(limits));
    ::AssignProcessToJobObject(job_, process_);
  }

  // Handshake: the helper opens with a random challenge we must echo before it
  // serves anything.
  base::Value challenge_frame;
  if (!ReadFrame(&challenge_frame, 10000) || !challenge_frame.is_dict()) {
    LOG(ERROR) << "[PhiVault] no challenge from helper";
    Stop();
    return false;
  }
  const std::string* challenge =
      challenge_frame.GetDict().FindString("challenge");
  if (!challenge || challenge->empty()) {
    LOG(ERROR) << "[PhiVault] malformed challenge";
    Stop();
    return false;
  }
  base::DictValue ack;
  ack.Set("challenge_response", *challenge);
  if (!WriteFrame(base::Value(std::move(ack)))) {
    Stop();
    return false;
  }
  handshaked_ = true;
  LOG(INFO) << "[PhiVault] helper ready";
  return true;
}

void PhiVaultHelper::Stop() {
  handshaked_ = false;
  // Closing our write end signals EOF; the helper exits on its own. The job
  // handle is the backstop for a wedged one.
  if (write_) {
    ::CloseHandle(write_);
    write_ = nullptr;
  }
  if (read_) {
    ::CloseHandle(read_);
    read_ = nullptr;
  }
  if (process_) {
    if (::WaitForSingleObject(process_, 2000) == WAIT_TIMEOUT) {
      ::TerminateProcess(process_, 0);
    }
    ::CloseHandle(process_);
    process_ = nullptr;
  }
  if (job_) {
    ::CloseHandle(job_);  // KILL_ON_JOB_CLOSE reaps anything left.
    job_ = nullptr;
  }
}

bool PhiVaultHelper::Restart() {
  if (running()) {
    return true;
  }
  constexpr ULONGLONG kRestartIntervalMs = 60 * 1000;
  const ULONGLONG now = ::GetTickCount64();
  if (path_.empty() ||
      (last_restart_tick_ && now - last_restart_tick_ < kRestartIntervalMs)) {
    return false;
  }
  last_restart_tick_ = now;
  LOG(INFO) << "[PhiVault] restarting the helper";
  return Start(path_);
}

bool PhiVaultHelper::WriteFrame(const base::Value& value) {
  if (!write_) {
    return false;
  }
  std::string json;
  if (!base::JSONWriter::Write(value, &json) || json.size() > kMaxFrame) {
    return false;
  }
  const uint32_t length = static_cast<uint32_t>(json.size());
  const uint8_t header[4] = {
      static_cast<uint8_t>((length >> 24) & 0xff),
      static_cast<uint8_t>((length >> 16) & 0xff),
      static_cast<uint8_t>((length >> 8) & 0xff),
      static_cast<uint8_t>(length & 0xff)};
  if (!WriteAll(write_, header, sizeof(header))) {
    return false;
  }
  const bool ok = WriteAll(write_, json.data(), json.size());
  // The frame may have carried a master password; do not leave a copy in this
  // process's heap for a later memory dump to find.
  ::SecureZeroMemory(json.data(), json.size());
  return ok;
}

bool PhiVaultHelper::ReadFrame(base::Value* out, DWORD timeout_ms) {
  if (!read_) {
    return false;
  }
  uint8_t header[4] = {};
  if (!ReadExact(read_, header, sizeof(header), timeout_ms)) {
    return false;
  }
  const uint32_t length = (static_cast<uint32_t>(header[0]) << 24) |
                          (static_cast<uint32_t>(header[1]) << 16) |
                          (static_cast<uint32_t>(header[2]) << 8) |
                          static_cast<uint32_t>(header[3]);
  if (length == 0 || length > kMaxFrame) {
    return false;  // Refuse an unbounded allocation.
  }
  std::string body;
  body.resize(length);
  if (!ReadExact(read_, body.data(), length, timeout_ms)) {
    ::SecureZeroMemory(body.data(), body.size());
    return false;
  }
  std::optional<base::Value> parsed =
      base::JSONReader::Read(body, base::JSON_PARSE_RFC);
  // The body may hold a vault secret — scrub the raw buffer once parsed.
  ::SecureZeroMemory(body.data(), body.size());
  if (!parsed) {
    return false;
  }
  *out = std::move(*parsed);
  return true;
}

std::optional<base::DictValue> PhiVaultHelper::Call(
    const std::string& method,
    const base::DictValue& params,
    std::string* error,
    DWORD timeout_ms) {
  if (!running()) {
    if (error) {
      *error = "helper_unavailable";
    }
    return std::nullopt;
  }
  const std::string request_id = base::NumberToString(next_request_id_++);
  base::DictValue request;
  request.Set("method", method);
  request.Set("params", params.Clone());
  request.Set("request_id", request_id);
  if (!WriteFrame(base::Value(std::move(request)))) {
    // A half-written request desyncs the helper's reader the same way.
    Stop();
    if (error) {
      *error = "helper_write_failed";
    }
    return std::nullopt;
  }
  // Replies may arrive out of order (the helper answers each request on its
  // own thread) and `persist` events interleave with them, so read until the
  // frame echoing OUR request_id shows up.
  const ULONGLONG deadline = ::GetTickCount64() + timeout_ms;
  for (;;) {
    const ULONGLONG now = ::GetTickCount64();
    if (now >= deadline) {
      if (error) {
        *error = "helper_timeout";
      }
      return std::nullopt;
    }
    base::Value frame;
    if (!ReadFrame(&frame, static_cast<DWORD>(deadline - now)) ||
        !frame.is_dict()) {
      // The read may have stopped inside a frame (a timeout after the
      // header): the stream cannot be trusted again. Restart() respawns.
      LOG(WARNING) << "[PhiVault] helper reply unreadable; stopping it";
      Stop();
      if (error) {
        *error = "helper_read_failed";
      }
      return std::nullopt;
    }
    base::DictValue& dict = frame.GetDict();
    const std::string* id = dict.FindString("request_id");
    if (!id || *id != request_id) {
      // A `persist` event or another call's reply — the session store is not
      // wired on Windows yet, so drop it and keep waiting for ours.
      continue;
    }
    if (!dict.FindBool("ok").value_or(false)) {
      if (error) {
        const base::DictValue* err = dict.FindDict("error");
        const std::string* message =
            err ? err->FindString("message") : nullptr;
        *error = message ? *message : "helper_error";
      }
      return std::nullopt;
    }
    base::DictValue* result = dict.FindDict("result");
    return result ? std::make_optional(result->Clone())
                  : std::make_optional(base::DictValue());
  }
}

}  // namespace phi_client
