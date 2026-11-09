// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifdef UNSAFE_BUFFERS_BUILD
// Raw pipe byte-buffer and WebSocket framing code; pointer arithmetic is
// inherent.
#pragma allow_unsafe_buffers
#endif

#include "client/phi_control_server.h"

#include <bcrypt.h>
#include <sddl.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include "base/base64.h"
#include "base/hash/sha1.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/memory/raw_ptr_exclusion.h"
#include "base/strings/utf_string_conversions.h"
#include "base/synchronization/lock.h"
#include "base/values.h"
#include "chrome/browser/phinomenon/phi_framework/phi_framework_api.h"

#pragma comment(lib, "bcrypt.lib")

namespace phi_client {

namespace {

constexpr int kBufSize = 32 * 1024;
constexpr char kControlPathPrefix[] = "/phi-agent";
constexpr char kDevToolsPathPrefix[] = "/devtools/";
constexpr char kTokenHeader[] = "x-phi-token";
// The agent capability (lane 5 §4.1) a peer presents to join an existing
// principal, and the name the 101 echoes it back under. The skill parses
// exactly this alphabet and length band (cdp.mjs agentCapabilityHeader).
constexpr char kCapabilityHeader[] = "x-phi-agent-capability";
constexpr char kCapabilityEchoHeader[] = "X-Phi-Agent-Capability";
// The control-plane wire version the peer asks for on its /phi-agent
// upgrade, and the name the 101 confirms it under (lane 5 §4.4). Absent — or
// anything but "2" — means protocol 1, the byte-compatible legacy envelope.
constexpr char kProtocolHeader[] = "x-phi-protocol";
constexpr char kProtocolEchoHeader[] = "X-Phi-Protocol";
constexpr int kProtocolV1 = 1;
constexpr int kProtocolV2 = 2;
// Concurrent connections beyond this get 429 without touching the delegate:
// every un-decided connection is a potential consent dialog, and a local
// process must not be able to stack fifty of them.
constexpr int kMaxConnections = 16;
// One WebSocket message (control or CDP) — a screenshot rides in one frame.
constexpr size_t kMaxWsMessage = 16u << 20;
// Bytes of DevTools output allowed to pile up for a peer that has stopped
// reading before the session is dropped instead of the browser's memory.
constexpr size_t kMaxQueuedDevToolsBytes = 64u << 20;
// The same bound for pushed control events waiting on one channel.
constexpr size_t kMaxQueuedEventBytes = 16u << 20;
// How long the connection thread waits on a write before deciding the peer
// is wedged. Only that connection's own thread ever waits this long.
constexpr DWORD kPeerWriteTimeoutMs = 30000;
// How long a peer has to deliver its whole HTTP request head. A connection
// that never finishes one would otherwise hold a kMaxConnections slot (and
// its thread) for good.
constexpr ULONGLONG kHeadReadTimeoutMs = 10000;
// Pipe: owner + SYSTEM + admins only, never remote, and an explicit medium
// integrity label with no-write-up so a sandboxed (low-IL / AppContainer)
// process cannot open it for write even if a future edit drops the DACL.
constexpr wchar_t kPipeSddl[] =
    L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;OW)S:(ML;;NW;;;ME)";
// Pointer file: owner + SYSTEM, nothing inherited. Being able to read it is
// what "may talk to Phi" means; the token inside is the proof.
constexpr wchar_t kPointerSddl[] = L"D:P(A;;GA;;;SY)(A;;GA;;;OW)";

// --- Small string helpers ---------------------------------------------------

std::string ToLowerAscii(std::string s) {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return s;
}

std::string TrimSpaces(const std::string& s) {
  size_t b = 0;
  size_t e = s.size();
  while (b < e && (s[b] == ' ' || s[b] == '\t')) {
    ++b;
  }
  while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) {
    --e;
  }
  return s.substr(b, e - b);
}

// The value of header `lower_name` (already lower-case) in an HTTP head, or
// nullopt when absent. Header names are case-insensitive (RFC 7230); Node's
// http module lower-cases them, the harness does not.
std::optional<std::string> HeaderValue(const std::string& head,
                                       const std::string& lower_name) {
  size_t pos = head.find("\r\n");  // Skip the request line.
  while (pos != std::string::npos) {
    pos += 2;
    size_t eol = head.find("\r\n", pos);
    if (eol == std::string::npos) {
      eol = head.size();
    }
    if (eol == pos) {
      break;  // Blank line: end of head.
    }
    size_t colon = head.find(':', pos);
    if (colon != std::string::npos && colon < eol) {
      std::string name = ToLowerAscii(TrimSpaces(head.substr(pos, colon - pos)));
      if (name == lower_name) {
        return TrimSpaces(head.substr(colon + 1, eol - colon - 1));
      }
    }
    pos = eol;
  }
  return std::nullopt;
}

std::string RequestMethod(const std::string& head) {
  size_t sp = head.find(' ');
  return sp == std::string::npos ? std::string() : head.substr(0, sp);
}

std::string RequestPath(const std::string& head) {
  // "METHOD /path HTTP/1.1\r\n..."
  size_t sp1 = head.find(' ');
  if (sp1 == std::string::npos) {
    return {};
  }
  size_t sp2 = head.find(' ', sp1 + 1);
  size_t eol = head.find("\r\n", sp1 + 1);
  size_t end = std::min(sp2, eol);
  if (end == std::string::npos) {
    return {};
  }
  return head.substr(sp1 + 1, end - (sp1 + 1));
}

// `/json/list/?x=1` -> `/json/list`: the framework keys its discovery
// documents by the bare path.
std::string NormalizePath(std::string path) {
  size_t q = path.find_first_of("?#");
  if (q != std::string::npos) {
    path.resize(q);
  }
  while (path.size() > 1 && path.back() == '/') {
    path.pop_back();
  }
  return path;
}

bool IsWebSocketUpgrade(const std::string& head) {
  std::optional<std::string> upgrade = HeaderValue(head, "upgrade");
  return upgrade && ToLowerAscii(*upgrade).find("websocket") !=
                        std::string::npos;
}

// Length-independent-ish equality: the loop always walks the whole expected
// token so a wrong guess costs the same as a near miss.
bool TokenMatches(const std::string& expected, const std::string& presented) {
  const std::string given = ToLowerAscii(presented);
  unsigned diff = given.size() == expected.size() ? 0u : 1u;
  for (size_t i = 0; i < expected.size(); ++i) {
    const char g = i < given.size() ? given[i] : '\0';
    diff |= static_cast<unsigned>(static_cast<unsigned char>(g) ^
                                  static_cast<unsigned char>(expected[i]));
  }
  return diff == 0;
}

// The wire form of a capability id: [A-Za-z0-9_-]{32,128}. Anything else
// is dropped before it reaches the delegate, so a malformed header can
// neither match nor be echoed.
bool IsCapabilityToken(const std::string& value) {
  if (value.size() < 32 || value.size() > 128) {
    return false;
  }
  for (char c : value) {
    const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') || c == '_' || c == '-';
    if (!ok) {
      return false;
    }
  }
  return true;
}

