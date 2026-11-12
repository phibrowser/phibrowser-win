// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_VAULT_HELPER_H_
#define PHIBROWSER_WIN_CLIENT_PHI_VAULT_HELPER_H_

#include <windows.h>

#include <map>
#include <memory>
#include <string>

#include "base/values.h"

namespace phi_client {

// Client for the out-of-process Bitwarden vault helper (PhiBitwardenHelper) —
// the Windows twin of the Mac BitwardenHelperClient. The helper links the
// GPL-3.0 Bitwarden SDK; keeping it in a SEPARATE PROCESS is what lets the
// client stay Apache-2.0, so nothing here may ever link vault code directly.
//
// SECURITY MODEL (mirrors the Mac socketpair contract):
//   * The channel is TWO ANONYMOUS PIPES (CreatePipe) handed to the child as
//     its stdin/stdout. Anonymous pipes have NO NAME, so — unlike a named pipe
//     — no other process can connect to or impersonate either end, and no peer
//     authentication is needed: the kernel pins both identities.
//   * ONLY those two handles are inherited (PROC_THREAD_ATTRIBUTE_HANDLE_LIST).
//     A blanket bInheritHandles would leak every inheritable handle the browser
//     holds into a process that talks to the network.
//   * The helper is spawned by ABSOLUTE PATH from our own directory, passed as
//     lpApplicationName — never a PATH search and never an unquoted command
//     line, so no binary-planting or argument-injection.
//   * The child joins a job object with KILL_ON_JOB_CLOSE, so it cannot outlive
//     the browser holding a decrypted vault in memory.
//   * SECRETS NEVER TOUCH THE COMMAND LINE (it is world-readable via WMI) or
//     the log. Master passwords travel only inside frames on the pipe.
//   * The helper opens the exchange with a random challenge that we must echo
//     before it serves anything; a mismatch means it refuses.
//
// Threading: Start() and Call() run on the browser UI thread and Call() BLOCKS
// on the helper's reply (bounded by a timeout — the agent-facing callers pass
// a few seconds). Vault operations are user-initiated and rare, so a blocking
// call is acceptable — but never call it from a paint path.
//
// A call whose reply could not be read whole stops the helper: a timeout
// after a frame header leaves the pipe mid-frame, and every later read would
// parse garbage. Restart() brings it back for the next call.
class PhiVaultHelper {
 public:
  PhiVaultHelper();
  ~PhiVaultHelper();

  PhiVaultHelper(const PhiVaultHelper&) = delete;
  PhiVaultHelper& operator=(const PhiVaultHelper&) = delete;

  // Spawns the helper (idempotent) and completes the challenge handshake.
  // `helper_path` is absolute; when empty, PhiBitwardenHelper.exe beside our
  // own module is used. False when the binary is missing or the handshake
  // fails — callers then fall back to the local dev vault.
  bool Start(const std::wstring& helper_path = std::wstring());

  // True once the handshake completed and the helper is still alive.
  bool running() const;

  // Sends `method` with `params` and waits for its reply. Returns the
  // `result` object on success. On failure returns nullopt and, when the
  // helper answered with an error envelope, copies its message into `error`
  // (never a secret — the helper's errors carry no vault values).
  std::optional<base::DictValue> Call(const std::string& method,
                                      const base::DictValue& params,
                                      std::string* error,
                                      DWORD timeout_ms = 30000);

  // Terminates the helper and closes the channel.
  void Stop();

  // Respawns the helper Start() last launched (the user must unlock the
  // vault again — its state died with it), at most once a minute so a
  // helper that keeps failing cannot stall every caller. False when nothing
  // was ever started or the respawn failed.
  bool Restart();

 private:
  // Reads/writes one 4-byte big-endian length-prefixed JSON frame. Frames
  // larger than kMaxFrame are refused (a hostile or wedged peer must not be
  // able to make us allocate without bound).
  bool WriteFrame(const base::Value& value);
  bool ReadFrame(base::Value* out, DWORD timeout_ms);

  HANDLE process_ = nullptr;
  HANDLE job_ = nullptr;
  // Our ends of the two pipes: we write requests to `write_`, read replies
  // from `read_`.
  HANDLE write_ = nullptr;
  HANDLE read_ = nullptr;
  bool handshaked_ = false;
  uint64_t next_request_id_ = 1;
  // The binary Start() resolved, for Restart(); empty until a Start found
  // one.
  std::wstring path_;
  ULONGLONG last_restart_tick_ = 0;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_VAULT_HELPER_H_
