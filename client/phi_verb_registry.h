// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_VERB_REGISTRY_H_
#define PHIBROWSER_WIN_CLIENT_PHI_VERB_REGISTRY_H_

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/containers/span.h"
#include "base/memory/raw_ptr_exclusion.h"
#include "base/values.h"

// The typed verb registry of the control plane (docs/architecture-review.md
// §3.7 item 4, lane 5 §4.3/§4.4): the leaf types every party shares — the
// capability bitset, the closed error-code enum, the result a handler
// returns, the parameter schema a verb declares and the typed view a
// handler reads its payload through, and the spec row that ties them to a
// handler. The table itself is PhiControlDispatcher::Registry()
// (phi_verb_registry.cc); the pipe server formats results onto the wire
// (protocol v1 or v2) without knowing any verb.

namespace phi_client {

class PhiControlDispatcher;
struct PhiVerbContext;

// What a control-plane principal may do (docs/architecture-review.md §3.7
// item 3, lane 5 §4.2). A bitset: every verb's spec names the capabilities
// it needs and the dispatcher refuses before the handler runs. The set a
// pipe principal holds is re-evaluated on every request from the
// coordinator's settings, so flipping a toggle takes effect on the agent's
// next verb.
enum PhiCapability : uint32_t {
  // Its own agent Spaces and shadow windows (the consent default).
  kAgentSpace = 1,
  // Read the user's own Spaces, rows, rules and app state.
  kUserSpaceRead = 2,
  // Change them, and drive tabs in the user's windows — Settings ▸
  // Developer ▸ "Allow agents to operate your Spaces".
  kUserSpaceWrite = 4,
  // Deletions that cannot be undone: Spaces, profiles, folders, groups,
  // user windows/tabs, URL rules. Test rig only for now.
  kDestructive = 8,
  // App-wide settings, theme, layout, menus, feedback — developer mode.
  kAppSettings = 16,
  // The box's own hooks: HWNDs, PNGs on disk, analytics, fake accounts,
  // lazy-restore probes, forging the transcript's "You:" line, and seeing
  // or driving EVERY task. Test rig only, never a shipped agent.
  kHost = 32,
  // credentials.autofill / credentials.get + getTotp (their own prompts
  // still apply on top).
  kCredentialFill = 64,
  kCredentialReveal = 128,
  kAll = 0xFFFFFFFF,
};

// The closed set of error codes a verb can fail with (lane 5 §4.4). The
// wire name is the enum name in snake_case (PhiErrorCodeName). Handlers
// that answered a finer legacy token (`unknown_space`, `ambiguous`, …)
// keep it as the result's `reason`: protocol v1 emits the token verbatim
// as `error`, protocol v2 emits the code and carries the token in
// `error.details.reason`.
enum class PhiErrorCode : uint8_t {
  kInvalidPayload,
  kUnknownVerb,
  kScopeNotAllowed,
  kNotOwner,
  kUnknownTask,
  kSpaceNotOpen,
  kWindowNotOpen,
  kUserDenied,
  kOriginMismatch,
  kOriginChanged,
  kCapabilityRequired,
  kUserSpaceOperationsDisabled,
  kTestRigOnly,
  kPrefNotAgentSettable,
  kConfirmRequired,
  kBusy,
  kTimeout,
  kCreateFailed,
  kCreateTimeout,
  kNotInstalled,
  kTotpNotSupported,
  kInternal,
  kMaxValue = kInternal,
};

// The wire name of a code.
std::string_view PhiErrorCodeName(PhiErrorCode code);
// Every code's wire name, in enum order (agentSpace.capabilities, verbs.md).
base::span<const std::string_view> PhiErrorCodeNames();
// The code a legacy error token maps onto: a code's own name, one of the
// finer tokens the handlers answer (see kReasonCodes in the .cc), or
// kInternal for a token nobody declared.
PhiErrorCode PhiErrorCodeForReason(std::string_view reason);

// What a handler answers. Exactly one of: success with `fields`; failure
// with `code` (+ `reason`, the legacy token, + `fields` as details); or a
// deferred reply (`pending_reply` names a PhiControlDispatcher slot the
// coordinator answers later, in legacy JSON — FromLegacyJson turns it back
// into one of these).
struct PhiVerbResult {
  PhiVerbResult();
  PhiVerbResult(PhiVerbResult&&) noexcept;
  PhiVerbResult& operator=(PhiVerbResult&&) noexcept;
  ~PhiVerbResult();