bool RandomHex(size_t bytes, std::string* out) {
  std::vector<uint8_t> raw(bytes);
  if (::BCryptGenRandom(nullptr, raw.data(), static_cast<ULONG>(raw.size()),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
    return false;
  }
  static const char kDigits[] = "0123456789abcdef";
  out->clear();
  out->reserve(bytes * 2);
  for (uint8_t b : raw) {
    out->push_back(kDigits[b >> 4]);
    out->push_back(kDigits[b & 0x0f]);
  }
  return true;
}

// --- HTTP responses ---------------------------------------------------------

std::string HttpResponse(const char* status,
                         const char* content_type,
                         const std::string& body) {
  return std::string("HTTP/1.1 ") + status + "\r\nContent-Type: " +
         content_type + "\r\nContent-Length: " + std::to_string(body.size()) +
         "\r\nConnection: close\r\n\r\n" + body;
}

std::string HttpText(const char* status, const std::string& body) {
  return HttpResponse(status, "text/plain; charset=UTF-8", body);
}

// --- Overlapped pipe I/O ----------------------------------------------------
// The pipe is opened FILE_FLAG_OVERLAPPED so a read can be pending while the
// same thread (or another one) writes — a synchronous handle serializes I/O,
// which deadlocks a bidirectional pump. Each call supplies a reusable
// manual-reset event.

bool OvRead(HANDLE pipe, HANDLE ev, void* buf, DWORD len, DWORD* read) {
  OVERLAPPED ov = {};
  ov.hEvent = ev;
  ::ResetEvent(ev);
  BOOL ok = ::ReadFile(pipe, buf, len, nullptr, &ov);
  if (!ok && ::GetLastError() != ERROR_IO_PENDING) {
    return false;
  }
  return ::GetOverlappedResult(pipe, &ov, read, TRUE) && *read > 0;
}

// OvRead with a deadline: false on disconnect or when nothing arrived in
// `timeout_ms`, in which case the read is cancelled AND reaped (the
// OVERLAPPED lives on this stack frame).
bool OvReadTimeout(HANDLE pipe, HANDLE ev, void* buf, DWORD len, DWORD* read,
                   DWORD timeout_ms) {
  OVERLAPPED ov = {};
  ov.hEvent = ev;
  ::ResetEvent(ev);
  BOOL ok = ::ReadFile(pipe, buf, len, nullptr, &ov);
  if (!ok && ::GetLastError() != ERROR_IO_PENDING) {
    return false;
  }
  if (::WaitForSingleObject(ev, timeout_ms) != WAIT_OBJECT_0) {
    ::CancelIoEx(pipe, &ov);
    ::GetOverlappedResult(pipe, &ov, read, TRUE);
    return false;
  }
  return ::GetOverlappedResult(pipe, &ov, read, FALSE) && *read > 0;
}

// Writes all of `data`, giving up after `timeout_ms` per chunk. A pushed
// event must never stall the thread that raised it (the UI thread) on a peer
// that has stopped reading; a connection thread uses a long timeout for the
// same reason in the other direction. On timeout the write is cancelled AND
// waited for, because the OVERLAPPED lives on this stack frame.
bool OvWriteTimeout(HANDLE pipe, HANDLE ev, const char* data, size_t len,
                    DWORD timeout_ms) {
  size_t off = 0;
  while (off < len) {
    OVERLAPPED ov = {};
    ov.hEvent = ev;
    ::ResetEvent(ev);
    DWORD wrote = 0;
    BOOL ok = ::WriteFile(pipe, data + off, static_cast<DWORD>(len - off),
                          nullptr, &ov);
    if (!ok && ::GetLastError() != ERROR_IO_PENDING) {
      return false;
    }
    if (::WaitForSingleObject(ev, timeout_ms) != WAIT_OBJECT_0) {
      ::CancelIoEx(pipe, &ov);
      ::GetOverlappedResult(pipe, &ov, &wrote, TRUE);
      return false;
    }
    if (!::GetOverlappedResult(pipe, &ov, &wrote, FALSE) || wrote == 0) {
      return false;
    }
    off += wrote;
  }
  return true;
}

bool OvWrite(HANDLE pipe, HANDLE ev, const char* data, size_t len) {
  return OvWriteTimeout(pipe, ev, data, len, kPeerWriteTimeoutMs);
}

bool OvWriteString(HANDLE pipe, HANDLE ev, const std::string& s) {
  return OvWrite(pipe, ev, s.data(), s.size());
}

// --- WebSocket framing ------------------------------------------------------

enum WsOpcode : int {
  kWsContinuation = 0,
  kWsText = 1,
  kWsBinary = 2,
  kWsClose = 8,
  kWsPing = 9,
  kWsPong = 10,
};

// Frames `payload` as a single unmasked server->client frame.
std::string WsFrame(int opcode, const std::string& payload) {
  std::string frame;
  frame.push_back(static_cast<char>(0x80 | (opcode & 0x0f)));
  const size_t len = payload.size();
  if (len < 126) {
    frame.push_back(static_cast<char>(len));
  } else if (len < 65536) {
    frame.push_back(126);
    frame.push_back(static_cast<char>((len >> 8) & 0xff));
    frame.push_back(static_cast<char>(len & 0xff));
  } else {
    frame.push_back(127);
    for (int shift = 56; shift >= 0; shift -= 8) {
      frame.push_back(static_cast<char>((len >> shift) & 0xff));
    }
  }
  return frame + payload;
}

std::string WsCloseFrame(uint16_t code) {
  std::string body;
  body.push_back(static_cast<char>(code >> 8));
  body.push_back(static_cast<char>(code & 0xff));
  return WsFrame(kWsClose, body);
}

// Incremental RFC 6455 decoder over a byte buffer. Data frames are
// reassembled across continuations; control frames (close/ping/pong) are
// delivered the moment they arrive, even in the middle of a fragmented
// message — the spec allows them there, and the old decoder glued them into
// the message body.
class WsDecoder {
 public:
  enum Result { kNeedMore, kMessage, kProtocolError };

  // Consumes complete frames from the front of `buf`. kMessage fills
  // `opcode` (text/binary/close/ping/pong) and `payload`.
  Result Next(std::string* buf, int* opcode, std::string* payload) {
    for (;;) {
      if (buf->size() < 2) {
        return kNeedMore;
      }
      const uint8_t b0 = static_cast<uint8_t>((*buf)[0]);
      const uint8_t b1 = static_cast<uint8_t>((*buf)[1]);
      const bool fin = (b0 & 0x80) != 0;
      const int op = b0 & 0x0f;
      const bool masked = (b1 & 0x80) != 0;
      uint64_t len = b1 & 0x7f;
      size_t off = 2;
      if (len == 126) {
        if (buf->size() < 4) {
          return kNeedMore;
        }
        len = (static_cast<uint64_t>(static_cast<uint8_t>((*buf)[2])) << 8) |
              static_cast<uint8_t>((*buf)[3]);
        off = 4;
      } else if (len == 127) {
        if (buf->size() < 10) {
          return kNeedMore;
        }
        len = 0;
        for (size_t i = 2; i < 10; ++i) {
          len = (len << 8) | static_cast<uint8_t>((*buf)[i]);
        }
        off = 10;
      }
      if (len > kMaxWsMessage) {
        return kProtocolError;
      }
      const size_t mask_off = off;
      if (masked) {
        off += 4;
      }
      if (buf->size() < off + len) {
        return kNeedMore;
      }
      std::string data = buf->substr(off, static_cast<size_t>(len));
      if (masked) {
        for (size_t i = 0; i < data.size(); ++i) {
          data[i] ^= (*buf)[mask_off + (i % 4)];
        }
      }
      buf->erase(0, off + static_cast<size_t>(len));

      if (op >= kWsClose) {
        // Control frame: never fragmented, at most 125 bytes.
        if (!fin || len > 125) {
          return kProtocolError;
        }
        *opcode = op;
        *payload = std::move(data);
        return kMessage;
      }
      if (op == kWsContinuation) {
        if (frag_opcode_ == 0) {
          return kProtocolError;  // Continuation of nothing.
        }
        frag_payload_ += data;
      } else if (op == kWsText || op == kWsBinary) {
        if (frag_opcode_ != 0) {
          return kProtocolError;  // New message inside a fragmented one.
        }
        frag_opcode_ = op;
        frag_payload_ = std::move(data);
      } else {
        return kProtocolError;  // Reserved opcode.
      }
      if (frag_payload_.size() > kMaxWsMessage) {
        return kProtocolError;
      }
      if (fin) {
        *opcode = frag_opcode_;
        *payload = std::move(frag_payload_);
        frag_opcode_ = 0;
        frag_payload_.clear();
        return kMessage;
      }
      // Fragment stored; loop for the next frame already in the buffer.
    }
  }

 private:
  int frag_opcode_ = 0;
  std::string frag_payload_;
};

// Validates the upgrade and builds the 101 (true) or a 400 with a body
// (false). The old server answered a bad handshake with silence, which left
// the peer hanging until its own timeout. `capability_id`, when non-empty,
// rides the 101 as X-Phi-Agent-Capability — the skill reads it off the
// upgrade response (cdp.mjs UnixWebSocket._onData) and presents it on its
// later connections.
bool BuildWsHandshake(const std::string& head,
                      const std::string& capability_id,
                      int protocol,
                      std::string* response) {
  std::optional<std::string> key = HeaderValue(head, "sec-websocket-key");
  if (!IsWebSocketUpgrade(head) || !key || key->empty()) {
    *response = HttpText(
        "400 Bad Request",
        "WebSocket upgrade required: send Upgrade: websocket and "
        "Sec-WebSocket-Key.\n");
    return false;
  }
  std::string accept = base::Base64Encode(
      base::SHA1HashString(*key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"));
  *response =
      "HTTP/1.1 101 Switching Protocols\r\n"
      "Upgrade: websocket\r\n"
      "Connection: Upgrade\r\n"
      "Sec-WebSocket-Accept: " +
      accept + "\r\n";
  if (!capability_id.empty() && IsCapabilityToken(capability_id)) {
    *response += std::string(kCapabilityEchoHeader) + ": " + capability_id +
                 "\r\n";
  }
  // Confirmed, never assumed: a peer that asked for 2 on a build that does
  // not know it sees no echo and stays on 1 (cdp.mjs reads exactly this).
  if (protocol == kProtocolV2) {
    *response += std::string(kProtocolEchoHeader) + ": 2\r\n";
  }
  *response += "\r\n";
  return true;
}

// The control wire the peer asked for on its upgrade.
int RequestedProtocol(const std::string& head) {
  std::optional<std::string> value = HeaderValue(head, kProtocolHeader);
  return value && *value == "2" ? kProtocolV2 : kProtocolV1;
}

// --- Peer identity ----------------------------------------------------------

// Who is on the other end of the pipe. The pid is the kernel's answer; the
// image path comes from the (still-running) process.
PhiAgentPeer ResolvePeer(HANDLE pipe) {
  PhiAgentPeer peer;
  if (!::GetNamedPipeClientProcessId(pipe, &peer.pid) || !peer.pid) {
    LOG(WARNING) << "[PhiAgent] GetNamedPipeClientProcessId failed: "
                 << ::GetLastError();
    return peer;
  }
  HANDLE process =
      ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, peer.pid);
  if (process) {
    wchar_t path[MAX_PATH * 2] = {};
    DWORD length = static_cast<DWORD>(std::size(path));
    if (::QueryFullProcessImageNameW(process, 0, path, &length)) {
      peer.image_path.assign(path, length);
    } else {
      LOG(WARNING) << "[PhiAgent] QueryFullProcessImageName failed for pid "
                   << peer.pid << ": " << ::GetLastError();
    }
    ::CloseHandle(process);
    return peer;
  }
  // A peer whose process cannot be opened (e.g. an ELEVATED client — its
  // default owner is Administrators, which denies even QUERY_LIMITED to the
  // non-elevated browser): fall back to the Toolhelp snapshot, which serves
  // NAMES without per-process rights. The "name:" prefix keeps this weaker
  // identity from ever matching a full-path grant (or the reverse).
  LOG(WARNING) << "[PhiAgent] OpenProcess failed for pid " << peer.pid << ": "
               << ::GetLastError() << " — name-only identity";
  HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot != INVALID_HANDLE_VALUE) {
    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    if (::Process32FirstW(snapshot, &entry)) {
      do {
        if (entry.th32ProcessID == peer.pid) {
          peer.image_path = std::wstring(L"name:") + entry.szExeFile;
          break;
        }
      } while (::Process32NextW(snapshot, &entry));
    }
    ::CloseHandle(snapshot);
  }
  return peer;
}

// /json and /json/list cut down to the targets `connection_id`'s principal
// may see (PhiControlDelegate::DevToolsTargetsInScope): the framework serves
// the process-wide list and leaves the filtering to the client
// (phi_devtools_transport.h). A document that does not parse becomes an
// empty list — fail closed.
std::string FilterTargetList(PhiControlDelegate* delegate,
                             uint64_t connection_id,
                             const std::string& doc) {
  std::optional<base::Value> parsed =
      base::JSONReader::Read(doc, base::JSON_PARSE_RFC);
  if (!delegate || !parsed || !parsed->is_list()) {
    return "[]";
  }
  base::ListValue& list = parsed->GetList();
  std::vector<PhiDevToolsTargetRef> targets;
  targets.reserve(list.size());
  for (const base::Value& entry : list) {
    PhiDevToolsTargetRef ref;
    if (const base::DictValue* dict = entry.GetIfDict()) {
      if (const std::string* id = dict->FindString("id")) {
        ref.id = *id;
      }
      if (const std::string* type = dict->FindString("type")) {
        ref.type = *type;
      }
    }
    targets.push_back(std::move(ref));
  }
  const std::vector<bool> keep =
      delegate->DevToolsTargetsInScope(connection_id, targets);
  base::ListValue out;
  for (size_t i = 0; i < list.size(); ++i) {
    if (i < keep.size() && keep[i] && !targets[i].id.empty()) {
      out.Append(std::move(list[i]));
    }
  }
  std::string json;
  base::JSONWriter::Write(out, &json);
  return json;
}

// The error a refused CDP command is answered with — Chromium's own wording
// for a method a session may not call, echoing the id (and the flattened
// child session, when the command named one) so the peer's promise settles.
std::string CdpNotAllowedReply(const base::DictValue& command) {
  base::DictValue reply;
  if (const base::Value* id = command.Find("id")) {
    reply.Set("id", id->Clone());
  }
  if (const std::string* session = command.FindString("sessionId")) {
    reply.Set("sessionId", *session);
  }
  base::DictValue error;
  error.Set("code", -32000);
  error.Set("message", "Not allowed in this session");
  reply.Set("error", std::move(error));
  std::string json;
  base::JSONWriter::Write(reply, &json);
  return json;
}

// --- DevTools session pump --------------------------------------------------

// The PhiDevToolsClient half of one CDP session. Callbacks land on the UI
// thread and must return at once, so they only append to a queue and signal
// `wake`; the connection thread drains the queue and does the pipe writes,
// where a wedged peer can only ever stall itself.
// The receiving end of DevToolsDiscoveryJson (api rule 6): the document
// lands in a client-owned std::string.
class StringSink : public phi_framework::PhiStringSink {
 public:
  explicit StringSink(std::string* out) : out_(out) {}
  void Append(const char* data, size_t length) override {
    out_->append(data, length);
  }

 private:
  RAW_PTR_EXCLUSION std::string* out_;
};

class DevToolsPump : public phi_framework::PhiDevToolsClient {
 public:
  explicit DevToolsPump(HANDLE wake) : wake_(wake) {}
  ~DevToolsPump() override = default;

  void OnDevToolsMessage(uint64_t /*session_id*/,
                         const char* json,
                         size_t length) override {
    {
      base::AutoLock lock(lock_);
      if (closed_) {
        return;
      }
      if (queued_bytes_ + length > kMaxQueuedDevToolsBytes) {
        overflow_ = true;
      } else {
        queue_.emplace_back(json, length);
        queued_bytes_ += length;
      }
    }
    ::SetEvent(wake_);
  }

  void OnDevToolsSessionClosed(uint64_t /*session_id*/) override {
    {
      base::AutoLock lock(lock_);
      closed_ = true;
    }
    ::SetEvent(wake_);
  }

  // Moves everything queued so far out. `closed` / `overflow` are sticky.
  void Take(std::vector<std::string>* out, bool* closed, bool* overflow) {
    base::AutoLock lock(lock_);
    out->swap(queue_);
    queue_.clear();
    queued_bytes_ = 0;
    *closed = closed_;
    *overflow = overflow_;
  }

 private:
  base::Lock lock_;
  std::vector<std::string> queue_;
  size_t queued_bytes_ = 0;
  bool closed_ = false;
  bool overflow_ = false;
  HANDLE wake_;
};

}  // namespace

