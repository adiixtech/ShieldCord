#pragma once
// ============================================================
// ShieldCord — service_controller.h
// Windows Service SCM integration
// ============================================================
#include "../shared/common.h"
#include "../driver/filter/driver_protocol.h"   // SC_BLOCK_ALERT
#include <functional>

namespace sc {

class ServiceController {
public:
    // Entry point called by SCM via StartServiceCtrlDispatcher
    static void WINAPI ServiceMain(DWORD argc, LPWSTR* argv);

    // SCM control handler (stop, pause, etc.)
    static DWORD WINAPI ControlHandler(DWORD control,
                                      DWORD eventType,
                                      LPVOID eventData,
                                      LPVOID context);

    // Register and start the service (called from installer)
    static bool InstallService(const std::wstring& exePath);

    // Remove the service (called from uninstaller)
    static bool UninstallService();

    static SERVICE_STATUS_HANDLE s_hStatus;
    static SERVICE_STATUS        s_status;

private:
    static void SetStatus(DWORD currentState, DWORD exitCode = NO_ERROR,
                          DWORD waitHint = 0);
};

// Entry point: call from main() to dispatch to SCM
void RunService();

// Loads the ShieldCordFilter kernel minifilter (a DEMAND-start driver service)
// so protection is active without re-running install_driver.bat after a reboot.
// Call from the service (SYSTEM) or an elevated console right before
// DriverClient::Connect(). Safe to call if already running; non-fatal if the
// driver is not installed (callers fall back to DACL-only mode).
bool EnsureKernelFilterLoaded();

// Brings the minifilter up and ARMS it — the whole "make the driver live"
// sequence, in the order that keeps default-deny from biting a legitimate app:
// trust the apps already running plus ShieldCord itself, push the protected
// path list, apply the feature flags, and turn enforcement on LAST.
//
// Called by both engines at startup, and again by the "reconnect_driver" verb
// after the driver has been installed while the engine was already running. The
// re-arm matters: the engine only ever connects once at startup, so a driver
// installed later would otherwise be registered, loaded, and enforcing nothing
// — a fail-open driver the status reply would still call "connected".
//
// 'alertHandler' receives kernel block alerts. It is attached only on the
// connect path, so re-arming an already-connected driver does not start a
// second receiver thread.
//
// Returns false when the driver is not available; the engine then runs without
// file protection, which is a state callers must report rather than paper over.
bool BringUpDriver(std::function<void(const SC_BLOCK_ALERT&)> alertHandler);

// Re-pushes trust, the protected paths, the feature flags and enforcement, and
// nothing else. No-op returning false when the driver is not connected.
//
// Separate from BringUpDriver because "connected but unarmed" is its own state:
// it is what a fresh install leaves behind, and re-arming is the fix.
bool ArmDriver();

} // namespace sc