  PhiVerbResult(const PhiVerbResult&) = delete;
  PhiVerbResult& operator=(const PhiVerbResult&) = delete;

  static PhiVerbResult Ok();
  static PhiVerbResult Ok(base::DictValue fields);
  // A refusal named by its code (`reason` = the code's wire name).
  static PhiVerbResult Fail(PhiErrorCode code);
  // A refusal with a finer legacy token and optional detail fields.
  static PhiVerbResult Fail(PhiErrorCode code,
                            std::string reason,
                            base::DictValue details = base::DictValue());
  // A refusal by legacy token alone; the code comes from
  // PhiErrorCodeForReason.
  static PhiVerbResult Fail(std::string reason,
                            base::DictValue details = base::DictValue());
  // A refusal the handler does not name — `{ok:false}` plus `fields`.
  // The dispatcher names it on the way out (the scope's error when the
  // payload's window, Space or task names nothing, else `code`), so no
  // bare refusal reaches the wire.
  static PhiVerbResult Refuse(PhiErrorCode code,
                              base::DictValue fields = base::DictValue());
  static PhiVerbResult Deferred(int pending_reply);
  // Parses a legacy reply body (`{ok, error?, …}`) — what the coordinator
  // hands CompletePendingReply, and what the handlers that still call a
  // coordinator method taking a `base::DictValue* out` get back.
  static PhiVerbResult FromLegacyDict(base::DictValue body);
  static PhiVerbResult FromLegacyJson(std::string_view json);

  bool deferred() const { return pending_reply != 0; }
  // The v1 body: `fields` + `ok:true`, or `ok:false` + `error` (when
  // `reason` is set) + `fields`; `{"pendingReply":<id>}` when deferred.
  base::DictValue ToLegacyDict() const;
  std::string ToLegacyJson() const;
  // The v2 `error` object: `{code, message?, details?}`.
  base::DictValue ToV2Error() const;

  bool ok = true;
  PhiErrorCode code = PhiErrorCode::kInternal;
  // The v1 `error` token; empty means a bare `ok:false`.
  std::string reason;
  // Human-readable, v2 only (validation: which field, what was wrong).
  std::string message;
  int pending_reply = 0;
  base::DictValue fields;
};

// The `out.Set("ok", ok)` answer most of the client-dialect verbs have
// always given: success with `fields`, or an unnamed refusal (Refuse) —
// the dispatcher answers it as the scope's error when the payload's
// window, Space or task names nothing, else as invalid_payload.
PhiVerbResult PhiVerbOk(bool ok, base::DictValue fields = base::DictValue());

// --- Parameter schema --------------------------------------------------------

enum PhiParamType : uint8_t {
  kParamString,
  // A JSON number; read as int (doubles truncate, the wire's windowId is a
  // double-typed integer).
  kParamInt,
  kParamBool,
  kParamDouble,
  // A list whose string entries are read (other entries are skipped).
  kParamStringList,
  kParamList,
  kParamObject,
};

constexpr bool kRequired = true;
constexpr bool kOptional = false;

// One declared payload field. `alias` lists alternate spellings the wire
// accepts for the same field, '|'-separated ("profileName" for profileId,
// "displayName" for name) — declared here ONCE, resolved by PhiParams.
struct PhiParamField {
  std::string_view name;
  PhiParamType type;
  bool required;
  std::string_view alias;
  std::string_view doc;
};
using PhiParamSchema = base::span<const PhiParamField>;

// The typed view of one request's payload through its verb's schema: one
// parse (the dispatcher's), aliases resolved here, so a handler asks for
// `params.String("spaceId")` and never re-reads the JSON. A name the
// schema does not declare is still looked up verbatim (nothing hides a
// field), but only declared fields are validated and documented.
class PhiParams {
 public:
  PhiParams(const base::DictValue* payload, PhiParamSchema schema);
  PhiParams(const PhiParams&) = default;
  PhiParams& operator=(const PhiParams&) = default;

  // Required fields present and every present field of its declared type;
  // false names the offending field in `*field` and says why in
  // `*message`.
  bool Validate(std::string* field, std::string* message) const;