// One accepted pipe. `inbuf` holds bytes read past what the caller has
// consumed (the tail of the chunk that completed the HTTP head is the first
// WebSocket frame when a client pipelines).
struct PhiControlServer::Connection {
  HANDLE pipe = nullptr;
  HANDLE read_event = nullptr;
  HANDLE write_event = nullptr;
  std::string inbuf;

  // Blocking read of the next chunk into `inbuf`. False on disconnect.
  bool ReadMore() {
    char buf[kBufSize];
    DWORD read = 0;
    if (!OvRead(pipe, read_event, buf, sizeof(buf), &read)) {
      return false;
    }
    inbuf.append(buf, read);
    return true;
  }

  bool Write(const std::string& bytes) {
    return OvWriteString(pipe, write_event, bytes);
  }

  // Reads the HTTP request head (through the blank line) into `head`,
  // leaving whatever followed it in `inbuf`. Gives up kHeadReadTimeoutMs
  // after it started.
  bool ReadHttpHead(std::string* head) {
    const ULONGLONG deadline = ::GetTickCount64() + kHeadReadTimeoutMs;
    for (;;) {
      size_t end = inbuf.find("\r\n\r\n");
      if (end != std::string::npos) {
        head->assign(inbuf, 0, end + 4);
        inbuf.erase(0, end + 4);
        return true;
      }
      if (inbuf.size() > (1u << 20)) {
        return false;  // Runaway head.
      }
      const ULONGLONG now = ::GetTickCount64();
      if (now >= deadline) {
        return false;  // Too slow: free the slot.
      }
      char buf[kBufSize];
      DWORD read = 0;
      if (!OvReadTimeout(pipe, read_event, buf, sizeof(buf), &read,
                         static_cast<DWORD>(deadline - now))) {
        return false;
      }
      inbuf.append(buf, read);
    }
  }
};

