#pragma once
// ============================================================
// ShieldCord — ipc_protocol.h
//
// THE IPC CONTRACT — single source of truth for the named-pipe
// protocol. Shared by three parties:
//
//   * the engine      C++  src/service/  (service + console mode)
//   * shieldcord_ctl  C++  src/ctl/
//   * shieldcordui    C#   src/ui/Protocol.cs   <-- MIRROR
//
// If you change ANYTHING here, change src/ui/Protocol.cs to match.
// The two are kept in sync by hand; there is no code generator.
//
// ------------------------------------------------------------
// TRANSPORT
// ------------------------------------------------------------
// Pipe:     \\.\pipe\ShieldCord          (SC_PIPE_NAME, shared/common.h)
// Encoding: newline-delimited JSON, UTF-8, one message per line.
// Max line: 4096 bytes. A client that sends more is disconnected.
//
// ------------------------------------------------------------
// CORRELATION (important)
// ------------------------------------------------------------
// The pipe is DUPLEX and unsolicited: the server broadcasts `alert`
// messages to every connected client at any moment, interleaved with
// replies to that client's own requests. A client therefore CANNOT
// assume the next line it reads is the answer to what it just sent.
//
// Every request MAY carry an "id" (any JSON scalar; a counter is
// conventional). Every reply ECHOES that id verbatim. Clients should
// match on the echoed id, and treat any line whose type is `alert`
// as an out-of-band event regardless of id.
//
// A request without an "id" still gets a reply (with no id echoed) —
// acceptable for one-shot CLI use, not for the UI.
//
// ------------------------------------------------------------
// AUTHORIZATION
// ------------------------------------------------------------
// The pipe ACL grants Authenticated Users read+write so the
// non-elevated tray UI can render status and receive alerts.
// Authorization is therefore NOT the pipe ACL — it is per-verb:
// every state-changing verb is rejected with
// {"type":"error","error":"not_authorized"} unless the CALLING
// PROCESS TOKEN is elevated. See ClientIsElevated in ipc_server.cpp.
//
// Read verbs  (no elevation): get_status, get_config, get_alerts,
//                             get_protected_apps, get_protected_paths
// Write verbs (elevation):    set_enforcement, set_features, set_decoy,
//                             set_memory_monitor, kill_process,
//                             trust_publisher, untrust_publisher,
//                             shutdown
//
// ------------------------------------------------------------
// EXAMPLES
// ------------------------------------------------------------
// Client -> Server:
//   {"id":7,"type":"get_status"}
//   {"id":8,"type":"set_enforcement","enabled":false}
//   {"id":9,"type":"get_alerts","since":0}
//   {"id":10,"type":"kill_process","pid":4521}
//   {"id":11,"type":"trust_publisher","publisher":"Some Vendor Ltd"}
//
// Server -> Client (reply):
//   {"id":7,"type":"status_update","driver_connected":true, ... }
//   {"id":8,"type":"ok","enforcement":false}
//   {"id":9,"type":"alerts","records":[{...}, {...}]}
//   {"id":11,"type":"error","error":"not_authorized"}
//
// Server -> Client (unsolicited, never carries an id):
//   {"type":"alert","id":42,"time":"2026-09-11T14:03:22","severity":"high",
//    "source":"driver","pid":4521,"process_name":"crabber.exe",
//    "publisher":"","path":"\\Device\\HarddiskVolume3\\...\\leveldb",
//    "action":"blocked","message":"Kernel blocked an untrusted process ..."}
//
// NOTE the collision: the alert's "id" is the RECORD's monotonic id
// (for get_alerts pagination), NOT a correlation echo. That is why
// unsolicited alerts never echo a request id — a client telling them
// apart must not rely on "id" alone, but on "type".
// ============================================================

#include <string>