  // The raw value under `name` or one of its aliases; null when absent.
  const base::Value* Find(std::string_view name) const;
  // Key presence (any type) — the "present clears, absent keeps" fields.
  bool Has(std::string_view name) const;
  const std::string* FindString(std::string_view name) const;
  std::string String(std::string_view name,
                     std::string_view fallback = std::string_view()) const;
  std::optional<int> FindInt(std::string_view name) const;
  int Int(std::string_view name, int fallback) const;
  std::optional<int64_t> FindInt64(std::string_view name) const;
  int64_t Int64(std::string_view name, int64_t fallback) const;
  std::optional<double> FindDouble(std::string_view name) const;
  double Double(std::string_view name, double fallback) const;
  std::optional<bool> FindBool(std::string_view name) const;
  bool Bool(std::string_view name, bool fallback) const;
  const base::ListValue* FindList(std::string_view name) const;
  const base::DictValue* FindDict(std::string_view name) const;
  // The string entries of a list field (non-strings skipped); empty when
  // absent.
  std::vector<std::string> StringList(std::string_view name) const;
  const base::DictValue& raw() const { return *payload_; }

 private:
  const PhiParamField* Field(std::string_view name) const;

  // The dispatcher's parsed payload; outlives the view (stack of the
  // request).
  RAW_PTR_EXCLUSION const base::DictValue* payload_;
  // Static table data for the life of the process (the schemas are
  // constexpr arrays in phi_verb_registry.cc), so it is not raw_span's to
  // watch — and a raw_span could not sit in a constexpr PhiVerbSpec row.
  RAW_PTR_EXCLUSION PhiParamSchema schema_;
};

// --- Verb spec ---------------------------------------------------------------

// Which window kinds a verb may be aimed at (ResolveScope decides which
// one a payload names). 0 = app-wide: no scope is resolved.
enum PhiScopeKind : uint8_t {
  kScopeAgent = 1,
  kScopeShadow = 2,
  kScopeUser = 4,
};
constexpr uint8_t kScopeNone = 0;
constexpr uint8_t kScopeTask = kScopeAgent | kScopeShadow;
constexpr uint8_t kScopeAny = kScopeAgent | kScopeShadow | kScopeUser;

enum PhiVerbFlag : uint16_t {
  kVerbPlain = 0,
  // Changes tabs, navigation or Spaces: arms the agent drive context on a
  // user scope (or app-wide) so the framework reports and refuses like a
  // CDP drive.
  kVerbMutates = 1,
  // Adds kDestructive when the scope is a user window (closing the user's
  // window, tab or group).
  kVerbDestructiveOnUser = 2,
  // No scope in the payload means the first user window (the client
  // dialect's windowId 0), not a refusal.
  kVerbDefaultsToUser = 4,
  // Adds kHost when the payload carries `outPath` (a PNG on the host's
  // disk).
  kVerbHostForOutPath = 8,
  // Adds kDestructive when `guid` names a bookmark folder.
  kVerbDestructiveOnFolder = 16,
};

// A handler is reached through a trampoline (PhiVerbCall, phi_verb_registry.cc)
// rather than a pointer-to-member: PhiVerbSpec has to be a complete type
// before PhiControlDispatcher is even declared (the dispatcher's header
// includes this one), and a pointer-to-member of an incomplete class is not
// expressible under the MSVC ABI — the compiler cannot pick the
// representation without knowing the inheritance. The trampoline costs one
// indirect call and keeps the registry free of the dispatcher's definition.
using PhiVerbHandler = PhiVerbResult (*)(PhiControlDispatcher* dispatcher,
                                         const PhiVerbContext& ctx);

// One row of the registry. Looked up by exact `name`; when two rows share
// a name, `selector` (a payload key) says which claims the request — the
// row whose selector is present wins, the row with none is the default.
struct PhiVerbSpec {
  std::string_view name;
  std::string_view selector;
  // PhiScopeKind bits; kScopeNone for an app-wide verb.
  uint8_t scopes;
  // PhiCapability bits needed app-wide, or on a USER-window scope. A
  // scoped verb aimed at an agent or shadow window needs kAgentSpace plus
  // ownership of the task, whatever this says.
  uint32_t required;
  // PhiVerbFlag bits.
  uint16_t flags;
  RAW_PTR_EXCLUSION PhiParamSchema params;
  // Shows UI (a prompt): serialised behind the dispatcher's prompt mutex.
  bool prompts;
  // Answers through the pending-reply slot (the coordinator completes it).
  bool deferred;
  PhiVerbHandler handler;
  // Documentation: the error tokens this verb answers, space-separated.
  std::string_view errors;
  std::string_view doc;
};

// The row for `name` given `payload` (see PhiVerbSpec::selector); null for
// a name the table does not know.
const PhiVerbSpec* FindVerbSpec(base::span<const PhiVerbSpec> table,
                                std::string_view name,
                                const base::DictValue& payload);

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_VERB_REGISTRY_H_