PhiControlServer::ControlChannel::ControlChannel()
    : wake(::CreateEventW(nullptr, FALSE, FALSE, nullptr)) {}

PhiControlServer::ControlChannel::~ControlChannel() {
  if (wake) {
    ::CloseHandle(wake);
  }
}

PhiControlServer::PhiControlServer() = default;

PhiControlServer::~PhiControlServer() {
  if (listen_thread_) {
    ::CloseHandle(listen_thread_);
  }
  // Best effort: a stale pointer file only costs the skill one refused
  // connect, but the token in it is dead and should not linger on disk.
  if (!user_data_dir_.empty()) {
    ::DeleteFileW((user_data_dir_ + L"\\CDPAgentSocket").c_str());
  }
}

void PhiControlServer::Start(const std::wstring& user_data_dir,
                             PhiControlDelegate* delegate,
                             phi_framework::PhiFramework* framework) {
  if (started_) {
    return;
  }
  started_ = true;
  user_data_dir_ = user_data_dir;
  delegate_ = delegate;
  framework_ = framework;

  // The token is the second gate (see the class comment); the random pipe
  // suffix makes the name unguessable so nobody can pre-create it. Without
  // a working RNG neither holds, so serve nothing rather than something
  // weaker.
  std::string suffix;
  if (!RandomHex(32, &token_hex_) || !RandomHex(8, &suffix)) {
    LOG(ERROR) << "[PhiControl] BCryptGenRandom failed; control socket "
                  "NOT started";
    token_hex_.clear();
    return;
  }
  pipe_name_ = L"\\\\.\\pipe\\phi-cdp." +
               std::to_wstring(::GetCurrentProcessId()) + L"." +
               base::UTF8ToWide(suffix);

  listen_thread_ = ::CreateThread(
      nullptr, 0,
      [](void* self) -> DWORD {
        static_cast<PhiControlServer*>(self)->ListenLoop();
        return 0;
      },
      this, 0, nullptr);
  if (!listen_thread_) {
    LOG(ERROR) << "[PhiControl] listener thread failed err="
               << ::GetLastError();
    return;
  }
  LOG(INFO) << "[PhiControl] serving " << base::WideToUTF8(pipe_name_);
}