namespace sc::ipc {

// ─── Request verbs (client → server) ─────────────────────────
constexpr auto MSG_GET_STATUS          = "get_status";
constexpr auto MSG_GET_CONFIG          = "get_config";
constexpr auto MSG_GET_ALERTS          = "get_alerts";
constexpr auto MSG_GET_PROTECTED_APPS  = "get_protected_apps";
constexpr auto MSG_GET_PROTECTED_PATHS = "get_protected_paths";

constexpr auto MSG_SET_ENFORCEMENT     = "set_enforcement";
constexpr auto MSG_SET_FEATURES        = "set_features";
constexpr auto MSG_SET_DECOY           = "set_decoy";
constexpr auto MSG_SET_MEMORY_MONITOR  = "set_memory_monitor";
constexpr auto MSG_KILL_PROCESS        = "kill_process";
constexpr auto MSG_TRUST_PUBLISHER     = "trust_publisher";
constexpr auto MSG_UNTRUST_PUBLISHER   = "untrust_publisher";
constexpr auto MSG_SHUTDOWN            = "shutdown";

// Re-load and re-arm the kernel minifilter. Reply: {"type":"ok",
// "driver_connected":bool} — an "ok" whose OUTCOME is reported in that field,
// the same shape as kill_result, because "the request was accepted" and "the
// driver is now protecting this machine" are different claims.
//
// Exists because the engine brings the driver up exactly once, at startup. The
// app installs the driver while the engine is already running, so without this
// the driver would be registered and loaded but never armed — fail-open, with
// the status reply still reporting it connected.
//
// Elevation-gated like every other write verb: an unprivileged caller must not
// be able to make the engine re-read its trust list.
constexpr auto MSG_RECONNECT_DRIVER    = "reconnect_driver";

// ─── Reply / event types (server → client) ───────────────────
constexpr auto MSG_STATUS_UPDATE       = "status_update";
constexpr auto MSG_CONFIG              = "config";
constexpr auto MSG_ALERTS              = "alerts";
constexpr auto MSG_PROTECTED_APPS      = "protected_apps";
constexpr auto MSG_PROTECTED_PATHS     = "protected_paths";
constexpr auto MSG_KILL_RESULT         = "kill_result";
constexpr auto MSG_OK                  = "ok";
constexpr auto MSG_ERROR               = "error";
constexpr auto MSG_ALERT               = "alert";

// ─── Error codes (value of the "error" field) ────────────────
constexpr auto ERR_NOT_AUTHORIZED      = "not_authorized";  // caller not elevated
constexpr auto ERR_UNKNOWN_TYPE        = "unknown_type";
constexpr auto ERR_PARSE_ERROR         = "parse_error";
constexpr auto ERR_BAD_REQUEST         = "bad_request";     // missing/!valid field

// ─── Alert severity values ───────────────────────────────────
constexpr auto SEV_INFO                = "info";
constexpr auto SEV_WARN                = "warning";
constexpr auto SEV_HIGH                = "high";

// ─── Alert source values ("source" field) ────────────────────
constexpr auto SOURCE_DRIVER           = "driver";   // minifilter blocked a file open
constexpr auto SOURCE_MEMORY           = "memory";   // memory monitor saw a VM_READ

// ─── Alert action values ("action" field) ────────────────────
constexpr auto ACTION_BLOCKED          = "blocked";        // file open denied
constexpr auto ACTION_KILLED           = "killed";         // attacker terminated
constexpr auto ACTION_KILL_FAILED      = "kill_failed";    // tried, could not
constexpr auto ACTION_DETECTED_ONLY    = "detected_only";  // kill disabled (console)

// ─── Engine mode values ("service_mode" field) ───────────────
// WIDE literals: these are assigned to IpcContext::mode (const wchar_t*) and
// narrowed only when they go on the wire.
constexpr auto MODE_SERVICE            = L"service";
constexpr auto MODE_CONSOLE            = L"console";

// ─── JSON field names ────────────────────────────────────────
// Named constants so a typo is a compile error in C++ (and a
// nameof-style constant in the C# mirror) rather than a silently
// missing field the other side reads as default.
namespace field {
constexpr auto ID            = "id";
constexpr auto TYPE          = "type";
// Named ERR, not ERROR: <wingdi.h> defines ERROR as a macro, so `field::ERROR`
// would macro-expand into `field::0` and fail to compile.
constexpr auto ERR           = "error";
constexpr auto ENABLED       = "enabled";
constexpr auto FILE_BLOCK    = "file_block";
constexpr auto ALERTS        = "alerts";
constexpr auto SINCE         = "since";
constexpr auto RECORDS       = "records";
constexpr auto PID           = "pid";
constexpr auto PUBLISHER     = "publisher";
constexpr auto SUCCESS       = "success";
} // namespace field

} // namespace sc::ipc
