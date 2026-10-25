// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_verb_registry.h"

#include <array>

#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "phibrowser-win/client/phi_control_dispatcher.h"

// The leaf types of the control plane's verb registry, and THE table. Every
// verb the /phi-agent channel serves has exactly one row here; the pipeline
// (PhiControlDispatcher::Execute) reads nothing else to decide what a
// request may do, what its payload must look like, and who runs it.
// tools/gen-verbs-md.py parses the table below into
// tools/phi-browser-skill/references/verbs.md, so the `errors` and `doc`
// columns are the published documentation, not a comment.

namespace phi_client {

namespace {

// The wire name of every code, in enum order. Kept beside the enum so a new
// code cannot be added without naming it.
constexpr std::string_view kErrorCodeNames[] = {
    "invalid_payload",
    "unknown_verb",
    "scope_not_allowed",
    "not_owner",
    "unknown_task",
    "space_not_open",
    "window_not_open",
    "user_denied",
    "origin_mismatch",
    "origin_changed",
    "capability_required",
    "user_space_operations_disabled",
    "test_rig_only",
    "pref_not_agent_settable",
    "confirm_required",
    "busy",
    "timeout",
    "create_failed",
    "create_timeout",
    "not_installed",
    "totp_not_supported",
    "internal",
};
static_assert(std::size(kErrorCodeNames) ==
                  static_cast<size_t>(PhiErrorCode::kMaxValue) + 1,
              "every PhiErrorCode needs a wire name");

// The finer legacy tokens the handlers answer, and the code each one
// reports as on protocol v2. A token absent from here is a code's own name
// (looked up in kErrorCodeNames first) or, failing that, kInternal.
struct ReasonCode {
  std::string_view reason;
  PhiErrorCode code;
};
constexpr ReasonCode kReasonCodes[] = {
    // "the payload named something that is not there" — one shape, many
    // nouns, because the skill's messages read better with the noun.
    {"unknown_space", PhiErrorCode::kInvalidPayload},
    {"unknown_rule", PhiErrorCode::kInvalidPayload},
    {"unknown_bookmark", PhiErrorCode::kInvalidPayload},
    {"unknown_folder", PhiErrorCode::kInvalidPayload},
    {"unknown_pin", PhiErrorCode::kInvalidPayload},
    {"unknown pin", PhiErrorCode::kInvalidPayload},
    {"unknown_download", PhiErrorCode::kInvalidPayload},
    {"unknown_target", PhiErrorCode::kInvalidPayload},
    {"duplicate_name", PhiErrorCode::kInvalidPayload},
    {"unknown_theme", PhiErrorCode::kInvalidPayload},
    {"invalid_color", PhiErrorCode::kInvalidPayload},
    {"invalid_icon", PhiErrorCode::kInvalidPayload},
    {"shadow_incognito_mismatch", PhiErrorCode::kInvalidPayload},
    {"fill_requires_autofill", PhiErrorCode::kInvalidPayload},
    {"not_a_login", PhiErrorCode::kInvalidPayload},
    {"field_unavailable", PhiErrorCode::kInvalidPayload},
    {"page_not_found", PhiErrorCode::kInvalidPayload},
    {"target_not_found", PhiErrorCode::kInvalidPayload},
    {"not_found", PhiErrorCode::kInvalidPayload},
    {"ambiguous", PhiErrorCode::kInvalidPayload},
    {"No target with given id found", PhiErrorCode::kInvalidPayload},
    // No window to aim at.
    {"no_window", PhiErrorCode::kWindowNotOpen},
    {"no_focused_window", PhiErrorCode::kWindowNotOpen},
    // Ask again later.
    {"not_ready", PhiErrorCode::kBusy},
    // Something inside the client failed, not the request.
    {"capture_failed", PhiErrorCode::kInternal},
    {"no_framework", PhiErrorCode::kInternal},
    {"provider_error", PhiErrorCode::kInternal},
    // The vault helper is not installed / cannot fill.
    {"autofill_not_available", PhiErrorCode::kNotInstalled},
    // notification.show (the Mac's callPrivate contract).
    {"invalid_params", PhiErrorCode::kInvalidPayload},
    {"agent_session_required", PhiErrorCode::kCapabilityRequired},
};

// The name of one '|'-separated alias, or the empty view past the end.
std::string_view NextAlias(std::string_view* rest) {
  if (rest->empty()) {
    return std::string_view();
  }
  const size_t bar = rest->find('|');
  if (bar == std::string_view::npos) {
    const std::string_view one = *rest;
    *rest = std::string_view();
    return one;
  }
  const std::string_view one = rest->substr(0, bar);
  *rest = rest->substr(bar + 1);
  return one;
}

const char* TypeName(PhiParamType type) {
  switch (type) {
    case kParamString:
      return "a string";
    case kParamInt:
      return "an integer";
    case kParamBool:
      return "a boolean";
    case kParamDouble:
      return "a number";
    case kParamStringList:
    case kParamList:
      return "a list";
    case kParamObject:
      return "an object";
  }
  return "a value";
}

bool TypeMatches(PhiParamType type, const base::Value& value) {
  switch (type) {
    case kParamString:
      return value.is_string();
    case kParamInt:
    case kParamDouble:
      // The wire has no integer type of its own: windowId and friends arrive
      // as JSON numbers, and base::Value keeps whichever fits.
      return value.is_int() || value.is_double();
    case kParamBool:
      return value.is_bool();
    case kParamStringList:
    case kParamList:
      return value.is_list();
    case kParamObject:
      return value.is_dict();
  }
  return true;
}

}  // namespace

// --- Error codes -------------------------------------------------------------

std::string_view PhiErrorCodeName(PhiErrorCode code) {
  const base::span<const std::string_view> names(kErrorCodeNames);
  const size_t index = static_cast<size_t>(code);
  // Out of range can only mean a code nobody named; report it as the last
  // one (kInternal) rather than reading past the table.
  return names[index < names.size() ? index : names.size() - 1];
}

base::span<const std::string_view> PhiErrorCodeNames() {
  return base::span<const std::string_view>(kErrorCodeNames);
}

PhiErrorCode PhiErrorCodeForReason(std::string_view reason) {
  size_t index = 0;
  for (std::string_view name : base::span<const std::string_view>(
           kErrorCodeNames)) {
    if (name == reason) {
      return static_cast<PhiErrorCode>(index);
    }
    ++index;
  }
  for (const ReasonCode& row : kReasonCodes) {
    if (row.reason == reason) {
      return row.code;
    }
  }
  return PhiErrorCode::kInternal;
}

// --- PhiVerbResult -----------------------------------------------------------

PhiVerbResult::PhiVerbResult() = default;
PhiVerbResult::PhiVerbResult(PhiVerbResult&&) noexcept = default;
PhiVerbResult& PhiVerbResult::operator=(PhiVerbResult&&) noexcept = default;
PhiVerbResult::~PhiVerbResult() = default;

// static
PhiVerbResult PhiVerbResult::Ok() {
  return PhiVerbResult();
}

// static
PhiVerbResult PhiVerbResult::Ok(base::DictValue fields) {
  PhiVerbResult result;
  result.fields = std::move(fields);
  return result;
}

// static
PhiVerbResult PhiVerbResult::Fail(PhiErrorCode code) {
  return Fail(code, std::string(PhiErrorCodeName(code)));
}

// static
PhiVerbResult PhiVerbResult::Fail(PhiErrorCode code,
                                  std::string reason,
                                  base::DictValue details) {
  PhiVerbResult result;
  result.ok = false;
  result.code = code;
  result.reason = std::move(reason);
  result.fields = std::move(details);
  return result;
}

// static
PhiVerbResult PhiVerbResult::Fail(std::string reason,
                                  base::DictValue details) {
  const PhiErrorCode code = PhiErrorCodeForReason(reason);
  return Fail(code, std::move(reason), std::move(details));
}

// static
PhiVerbResult PhiVerbResult::Refuse(PhiErrorCode code,
                                    base::DictValue fields) {
  PhiVerbResult result;
  result.ok = false;
  result.code = code;
  result.fields = std::move(fields);
  return result;
}

// static
PhiVerbResult PhiVerbResult::Deferred(int pending_reply) {
  PhiVerbResult result;
  result.pending_reply = pending_reply;
  return result;
}

// static
PhiVerbResult PhiVerbResult::FromLegacyDict(base::DictValue body) {
  if (std::optional<int> pending = body.FindInt("pendingReply")) {
    return Deferred(*pending);
  }
  // A body with no `ok` at all is a success: that is how the ladder's
  // read-only arms answered before the registry, and how the coordinator's
  // deferred completions still can.
  const bool ok = body.FindBool("ok").value_or(true);
  std::string reason;
  if (const std::string* error = body.FindString("error")) {
    reason = *error;
  }
  body.Remove("ok");
  body.Remove("error");
  if (ok) {
    return Ok(std::move(body));
  }
  if (reason.empty()) {
    return Refuse(PhiErrorCode::kInternal, std::move(body));
  }
  const PhiErrorCode code = PhiErrorCodeForReason(reason);
  return Fail(code, std::move(reason), std::move(body));
}

// static
PhiVerbResult PhiVerbResult::FromLegacyJson(std::string_view json) {
  std::optional<base::DictValue> body =
      base::JSONReader::ReadDict(json, base::JSON_PARSE_RFC);
  if (!body) {
    return Fail(PhiErrorCode::kInternal);
  }
  return FromLegacyDict(std::move(*body));
}

base::DictValue PhiVerbResult::ToLegacyDict() const {
  base::DictValue body;
  if (deferred()) {
    body.Set("pendingReply", pending_reply);
    return body;
  }
  body = fields.Clone();
  body.Set("ok", ok);
  if (!ok && !reason.empty()) {
    body.Set("error", reason);
  }
  return body;
}

std::string PhiVerbResult::ToLegacyJson() const {
  std::string json;
  base::JSONWriter::Write(ToLegacyDict(), &json);
  return json;
}

base::DictValue PhiVerbResult::ToV2Error() const {
  base::DictValue error;
  error.Set("code", PhiErrorCodeName(code));
  if (!message.empty()) {
    error.Set("message", message);
  }
  base::DictValue details = fields.Clone();
  if (!reason.empty() && reason != PhiErrorCodeName(code)) {
    details.Set("reason", reason);
  }
  if (!details.empty()) {
    error.Set("details", std::move(details));
  }
  return error;
}

PhiVerbResult PhiVerbOk(bool ok, base::DictValue fields) {
  return ok ? PhiVerbResult::Ok(std::move(fields))
            : PhiVerbResult::Refuse(PhiErrorCode::kInvalidPayload,
                                    std::move(fields));
}

// --- PhiParams ---------------------------------------------------------------

PhiParams::PhiParams(const base::DictValue* payload, PhiParamSchema schema)
    : payload_(payload), schema_(schema) {}

const PhiParamField* PhiParams::Field(std::string_view name) const {
  for (const PhiParamField& field : schema_) {
    if (field.name == name) {
      return &field;
    }
  }
  return nullptr;
}

const base::Value* PhiParams::Find(std::string_view name) const {
  if (!payload_) {
    return nullptr;
  }
  if (const base::Value* value = payload_->Find(name)) {
    return value;
  }
  const PhiParamField* field = Field(name);
  if (!field) {
    return nullptr;
  }
  std::string_view rest = field->alias;
  for (std::string_view alias = NextAlias(&rest); !alias.empty();
       alias = NextAlias(&rest)) {
    if (const base::Value* value = payload_->Find(alias)) {
      return value;
    }
  }
  return nullptr;
}

bool PhiParams::Validate(std::string* field_out, std::string* message) const {
  for (const PhiParamField& field : schema_) {
    const base::Value* value = Find(field.name);
    if (!value) {
      if (!field.required) {
        continue;
      }
      *field_out = std::string(field.name);
      *message = "missing required field `" + *field_out + "`";
      return false;
    }
    if (!TypeMatches(field.type, *value)) {
      *field_out = std::string(field.name);
      *message = "field `" + *field_out + "` must be " + TypeName(field.type);
      return false;
    }
    // A required string is required to say something: every ladder arm that
    // answered invalid_payload for a missing name did so for an empty one
    // too.
    if (field.required && field.type == kParamString &&
        value->GetString().empty()) {
      *field_out = std::string(field.name);
      *message = "field `" + *field_out + "` must not be empty";
      return false;
    }
  }
  return true;
}

bool PhiParams::Has(std::string_view name) const {
  return Find(name) != nullptr;
}

const std::string* PhiParams::FindString(std::string_view name) const {
  const base::Value* value = Find(name);
  return value ? value->GetIfString() : nullptr;
}

std::string PhiParams::String(std::string_view name,
                              std::string_view fallback) const {
  const std::string* value = FindString(name);
  return value ? *value : std::string(fallback);
}

std::optional<int> PhiParams::FindInt(std::string_view name) const {
  const base::Value* value = Find(name);
  if (!value) {
    return std::nullopt;
  }
  if (std::optional<int> as_int = value->GetIfInt()) {
    return as_int;
  }
  if (std::optional<double> as_double = value->GetIfDouble()) {
    return static_cast<int>(*as_double);
  }
  return std::nullopt;
}

int PhiParams::Int(std::string_view name, int fallback) const {
  return FindInt(name).value_or(fallback);
}

std::optional<int64_t> PhiParams::FindInt64(std::string_view name) const {
  std::optional<double> value = FindDouble(name);
  return value ? std::optional<int64_t>(static_cast<int64_t>(*value))
               : std::nullopt;
}

int64_t PhiParams::Int64(std::string_view name, int64_t fallback) const {
  return FindInt64(name).value_or(fallback);
}

std::optional<double> PhiParams::FindDouble(std::string_view name) const {
  const base::Value* value = Find(name);
  return value ? value->GetIfDouble() : std::nullopt;
}

double PhiParams::Double(std::string_view name, double fallback) const {
  return FindDouble(name).value_or(fallback);
}

std::optional<bool> PhiParams::FindBool(std::string_view name) const {
  const base::Value* value = Find(name);
  return value ? value->GetIfBool() : std::nullopt;
}

bool PhiParams::Bool(std::string_view name, bool fallback) const {
  return FindBool(name).value_or(fallback);
}

const base::ListValue* PhiParams::FindList(std::string_view name) const {
  const base::Value* value = Find(name);
  return value ? value->GetIfList() : nullptr;
}

const base::DictValue* PhiParams::FindDict(std::string_view name) const {
  const base::Value* value = Find(name);
  return value ? value->GetIfDict() : nullptr;
}

std::vector<std::string> PhiParams::StringList(std::string_view name) const {
  std::vector<std::string> items;
  if (const base::ListValue* list = FindList(name)) {
    for (const base::Value& value : *list) {
      if (value.is_string()) {
        items.push_back(value.GetString());
      }
    }
  }
  return items;
}

// --- Lookup ------------------------------------------------------------------

const PhiVerbSpec* FindVerbSpec(base::span<const PhiVerbSpec> table,
                                std::string_view name,
                                const base::DictValue& payload) {
  const PhiVerbSpec* fallback = nullptr;
  for (const PhiVerbSpec& row : table) {
    if (row.name != name) {
      continue;
    }
    if (row.selector.empty()) {
      fallback = &row;  // The row that claims the name when nothing else does.
      continue;
    }
    if (payload.Find(row.selector)) {
      return &row;
    }
  }
  return fallback;
}

// --- The table ---------------------------------------------------------------

namespace {

// Scope shorthands for the rows below.
constexpr uint8_t kAny = kScopeAny;
constexpr uint8_t kTask = kScopeTask;
// A verb that reads the window the payload names, or the first user window
// when it names none (the client dialect's windowId 0).
constexpr uint16_t kReadDefault = kVerbDefaultsToUser;
// The same, mutating: the agent drive context arms on a user scope.
constexpr uint16_t kWriteDefault = kVerbMutates | kVerbDefaultsToUser;
// A verb that needs an explicit window (no fallback), mutating.
constexpr uint16_t kTargetWrite = kVerbMutates;

// --- Schemas ---------------------------------------------------------------
// One array per payload shape. Aliases are declared HERE and nowhere else:
// PhiParams resolves them, so no handler ever spells a second key.

constexpr PhiParamField kTaskOnly[] = {
    {"taskId", kParamString, kRequired, "", "The task the verb acts on."},
};
constexpr PhiParamField kTaskOptional[] = {
    {"taskId", kParamString, kOptional, "", "The task the verb acts on."},
};
constexpr PhiParamField kTaskPing[] = {
    {"taskId", kParamString, kRequired, "", "The task to keep alive."},
    {"ttlSeconds", kParamDouble, kOptional, "",
     "Seconds of keep-alive; 0 keeps the current TTL."},
};
constexpr PhiParamField kAgentCreate[] = {
    {"taskId", kParamString, kRequired, "", "The task to bind or create."},
    {"profileId", kParamString, kOptional, "profileName",
     "The profile the Space runs in."},
    {"persistent", kParamBool, kOptional, "",
     "Keep the window when the task completes."},
    {"agentName", kParamString, kOptional, "",
     "The driving agent's product name (\"Claude Code\", \"Codex\", "
     "\"Pi\", …): the Space's pip wears that agent's mark and the watch "
     "pill names it."},
};
constexpr PhiParamField kAgentComplete[] = {
    {"taskId", kParamString, kRequired, "", "The task to complete."},
    {"status", kParamString, kOptional, "",
     "\"success\" (default) or anything else for a failure."},
    {"keep", kParamBool, kOptional, "",
     "Keep a persistent Space's window (default true)."},
    {"message", kParamString, kOptional, "",
     "A closing line for the transcript (a failure's reason)."},
};
constexpr PhiParamField kAgentHandoff[] = {
    {"taskId", kParamString, kRequired, "", "The task."},
    {"message", kParamString, kOptional, "",
     "Why the user is needed; shown in the prompt and the transcript."},
};
constexpr PhiParamField kAgentSetState[] = {
    {"taskId", kParamString, kRequired, "", "The task."},
    {"caption", kParamString, kOptional, "", "The pill's caption."},
    {"state", kParamString, kOptional, "", "The lifecycle state name."},
};
constexpr PhiParamField kAgentLog[] = {
    {"taskId", kParamString, kRequired, "", "The task."},
    {"text", kParamString, kOptional, "", "One transcript line."},
    {"entries", kParamList, kOptional, "",
     "Many lines, oldest first (max 200), each {text, kind?, detail?, "
     "agent?, ts?, sourceId?, toolState?}. kind: action | narration | "
     "error | user | assistant | thinking | tool (status and round are "
     "the app's own and downgrade to action); kind \"activity\" is a "
     "driver's live tail row (codex: activity working|waiting; claude: "
     "phase working|idle), never history."},
    {"kind", kParamString, kOptional, "", "The single line's kind."},
    {"detail", kParamString, kOptional, "", "The single line's detail."},
    {"agent", kParamString, kOptional, "", "The single line's agent."},
    {"touch", kParamBool, kOptional, "",
     "Refresh the keep-alive too (default true); a mirror passes false."},
};
constexpr PhiParamField kAgentMarkError[] = {
    {"taskId", kParamString, kRequired, "", "The task."},
    {"message", kParamString, kOptional, "", "What went wrong."},
};
constexpr PhiParamField kAgentUserMessage[] = {
    {"taskId", kParamString, kRequired, "", "The task."},
    {"text", kParamString, kRequired, "", "The user's line."},
};
constexpr PhiParamField kAgentOpenTab[] = {
    {"taskId", kParamString, kRequired, "", "The task."},
    {"url", kParamString, kRequired, "", "Where the new tab goes."},
};
constexpr PhiParamField kAgentCursor[] = {
    {"taskId", kParamString, kRequired, "", "The task."},
    {"x", kParamDouble, kRequired, "", "Cursor x, in the surface's points."},
    {"y", kParamDouble, kRequired, "", "Cursor y."},
};
constexpr PhiParamField kAgentEffect[] = {
    {"taskId", kParamString, kRequired, "", "The task."},
    {"kind", kParamString, kRequired, "", "Which effect to play."},
    {"x", kParamDouble, kOptional, "", "Effect origin x."},
    {"y", kParamDouble, kOptional, "", "Effect origin y."},
    {"w", kParamDouble, kOptional, "", "Effect width."},
    {"h", kParamDouble, kOptional, "", "Effect height."},
    {"dy", kParamDouble, kOptional, "", "Scroll delta."},
};
constexpr PhiParamField kAgentCapture[] = {
    {"taskId", kParamString, kRequired, "", "The task."},
    {"outPath", kParamString, kOptional, "",
     "Where the PNG lands on the host's disk (needs the host capability); "
     "absent, Phi writes it under the user's temp folder and answers the "
     "path."},
    {"webPath", kParamString, kOptional, "",
     "A CDP capture of the page to composite under the chrome."},
};
constexpr PhiParamField kReader[] = {
    {"taskId", kParamString, kOptional, "", "The window's task."},
    {"windowId", kParamInt, kOptional, "", "The window, by slot or browser."},
    {"spaceId", kParamString, kOptional, "space", "A user Space's window."},
    {"targetId", kParamString, kOptional, "",
     "The CDP target to distil; the active tab when absent."},
    {"includeHTML", kParamBool, kOptional, "",
     "readerArticle only: include the distilled HTML."},
};
constexpr PhiParamField kShadowCreate[] = {
    {"taskId", kParamString, kRequired, "", "The task to bind or create."},
    {"profileId", kParamString, kOptional, "profileName", "The profile."},
    {"incognito", kParamBool, kOptional, "",
     "Fixed at creation; a re-bind that disagrees is refused."},
};
constexpr PhiParamField kShadowOpenTab[] = {
    {"taskId", kParamString, kRequired, "", "The shadow window's task."},
    {"url", kParamString, kRequired, "", "Where the new tab goes."},
};
constexpr PhiParamField kResolveTabIds[] = {
    {"targetIds", kParamStringList, kOptional, "",
     "CDP target ids to translate into chrome tab ids."},
};
constexpr PhiParamField kJsDialogState[] = {
    {"targetId", kParamString, kRequired, "", "The CDP target."},
};
constexpr PhiParamField kJsDialogHandle[] = {
    {"targetId", kParamString, kRequired, "", "The CDP target."},
    {"accept", kParamBool, kOptional, "", "Accept rather than dismiss."},
    {"promptText", kParamString, kOptional, "", "Text for a prompt() dialog."},
};
constexpr PhiParamField kLazyRestore[] = {
    {"profile", kParamString, kOptional, "", "The profile whose session."},
    {"armed", kParamBool, kOptional, "", "Arm the lazy-restore path."},
    {"eagerWindowIds", kParamList, kOptional, "",
     "Windows to materialise immediately."},
};
constexpr PhiParamField kGhost[] = {
    {"profile", kParamString, kRequired, "", "The parked window's profile."},
    {"windowId", kParamInt, kRequired, "", "The parked window's id."},
};

// --- Spaces, profiles, rules, rows -----------------------------------------

constexpr PhiParamField kSpacesList[] = {
    {"windowId", kParamInt, kOptional, "",
     "Report `active`/`open` against this window."},
};
constexpr PhiParamField kSpacesCreate[] = {
    {"name", kParamString, kOptional, "displayName", "The Space's name."},
    {"profileId", kParamString, kOptional, "profileName",
     "The profile it binds (Default when absent)."},
    {"themeId", kParamString, kOptional, "",
     "A built-in theme by id or name (pure mist mint aqua iris petal coral "
     "amber); an agent's Space pins the app-wide theme when neither this "
     "nor colorHex is given."},
    {"colorHex", kParamString, kOptional, "",
     "\"#RRGGBB\", snapped to the nearest built-in theme by hue "
     "(themeId wins)."},
    {"iconName", kParamString, kOptional, "",
     "A phi: icon, a bare catalog name, an emoji:<codepoints> or the emoji "
     "itself; a random catalog draw when absent."},
    {"activate", kParamBool, kOptional, "",
     "Surface it in the focused user window."},
};
constexpr PhiParamField kSpacesSwitch[] = {
    {"windowId", kParamInt, kOptional, "", "The slot to switch."},
    {"spaceId", kParamString, kOptional, "space", "The Space to show."},
};
constexpr PhiParamField kSpacesRename[] = {
    {"spaceId", kParamString, kOptional, "space", "The Space."},
    {"name", kParamString, kOptional, "displayName", "Its new name."},
};
constexpr PhiParamField kSpacesDelete[] = {
    {"spaceId", kParamString, kOptional, "space", "The Space to delete."},
};
constexpr PhiParamField kSpacesReorder[] = {
    {"spaceId", kParamString, kOptional, "space", "The Space to move."},
    {"index", kParamInt, kOptional, "", "Its new position."},
};
constexpr PhiParamField kSpacesSetTheme[] = {
    {"spaceId", kParamString, kOptional, "space", "The Space."},
    {"themeId", kParamString, kOptional, "",
     "A built-in theme id; empty clears the override."},
};
constexpr PhiParamField kSpacesSetIcon[] = {
    {"spaceId", kParamString, kOptional, "space", "The Space."},
    {"icon", kParamString, kOptional, "", "A phi: icon id."},
};
constexpr PhiParamField kSpacesListTabs[] = {
    {"spaceId", kParamString, kOptional, "space", "The Space's open window."},
    {"windowId", kParamInt, kOptional, "", "That window, directly."},
};
constexpr PhiParamField kSpacesOpenTab[] = {
    {"spaceId", kParamString, kOptional, "space", "The Space's open window."},
    {"windowId", kParamInt, kOptional, "", "That window, directly."},
    {"url", kParamString, kRequired, "", "Where the new tab goes."},
    {"activate", kParamBool, kOptional, "",
     "Leave the new tab selected (default true)."},
};
constexpr PhiParamField kSpaceIdOnly[] = {
    {"spaceId", kParamString, kRequired, "space", "The Space."},
};
constexpr PhiParamField kSpacesUpdate[] = {
    {"spaceId", kParamString, kRequired, "space", "The Space."},
    {"name", kParamString, kOptional, "displayName", "Its new name."},
    {"themeId", kParamString, kOptional, "",
     "A built-in theme by id or name to pin."},
    {"colorHex", kParamString, kOptional, "",
     "\"#RRGGBB\", snapped to the nearest built-in theme by hue "
     "(themeId wins)."},
    {"iconName", kParamString, kOptional, "",
     "A phi: icon, a bare catalog name, an emoji:<codepoints> or the emoji "
     "itself."},
};
constexpr PhiParamField kProfileName[] = {
    {"name", kParamString, kOptional, "displayName", "The display name."},
};
constexpr PhiParamField kProfileRename[] = {
    {"id", kParamString, kOptional, "profileId", "The profile."},
    {"name", kParamString, kOptional, "displayName", "Its new name."},
};
constexpr PhiParamField kProfileDelete[] = {
    {"id", kParamString, kOptional, "profileId", "The profile."},
    {"confirm", kParamBool, kOptional, "",
     "Required: deleting a profile erases its data on disk."},
};
constexpr PhiParamField kUrlRuleAdd[] = {
    {"spaceId", kParamString, kRequired, "space",
     "Where matches are routed: a Space id, space.incognito (the first "
     "Incognito Space, opened if none is) or __phi_kiosk__ (a new Kiosk "
     "window)."},
    {"host", kParamString, kRequired, "", "The host to match."},
    {"pathPrefix", kParamString, kOptional, "", "An optional path prefix."},
    {"ask", kParamBool, kOptional, "", "Ask instead of routing silently."},
};
constexpr PhiParamField kUrlRuleUpdate[] = {
    {"id", kParamString, kOptional, "",
     "The rule's wire id from urlRules.list; ids regenerate on every write."},
    {"spaceId", kParamString, kOptional, "space",
     "A new target: a Space id, space.incognito or __phi_kiosk__."},
    {"host", kParamString, kOptional, "", "A new host."},
    {"pathPrefix", kParamString, kOptional, "",
     "A new path prefix; present clears, absent keeps."},
    {"ask", kParamBool, kOptional, "", "A new ask flag."},
};
constexpr PhiParamField kUrlRuleDelete[] = {
    {"id", kParamString, kOptional, "", "The rule's wire id."},
};
constexpr PhiParamField kPinScopeSet[] = {
    {"scope", kParamString, kOptional, "", "app, profile or space."},
};
constexpr PhiParamField kWindowIdOnly[] = {
    {"windowId", kParamInt, kOptional, "", "The slot."},
};
constexpr PhiParamField kGuidOnly[] = {
    {"guid", kParamString, kOptional, "", "The stored row."},
};
constexpr PhiParamField kGuidRequired[] = {
    {"guid", kParamString, kRequired, "", "The stored row."},
};
constexpr PhiParamField kPinnedAdd[] = {
    {"windowId", kParamInt, kOptional, "", "The slot that owns the pin."},
    {"title", kParamString, kOptional, "", "Its title."},
    {"url", kParamString, kOptional, "",
     "Its url; the slot's live url when absent."},
};
constexpr PhiParamField kBookmarksListFlat[] = {
    {"windowId", kParamInt, kRequired, "", "The slot that owns the rows."},
    {"parentGuid", kParamString, kOptional, "", "List inside this folder."},
};
constexpr PhiParamField kBookmarksAddFlat[] = {
    {"windowId", kParamInt, kRequired, "", "The slot that owns the row."},
    {"title", kParamString, kOptional, "", "Its title."},
    {"url", kParamString, kOptional, "",
     "Its url; the slot's live url when absent."},
    {"parentGuid", kParamString, kOptional, "", "Add inside this folder."},
    {"folder", kParamBool, kOptional, "", "Add a folder instead."},
};
constexpr PhiParamField kBookmarksListTree[] = {
    {"spaceId", kParamString, kOptional, "space",
     "The Space whose tree; the default Space when absent."},
};
constexpr PhiParamField kBookmarksAddTree[] = {
    {"spaceId", kParamString, kOptional, "space", "The Space."},
    {"url", kParamString, kRequired, "", "The bookmark's url."},
    {"title", kParamString, kOptional, "", "Its title."},
    {"parentGuid", kParamString, kOptional, "", "A folder in the same Space."},
    {"index", kParamInt, kOptional, "", "Its position among its siblings."},
};
constexpr PhiParamField kBookmarksAddFolder[] = {
    {"spaceId", kParamString, kOptional, "space", "The Space."},
    {"title", kParamString, kRequired, "", "The folder's name."},
    {"parentGuid", kParamString, kOptional, "", "A folder in the same Space."},
    {"index", kParamInt, kOptional, "", "Its position among its siblings."},
};
constexpr PhiParamField kBookmarksUpdate[] = {
    {"guid", kParamString, kRequired, "", "The row."},
    {"title", kParamString, kOptional, "", "A new title."},
    {"icon", kParamString, kOptional, "", "A folder icon id (folders only)."},
    {"layout", kParamString, kOptional, "",
     "vertical or horizontal (split rows only)."},
    {"url", kParamString, kOptional, "", "A new url."},
};
constexpr PhiParamField kBookmarksMove[] = {
    {"guid", kParamString, kRequired, "", "The row."},
    {"parentGuid", kParamString, kOptional, "", "Its new folder (root when absent)."},
    {"index", kParamInt, kOptional, "", "Its new position."},
};
constexpr PhiParamField kPinnedTabsList[] = {
    {"profileId", kParamString, kOptional, "",
     "Whose pins; the focused window's profile when absent."},
};
constexpr PhiParamField kPinnedTabsAdd[] = {
    {"profileId", kParamString, kOptional, "", "Whose pins."},
    {"url", kParamString, kRequired, "", "The pin's url."},
    {"title", kParamString, kOptional, "", "Its title."},
    {"index", kParamInt, kOptional, "", "Its position."},
};
constexpr PhiParamField kPinnedTabsUpdate[] = {
    {"guid", kParamString, kRequired, "", "The pin."},
    {"title", kParamString, kOptional, "", "A new title."},
    {"url", kParamString, kOptional, "", "A new url."},
};
constexpr PhiParamField kRowsReorder[] = {
    {"guid", kParamString, kOptional, "", "The row to move."},
    {"index", kParamInt, kOptional, "", "Its new position."},
};
constexpr PhiParamField kEmojiList[] = {
    {"group", kParamString, kOptional, "", "Restrict to one group."},
    {"limit", kParamDouble, kOptional, "", "How many to return (default 64)."},
};

// --- Windows, tabs, navigation ---------------------------------------------

constexpr PhiParamField kNavigate[] = {
    {"windowId", kParamInt, kOptional, "", "The window."},
    {"taskId", kParamString, kOptional, "", "…or the task's window."},
    {"spaceId", kParamString, kOptional, "space", "…or a Space's window."},
    {"url", kParamString, kOptional, "", "Where to go."},
};
constexpr PhiParamField kScopeOnly[] = {
    {"windowId", kParamInt, kOptional, "", "The window."},
    {"taskId", kParamString, kOptional, "", "…or the task's window."},
    {"spaceId", kParamString, kOptional, "space", "…or a Space's window."},
};
constexpr PhiParamField kSetBounds[] = {
    {"windowId", kParamInt, kOptional, "", "The window."},
    {"x", kParamDouble, kOptional, "", "Left, in physical pixels."},
    {"y", kParamDouble, kOptional, "", "Top."},
    {"width", kParamDouble, kOptional, "", "Width; must be > 0."},
    {"height", kParamDouble, kOptional, "", "Height; must be > 0."},
};
constexpr PhiParamField kMenuClick[] = {
    {"windowId", kParamInt, kOptional, "", "The window."},
    {"path", kParamStringList, kOptional, "",
     "Title path, e.g. [\"File\",\"New Tab\"]."},
};
constexpr PhiParamField kTabsNew[] = {
    {"windowId", kParamInt, kOptional, "", "The window."},
    {"url", kParamString, kOptional, "", "Where the new tab goes."},
};
constexpr PhiParamField kTabByIndex[] = {
    {"windowId", kParamInt, kOptional, "", "The window."},
    {"index", kParamInt, kOptional, "", "The strip index."},
};
constexpr PhiParamField kTabsMove[] = {
    {"windowId", kParamInt, kOptional, "", "The window."},
    {"from", kParamInt, kOptional, "", "The tab's current index."},
    {"to", kParamInt, kOptional, "", "Its final index."},
};
constexpr PhiParamField kTabsStoreSplit[] = {
    {"windowId", kParamInt, kOptional, "", "The slot."},
    {"indexA", kParamInt, kOptional, "", "First member's strip index."},
    {"indexB", kParamInt, kOptional, "", "Second member's strip index."},
    {"asBookmark", kParamBool, kOptional, "",
     "Store a split bookmark rather than a pinned pair."},
};
constexpr PhiParamField kNavReload[] = {
    {"windowId", kParamInt, kOptional, "", "The window."},
    {"bypassCache", kParamBool, kOptional, "", "Hard reload."},
};
constexpr PhiParamField kGroupOp[] = {
    {"windowId", kParamInt, kOptional, "", "The window."},
    {"groupId", kParamString, kOptional, "", "The tab group."},
    {"title", kParamString, kOptional, "", "A new title (rename)."},
    {"color", kParamString, kOptional, "", "A new colour name (color)."},
    {"collapsed", kParamBool, kOptional, "", "Collapsed state (collapse)."},
};
constexpr PhiParamField kTabGroupsCreate[] = {
    {"windowId", kParamInt, kOptional, "", "The window."},
    {"spaceId", kParamString, kOptional, "space", "…or a Space's window."},
    {"taskId", kParamString, kOptional, "", "…or a task's window."},
    {"tabIds", kParamList, kOptional, "", "The tabs to group, by id."},
    {"title", kParamString, kOptional, "", "The group's title."},
    {"color", kParamString, kOptional, "", "Its colour name."},
};
constexpr PhiParamField kTabGroupsUpdate[] = {
    {"windowId", kParamInt, kOptional, "", "The window."},
    {"spaceId", kParamString, kOptional, "space", "…or a Space's window."},
    {"taskId", kParamString, kOptional, "", "…or a task's window."},
    {"token", kParamString, kRequired, "", "The group token."},
    {"title", kParamString, kOptional, "", "A new title."},
    {"color", kParamString, kOptional, "", "A new colour name."},
    {"collapsed", kParamBool, kOptional, "", "A new collapsed state."},
};
constexpr PhiParamField kTabGroupsAddTabs[] = {
    {"windowId", kParamInt, kOptional, "", "The window."},
    {"spaceId", kParamString, kOptional, "space", "…or a Space's window."},
    {"taskId", kParamString, kOptional, "", "…or a task's window."},
    {"token", kParamString, kRequired, "", "The group token."},
    {"tabIds", kParamList, kOptional, "", "The tabs to add, by id."},
};
constexpr PhiParamField kTabGroupsRemoveTabs[] = {
    {"windowId", kParamInt, kOptional, "", "The window."},
    {"spaceId", kParamString, kOptional, "space", "…or a Space's window."},
    {"taskId", kParamString, kOptional, "", "…or a task's window."},
    {"tabIds", kParamList, kOptional, "", "The tabs to ungroup, by id."},
};
constexpr PhiParamField kTabGroupsToken[] = {
    {"windowId", kParamInt, kOptional, "", "The window."},
    {"spaceId", kParamString, kOptional, "space", "…or a Space's window."},
    {"taskId", kParamString, kOptional, "", "…or a task's window."},
    {"token", kParamString, kRequired, "", "The group token."},
};
constexpr PhiParamField kSplitCreate[] = {
    {"windowId", kParamInt, kOptional, "", "The window."},
    {"spaceId", kParamString, kOptional, "space", "…or a Space's window."},
    {"taskId", kParamString, kOptional, "", "…or a task's window."},
    {"primaryTabId", kParamInt, kOptional, "", "The left/top tab."},
    {"secondaryTabId", kParamInt, kOptional, "", "The right/bottom tab."},
    {"layout", kParamString, kOptional, "", "vertical or horizontal."},
};
constexpr PhiParamField kSplitUpdate[] = {
    {"windowId", kParamInt, kOptional, "", "The window."},
    {"spaceId", kParamString, kOptional, "space", "…or a Space's window."},
    {"taskId", kParamString, kOptional, "", "…or a task's window."},
    {"splitId", kParamString, kRequired, "", "The split."},
    {"layout", kParamString, kOptional, "", "A new axis."},
    {"ratio", kParamDouble, kOptional, "", "A new divider ratio."},
};
constexpr PhiParamField kSplitId[] = {
    {"windowId", kParamInt, kOptional, "", "The window."},
    {"spaceId", kParamString, kOptional, "space", "…or a Space's window."},
    {"taskId", kParamString, kOptional, "", "…or a task's window."},
    {"splitId", kParamString, kRequired, "", "The split."},
};
constexpr PhiParamField kDownloadGuid[] = {
    {"windowId", kParamInt, kOptional, "", "The window."},
    {"spaceId", kParamString, kOptional, "space", "…or a Space's window."},
    {"taskId", kParamString, kOptional, "", "…or a task's window."},
    {"guid", kParamString, kRequired, "", "The download."},
};
constexpr PhiParamField kLayoutSet[] = {
    {"mode", kParamString, kOptional, "", "A layout mode name."},
    {"sidebarWidth", kParamInt, kOptional, "", "The sidebar's width."},
};
constexpr PhiParamField kThemeSet[] = {
    {"themeId", kParamString, kOptional, "", "A built-in theme id."},
    {"appearance", kParamString, kOptional, "", "system, light or dark."},
};
constexpr PhiParamField kSettingsGet[] = {
    {"profileId", kParamString, kOptional, "",
     "Also report this profile's browsing settings."},
};
constexpr PhiParamField kSettingsSet[] = {
    {"saturation", kParamInt, kOptional, "", "Tint strength (clamped)."},
    {"alwaysShowFullUrl", kParamBool, kOptional, "", "Omnibox url display."},
    {"showTabPreviews", kParamBool, kOptional, "", "Hover previews."},
    {"showOpenTabIndicators", kParamBool, kOptional, "", "Open-tab dots."},
    {"dimUnloadedTabIcons", kParamBool, kOptional, "",
     "Reclaimed tabs' icons at 30%."},
    {"restoreLastSession", kParamBool, kOptional, "", "Session restore."},
    {"peekViewEnabled", kParamBool, kOptional, "", "Peek's master switch."},
    {"autoPeekViewEnabled", kParamBool, kOptional, "", "Automatic peek."},
    {"openKioskOnCtrlAltClick", kParamBool, kOptional, "",
     "Ctrl+Alt-click a link opens a Kiosk."},
    {"newTabBehavior", kParamString, kOptional, "", "omnibox or newTabPage."},
    {"developerMode", kParamBool, kOptional, "", "Never agent-settable."},
    {"section", kParamString, kOptional, "", "Show this settings pane."},
    {"profileId", kParamString, kOptional, "", "Whose browsing settings."},
    {"searchEngine", kParamString, kOptional, "", "Its default engine keyword."},
    {"downloadDirectory", kParamString, kOptional, "", "Its download folder."},
};
constexpr PhiParamField kFeedbackForm[] = {
    {"description", kParamString, kOptional, "", "The report's body."},
    {"url", kParamString, kOptional, "", "The url field."},
    {"attachFiles", kParamStringList, kOptional, "", "Paths to attach."},
    {"send", kParamBool, kOptional, "", "Press Send."},
};
constexpr PhiParamField kOnboardingDrive[] = {
    {"action", kParamString, kRequired, "",
     "signIn, guest, cancel, legal, metrics, name, swatch, layout, next, close."},
    {"value", kParamString, kOptional, "", "The name / theme id / layout."},
};
constexpr PhiParamField kSyncProfileKey[] = {
    {"profileId", kParamString, kRequired, "", "The Chromium profile id."},
    {"uuid", kParamString, kOptional, "",
     "The account-global profile uuid; empty removes the key."},
    {"passphrase", kParamString, kOptional, "",
     "The profile key, 64 lowercase hex chars."},
};
constexpr PhiParamField kSyncKeyOp[] = {
    {"op", kParamString, kRequired, "",
     "unlock, resolve, beginSetup, bootstrap, requestJoin, pollJoin, "
     "cancelJoin, recover, startPairing, applyDecisions, refresh, approve, "
     "deny, removeDevice, peek (read-only: the account's Phi sync data), "
     "syncNow (a Phi native sync round)."},
    {"requestId", kParamString, kOptional, "",
     "pollJoin / cancelJoin / approve / deny: the join request."},
    {"code", kParamString, kOptional, "", "recover: the recovery code."},
    {"confirm", kParamString, kOptional, "",
     "bootstrap: must be \"bootstrap\" (it creates the account's keys)."},
    {"decisions", kParamList, kOptional, "",
     "applyDecisions: [{kind: adopt|registerNew|createLocal, localProfileId, "
     "remoteUuid, displayName}]."},
};
constexpr PhiParamField kSyncSetupDrive[] = {
    {"action", kParamString, kRequired, "",
     "continue, saved, copy, requestApproval, useRecovery, cancel, code, "
     "submit, tryAnother, tryAgain, local, remote, continueProfiles, "
     "finishLater."},
    {"value", kParamString, kOptional, "",
     "code: the text; local: \"<row>:<remote row>|new\"; remote: "
     "\"<row>:create|assign:<local row>|none\"."},
};
constexpr PhiParamField kImportDrive[] = {
    {"action", kParamString, kRequired, "",
     "expand, toggle, profile, file, import, cancel."},
    {"value", kParamString, kOptional, "",
     "expand: a source (chrome, edge, brave, arc, file); toggle: "
     "\"<source>:<bookmarks|history|cookies|extensions>\"; profile: "
     "\"<source>:<index>\" (an Arc Space for arc); file: a path."},
};
constexpr PhiParamField kFeedbackAccount[] = {
    {"userId", kParamString, kOptional, "", "A stand-in account id."},
    {"email", kParamString, kOptional, "", "Its email."},
};

// --- Credentials -----------------------------------------------------------

constexpr PhiParamField kCredentialsGet[] = {
    {"query", kParamObject, kOptional, "",
     "{domain|id|search|username} — the item to look up."},
    {"domain", kParamString, kOptional, "",
     "A bare domain, when no `query` is given."},
    {"mode", kParamString, kOptional, "",
     "\"fill\" is refused here: a fill is credentials.autofill's job."},
    {"fields", kParamStringList, kOptional, "",
     "Which fields to return; a type default when absent."},
    {"purpose", kParamString, kOptional, "", "Shown in the approval prompt."},
};
constexpr PhiParamField kCredentialsAutofill[] = {
    {"query", kParamObject, kOptional, "", "{domain|id|search|username}."},
    {"domain", kParamString, kOptional, "", "A bare domain."},
    {"token", kParamString, kRequired, "", "The tagged field's token."},
    {"targetId", kParamString, kRequired, "", "The page's CDP target."},
    {"field", kParamString, kOptional, "", "password (default) or username."},
    {"allowCrossOrigin", kParamBool, kOptional, "",
     "Allow a fill whose item does not belong to the page's host."},
};

// --- Notification cards -----------------------------------------------------
// Every field optional at the schema: a missing or blank one is the
// handler's invalid_params, the Mac's answer, not the schema's.
constexpr PhiParamField kNotificationShow[] = {
    {"title", kParamString, kOptional, "", "The card's title (nonblank)."},
    {"message", kParamString, kOptional, "",
     "The card's message (nonblank)."},
    {"buttonTitle", kParamString, kOptional, "",
     "The accept button's label (default Run)."},
    {"expiresInSeconds", kParamDouble, kOptional, "",
     "Whole seconds, 1 to 86400 (default 300); the card then answers "
     "timeout."},
};

}  // namespace

// static
base::span<const PhiVerbSpec> PhiControlDispatcher::Registry() {
  // Order is documentation order (verbs.md follows it), not lookup order:
  // FindVerbSpec scans by name and settles collisions with `selector`.
  static constexpr PhiVerbSpec kTable[] = {
      // --- The agent-Space lifecycle -------------------------------------
      {"agentSpace.ping", "taskId", kTask, kAgentSpace, kVerbPlain, kTaskPing,
       false, false, &Call<&PhiControlDispatcher::VerbAgentPing>,
       "unknown_task not_owner",
       "Refreshes a task's keep-alive."},
      {"agentSpace.ping", "", kScopeNone, kAgentSpace, kVerbPlain, {}, false,
       false, &Call<&PhiControlDispatcher::VerbPing>, "",
       "Liveness probe: the app phase and how many saves are still "
       "coalescing."},
      {"agentSpace.list", "", kScopeNone, kAgentSpace, kVerbPlain, {}, false,
       false, &Call<&PhiControlDispatcher::VerbList>, "",
       "The caller's tasks, the open window slots, and whether the Agent "
       "Transcript console is watching."},
      {"agentSpace.capabilities", "", kScopeNone, kAgentSpace, kVerbPlain, {},
       false, false, &Call<&PhiControlDispatcher::VerbCapabilities>, "",
       "What this connection may do: its capability bits, its tasks, the "
       "protocol version and every verb name it may call."},
      {"agentSpace.create", "", kTask, kAgentSpace, kVerbPlain, kAgentCreate,
       false, true, &Call<&PhiControlDispatcher::VerbAgentCreate>,
       "invalid_payload not_owner busy create_failed create_timeout "
       "profile_not_agent_allowed",
       "Binds a task to an agent Space, creating its hidden window when "
       "there is none (or the old one went tabless)."},
      {"agentSpace.complete", "", kTask, kAgentSpace, kVerbPlain,
       kAgentComplete, false, false,
       &Call<&PhiControlDispatcher::VerbAgentComplete>,
       "invalid_payload unknown_task not_owner",
       "Ends a task: an ephemeral Space is torn down, a persistent one is "
       "marked completed."},
      {"agentSpace.getOwnership", "", kScopeNone, kAgentSpace, kVerbPlain,
       kTaskOptional, false, false,
       &Call<&PhiControlDispatcher::VerbAgentGetOwnership>, "",
       "Who drives a task — agent, user, or none for a task nobody knows."},
      {"agentSpace.handoff", "", kTask, kAgentSpace, kVerbPlain, kAgentHandoff,
       false, false, &Call<&PhiControlDispatcher::VerbAgentHandoff>,
       "unknown_task not_owner", "Hands a task's Space to the user."},
      {"agentSpace.takeover", "", kTask, kAgentSpace, kVerbPlain, kTaskOnly,
       false, false, &Call<&PhiControlDispatcher::VerbAgentTakeover>,
       "unknown_task not_owner", "Takes a task's Space back for the agent."},
      {"agentSpace.setState", "", kTask, kAgentSpace, kVerbPlain,
       kAgentSetState, false, false,
       &Call<&PhiControlDispatcher::VerbAgentSetState>,
       "invalid_payload unknown_task not_owner",
       "Sets the watch-mode pill's caption and the task's state."},
      {"agentSpace.log", "", kTask, kAgentSpace, kVerbPlain, kAgentLog, false,
       false, &Call<&PhiControlDispatcher::VerbAgentLog>,
       "invalid_payload unknown_task not_owner",
       "Appends transcript lines and (unless touch:false) refreshes the "
       "keep-alive."},
      {"agentSpace.markError", "", kTask, kAgentSpace, kVerbPlain,
       kAgentMarkError, false, false,
       &Call<&PhiControlDispatcher::VerbAgentMarkError>,
       "invalid_payload unknown_task not_owner",
       "Marks a task failed and logs why."},
      {"agentSpace.readUserMessages", "", kTask, kAgentSpace, kVerbPlain,
       kTaskOnly, false, false,
       &Call<&PhiControlDispatcher::VerbAgentReadUserMessages>,
       "invalid_payload unknown_task not_owner",
       "Drains the console lines the user typed at this task."},
      {"agentSpace.userMessage", "", kScopeNone, kHost, kVerbPlain,
       kAgentUserMessage, false, false,
       &Call<&PhiControlDispatcher::VerbAgentUserMessage>,
       "invalid_payload unknown_task test_rig_only",
       "Forges the transcript's \"You:\" line — the test rig only."},
      {"agentSpace.openTab", "", kTask, kAgentSpace, kVerbPlain, kAgentOpenTab,
       false, false, &Call<&PhiControlDispatcher::VerbAgentOpenTab>,
       "invalid_payload unknown_task not_owner",
       "Opens a tab in the task's agent Space."},
      {"agentSpace.listProfiles", "", kScopeNone, kAgentSpace, kVerbPlain, {},
       false, false, &Call<&PhiControlDispatcher::VerbAgentListProfiles>, "",
       "The profiles an agent Space may be created in."},
      {"agentSpace.panelSize", "", kTask, kAgentSpace, kVerbPlain, {}, false,
       false, &Call<&PhiControlDispatcher::VerbAgentPanelSize>, "",
       "The agent surface's content size."},
      {"agentSpace.cursor", "", kTask, kAgentSpace, kVerbPlain, kAgentCursor,
       false, false, &Call<&PhiControlDispatcher::VerbAgentCursor>,
       "invalid_payload unknown_task not_owner",
       "Moves the watcher overlay's agent cursor."},
      {"agentSpace.effect", "", kTask, kAgentSpace, kVerbPlain, kAgentEffect,
       false, false, &Call<&PhiControlDispatcher::VerbAgentEffect>,
       "invalid_payload unknown_task not_owner",
       "Plays an input-mirror effect on the watcher overlay."},
      {"agentSpace.captureWindow", "", kTask, kAgentSpace,
       kVerbHostForOutPath, kAgentCapture, false, false,
       &Call<&PhiControlDispatcher::VerbAgentCaptureWindow>,
       "invalid_payload unknown_task no_window capture_failed test_rig_only",
       "Renders the task window's client-drawn chrome to a PNG, compositing "
       "a CDP page capture over the content rect."},
      {"agentSpace.readerArticle", "", kAny, kUserSpaceRead, kVerbPlain,
       kReader, false, true,
       &Call<&PhiControlDispatcher::VerbReaderArticle>,
       "unknown_task window_not_open space_not_open unknown_target timeout "
       "scope_not_allowed",
       "Distils the scope's tab into a reader article without navigating "
       "it."},
      {"agentSpace.readerDocument", "", kAny, kUserSpaceRead, kVerbPlain,
       kReader, false, true,
       &Call<&PhiControlDispatcher::VerbReaderDocument>,
       "unknown_task window_not_open space_not_open unknown_target timeout "
       "scope_not_allowed",
       "The same distillation as a saveable document."},

      // --- Shadow windows -------------------------------------------------
      {"agentSpace.shadow.list", "", kTask, kAgentSpace, kVerbPlain, {}, false,
       false, &Call<&PhiControlDispatcher::VerbShadowList>, "",
       "The caller's live shadow windows (every one, with the host "
       "capability)."},
      {"agentSpace.shadow.create", "", kTask, kAgentSpace, kVerbPlain,
       kShadowCreate, false, true,
       &Call<&PhiControlDispatcher::VerbShadowCreate>,
       "invalid_payload not_owner busy create_failed create_timeout "
       "shadow_incognito_mismatch profile_not_agent_allowed",
       "Binds a task to a hidden background window, creating it when there "
       "is none."},
      {"agentSpace.shadow.openTab", "", kTask, kAgentSpace, kVerbPlain,
       kShadowOpenTab, false, false,
       &Call<&PhiControlDispatcher::VerbShadowOpenTab>,
       "invalid_payload unknown_task not_owner",
       "Opens a tab in the shadow window."},
      {"agentSpace.shadow.ping", "", kTask, kAgentSpace, kVerbPlain, kTaskPing,
       false, false, &Call<&PhiControlDispatcher::VerbShadowPing>,
       "invalid_payload unknown_task not_owner",
       "Refreshes the shadow window's keep-alive."},
      {"agentSpace.shadow.close", "", kTask, kAgentSpace, kVerbPlain,
       kTaskOnly, false, false,
       &Call<&PhiControlDispatcher::VerbShadowClose>,
       "invalid_payload unknown_task not_owner",
       "Closes the shadow window."},

      // --- Task-less probes ------------------------------------------------
      {"agentSpace.resolveTabIds", "", kScopeNone, kAgentSpace, kVerbPlain,
       kResolveTabIds, false, false,
       &Call<&PhiControlDispatcher::VerbResolveTabIds>, "",
       "Translates CDP target ids into chrome tab ids (-1 for one that names "
       "nothing, or a tab outside the caller's windows)."},
      {"agentSpace.getJavaScriptDialogState", "", kScopeNone, kAgentSpace,
       kVerbPlain, kJsDialogState, false, false,
       &Call<&PhiControlDispatcher::VerbJsDialogState>,
       "invalid_payload scope_not_allowed", "Whether a target has a JavaScript dialog up."},
      {"agentSpace.handleJavaScriptDialog", "", kScopeNone, kAgentSpace,
       kVerbPlain, kJsDialogHandle, false, false,
       &Call<&PhiControlDispatcher::VerbJsDialogHandle>,
       "invalid_payload scope_not_allowed", "Accepts or dismisses a target's dialog."},
      {"agentSpace.telemetry.status", "", kScopeNone, kAgentSpace, kVerbPlain,
       {}, false, false, &Call<&PhiControlDispatcher::VerbTelemetryStatus>, "",
       "What the outbound telemetry pipeline is doing."},

      // --- Host hooks (the box) --------------------------------------------
      {"agentSpace.analytics.list", "", kScopeNone, kHost, kVerbPlain, {},
       false, false, &Call<&PhiControlDispatcher::VerbAnalyticsList>,
       "test_rig_only",
       "Every analytics event the browser delivered this session."},
      {"agentSpace.lazyRestore.ghosts", "", kScopeNone, kHost, kVerbPlain, {},
       false, false, &Call<&PhiControlDispatcher::VerbLazyRestoreGhosts>,
       "test_rig_only no_framework",
       "The parked windows and the last restore receipt."},
      {"agentSpace.lazyRestore.restore", "", kScopeNone, kHost, kVerbPlain,
       kLazyRestore, false, false,
       &Call<&PhiControlDispatcher::VerbLazyRestoreRestore>,
       "test_rig_only no_framework", "Restores the previous session."},
      {"agentSpace.lazyRestore.materialize", "", kScopeNone, kHost,
       kVerbPlain, kGhost, false, false,
       &Call<&PhiControlDispatcher::VerbLazyRestoreMaterialize>,
       "test_rig_only no_framework invalid_payload",
       "Materialises one parked window; reports materialized / refusedForNow "
       "/ noSuchGhost by name."},
      {"agentSpace.lazyRestore.drop", "", kScopeNone, kHost, kVerbPlain,
       kGhost, false, false,
       &Call<&PhiControlDispatcher::VerbLazyRestoreDrop>,
       "test_rig_only no_framework invalid_payload",
       "Drops one parked window."},

      // --- The user's Spaces -----------------------------------------------
      {"agentSpace.spaces.list", "", kScopeNone, kAgentSpace, kVerbPlain,
       kSpacesList, false, false,
       &Call<&PhiControlDispatcher::VerbSpacesList>, "",
       "Every Space, in both the client-UI and the management shape, with "
       "the windows each is open in."},
      {"agentSpace.spaces.create", "", kScopeNone, kUserSpaceWrite,
       kVerbMutates, kSpacesCreate, false, false,
       &Call<&PhiControlDispatcher::VerbSpacesCreate>,
       "invalid_payload unknown_theme invalid_color invalid_icon "
       "user_space_operations_disabled",
       "Creates a Space with a pinned theme; answers the themeId and "
       "iconName that landed."},
      {"agentSpace.spaces.switch", "", kScopeNone, kUserSpaceWrite,
       kVerbMutates, kSpacesSwitch, false, false,
       &Call<&PhiControlDispatcher::VerbSpacesSwitch>,
       "user_space_operations_disabled scope_not_allowed",
       "Shows a Space in a user slot (never an agent or incognito window, "
       "never an Incognito Space for an agent), cold-spawning its browser "
       "when needed."},
      {"agentSpace.spaces.rename", "", kScopeNone, kUserSpaceWrite,
       kVerbMutates, kSpacesRename, false, false,
       &Call<&PhiControlDispatcher::VerbSpacesRename>,
       "user_space_operations_disabled", "Renames a Space."},
      {"agentSpace.spaces.delete", "", kScopeNone,
       kUserSpaceWrite | kDestructive, kVerbPlain, kSpacesDelete, false, false,
       &Call<&PhiControlDispatcher::VerbSpacesDelete>,
       "user_space_operations_disabled test_rig_only", "Deletes a Space."},
      {"agentSpace.spaces.reorder", "", kScopeNone, kUserSpaceWrite,
       kVerbMutates, kSpacesReorder, false, false,
       &Call<&PhiControlDispatcher::VerbSpacesReorder>,
       "user_space_operations_disabled", "Moves a Space in the sidebar."},
      {"agentSpace.spaces.setTheme", "", kScopeNone, kUserSpaceWrite,
       kVerbMutates, kSpacesSetTheme, false, false,
       &Call<&PhiControlDispatcher::VerbSpacesSetTheme>,
       "user_space_operations_disabled", "Pins a Space's theme."},
      {"agentSpace.spaces.setIcon", "", kScopeNone, kUserSpaceWrite,
       kVerbMutates, kSpacesSetIcon, false, false,
       &Call<&PhiControlDispatcher::VerbSpacesSetIcon>,
       "user_space_operations_disabled", "Sets a Space's icon."},
      {"agentSpace.spaces.listTabs", "", kAny, kUserSpaceRead, kVerbPlain,
       kSpacesListTabs, false, false,
       &Call<&PhiControlDispatcher::VerbSpacesListTabs>,
       "space_not_open window_not_open user_space_operations_disabled",
       "The tabs of a user Space's open window, with their binding tags and "
       "splits."},
      {"agentSpace.spaces.openTab", "", kAny, kUserSpaceWrite, kTargetWrite,
       kSpacesOpenTab, false, false,
       &Call<&PhiControlDispatcher::VerbSpacesOpenTab>,
       "invalid_payload unknown_space window_not_open create_failed "
       "create_timeout user_space_operations_disabled",
       "Opens a tab in a user Space's window, bypassing url routing. A Space "
       "with no window gets one first (surfaced when activate, else behind "
       "the user's windows without taking focus) and the reply says "
       "windowOpened:true; a New Tab URL is then the window's own seed tab. "
       "A windowId never opens one."},
      {"agentSpace.spaces.focus", "", kScopeNone, kAgentSpace, kVerbPlain, {},
       false, false, &Call<&PhiControlDispatcher::VerbSpacesFocus>,
       "no_window",
       "What the foreground window is showing — the orientation read every "
       "round starts with."},
      {"agentSpace.spaces.activate", "", kScopeNone, kUserSpaceWrite,
       kVerbMutates, kSpaceIdOnly, false, false,
       &Call<&PhiControlDispatcher::VerbSpacesActivate>,
       "invalid_payload unknown_space no_focused_window "
       "user_space_operations_disabled",
       "Surfaces a Space in whichever window already hosts it, else in the "
       "focused one; with no user window open at all there is nothing to "
       "switch (spaces.openTab opens one)."},
      {"agentSpace.spaces.update", "", kScopeNone, kUserSpaceWrite,
       kVerbMutates, kSpacesUpdate, false, false,
       &Call<&PhiControlDispatcher::VerbSpacesUpdate>,
       "invalid_payload unknown_space unknown_theme invalid_color "
       "invalid_icon user_space_operations_disabled",
       "Renames, re-icons or re-themes a Space in one call, validating every "
       "field before writing any; answers the themeId (and iconName) that "
       "landed."},

      // --- Profiles ---------------------------------------------------------
      {"agentSpace.profiles.list", "", kScopeNone, kAgentSpace, kVerbPlain, {},
       false, false, &Call<&PhiControlDispatcher::VerbProfilesList>, "",
       "The browser's profiles."},
      {"agentSpace.profiles.create", "", kScopeNone, kUserSpaceWrite,
       kVerbPlain, kProfileName, false, true,
       &Call<&PhiControlDispatcher::VerbProfilesCreate>,
       "duplicate_name user_space_operations_disabled create_timeout",
       "Creates a profile and answers with the basename the browser gave "
       "it."},
      {"agentSpace.profiles.rename", "", kScopeNone, kUserSpaceWrite,
       kVerbPlain, kProfileRename, false, false,
       &Call<&PhiControlDispatcher::VerbProfilesRename>,
       "user_space_operations_disabled", "Renames a profile."},
      {"agentSpace.profiles.delete", "", kScopeNone,
       kUserSpaceWrite | kDestructive, kVerbPlain, kProfileDelete, false,
       false, &Call<&PhiControlDispatcher::VerbProfilesDelete>,
       "test_rig_only confirm_required user_space_operations_disabled",
       "Erases a profile and its data — the test rig only, and only with "
       "confirm:true."},

      // --- URL routing rules ------------------------------------------------
      {"agentSpace.urlRules.list", "", kScopeNone, kUserSpaceRead, kVerbPlain,
       {}, false, false, &Call<&PhiControlDispatcher::VerbUrlRulesList>,
       "user_space_operations_disabled",
       "The routing rules, with the wire ids that regenerate on every "
       "write."},
      {"agentSpace.urlRules.add", "", kScopeNone, kUserSpaceWrite, kVerbPlain,
       kUrlRuleAdd, false, false,
       &Call<&PhiControlDispatcher::VerbUrlRulesAdd>,
       "invalid_payload unknown_space user_space_operations_disabled",
       "Adds a routing rule."},
      {"agentSpace.urlRules.update", "", kScopeNone, kUserSpaceWrite,
       kVerbPlain, kUrlRuleUpdate, false, false,
       &Call<&PhiControlDispatcher::VerbUrlRulesUpdate>,
       "invalid_payload unknown_rule unknown_space "
       "user_space_operations_disabled",
       "Changes a rule's pattern or target; absent fields keep their value."},
      {"agentSpace.urlRules.delete", "", kScopeNone,
       kUserSpaceWrite | kDestructive, kVerbPlain, kUrlRuleDelete, false,
       false, &Call<&PhiControlDispatcher::VerbUrlRulesDelete>,
       "invalid_payload unknown_rule test_rig_only "
       "user_space_operations_disabled",
       "Removes a routing rule."},

      // --- Pins and bookmarks (the client dialect) ---------------------------
      {"agentSpace.pinned.scope.get", "", kScopeNone, kUserSpaceRead,
       kVerbPlain, {}, false, false,
       &Call<&PhiControlDispatcher::VerbPinnedScopeGet>,
       "user_space_operations_disabled", "How pins are scoped."},
      {"agentSpace.pinned.scope.set", "", kScopeNone, kUserSpaceWrite,
       kVerbPlain, kPinScopeSet, false, false,
       &Call<&PhiControlDispatcher::VerbPinnedScopeSet>,
       "user_space_operations_disabled", "Changes how pins are scoped."},
      {"agentSpace.pinned.list", "", kAny, kUserSpaceRead, kReadDefault,
       kWindowIdOnly, false, false,
       &Call<&PhiControlDispatcher::VerbPinnedList>,
       "user_space_operations_disabled", "A slot's pins."},
      {"agentSpace.pinned.open", "", kAny, kUserSpaceWrite, kWriteDefault,
       kGuidOnly, false, false,
       &Call<&PhiControlDispatcher::VerbPinnedOpen>,
       "unknown pin user_space_operations_disabled",
       "Focuses a pin's live tab, or opens one bound to it."},
      {"agentSpace.pinned.add", "", kAny, kUserSpaceWrite, kWriteDefault,
       kPinnedAdd, false, false,
       &Call<&PhiControlDispatcher::VerbPinnedAdd>,
       "user_space_operations_disabled",
       "Pins a url (the slot's live tab when none is given), capturing its "
       "favicon when it is the live one."},
      {"agentSpace.pinned.remove", "", kScopeNone, kUserSpaceWrite,
       kVerbPlain, kGuidOnly, false, false,
       &Call<&PhiControlDispatcher::VerbPinnedRemove>,
       "user_space_operations_disabled",
       "Removes a pin, clearing the identity tag from any tab bound to it."},
      {"agentSpace.bookmarks.list", "windowId", kAny, kUserSpaceRead,
       kReadDefault, kBookmarksListFlat, false, false,
       &Call<&PhiControlDispatcher::VerbBookmarksListFlat>,
       "user_space_operations_disabled",
       "One folder's rows for a slot (the client dialect: flat, windowId-"
       "scoped)."},
      {"agentSpace.bookmarks.list", "", kAny, kUserSpaceRead, kReadDefault,
       kBookmarksListTree, false, false,
       &Call<&PhiControlDispatcher::VerbBookmarksListTree>,
       "unknown_space user_space_operations_disabled",
       "A Space's whole bookmark tree (the skill dialect: folders carry "
       "`children`)."},
      {"agentSpace.bookmarks.add", "windowId", kAny, kUserSpaceWrite,
       kWriteDefault, kBookmarksAddFlat, false, false,
       &Call<&PhiControlDispatcher::VerbBookmarksAddFlat>,
       "user_space_operations_disabled",
       "Adds a bookmark or folder to a slot's rows."},
      {"agentSpace.bookmarks.add", "", kAny, kUserSpaceWrite, kWriteDefault,
       kBookmarksAddTree, false, false,
       &Call<&PhiControlDispatcher::VerbBookmarksAddTree>,
       "invalid_payload unknown_space unknown_folder create_failed "
       "user_space_operations_disabled",
       "Adds a bookmark inside a Space's tree."},
      {"agentSpace.bookmarks.addFolder", "", kScopeNone, kUserSpaceWrite,
       kVerbPlain, kBookmarksAddFolder, false, false,
       &Call<&PhiControlDispatcher::VerbBookmarksAddFolder>,
       "invalid_payload unknown_space unknown_folder create_failed "
       "user_space_operations_disabled",
       "Adds a folder inside a Space's tree."},
      {"agentSpace.bookmarks.update", "", kScopeNone, kUserSpaceWrite,
       kVerbPlain, kBookmarksUpdate, false, false,
       &Call<&PhiControlDispatcher::VerbBookmarksUpdate>,
       "invalid_payload unknown_bookmark user_space_operations_disabled",
       "Retitles, re-icons, re-lays-out or re-points a stored row."},
      {"agentSpace.bookmarks.move", "", kScopeNone, kUserSpaceWrite,
       kVerbPlain, kBookmarksMove, false, false,
       &Call<&PhiControlDispatcher::VerbBookmarksMove>,
       "invalid_payload unknown_bookmark user_space_operations_disabled",
       "Moves a stored row into a folder and position."},
      {"agentSpace.bookmarks.remove", "", kScopeNone, kUserSpaceWrite,
       kVerbDestructiveOnFolder, kGuidOnly, false, false,
       &Call<&PhiControlDispatcher::VerbBookmarksRemove>,
       "test_rig_only user_space_operations_disabled",
       "Removes a stored row; a FOLDER also needs the destructive "
       "capability."},

      // --- Pinned tabs (the skill dialect, per profile) ----------------------
      {"agentSpace.pinnedTabs.list", "", kScopeNone, kUserSpaceRead,
       kVerbPlain, kPinnedTabsList, false, false,
       &Call<&PhiControlDispatcher::VerbPinnedTabsList>,
       "no_window user_space_operations_disabled", "A profile's pins."},
      {"agentSpace.pinnedTabs.add", "", kScopeNone, kUserSpaceWrite,
       kVerbPlain, kPinnedTabsAdd, false, false,
       &Call<&PhiControlDispatcher::VerbPinnedTabsAdd>,
       "invalid_payload no_window create_failed "
       "user_space_operations_disabled",
       "Pins a url for a profile."},
      {"agentSpace.pinnedTabs.update", "", kScopeNone, kUserSpaceWrite,
       kVerbPlain, kPinnedTabsUpdate, false, false,
       &Call<&PhiControlDispatcher::VerbPinnedTabsUpdate>,
       "invalid_payload unknown_pin user_space_operations_disabled",
       "Retitles or re-points a pin."},
      {"agentSpace.pinnedTabs.remove", "", kScopeNone, kUserSpaceWrite,
       kVerbPlain, kGuidRequired, false, false,
       &Call<&PhiControlDispatcher::VerbPinnedTabsRemove>,
       "invalid_payload unknown_pin user_space_operations_disabled",
       "Removes a pin."},

      // --- Rows and the emoji catalog ---------------------------------------
      {"agentSpace.rows.reorder", "", kScopeNone, kUserSpaceWrite, kVerbPlain,
       kRowsReorder, false, false,
       &Call<&PhiControlDispatcher::VerbRowsReorder>,
       "user_space_operations_disabled",
       "Moves a stored row within the sidebar."},
      {"agentSpace.emoji.groups", "", kScopeNone, kUserSpaceRead, kVerbPlain,
       {}, false, false, &Call<&PhiControlDispatcher::VerbEmojiGroups>,
       "user_space_operations_disabled", "The bundled emoji groups."},
      {"agentSpace.emoji.list", "", kScopeNone, kUserSpaceRead, kVerbPlain,
       kEmojiList, false, false,
       &Call<&PhiControlDispatcher::VerbEmojiList>,
       "user_space_operations_disabled", "Emoji from the catalog."},

      // --- Windows -----------------------------------------------------------
      {"agentSpace.newWindow", "", kScopeNone, kUserSpaceWrite, kVerbPlain, {},
       false, false, &Call<&PhiControlDispatcher::VerbNewWindow>,
       "user_space_operations_disabled",
       "Opens a user window in the first user window's profile."},
      {"agentSpace.close", "", kAny, kUserSpaceWrite,
       kWriteDefault | kVerbDestructiveOnUser, kWindowIdOnly, false, false,
       &Call<&PhiControlDispatcher::VerbClose>,
       "test_rig_only user_space_operations_disabled",
       "Closes a window — every Space browser its slot hosts, as its close "
       "box would."},
      {"agentSpace.navigate", "", kAny, kUserSpaceWrite, kTargetWrite,
       kNavigate, false, false, &Call<&PhiControlDispatcher::VerbNavigate>,
       "window_not_open user_space_operations_disabled",
       "Navigates the scope's active tab."},
      {"agentSpace.window.profile", "", kAny, kUserSpaceRead, kReadDefault,
       kScopeOnly, false, false,
       &Call<&PhiControlDispatcher::VerbWindowProfile>,
       "user_space_operations_disabled", "The window's profile."},
      {"agentSpace.window.layout", "", kAny, kUserSpaceRead, kReadDefault,
       kScopeOnly, false, false,
       &Call<&PhiControlDispatcher::VerbWindowLayout>,
       "user_space_operations_disabled",
       "The chrome geometry the shell is rendering, in physical pixels — so "
       "layout can be asserted without pixels."},
      {"agentSpace.window.bounds", "", kAny, kUserSpaceRead, kReadDefault,
       kScopeOnly, false, false,
       &Call<&PhiControlDispatcher::VerbWindowBounds>,
       "user_space_operations_disabled", "The shell window's bounds."},
      {"agentSpace.window.setBounds", "", kAny, kAppSettings, kReadDefault,
       kSetBounds, false, false,
       &Call<&PhiControlDispatcher::VerbWindowSetBounds>,
       "capability_required", "Moves and resizes the shell window."},
      {"agentSpace.window.handle", "", kScopeNone, kHost, kVerbPlain,
       kWindowIdOnly, false, false,
       &Call<&PhiControlDispatcher::VerbWindowHandle>, "test_rig_only",
       "The slot's shell HWND, for a capture tool."},
      {"agentSpace.menu.list", "", kAny, kUserSpaceRead, kReadDefault,
       kScopeOnly, false, false,
       &Call<&PhiControlDispatcher::VerbMenuList>,
       "user_space_operations_disabled",
       "The menu bar as the user would see it, dynamic rows included."},
      {"agentSpace.menu.click", "", kAny, kAppSettings, kWriteDefault,
       kMenuClick, false, false,
       &Call<&PhiControlDispatcher::VerbMenuClick>, "capability_required",
       "Activates a menu item by its title path; a disabled item refuses."},

      // --- Tabs ---------------------------------------------------------------
      {"agentSpace.tabs.list", "", kAny, kUserSpaceRead, kVerbPlain,
       kWindowIdOnly, false, false,
       &Call<&PhiControlDispatcher::VerbTabsList>,
       "user_space_operations_disabled", "The window's tabs."},
      {"agentSpace.tabs.new", "", kAny, kUserSpaceWrite, kTargetWrite,
       kTabsNew, false, false, &Call<&PhiControlDispatcher::VerbTabsNew>,
       "user_space_operations_disabled",
       "Opens a tab and answers with its strip index and id."},
      {"agentSpace.tabs.activate", "", kAny, kUserSpaceWrite, kTargetWrite,
       kTabByIndex, false, false,
       &Call<&PhiControlDispatcher::VerbTabsActivate>,
       "user_space_operations_disabled", "Selects the tab at an index."},
      {"agentSpace.tabs.close", "", kAny, kUserSpaceWrite,
       kTargetWrite | kVerbDestructiveOnUser, kTabByIndex, false, false,
       &Call<&PhiControlDispatcher::VerbTabsClose>,
       "test_rig_only user_space_operations_disabled",
       "Closes the tab at an index."},
      {"agentSpace.tabs.split", "", kAny, kUserSpaceWrite, kTargetWrite,
       kTabByIndex, false, false,
       &Call<&PhiControlDispatcher::VerbTabsSplit>,
       "user_space_operations_disabled",
       "Splits the active tab with the tab at an index."},
      {"agentSpace.tabs.unsplit", "", kAny, kUserSpaceWrite, kTargetWrite,
       kTabByIndex, false, false,
       &Call<&PhiControlDispatcher::VerbTabsUnsplit>,
       "user_space_operations_disabled", "Unsplits the tab at an index."},
      {"agentSpace.tabs.move", "", kAny, kUserSpaceWrite, kTargetWrite,
       kTabsMove, false, false, &Call<&PhiControlDispatcher::VerbTabsMove>,
       "user_space_operations_disabled",
       "Moves a tab to a final index; a split member is refused rather than "
       "dragging its pair along."},
      {"agentSpace.tabs.group", "", kAny, kUserSpaceWrite, kTargetWrite,
       kTabByIndex, false, false,
       &Call<&PhiControlDispatcher::VerbTabsGroup>,
       "user_space_operations_disabled",
       "Puts the tab at an index in a new group."},
      {"agentSpace.tabs.ungroup", "", kAny, kUserSpaceWrite, kTargetWrite,
       kTabByIndex, false, false,
       &Call<&PhiControlDispatcher::VerbTabsUngroup>,
       "user_space_operations_disabled",
       "Takes the tab at an index out of its group."},
      {"agentSpace.tabs.storeSplit", "", kAny, kUserSpaceWrite, kTargetWrite,
       kTabsStoreSplit, false, false,
       &Call<&PhiControlDispatcher::VerbTabsStoreSplit>,
       "user_space_operations_disabled",
       "Stores a live split as a pinned pair or a split bookmark."},

      // --- Navigation ----------------------------------------------------------
      {"agentSpace.nav.state", "", kAny, kUserSpaceRead, kVerbPlain,
       kWindowIdOnly, false, false,
       &Call<&PhiControlDispatcher::VerbNavState>,
       "user_space_operations_disabled",
       "The window's url, title, loading and back/forward state."},
      {"agentSpace.nav.back", "", kAny, kUserSpaceWrite, kTargetWrite,
       kWindowIdOnly, false, false,
       &Call<&PhiControlDispatcher::VerbNavBack>,
       "user_space_operations_disabled", "Goes back."},
      {"agentSpace.nav.forward", "", kAny, kUserSpaceWrite, kTargetWrite,
       kWindowIdOnly, false, false,
       &Call<&PhiControlDispatcher::VerbNavForward>,
       "user_space_operations_disabled", "Goes forward."},
      {"agentSpace.nav.reload", "", kAny, kUserSpaceWrite, kTargetWrite,
       kNavReload, false, false,
       &Call<&PhiControlDispatcher::VerbNavReload>,
       "user_space_operations_disabled", "Reloads."},
      {"agentSpace.nav.stop", "", kAny, kUserSpaceWrite, kTargetWrite,
       kWindowIdOnly, false, false,
       &Call<&PhiControlDispatcher::VerbNavStop>,
       "user_space_operations_disabled", "Stops loading."},

      // --- Tab groups (the client dialect: groupId + strip indices) ------------
      {"agentSpace.groups.list", "", kAny, kUserSpaceRead, kVerbPlain,
       kWindowIdOnly, false, false,
       &Call<&PhiControlDispatcher::VerbGroupsList>,
       "user_space_operations_disabled", "The window's tab groups."},
      {"agentSpace.groups.rename", "", kAny, kUserSpaceWrite, kTargetWrite,
       kGroupOp, false, false,
       &Call<&PhiControlDispatcher::VerbGroupsRename>,
       "user_space_operations_disabled", "Renames a group."},
      {"agentSpace.groups.color", "", kAny, kUserSpaceWrite, kTargetWrite,
       kGroupOp, false, false, &Call<&PhiControlDispatcher::VerbGroupsColor>,
       "user_space_operations_disabled", "Recolours a group."},
      {"agentSpace.groups.collapse", "", kAny, kUserSpaceWrite, kTargetWrite,
       kGroupOp, false, false,
       &Call<&PhiControlDispatcher::VerbGroupsCollapse>,
       "user_space_operations_disabled", "Collapses or expands a group."},
      {"agentSpace.groups.ungroup", "", kAny, kUserSpaceWrite, kTargetWrite,
       kGroupOp, false, false,
       &Call<&PhiControlDispatcher::VerbGroupsUngroup>,
       "user_space_operations_disabled", "Ungroups a group's tabs."},
      {"agentSpace.groups.close", "", kAny, kUserSpaceWrite,
       kTargetWrite | kVerbDestructiveOnUser, kGroupOp, false, false,
       &Call<&PhiControlDispatcher::VerbGroupsClose>,
       "test_rig_only user_space_operations_disabled",
       "Closes a group and its tabs."},

      // --- Tab groups (the skill dialect: token + stable tab ids) --------------
      {"agentSpace.tabGroups.list", "", kAny, kUserSpaceRead, kVerbPlain,
       kScopeOnly, false, false,
       &Call<&PhiControlDispatcher::VerbTabGroupsList>,
       "space_not_open window_not_open user_space_operations_disabled",
       "The scope's tab groups, with their member tab ids."},
      {"agentSpace.tabGroups.create", "", kAny, kUserSpaceWrite, kTargetWrite,
       kTabGroupsCreate, false, false,
       &Call<&PhiControlDispatcher::VerbTabGroupsCreate>,
       "space_not_open create_failed user_space_operations_disabled",
       "Groups tabs by id and answers with the group token."},
      {"agentSpace.tabGroups.update", "", kAny, kUserSpaceWrite, kTargetWrite,
       kTabGroupsUpdate, false, false,
       &Call<&PhiControlDispatcher::VerbTabGroupsUpdate>,
       "invalid_payload space_not_open user_space_operations_disabled",
       "Retitles, recolours or collapses a group."},
      {"agentSpace.tabGroups.addTabs", "", kAny, kUserSpaceWrite, kTargetWrite,
       kTabGroupsAddTabs, false, false,
       &Call<&PhiControlDispatcher::VerbTabGroupsAddTabs>,
       "invalid_payload space_not_open user_space_operations_disabled",
       "Adds tabs to a group."},
      {"agentSpace.tabGroups.removeTabs", "", kAny, kUserSpaceWrite,
       kTargetWrite, kTabGroupsRemoveTabs, false, false,
       &Call<&PhiControlDispatcher::VerbTabGroupsRemoveTabs>,
       "space_not_open user_space_operations_disabled",
       "Takes tabs out of their groups."},
      {"agentSpace.tabGroups.ungroup", "", kAny, kUserSpaceWrite, kTargetWrite,
       kTabGroupsToken, false, false,
       &Call<&PhiControlDispatcher::VerbTabGroupsUngroup>,
       "invalid_payload space_not_open user_space_operations_disabled",
       "Ungroups a group."},
      {"agentSpace.tabGroups.close", "", kAny, kUserSpaceWrite,
       kTargetWrite | kVerbDestructiveOnUser, kTabGroupsToken, false, false,
       &Call<&PhiControlDispatcher::VerbTabGroupsClose>,
       "invalid_payload space_not_open test_rig_only "
       "user_space_operations_disabled",
       "Closes a group and its tabs."},

      // --- Split view ----------------------------------------------------------
      {"agentSpace.splitView.list", "", kAny, kUserSpaceRead, kVerbPlain,
       kScopeOnly, false, false,
       &Call<&PhiControlDispatcher::VerbSplitViewList>,
       "space_not_open window_not_open user_space_operations_disabled",
       "The scope's splits."},
      {"agentSpace.splitView.create", "", kAny, kUserSpaceWrite, kTargetWrite,
       kSplitCreate, false, false,
       &Call<&PhiControlDispatcher::VerbSplitViewCreate>,
       "space_not_open create_failed user_space_operations_disabled",
       "Splits two tabs and answers with the split id."},
      {"agentSpace.splitView.update", "", kAny, kUserSpaceWrite, kTargetWrite,
       kSplitUpdate, false, false,
       &Call<&PhiControlDispatcher::VerbSplitViewUpdate>,
       "invalid_payload space_not_open user_space_operations_disabled",
       "Changes a split's axis or divider ratio."},
      {"agentSpace.splitView.swap", "", kAny, kUserSpaceWrite, kTargetWrite,
       kSplitId, false, false,
       &Call<&PhiControlDispatcher::VerbSplitViewSwap>,
       "invalid_payload space_not_open user_space_operations_disabled",
       "Swaps a split's two members."},
      {"agentSpace.splitView.remove", "", kAny, kUserSpaceWrite, kTargetWrite,
       kSplitId, false, false,
       &Call<&PhiControlDispatcher::VerbSplitViewRemove>,
       "invalid_payload space_not_open user_space_operations_disabled",
       "Dissolves a split."},

      // --- Downloads (observe and control only — never open) -------------------
      {"agentSpace.downloads.list", "", kAny, kUserSpaceRead, kVerbPlain,
       kScopeOnly, false, false,
       &Call<&PhiControlDispatcher::VerbDownloadsList>,
       "space_not_open window_not_open user_space_operations_disabled",
       "The scope's downloads."},
      {"agentSpace.downloads.get", "", kAny, kUserSpaceRead, kVerbPlain,
       kDownloadGuid, false, false,
       &Call<&PhiControlDispatcher::VerbDownloadsGet>,
       "invalid_payload unknown_download space_not_open "
       "user_space_operations_disabled",
       "One download."},
      {"agentSpace.downloads.pause", "", kAny, kUserSpaceWrite, kVerbPlain,
       kDownloadGuid, false, false,
       &Call<&PhiControlDispatcher::VerbDownloadsPause>,
       "invalid_payload unknown_download space_not_open "
       "user_space_operations_disabled",
       "Pauses a download."},
      {"agentSpace.downloads.resume", "", kAny, kUserSpaceWrite, kVerbPlain,
       kDownloadGuid, false, false,
       &Call<&PhiControlDispatcher::VerbDownloadsResume>,
       "invalid_payload unknown_download space_not_open "
       "user_space_operations_disabled",
       "Resumes a download."},
      {"agentSpace.downloads.cancel", "", kAny, kUserSpaceWrite, kVerbPlain,
       kDownloadGuid, false, false,
       &Call<&PhiControlDispatcher::VerbDownloadsCancel>,
       "invalid_payload unknown_download space_not_open "
       "user_space_operations_disabled",
       "Cancels a download."},
      {"agentSpace.downloads.remove", "", kAny, kUserSpaceWrite, kVerbPlain,
       kDownloadGuid, false, false,
       &Call<&PhiControlDispatcher::VerbDownloadsRemove>,
       "invalid_payload unknown_download space_not_open "
       "user_space_operations_disabled",
       "Removes a download from the list."},

      // --- App-wide looks and settings ------------------------------------------
      {"agentSpace.layout.get", "", kScopeNone, kUserSpaceRead, kVerbPlain, {},
       false, false, &Call<&PhiControlDispatcher::VerbLayoutGet>,
       "user_space_operations_disabled", "The layout mode."},
      {"agentSpace.layout.set", "", kScopeNone, kAppSettings, kVerbPlain,
       kLayoutSet, false, false,
       &Call<&PhiControlDispatcher::VerbLayoutSet>, "capability_required",
       "Sets the layout mode and sidebar width."},
      {"agentSpace.theme.get", "", kScopeNone, kUserSpaceRead, kVerbPlain, {},
       false, false, &Call<&PhiControlDispatcher::VerbThemeGet>,
       "user_space_operations_disabled",
       "The theme, the appearance and the built-in theme list."},
      {"agentSpace.theme.set", "", kScopeNone, kAppSettings, kVerbPlain,
       kThemeSet, false, false, &Call<&PhiControlDispatcher::VerbThemeSet>,
       "capability_required", "Sets the theme or the appearance."},
      {"agentSpace.settings.get", "", kScopeNone, kUserSpaceRead, kVerbPlain,
       kSettingsGet, false, false,
       &Call<&PhiControlDispatcher::VerbSettingsGet>,
       "user_space_operations_disabled",
       "Everything the settings window shows, in one read."},
      {"agentSpace.settings.set", "", kScopeNone, kAppSettings, kVerbPlain,
       kSettingsSet, false, false,
       &Call<&PhiControlDispatcher::VerbSettingsSet>,
       "capability_required pref_not_agent_settable",
       "Changes app settings. developerMode / allowAgentUserSpaces / testRig "
       "/ analyticsEnabled are never an agent's to set — a payload carrying "
       "one is refused whole."},
      {"agentSpace.settings.open", "", kScopeNone, kAppSettings, kVerbPlain,
       {}, false, false, &Call<&PhiControlDispatcher::VerbSettingsOpen>,
       "capability_required", "Opens the settings window."},

      // --- Feedback --------------------------------------------------------------
      {"agentSpace.feedback.status", "", kScopeNone, kAppSettings, kVerbPlain,
       {}, false, false, &Call<&PhiControlDispatcher::VerbFeedbackStatus>,
       "capability_required", "The outbox and the open form."},
      {"agentSpace.feedback.open", "", kAny, kAppSettings, kReadDefault,
       kWindowIdOnly, false, false,
       &Call<&PhiControlDispatcher::VerbFeedbackOpen>, "capability_required",
       "Opens the feedback form, as Help ▸ Report an Issue does."},
      {"agentSpace.feedback.form", "", kScopeNone, kAppSettings, kVerbPlain,
       kFeedbackForm, false, false,
       &Call<&PhiControlDispatcher::VerbFeedbackForm>, "capability_required",
       "Drives the open form, Send included."},
      {"agentSpace.feedback.setAccount", "", kScopeNone, kHost, kVerbPlain,
       kFeedbackAccount, false, false,
       &Call<&PhiControlDispatcher::VerbFeedbackSetAccount>, "test_rig_only",
       "Stands a signed-in account in on a box without one."},

      // --- Account (the Auth0 session; docs/login-port.md) ------------------------
      {"agentSpace.account.status", "", kScopeNone, kHost, kVerbPlain, {},
       false, false, &Call<&PhiControlDispatcher::VerbAccountStatus>,
       "test_rig_only",
       "The session: signedIn, signingIn, reauthRequired, the identity, the "
       "cached profile and the last error."},
      {"agentSpace.account.signIn", "", kScopeNone, kHost, kVerbPlain, {},
       false, false, &Call<&PhiControlDispatcher::VerbAccountSignIn>,
       "test_rig_only window_not_open",
       "Starts the hosted sign-in in the first user window, as Settings ▸ "
       "You ▸ Sign in does."},
      {"agentSpace.account.cancelSignIn", "", kScopeNone, kHost, kVerbPlain,
       {}, false, false, &Call<&PhiControlDispatcher::VerbAccountCancelSignIn>,
       "test_rig_only", "Abandons the sign-in in flight."},
      {"agentSpace.account.signOut", "", kScopeNone, kHost, kVerbPlain, {},
       false, false, &Call<&PhiControlDispatcher::VerbAccountSignOut>,
       "test_rig_only", "Signs out without the pane's confirmation."},
      {"agentSpace.onboarding.status", "", kScopeNone, kHost, kVerbPlain, {},
       false, false, &Call<&PhiControlDispatcher::VerbOnboardingStatus>,
       "test_rig_only", "The onboarding gate and the window's page."},
      {"agentSpace.onboarding.drive", "", kScopeNone, kHost, kVerbPlain,
       kOnboardingDrive, false, false,
       &Call<&PhiControlDispatcher::VerbOnboardingDrive>,
       "test_rig_only not_open wrong_page unknown_action",
       "Presses a control of the onboarding window by name."},
      // --- Chromium sync (docs/user-data-parity.md S2) ----------------------------
      {"agentSpace.sync.status", "", kScopeNone, kHost, kVerbPlain, {}, false,
       false, &Call<&PhiControlDispatcher::VerbSyncStatus>, "test_rig_only",
       "What Chromium sync is told: the account state, the server, whether a "
       "bearer token is at hand, and which profiles have a key."},
      {"agentSpace.sync.setProfileKey", "", kScopeNone, kHost, kVerbPlain,
       kSyncProfileKey, false, false,
       &Call<&PhiControlDispatcher::VerbSyncSetProfileKey>,
       "test_rig_only invalid_payload",
       "Hands Chromium a profile's sync key by hand, standing in for the key "
       "layer."},
      {"agentSpace.sync.setup.open", "", kScopeNone, kHost, kVerbPlain, {},
       false, false, &Call<&PhiControlDispatcher::VerbSyncSetupOpen>,
       "test_rig_only no_key_layer",
       "Opens the sync setup window, as Settings ▸ Sync's button does."},
      {"agentSpace.sync.setup.status", "", kScopeNone, kHost, kVerbPlain, {},
       false, false, &Call<&PhiControlDispatcher::VerbSyncSetupStatus>,
       "test_rig_only not_open",
       "The sync setup window's page and state."},
      {"agentSpace.sync.setup.drive", "", kScopeNone, kHost, kVerbPlain,
       kSyncSetupDrive, false, false,
       &Call<&PhiControlDispatcher::VerbSyncSetupDrive>,
       "test_rig_only not_open wrong_page unknown_action invalid_value busy "
       "undecided",
       "Presses a control of the sync setup window by name."},
      // --- Import from another browser (phi_import_window.h) ---------------------
      {"agentSpace.import.open", "", kScopeNone, kHost, kVerbPlain, {}, false,
       false, &Call<&PhiControlDispatcher::VerbImportOpen>,
       "test_rig_only not_open",
       "Opens the import window for the focused window's Space, as the app "
       "menu's \"Import from another browser…\" does."},
      {"agentSpace.import.status", "", kScopeNone, kHost, kVerbPlain, {},
       false, false, &Call<&PhiControlDispatcher::VerbImportStatus>,
       "test_rig_only not_open",
       "The import window's rows, selections, phase and status line, and "
       "whether an import is running."},
      {"agentSpace.import.drive", "", kScopeNone, kHost, kVerbPlain,
       kImportDrive, false, false,
       &Call<&PhiControlDispatcher::VerbImportDrive>,
       "test_rig_only not_open wrong_phase unknown_action invalid_value "
       "nothing_selected",
       "Presses a control of the import window by name."},
      {"agentSpace.sync.keyOp", "", kScopeNone, kHost, kVerbPlain, kSyncKeyOp,
       false, false, &Call<&PhiControlDispatcher::VerbSyncKeyOp>,
       "test_rig_only invalid_payload no_key_layer",
       "Starts one sync key-layer operation (the setup window's and Settings ▸ "
       "Sync's steps); agentSpace.sync.status's lastOp reports the outcome."},

      // --- Credentials -------------------------------------------------------------
      {"credentials.status", "", kScopeNone, kAgentSpace, kVerbPlain, {},
       false, false, &Call<&PhiControlDispatcher::VerbCredentialsStatus>,
       "not_installed", "Whether the vault is there, unlocked and ready."},
      {"credentials.get", "", kScopeNone, kCredentialReveal, kVerbPlain,
       kCredentialsGet, true, false,
       &Call<&PhiControlDispatcher::VerbCredentialsGet>,
       "invalid_payload fill_requires_autofill not_installed not_ready "
       "not_found ambiguous provider_error user_denied capability_required",
       "Reveals a credential TO the agent, behind the user's approval."},
      {"credentials.getTotp", "", kScopeNone, kCredentialReveal, kVerbPlain,
       {}, false, false,
       &Call<&PhiControlDispatcher::VerbCredentialsGetTotp>,
       "totp_not_supported",
       "Withdrawn: a second factor handed to an agent that already holds the "
       "first is worth nothing."},
      {"credentials.autofill", "", kScopeNone, kCredentialFill, kVerbPlain,
       kCredentialsAutofill, true, false,
       &Call<&PhiControlDispatcher::VerbCredentialsAutofill>,
       "invalid_payload not_installed not_ready not_found ambiguous "
       "provider_error not_a_login field_unavailable origin_mismatch "
       "origin_changed user_denied target_not_found page_not_found "
       "autofill_not_available capability_required scope_not_allowed",
       "Fills a tagged field on a page — the secret goes vault → framework → "
       "DOM and never to the agent."},

      // --- Notification cards ------------------------------------------------
      {"notification.show", "", kScopeNone, kAgentSpace, kVerbPlain,
       kNotificationShow, false, false,
       &Call<&PhiControlDispatcher::VerbNotificationShow>,
       "invalid_params agent_session_required",
       "Shows the user a card (title, message, one button) without taking "
       "focus; answers {notificationId} on queue insertion. The decision — "
       "accept (the button), reject (the close button) or timeout (expiry, "
       "or eviction from the shared five-card queue) — arrives later as a "
       "notification.response event {notificationId, decision} on this "
       "agent session's live connections only; hiding the stack decides "
       "nothing. Subscribe before sending."},
  };
  return base::span<const PhiVerbSpec>(kTable);
}

}  // namespace phi_client