bool PhiControlServer::WritePointerFile() {
  PSECURITY_DESCRIPTOR sd = nullptr;
  if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(
          kPointerSddl, SDDL_REVISION_1, &sd, nullptr)) {
    LOG(ERROR) << "[PhiControl] pointer-file SDDL failed err="
               << ::GetLastError();
    return false;
  }
  SECURITY_ATTRIBUTES sa = {sizeof(sa), sd, FALSE};
  const std::wstring path = user_data_dir_ + L"\\CDPAgentSocket";
  // CREATE_NEW, not CREATE_ALWAYS: opening an EXISTING file ignores the
  // security descriptor, so a leftover (crash, older build) would keep its
  // default DACL. Delete first; a reader holding the old file open defers
  // the delete for a moment, hence the short retry.
  HANDLE f = INVALID_HANDLE_VALUE;
  for (int attempt = 0; attempt < 20 && f == INVALID_HANDLE_VALUE; ++attempt) {
    ::DeleteFileW(path.c_str());
    f = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, &sa, CREATE_NEW,
                      FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
      ::Sleep(50);
    }
  }
  ::LocalFree(sd);
  if (f == INVALID_HANDLE_VALUE) {
    LOG(ERROR) << "[PhiControl] pointer file create failed err="
               << ::GetLastError();
    return false;
  }
  // Line 1: pipe name (the Mac discovery contract). Line 2: the launch token.
  const std::string body =
      base::WideToUTF8(pipe_name_) + "\n" + token_hex_ + "\n";
  DWORD wrote = 0;
  const BOOL ok = ::WriteFile(f, body.data(), static_cast<DWORD>(body.size()),
                              &wrote, nullptr);
  ::FlushFileBuffers(f);
  ::CloseHandle(f);
  if (!ok || wrote != body.size()) {
    LOG(ERROR) << "[PhiControl] pointer file write failed err="
               << ::GetLastError();
    ::DeleteFileW(path.c_str());
    return false;
  }
  return true;
}

void PhiControlServer::ListenLoop() {
  PSECURITY_DESCRIPTOR sd = nullptr;
  if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(
          kPipeSddl, SDDL_REVISION_1, &sd, nullptr)) {
    // The default DACL would let every local user at an unauthenticated
    // control surface; a silent fallback to it is worse than no socket.
    LOG(ERROR) << "[PhiControl] pipe SDDL failed err=" << ::GetLastError()
               << "; control socket NOT served";
    return;
  }
  SECURITY_ATTRIBUTES sa = {sizeof(sa), sd, FALSE};

  bool first_instance = true;
  DWORD backoff_ms = 250;
  for (;;) {
    // FIRST_PIPE_INSTANCE only on the first: it makes the create fail if the
    // name already exists (a squatter), and would make every LATER instance
    // of our own pipe fail for the same reason.
    const DWORD open_mode =
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED |
        (first_instance ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0u);
    HANDLE pipe = ::CreateNamedPipeW(
        pipe_name_.c_str(), open_mode,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
            PIPE_REJECT_REMOTE_CLIENTS,
        PIPE_UNLIMITED_INSTANCES, kBufSize, kBufSize, 0, &sa);
    if (pipe == INVALID_HANDLE_VALUE) {
      // Keep trying: a transient failure (or a squatter that goes away) must
      // not leave the browser without its control socket for the whole
      // session, which is what the old `return` did.
      LOG(ERROR) << "[PhiControl] CreateNamedPipe failed err="
                 << ::GetLastError() << "; retry in " << backoff_ms << " ms";
      ::Sleep(backoff_ms);
      backoff_ms = std::min<DWORD>(backoff_ms * 2, 5000);
      continue;
    }
    if (first_instance) {
      first_instance = false;
      // Only now is the name ours, so only now may the pointer file promise
      // it to the skill.
      WritePointerFile();
    }
    // Overlapped connect: wait for a client.
    OVERLAPPED ov = {};
    ov.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    bool connected = false;
    if (::ConnectNamedPipe(pipe, &ov)) {
      connected = true;
    } else {
      DWORD e = ::GetLastError();
      if (e == ERROR_PIPE_CONNECTED) {
        connected = true;
      } else if (e == ERROR_IO_PENDING) {
        ::WaitForSingleObject(ov.hEvent, INFINITE);
        DWORD ignored = 0;
        connected = ::GetOverlappedResult(pipe, &ov, &ignored, FALSE) != 0;
      }
    }
    ::CloseHandle(ov.hEvent);
    if (!connected) {
      LOG(ERROR) << "[PhiControl] ConnectNamedPipe failed err="
                 << ::GetLastError() << "; retry in " << backoff_ms << " ms";
      ::CloseHandle(pipe);
      ::Sleep(backoff_ms);
      backoff_ms = std::min<DWORD>(backoff_ms * 2, 5000);
      continue;
    }
    backoff_ms = 250;
    struct Conn {
      RAW_PTR_EXCLUSION PhiControlServer* self;  // Owns this; outlives the loop.
      HANDLE pipe;
    };
    auto* conn = new Conn{this, pipe};
    // Counted here, not in the thread, so the cap cannot be raced by a
    // burst of accepts whose threads have not started yet.
    active_connections_.fetch_add(1);
    HANDLE t = ::CreateThread(
        nullptr, 0,
        [](void* arg) -> DWORD {
          auto* c = static_cast<Conn*>(arg);
          c->self->HandleConnection(c->pipe);
          c->self->active_connections_.fetch_sub(1);
          delete c;
          return 0;
        },
        conn, 0, nullptr);
    if (t) {
      ::CloseHandle(t);
    } else {
      active_connections_.fetch_sub(1);
      ::CloseHandle(pipe);
      delete conn;
    }
  }
}

