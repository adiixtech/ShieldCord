#pragma once
// ============================================================
// ShieldCord — ipc_verbs.h
//
// ONE implementation of every named-pipe verb, shared by the installed
// service (service_controller.cpp) and the console engine (main.cpp).
//
// These used to be two copy-pasted switch statements, which had already
// drifted (they reported different version strings and different status
// fields). Anything that differs between the two engines belongs in
// IpcContext; everything else is identical by construction.
//
// The wire contract lives in shared/ipc_protocol.h — read that first.
// ============================================================
#include "../shared/common.h"
#include "../driver/filter/driver_protocol.h"   // SC_BLOCK_ALERT
#include "alert_history.h"      // AlertRecord — PublishAlert takes one by value
#include <functional>
#include <string>

namespace sc {

// The parts of verb handling that genuinely differ between engines.
struct IpcContext {
    // Reported to the UI as "service_mode". ipc::MODE_SERVICE / MODE_CONSOLE.
    const wchar_t* mode = L"service";

    // Signalled by the "shutdown" verb. The engine performs its own teardown
    // on its own thread — the verb handler must NEVER do it inline, because
    // IpcServer::Stop() joins the client thread this handler is running on.
    std::function<void()> requestStop;

    // Run IMMEDIATELY BEFORE requestStop, on the same client thread.
    //
    // A stop the user asked for has to be distinguishable from a crash, and the
    // one thing that cannot tell those apart is the SCM's failure-recovery
    // policy — it restarts either. The service uses this hook to clear that
    // policy before teardown even begins, so a deliberate stop cannot bounce. A
    // crash never reaches here, so recovery still does the job it exists for.
    // The console engine leaves it null: there is no service to reconfigure.
    std::function<void()> onDeliberateStop;

    // Called after a verb changes a persisted protection setting. The engine
    // reconciles the consequences that the verb itself should not know about
    // (create/remove the decoy folder, start/stop the memory monitor).
    std::function<void()> onSettingsChanged;

    // Loads and arms the kernel minifilter, attaching this engine's own alert
    // handler. Returns true when the driver is connected and armed.
    //
    // Supplied by the engine rather than called from here because the alert
    // handler belongs to the engine that owns the console output — see
    // BringUpDriver in service_controller.h. The "reconnect_driver" verb calls
    // it after the driver has been installed while the engine was already
    // running, which is the one case where the startup bring-up is not enough.
    //
    // Null in an engine that has no driver stream (the DACL-only --test mode).
    std::function<bool()> onReconnectDriver;

    // Captured when the engine builds its context, so "uptime_seconds" is
    // measured from engine start. GetTickCount64 is milliseconds since boot.
    unsigned long long startTick = GetTickCount64();
};

// Handle one newline-delimited JSON request and return the reply line
// (no trailing newline). Never throws; a malformed request yields a
// {"type":"error","error":"parse_error"} reply.
//
// 'privileged' is the caller's elevation, decided by IpcServer::ClientLoop.
// The handler is the ONLY place that enforces it — the pipe ACL deliberately
// allows non-elevated clients so the tray UI can read status.
std::wstring HandleIpcVerb(const std::wstring& json, bool privileged, IpcContext& ctx);

// Serialize an alert record into the broadcast shape and push it to every
// connected client. Also records it in AlertHistory (assigning its id).
// Both engines' alert sources funnel through here.
void PublishAlert(AlertRecord rec);

// Best-effort recursive delete, used to remove the decoy token tree when the
// user disables it. The service is trusted by the driver (TrustSelf), so it
// is allowed to touch the tree the driver protects. Returns true when the
// path no longer exists afterwards.
bool RemoveDecoyTree(const std::wstring& path);

// Handle a kernel block alert: repair the trust race, then publish the result
// only if it did NOT resolve. Shared by both engines so a block is treated
// identically no matter which one is running.
//
// A race we repair is caused by our own verification latency, costs the app
// nothing permanent, and arrives several times per browser restart — publishing
// it filled the timeline with entries that all said "nothing is still blocked".
// It is logged instead. A block we cannot repair is the real thing and is
// published as a high-severity alert.
//
// This is the only route from a kernel block to the UI: the record builder is
// static, so a caller cannot bypass the heal step.
void PublishDriverBlock(const SC_BLOCK_ALERT& alert);

// Trust-race repair. If this PID belongs to something we consider trusted —
// a built-in whitelist app, or a binary signed by a publisher the user added
// at runtime — tell the driver to trust it now and return true.
//
// Called from the block-alert handlers: a legitimate app can open its own
// storage in the window between its process starting and our trust message
// landing. Without this it would be denied by default-deny, which is exactly
// the "Discord logged me out" failure mode we must never ship.
bool SelfHealTrustedProcess(DWORD pid);

} // namespace sc