void PhiControlServer::HandleConnection(HANDLE pipe) {
  Connection conn;
  conn.pipe = pipe;
  conn.read_event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  conn.write_event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  // The delegate's handle for this connection's principal; allocated for
  // every accept so a refused connection never shares an id with a served
  // one.
  const uint64_t connection_id = next_connection_id_.fetch_add(1);
  std::string capability_id;
  bool consented = false;

  // Gates in order: cap, token, consent. Each one refuses without touching
  // the next, and the delegate (the consent dialog) is the LAST.
  std::string head;
  if (active_connections_.load() > kMaxConnections) {
    conn.Write(HttpText("429 Too Many Requests",
                        "Too many concurrent connections to Phi.\n"));
  } else if (!conn.ReadHttpHead(&head)) {
    // Disconnected or runaway head: nothing to answer.
  } else if (std::optional<std::string> token = HeaderValue(head, kTokenHeader);
             !token || !TokenMatches(token_hex_, *token)) {
    // No UI, no log spam: this is the case a process that cannot read the
    // profile directory hits, and it must learn nothing from it.
    conn.Write(HttpText("403 Forbidden", "Missing or invalid X-Phi-Token.\n"));
  } else {
    PhiAgentPeer peer = ResolvePeer(pipe);
    if (std::optional<std::string> presented =
            HeaderValue(head, kCapabilityHeader);
        presented && IsCapabilityToken(*presented)) {
      peer.presented_capability = *presented;
    }
    consented = delegate_ && delegate_->AuthorizeAgentPeer(connection_id, peer,
                                                           &capability_id);
    if (!consented) {
      conn.Write(HttpText(
          "403 Forbidden",
          "Phi refused this connection: agent control is off. Turn on "
          "\"Allow agents to control Phi\" in Settings > Developer.\n"));
    }
  }
  if (consented) {
    {
      base::AutoLock lock(live_pipes_lock_);
      live_pipes_.push_back(pipe);
    }
    const std::string raw_path = RequestPath(head);
    const std::string path = NormalizePath(raw_path);
    if (raw_path.rfind(kControlPathPrefix, 0) == 0) {
      ServeControlChannel(&conn, head, connection_id, capability_id);
    } else if (IsWebSocketUpgrade(head)) {
      // /devtools/<kind>/<id> — <id> may be empty for the browser target.
      if (path.rfind(kDevToolsPathPrefix, 0) != 0) {
        conn.Write(HttpText("404 Not Found", "No such WebSocket path.\n"));
      } else {
        const size_t kind_start = sizeof(kDevToolsPathPrefix) - 1;
        size_t id_start = path.find('/', kind_start);
        const std::string kind = path.substr(
            kind_start, id_start == std::string::npos ? std::string::npos
                                                      : id_start - kind_start);
        // /devtools/browser/<id> is always opened as THE browser target
        // (""): passing its id through would let a page id ride the
        // browser path past ServeDevToolsSession's scope check.
        const std::string target_id =
            id_start == std::string::npos || kind == "browser"
                ? std::string()
                : path.substr(id_start + 1);
        ServeDevToolsSession(&conn, head, target_id, connection_id,
                             capability_id);
      }
    } else if (RequestMethod(head) == "GET" &&
               (path == "/json" || path == "/json/version" ||
                path == "/json/list")) {
      std::string doc;
      StringSink sink(&doc);
      if (framework_ &&
          framework_->DevToolsDiscoveryJson(path.c_str(), &sink)) {
        if (path != "/json/version") {
          doc = FilterTargetList(delegate_, connection_id, doc);
        }
        conn.Write(HttpResponse("200 OK", "application/json; charset=UTF-8",
                                doc));
      } else {
        conn.Write(HttpText("503 Service Unavailable",
                            "DevTools discovery unavailable.\n"));
      }
    } else {
      conn.Write(HttpText("404 Not Found", "No such path.\n"));
    }
    // After the serve loops, so every session the connection opened has
    // already been reported closed.
    delegate_->OnConnectionClosed(connection_id);
    {
      base::AutoLock lock(live_pipes_lock_);
      live_pipes_.erase(
          std::remove(live_pipes_.begin(), live_pipes_.end(), pipe),
          live_pipes_.end());
    }
  }
  ::FlushFileBuffers(pipe);
  ::DisconnectNamedPipe(pipe);
  ::CloseHandle(pipe);
  ::CloseHandle(conn.read_event);
  ::CloseHandle(conn.write_event);
}

void PhiControlServer::ServeControlChannel(Connection* conn,
                                           const std::string& head,
                                           uint64_t connection_id,
                                           const std::string& capability_id) {
  // Which wire this connection speaks, decided ONCE on the upgrade and
  // fixed for its life: v2 when the peer sent `X-Phi-Protocol: 2` (the 101
  // confirms it), v1 otherwise.
  const int protocol = RequestedProtocol(head);
  std::string handshake;
  const bool ok = BuildWsHandshake(head, capability_id, protocol, &handshake);
  if (!conn->Write(handshake) || !ok) {
    return;
  }

  // Live for pushed events (ownership changes and friends) until this loop
  // ends. A pusher only queues on the channel and signals `wake`; this
  // thread is the pipe's one writer, so frames never interleave and a
  // wedged peer stalls nobody else.
  auto channel = std::make_shared<ControlChannel>();
  if (!channel->wake) {
    return;
  }
  channel->protocol = protocol;
  channel->connection_id = connection_id;
  RegisterChannel(channel);
  if (delegate_) {
    delegate_->OnControlProtocol(connection_id, protocol);
  }

  auto send_frame = [&](const std::string& frame) {
    return conn->Write(frame);
  };
  // Writes every queued event; false when the peer is gone or let the
  // outbox overflow.
  auto flush_events = [&]() {
    std::deque<std::string> frames;
    bool overflow = false;
    {
      base::AutoLock lock(channel->outbox_lock);
      frames.swap(channel->outbox);
      channel->outbox_bytes = 0;
      overflow = channel->overflow;
    }
    if (overflow) {
      return false;
    }
    for (const std::string& frame : frames) {
      if (!send_frame(frame)) {
        return false;
      }
    }
    return true;
  };
  // One thread, two sources (the DevTools pump's shape): a pending
  // overlapped read on the pipe and the channel's wake. Returns once more
  // bytes are in `inbuf`; false when the peer is gone.
  std::vector<char> read_buf(kBufSize);
  OVERLAPPED read_ov = {};
  read_ov.hEvent = conn->read_event;
  bool read_pending = false;
  auto read_more = [&]() {
    for (;;) {
      if (!read_pending) {
        ::ResetEvent(conn->read_event);
        if (!::ReadFile(conn->pipe, read_buf.data(), kBufSize, nullptr,
                        &read_ov) &&
            ::GetLastError() != ERROR_IO_PENDING) {
          return false;
        }
        read_pending = true;
      }
      HANDLE waits[2] = {conn->read_event, channel->wake};
      const DWORD w = ::WaitForMultipleObjects(2, waits, FALSE, INFINITE);
      if (w == WAIT_OBJECT_0) {
        DWORD read = 0;
        read_pending = false;
        if (!::GetOverlappedResult(conn->pipe, &read_ov, &read, FALSE) ||
            read == 0) {
          return false;
        }
        conn->inbuf.append(read_buf.data(), read);
        return true;
      }
      if (w != WAIT_OBJECT_0 + 1 || !flush_events()) {
        return false;
      }
    }
  };

  // Message loop. The delegate answers a PhiVerbResult either way; the two
  // wires differ only in how this loop spells it:
  //   v1  {id, type, payloadJson}  -> {id, responseJson:"{ok,…}"}
  //   v2  {id, type, payload}      -> {id, ok:true, result:{…}}
  //                                   {id, ok:false, error:{code,message?,
  //                                                         details?}}
  WsDecoder decoder;
  bool close_sent = false;
  for (;;) {
    int opcode = 0;
    std::string msg;
    const WsDecoder::Result r = decoder.Next(&conn->inbuf, &opcode, &msg);
    if (r == WsDecoder::kProtocolError) {
      close_sent = send_frame(WsCloseFrame(1002));
      break;
    }
    if (r == WsDecoder::kNeedMore) {
      if (!read_more()) {
        break;
      }
      continue;
    }
    if (opcode == kWsClose) {
      // Echo the close so the peer's own close handshake completes.
      close_sent = send_frame(WsFrame(kWsClose, msg.substr(0, 2)));
      break;
    }
    if (opcode == kWsPing) {
      if (!send_frame(WsFrame(kWsPong, msg))) {
        break;
      }
      continue;
    }
    if (opcode != kWsText) {
      continue;  // Pong / binary: nothing to do.
    }
    std::optional<base::DictValue> parsed =
        base::JSONReader::ReadDict(msg, base::JSON_PARSE_RFC);
    base::DictValue resp_dict;
    if (parsed) {
      const base::DictValue& req = *parsed;
      std::optional<int> id = req.FindInt("id");
      const std::string* type = req.FindString("type");
      if (id) {
        resp_dict.Set("id", *id);
      }
      if (type && delegate_) {
        // The payload reaches the delegate parsed, whichever wire brought
        // it: v1 carries it stringified under `payloadJson`, v2 carries the
        // object under `payload`. An unparseable v1 string is an empty
        // payload — exactly what the old reader did with it.
        base::DictValue payload;
        if (protocol == kProtocolV2) {
          if (const base::DictValue* given = req.FindDict("payload")) {
            payload = given->Clone();
          }
        } else if (const std::string* json = req.FindString("payloadJson")) {
          std::optional<base::DictValue> given =
              base::JSONReader::ReadDict(*json, base::JSON_PARSE_RFC);
          if (given) {
            payload = std::move(*given);
          }
        }
        PhiVerbResult result = delegate_->HandleControlCommand(
            connection_id, *type, std::move(payload));
        if (protocol == kProtocolV2) {
          resp_dict.Set("ok", result.ok);
          if (result.ok) {
            resp_dict.Set("result", std::move(result.fields));
          } else {
            resp_dict.Set("error", result.ToV2Error());
          }
        } else {
          resp_dict.Set("responseJson", result.ToLegacyJson());
        }
      } else if (protocol == kProtocolV2) {
        base::DictValue error;
        error.Set("code", PhiErrorCodeName(PhiErrorCode::kInvalidPayload));
        error.Set("message", "missing type");
        resp_dict.Set("ok", false);
        resp_dict.Set("error", std::move(error));
      } else {
        resp_dict.Set("error", "missing type");
      }
    } else if (protocol == kProtocolV2) {
      base::DictValue error;
      error.Set("code", PhiErrorCodeName(PhiErrorCode::kInvalidPayload));
      error.Set("message", "bad json");
      resp_dict.Set("ok", false);
      resp_dict.Set("error", std::move(error));
    } else {
      resp_dict.Set("error", "bad json");
    }
    std::string out;
    base::JSONWriter::Write(resp_dict, &out);
    if (!send_frame(WsFrame(kWsText, out))) {
      break;
    }
  }
  // Unregistered first, so no new event is queued for a loop that is gone;
  // the pending read is cancelled AND reaped before read_ov leaves scope.
  UnregisterChannel(channel);
  if (read_pending) {
    ::CancelIoEx(conn->pipe, &read_ov);
    DWORD ignored = 0;
    ::GetOverlappedResult(conn->pipe, &read_ov, &ignored, TRUE);
  }
  if (!close_sent) {
    send_frame(WsCloseFrame(1001));  // Going away; best effort.
  }
}

void PhiControlServer::ServeDevToolsSession(Connection* conn,
                                            const std::string& head,
                                            const std::string& target_id,
                                            uint64_t connection_id,
                                            const std::string& capability_id) {
  std::string handshake;
  // A CDP session carries no control envelope, so it never negotiates the
  // control wire.
  if (!BuildWsHandshake(head, capability_id, kProtocolV1, &handshake)) {
    conn->Write(handshake);
    return;
  }
  if (!framework_) {
    conn->Write(HttpText("503 Service Unavailable", "No DevTools.\n"));
    return;
  }
  // The framework opens whatever target it is handed; choosing only
  // in-scope ones is the client's job (phi_devtools_transport.h). The
  // browser target ("") is always allowed — the session scope filters it.
  if (!target_id.empty()) {
    const std::vector<bool> allowed = delegate_->DevToolsTargetsInScope(
        connection_id, {PhiDevToolsTargetRef{target_id, std::string()}});
    if (allowed.empty() || !allowed.front()) {
      conn->Write(HttpText("404 Not Found", "No such target.\n"));
      return;
    }
  }
  // Auto-reset: one wake drains the whole queue, and a message queued
  // between drain and wait leaves it signalled for the next wait.
  HANDLE wake = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
  DevToolsPump pump(wake);
  const uint64_t session = framework_->OpenDevToolsSession(
      target_id.c_str(), &pump);
  if (session == 0) {
    ::CloseHandle(wake);
    conn->Write(HttpText("503 Service Unavailable",
                         "DevTools is shutting down.\n"));
    return;
  }
  // Scoped BEFORE the 101: a session starts seeing nothing (the framework's
  // default), and the peer's first message must already find the
  // principal's windows in place rather than an empty target list.
  delegate_->OnDevToolsSessionOpened(connection_id, session);
  if (!conn->Write(handshake)) {
    framework_->CloseDevToolsSession(session);
    delegate_->OnDevToolsSessionClosed(connection_id, session);
    ::CloseHandle(wake);
    return;
  }

  // One thread, two sources: a pending overlapped read on the pipe and the
  // pump's wake event. Whichever fires is serviced, then we wait again.
  WsDecoder decoder;
  std::vector<char> buf(kBufSize);
  OVERLAPPED read_ov = {};
  read_ov.hEvent = conn->read_event;
  bool read_pending = false;
  bool close_sent = false;
  uint16_t close_code = 1001;
  for (;;) {
    if (!read_pending) {
      ::ResetEvent(conn->read_event);
      BOOL ok = ::ReadFile(conn->pipe, buf.data(), kBufSize, nullptr, &read_ov);
      if (!ok && ::GetLastError() != ERROR_IO_PENDING) {
        break;  // Peer gone.
      }
      read_pending = true;
    }
    HANDLE waits[2] = {conn->read_event, wake};
    const DWORD w = ::WaitForMultipleObjects(2, waits, FALSE, INFINITE);
    if (w == WAIT_OBJECT_0) {
      DWORD read = 0;
      read_pending = false;
      if (!::GetOverlappedResult(conn->pipe, &read_ov, &read, FALSE) ||
          read == 0) {
        break;  // Peer gone.
      }
      conn->inbuf.append(buf.data(), read);
      bool done = false;
      for (;;) {
        int opcode = 0;
        std::string msg;
        const WsDecoder::Result r = decoder.Next(&conn->inbuf, &opcode, &msg);
        if (r == WsDecoder::kNeedMore) {
          break;
        }
        if (r == WsDecoder::kProtocolError) {
          close_code = 1002;
          done = true;
          break;
        }
        if (opcode == kWsClose) {
          close_sent = conn->Write(WsFrame(kWsClose, msg.substr(0, 2)));
          done = true;
          break;
        }
        if (opcode == kWsPing) {
          if (!conn->Write(WsFrame(kWsPong, msg))) {
            done = true;
            break;
          }
          continue;
        }
        if (opcode == kWsText) {
          std::string refusal;
          if (RefusedCdpCommand(msg, connection_id, &refusal)) {
            if (!conn->Write(WsFrame(kWsText, refusal))) {
              done = true;
              break;
            }
            continue;
          }
          framework_->SendDevToolsMessage(session, msg.data(), msg.size());
        }
      }
      if (done) {
        break;
      }
    } else if (w == WAIT_OBJECT_0 + 1) {
      std::vector<std::string> out;
      bool closed = false;
      bool overflow = false;
      pump.Take(&out, &closed, &overflow);
      bool write_ok = true;
      for (const std::string& json : out) {
        if (!conn->Write(WsFrame(kWsText, json))) {
          write_ok = false;
          break;
        }
      }
      if (!write_ok || overflow) {
        break;  // Peer wedged or not reading: drop it.
      }
      if (closed) {
        close_code = 1001;  // Target went away.
        break;
      }
    } else {
      break;
    }
  }

  // Order matters: Close blocks until in-flight callbacks return, so the
  // pump (on this stack) is safe to destroy afterwards; and the pending
  // read must be cancelled AND reaped before read_ov goes out of scope.
  framework_->CloseDevToolsSession(session);
  delegate_->OnDevToolsSessionClosed(connection_id, session);
  if (read_pending) {
    ::CancelIoEx(conn->pipe, &read_ov);
    DWORD ignored = 0;
    ::GetOverlappedResult(conn->pipe, &read_ov, &ignored, TRUE);
  }
  if (!close_sent) {
    conn->Write(WsCloseFrame(close_code));
  }
  ::CloseHandle(wake);
}

bool PhiControlServer::RefusedCdpCommand(const std::string& message,
                                         uint64_t connection_id,
                                         std::string* reply) {
  // Target.createTarget opens a FOREGROUND tab in the user's last window —
  // outside an agent's own windows, and nothing the session scope can take
  // back. Cheap substring test first: only a candidate is parsed.
  if (message.find("Target.createTarget") == std::string::npos) {
    return false;
  }
  std::optional<base::DictValue> command =
      base::JSONReader::ReadDict(message, base::JSON_PARSE_RFC);
  const std::string* method = command ? command->FindString("method") : nullptr;
  if (!method || *method != "Target.createTarget" ||
      delegate_->DevToolsMayCreateTargets(connection_id)) {
    return false;
  }
  *reply = CdpNotAllowedReply(*command);
  return true;
}

void PhiControlServer::SeverAgentConnections() {
  size_t severed = 0;
  {
    // UNDER the lock: a serving thread takes its pipe out of the list (under
    // the same lock) before it closes the handle, so every handle touched
    // here is still open — never a closed one, or a reused value.
    base::AutoLock lock(live_pipes_lock_);
    for (HANDLE pipe : live_pipes_) {
      // The serving thread's pending read completes with an abort and its
      // loop breaks; the disconnect makes any write it tries fail too.
      // Neither call blocks.
      ::CancelIoEx(pipe, nullptr);
      ::DisconnectNamedPipe(pipe);
    }
    severed = live_pipes_.size();
  }
  if (severed) {
    LOG(INFO) << "[PhiAgent] severed " << severed << " agent connection(s)";
  }
}

void PhiControlServer::RegisterChannel(
    const std::shared_ptr<ControlChannel>& channel) {
  base::AutoLock lock(channels_lock_);
  channels_.push_back(channel);
}

void PhiControlServer::UnregisterChannel(
    const std::shared_ptr<ControlChannel>& channel) {
  base::AutoLock lock(channels_lock_);
  channels_.erase(std::remove(channels_.begin(), channels_.end(), channel),
                  channels_.end());
}

void PhiControlServer::BroadcastEvent(const std::string& type,
                                      const std::string& payload_json) {
  // An event about a task is that task's owner's business alone (a user's
  // console line, an ownership flip); the delegate says who that is.
  std::optional<std::set<uint64_t>> audience;
  if (delegate_) {
    std::optional<base::DictValue> parsed =
        base::JSONReader::ReadDict(payload_json, base::JSON_PARSE_RFC);
    if (!parsed) {
      parsed.emplace();
    }
    audience = delegate_->EventAudience(type, *parsed);
  }
  if (audience && audience->empty()) {
    return;
  }
  PushEvent(type, payload_json, audience ? &*audience : nullptr);
}

void PhiControlServer::SendEventToConnections(
    const std::string& type,
    const std::string& payload_json,
    const std::set<uint64_t>& connection_ids) {
  if (!connection_ids.empty()) {
    PushEvent(type, payload_json, &connection_ids);
  }
}

void PhiControlServer::PushEvent(const std::string& type,
                                 const std::string& payload_json,
                                 const std::set<uint64_t>* connection_ids) {
  // One event, two spellings — {event, payloadJson} on v1, {event, payload}
  // on v2. Both are built once here, not per channel.
  base::DictValue v1_frame;
  v1_frame.Set("event", type);
  v1_frame.Set("payloadJson", payload_json);
  std::string v1_text;
  base::JSONWriter::Write(v1_frame, &v1_text);
  const std::string v1_bytes = WsFrame(kWsText, v1_text);

  base::DictValue v2_frame;
  v2_frame.Set("event", type);
  std::optional<base::DictValue> parsed =
      base::JSONReader::ReadDict(payload_json, base::JSON_PARSE_RFC);
  v2_frame.Set("payload",
               parsed ? std::move(*parsed) : base::DictValue());
  std::string v2_text;
  base::JSONWriter::Write(v2_frame, &v2_text);
  const std::string v2_bytes = WsFrame(kWsText, v2_text);

  // Snapshot under the registry lock, then queue outside it. Nothing here
  // touches a pipe: each channel's own serve loop writes its frames.
  std::vector<std::shared_ptr<ControlChannel>> targets;
  {
    base::AutoLock lock(channels_lock_);
    for (const std::shared_ptr<ControlChannel>& channel : channels_) {
      if (!connection_ids || connection_ids->count(channel->connection_id)) {
        targets.push_back(channel);
      }
    }
  }
  for (const std::shared_ptr<ControlChannel>& channel : targets) {
    const std::string& bytes =
        channel->protocol == kProtocolV2 ? v2_bytes : v1_bytes;
    {
      base::AutoLock lock(channel->outbox_lock);
      if (channel->outbox_bytes + bytes.size() > kMaxQueuedEventBytes) {
        channel->overflow = true;  // The loop drops the peer.
      } else {
        channel->outbox.push_back(bytes);
        channel->outbox_bytes += bytes.size();
      }
    }
    ::SetEvent(channel->wake);
  }
}

}  // namespace phi_client
