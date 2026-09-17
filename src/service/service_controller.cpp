// ============================================================
// ShieldCord — service_controller.cpp
// Windows Service lifecycle: start, stop, SCM integration,
// and orchestration of all protection modules.
// ============================================================
#include "service_controller.h"
#include "logger.h"
#include "config.h"
#include "dacl_gatekeeper.h"
#include "app_watcher.h"
#include "memory_monitor.h"
#include "token_vault.h"
#include "ipc_server.h"
#include "ipc_verbs.h"
#include "driver_client.h"
#include "process_validator.h"
#include "utils.h"
#include "../shared/common.h"
#include "../shared/ipc_protocol.h"
#include "../shared/token_paths.h"
#include <nlohmann/json.hpp>
#include <atomic>
#include <sstream>
#include <cwchar>
#include <cstdlib>    // _countof

using json = nlohmann::json;

namespace sc {

SERVICE_STATUS_HANDLE ServiceController::s_hStatus = nullptr;
SERVICE_STATUS        ServiceController::s_status  = {};

// Manual-reset event signaled to request a clean stop. Created in ServiceMain
// BEFORE the control handler is registered so an early control can't race it.
// The SCM control handler and the IPC "shutdown" verb both just signal this;
// the actual (slow) teardown runs on the ServiceMain thread, never inside the
// control-handler callback (which must return promptly).
static HANDLE g_stopEvent = nullptr;

static void RequestStop() {
    if (g_stopEvent) SetEvent(g_stopEvent);
}

// -------------------------------------------------------
// SCM crash-recovery policy.
//
// The installer arms failure recovery so a CRASHING engine comes back — worth
// keeping, because this tool assumes the machine may already be infected and
// silently staying dead is a protection regression.
//
// But that policy cannot tell a crash from a stop the user asked for: it
// restarts either, which is why "Stop the engine" used to bring the service
// back a few seconds later. So a deliberate stop CLEARS the policy before it
// begins tearing down, and every real start puts it back. A deliberate stop can
// therefore never bounce even if teardown faults; a crash still recovers,
// because a crash is precisely the case that never ran the clear.
// -------------------------------------------------------
static void SetRecoveryArmed(bool armed) {
    SC_HANDLE hScm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!hScm) return;

    // SERVICE_CHANGE_CONFIG on ourselves: the service runs as LocalSystem, which
    // the default service DACL allows.
    SC_HANDLE hSvc = OpenServiceW(hScm, SC_SERVICE_NAME, SERVICE_CHANGE_CONFIG);
    if (hSvc) {
        SERVICE_FAILURE_ACTIONSW fa = {};

        if (armed) {
            SC_ACTION acts[3] = {
                { SC_ACTION_RESTART, 5000  },
                { SC_ACTION_RESTART, 10000 },
                { SC_ACTION_NONE,    0     }
            };
            fa.dwResetPeriod = 86400;
            fa.cActions      = 3;
            fa.lpsaActions   = acts;
            ChangeServiceConfig2W(hSvc, SERVICE_CONFIG_FAILURE_ACTIONS, &fa);
        } else {
            // cActions = 0 with a null array clears the policy outright.
            fa.dwResetPeriod = 0;
            fa.cActions      = 0;
            fa.lpsaActions   = nullptr;
            if (!ChangeServiceConfig2W(hSvc, SERVICE_CONFIG_FAILURE_ACTIONS, &fa))
                LOG_WARN(L"SVC", L"Could not clear recovery policy (error " +
                                 std::to_wstring(GetLastError()) + L").");
        }

        CloseServiceHandle(hSvc);
    }
    CloseServiceHandle(hScm);
}

static void ArmRecovery() { SetRecoveryArmed(true); }

static void DisarmRecoveryForDeliberateStop() {
    LOG_INFO(L"SVC", L"Deliberate stop — clearing auto-restart so it stays stopped.");
    SetRecoveryArmed(false);
}

// -------------------------------------------------------
// Alert sinks.
//
// Every alert in this engine funnels through PublishAlert, which records it
// in the shared history (assigning its id), broadcasts it to connected
// clients, and logs it. The console engine uses the identical path, so an
// alert looks the same whichever engine produced it.
// -------------------------------------------------------
static void OnMemoryAlert(const AlertRecord& rec) {
    PublishAlert(rec);
}

static void OnDriverAlert(const SC_BLOCK_ALERT& alert) {
    // Repair the trust race first, then publish only if it did not resolve.
    // The console engine calls the same function, so the two cannot drift apart
    // on what counts as worth reporting. See PublishDriverBlock.
    PublishDriverBlock(alert);
}

// -------------------------------------------------------
// IPC
// -------------------------------------------------------
static IpcContext g_ipcCtx;

/*
 * Reconcile runtime state with the persisted config after a UI/CLI change.
 *
 * The verb handler only records intent (it writes config and saves it); the
 * engine owns the consequences. Keeping that split means a new toggle needs
 * no knowledge of decoy trees or monitor threads.
 *
 * Also used at startup so the boot path and the toggle path cannot diverge.
 */
static void ApplySettings() {
    const Config& cfg = ConfigManager::Instance().Get();

    if (cfg.decoyFolderEnabled) {
        CreateDecoyFolder(cfg.decoyFolderPath);
        LOG_INFO(L"SVC", L"Decoy token folder ready: " + cfg.decoyFolderPath);
    } else if (RemoveDecoyTree(cfg.decoyFolderPath)) {
        LOG_INFO(L"SVC", L"Decoy token folder removed (disabled).");
    } else {
        LOG_WARN(L"SVC", L"Decoy token folder could not be fully removed.");
    }

    // Keep Windows Search out of the real token stores as well as the bait.
    // Deliberately outside the decoy toggle: this is about the user's own
    // browser files, and the indexer crawling them is what produced almost
    // every alert in the session that prompted this. See the header for why
    // whitelisting SearchIndexer is NOT the alternative.
    MarkTokenStoresNotIndexed();

    if (cfg.memoryMonitorEnabled) {
        MemoryMonitor::Instance().Start(cfg.memoryPollIntervalMs, OnMemoryAlert);
    } else {
        MemoryMonitor::Instance().Stop();
    }
}

// Thin adapter: the shared verb handler does all the work for both engines.
static std::wstring OnIpcMessage(const std::wstring& wMsg, bool privileged) {
    return HandleIpcVerb(wMsg, privileged, g_ipcCtx);
}

// -------------------------------------------------------
// ServiceMain — called by SCM when service starts
// -------------------------------------------------------
void WINAPI ServiceController::ServiceMain(DWORD /*argc*/, LPWSTR* /*argv*/) {
    // Create the stop event BEFORE registering the control handler so a control
    // that arrives immediately after registration has a valid event to signal.
    g_stopEvent = CreateEventW(nullptr, TRUE /*manual reset*/, FALSE, nullptr);

    // Register control handler
    s_hStatus = RegisterServiceCtrlHandlerExW(
        SC_SERVICE_NAME, ControlHandler, nullptr);
    if (!s_hStatus) {
        if (g_stopEvent) { CloseHandle(g_stopEvent); g_stopEvent = nullptr; }
        return;
    }

    s_status.dwServiceType             = SERVICE_WIN32_OWN_PROCESS;
    s_status.dwCurrentState            = SERVICE_START_PENDING;
    s_status.dwControlsAccepted        = 0;
    s_status.dwWin32ExitCode           = NO_ERROR;
    s_status.dwServiceSpecificExitCode = 0;
    s_status.dwCheckPoint              = 0;
    s_status.dwWaitHint                = 5000;
    SetStatus(SERVICE_START_PENDING, NO_ERROR, 5000);

    // Initialize logger
    Logger::Instance().Init(SC_LOG_FILE);
    LOG_INFO(L"SVC", L"=== ShieldCord engine " SC_VERSION L" (service mode) ===");
    LOG_INFO(L"SVC", L"ShieldCord service starting...");

    // Put SCM crash recovery back. A deliberate stop clears it (see
    // DisarmRecoveryForDeliberateStop); reaching this line at all means this is
    // a real start, so the policy belongs armed. Arming it this early means a
    // crash anywhere below still recovers.
    ArmRecovery();

    // Load config
    ConfigManager::Instance().Load(SC_CONFIG_FILE);
    const Config& cfg = ConfigManager::Instance().Get();

    // Wire the shared IPC handler. startTick is stamped here (not at static
    // init) so "uptime" measures from the moment the engine actually runs.
    g_ipcCtx.mode              = ipc::MODE_SERVICE;
    g_ipcCtx.requestStop       = RequestStop;
    g_ipcCtx.onDeliberateStop  = DisarmRecoveryForDeliberateStop;
    g_ipcCtx.onSettingsChanged = ApplySettings;
    g_ipcCtx.startTick         = GetTickCount64();

    // Lets the "reconnect_driver" verb bring the driver up mid-run, which is
    // what the in-app setup needs after installing it. Uses the same handler as
    // the startup path so a block that arrives on the reconnected port is
    // reported and self-healed identically.
    g_ipcCtx.onReconnectDriver = [] { return BringUpDriver(OnDriverAlert); };

    // Start IPC server
    IpcServer::Instance().Start(OnIpcMessage);

    // One-time legacy cleanup: an OLD build may have locked token/cookie folders
    // with SYSTEM-only DACLs. Restore them now (the call deletes the backup) so
    // no folder is left locked. Protection is driver-only from here on.
    if (utils::PathExists(SC_DACL_BACKUP_FILE)) {
        LOG_INFO(L"SVC", L"Legacy DACL backup found — restoring original permissions.");
        DaclGatekeeper::RestoreFromFile(SC_DACL_BACKUP_FILE);
    }

    // Layer 1 (SOLE file-protection layer): the kernel minifilter driver.
    // Load and arm it in one step — BringUpDriver is also what the
    // "reconnect_driver" verb calls, so the boot path and the post-install path
    // cannot drift apart.
    if (BringUpDriver(OnDriverAlert)) {
        LOG_INFO(L"SVC", cfg.driverEnforcementEnabled
            ? L"Kernel driver enforcement ARMED (default-deny)."
            : L"Kernel driver enforcement disabled by config.");
        LOG_INFO(L"SVC", L"Kernel driver connected — protection active.");
    } else {
        LOG_INFO(L"SVC", L"Kernel driver not available — NO file protection. "
                         L"Finish setup in the ShieldCord app so tokens stay protected.");
    }

    // Decoy tree + memory monitor. Both are user-toggleable at runtime, so
    // they go through the same reconcile path the UI's toggles use.
    // NOTE: this runs AFTER enforcement is armed. That is safe — TrustSelf()
    // already ran, so the service may write to the decoy tree it protects.
    ApplySettings();

    // The watcher's alert callback is unused (it grants trust, it does not
    // report threats); alerts come from the driver and the memory monitor.
    AppWatcher::Instance().Start(cfg.appWatcherIntervalMs,
                                 [](const std::wstring&) {});

    // Layer 2: Token Vault (stub)
    if (cfg.tokenVaultEnabled) {
        TokenVault::Instance().Initialize();
    }

    LOG_INFO(L"SVC", L"ShieldCord is running and protecting your tokens.");
    SetStatus(SERVICE_RUNNING, NO_ERROR, 0);

    // Block until a stop is requested (SCM control handler or IPC shutdown).
    if (g_stopEvent) WaitForSingleObject(g_stopEvent, INFINITE);

    // Teardown runs HERE, on the ServiceMain thread — not inside the control
    // handler (which must return promptly). Stop modules in reverse order.
    SetStatus(SERVICE_STOP_PENDING, NO_ERROR, 15000);
    LOG_INFO(L"SVC", L"Stop requested — shutting down...");
    MemoryMonitor::Instance().Stop();
    AppWatcher::Instance().Stop();
    DriverClient::Instance().SetEnforcement(false);   // disarm before disconnect
    DriverClient::Instance().Disconnect();
    TokenVault::Instance().Shutdown();
    IpcServer::Instance().Stop();
    LOG_INFO(L"SVC", L"ShieldCord stopped cleanly.");

    SetStatus(SERVICE_STOPPED, NO_ERROR, 0);

    if (g_stopEvent) { CloseHandle(g_stopEvent); g_stopEvent = nullptr; }
}

// -------------------------------------------------------
// ControlHandler — SCM sends stop/pause signals here
// -------------------------------------------------------
DWORD WINAPI ServiceController::ControlHandler(DWORD control,
                                               DWORD /*eventType*/,
                                               LPVOID /*eventData*/,
                                               LPVOID /*context*/) {
    switch (control) {
        case SERVICE_CONTROL_STOP:
        case SERVICE_CONTROL_SHUTDOWN:
            // The control handler MUST return promptly. Do NOT tear down modules
            // here — IpcServer::Stop() joins client threads and would block the
            // SCM dispatcher (and could deadlock if a control ever arrived on a
            // worker thread). Just report STOP_PENDING and signal ServiceMain,
            // which performs the actual teardown on its own thread.
            LOG_INFO(L"SVC", L"Stop signal received — signaling shutdown...");
            SetStatus(SERVICE_STOP_PENDING, NO_ERROR, 15000);
            RequestStop();
            break;

        case SERVICE_CONTROL_INTERROGATE:
        default:
            SetServiceStatus(s_hStatus, &s_status);
            break;
    }
    return NO_ERROR;
}

void ServiceController::SetStatus(DWORD currentState, DWORD exitCode, DWORD waitHint) {
    s_status.dwCurrentState  = currentState;
    s_status.dwWin32ExitCode = exitCode;
    s_status.dwWaitHint      = waitHint;

    if (currentState == SERVICE_RUNNING || currentState == SERVICE_STOPPED)
        s_status.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    else
        s_status.dwControlsAccepted = 0;

    if (s_hStatus) SetServiceStatus(s_hStatus, &s_status);
}

// -------------------------------------------------------
// RunService — dispatcher called from main()
// -------------------------------------------------------
void RunService() {
    SERVICE_TABLE_ENTRYW dispatchTable[] = {
        { const_cast<LPWSTR>(SC_SERVICE_NAME), ServiceController::ServiceMain },
        { nullptr, nullptr }
    };
    StartServiceCtrlDispatcherW(dispatchTable);
}

// -------------------------------------------------------
// ArmDriver
//
// The arming sequence, in the one order that is safe. It was previously
// copy-pasted into both engines' startup paths, which meant the reconnect path
// would have had to become a third copy — and any future change to the order
// would have applied to whichever copies someone remembered.
//
// Returns whether enforcement actually landed on the driver, which is NOT the
// same as "we are connected". m_connected is the client's belief, and it can be
// left over from a driver that has since been unloaded and replaced — which is
// exactly what happens when setup is re-run on a machine whose driver is
// already loaded, because driver_setup unloads it to replace the .sys. Trusting
// the flag there would report protection over a dead port.
// -------------------------------------------------------
bool ArmDriver() {
    if (!DriverClient::Instance().IsConnected()) return false;

    const Config& cfg = ConfigManager::Instance().Get();

    DriverClient::Instance().SendInitialTrustedPids();
    DriverClient::Instance().TrustSelf();
    DriverClient::Instance().SendProtectedPaths();
    DriverClient::Instance().SetFeatureFlags(
        (cfg.driverFileBlockEnabled ? SC_FEATURE_FILE_BLOCK : 0) |
        (cfg.driverAlertsEnabled    ? SC_FEATURE_ALERTS     : 0));

    // LAST, and the answer to "did this work". Everything above must have landed
    // first, or default-deny denies a legitimate app in the window before its
    // trust arrives.
    if (!DriverClient::Instance().SetEnforcement(cfg.driverEnforcementEnabled)) {
        LOG_WARN(L"SVC", L"ArmDriver: the driver did not accept enforcement — "
                         L"the connection is stale or the driver is gone.");
        return false;
    }

    return true;
}

// -------------------------------------------------------
// BringUpDriver
// -------------------------------------------------------
bool BringUpDriver(std::function<void(const SC_BLOCK_ALERT&)> alertHandler) {
    // DEMAND-start, so it is not running after a reboot — and not running at all
    // until it has been installed, which is the state the in-app setup fixes.
    EnsureKernelFilterLoaded();

    if (!DriverClient::Instance().IsConnected()) {
        if (!DriverClient::Instance().Connect()) return false;
        DriverClient::Instance().StartAlertReceiver(alertHandler);
        return ArmDriver();
    }

    if (ArmDriver()) return true;

    /*
     * Connected but the driver would not take enforcement: the port is left over
     * from a driver that has been unloaded and replaced underneath us. That is
     * not a hypothetical — driver_setup unloads the running filter when it has
     * to replace the .sys, so re-running setup on a protected machine does
     * exactly this.
     *
     * Drop the dead handle and connect to the one that is actually there. One
     * retry, because a second failure means the driver really is absent and
     * saying so is more useful than looping.
     */
    LOG_WARN(L"SVC", L"BringUpDriver: stale driver connection — reconnecting.");
    DriverClient::Instance().Disconnect();

    if (!DriverClient::Instance().Connect()) return false;
    DriverClient::Instance().StartAlertReceiver(alertHandler);
    return ArmDriver();
}

// -------------------------------------------------------
// EnsureKernelFilterLoaded
// The kernel minifilter is installed as DEMAND-start, so it is NOT running
// after a reboot. Start it now so the service can connect and arm it. The
// driver itself stays fail-open until the service connects — loading it here
// is safe and never blocks a legit app on its own.
// -------------------------------------------------------
bool EnsureKernelFilterLoaded() {
    SC_HANDLE hScm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!hScm) {
        LOG_WARN(L"SVC", L"EnsureKernelFilterLoaded: OpenSCManager failed (" +
                 std::to_wstring(GetLastError()) + L").");
        return false;
    }

    SC_HANDLE hSvc = OpenServiceW(hScm, SC_FILTER_SERVICE_NAME,
                                  SERVICE_START | SERVICE_QUERY_STATUS);
    if (!hSvc) {
        DWORD err = GetLastError();
        if (err == ERROR_SERVICE_DOES_NOT_EXIST) {
            LOG_INFO(L"SVC", L"EnsureKernelFilterLoaded: ShieldCordFilter not installed. "
                             L"No file protection.");
        } else {
            LOG_WARN(L"SVC", L"EnsureKernelFilterLoaded: OpenService failed (" +
                     std::to_wstring(err) + L").");
        }
        CloseServiceHandle(hScm);
        return false;
    }

    bool running = false;
    if (!StartServiceW(hSvc, 0, nullptr)) {
        DWORD err = GetLastError();
        if (err == ERROR_SERVICE_ALREADY_RUNNING)
            running = true;
        else
            LOG_WARN(L"SVC", L"EnsureKernelFilterLoaded: StartService failed (" +
                     std::to_wstring(err) + L").");
    }

    // If we requested the start, wait (briefly) for the driver service to
    // reach RUNNING so the comm port exists before we try to connect.
    if (!running) {
        SERVICE_STATUS ss = {};
        for (int i = 0; i < 100; i++) {                 // up to ~5 s
            if (!QueryServiceStatus(hSvc, &ss)) break;
            if (ss.dwCurrentState == SERVICE_RUNNING) { running = true; break; }
            if (ss.dwCurrentState == SERVICE_STOPPED ||
                ss.dwCurrentState == SERVICE_STOP_PENDING) break;
            Sleep(50);
        }
    }

    CloseServiceHandle(hSvc);
    CloseServiceHandle(hScm);

    if (running)
        LOG_INFO(L"SVC", L"EnsureKernelFilterLoaded: kernel filter driver running.");
    else
        LOG_WARN(L"SVC", L"EnsureKernelFilterLoaded: kernel filter did not reach RUNNING.");
    return running;
}

// -------------------------------------------------------
// InstallService (called from installer)
// -------------------------------------------------------
bool ServiceController::InstallService(const std::wstring& exePath) {
    SC_HANDLE hScm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
    if (!hScm) return false;

    // Quote the image path. If it contains a space (e.g. C:\Program Files\...)
    // an unquoted lpBinaryPathName is both an unquoted-service-path privilege-
    // escalation weakness and can make the SCM launch the wrong executable.
    std::wstring quotedPath = L"\"" + exePath + L"\"";

    SC_HANDLE hSvc = CreateServiceW(
        hScm,
        SC_SERVICE_NAME,
        SC_DISPLAY_NAME,
        SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START,
        SERVICE_ERROR_NORMAL,
        quotedPath.c_str(),
        nullptr, nullptr, nullptr, nullptr, nullptr);

    if (!hSvc) {
        // Maybe already installed
        hSvc = OpenServiceW(hScm, SC_SERVICE_NAME, SERVICE_ALL_ACCESS);
    }

    bool ok = (hSvc != nullptr);

    if (ok) {
        // Re-describe and re-arm recovery on every install, so an upgraded
        // binary inherits the current policy rather than whatever an older
        // version happened to write.
        SERVICE_DESCRIPTIONW desc = {};
        wchar_t descStr[] = L"Protects Discord and browser tokens from grabber malware.";
        desc.lpDescription = descStr;
        ChangeServiceConfig2W(hSvc, SERVICE_CONFIG_DESCRIPTION, &desc);

        SC_ACTION acts[3] = {
            { SC_ACTION_RESTART, 5000  },
            { SC_ACTION_RESTART, 10000 },
            { SC_ACTION_NONE,    0     }
        };
        SERVICE_FAILURE_ACTIONSW fa = {};
        fa.dwResetPeriod = 86400;
        fa.cActions      = 3;
        fa.lpsaActions   = acts;
        ChangeServiceConfig2W(hSvc, SERVICE_CONFIG_FAILURE_ACTIONS, &fa);

        CloseServiceHandle(hSvc);
    }

    CloseServiceHandle(hScm);
    return ok;
}

// -------------------------------------------------------
// UninstallService
// -------------------------------------------------------
bool ServiceController::UninstallService() {
    SC_HANDLE hScm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!hScm) return false;

    SC_HANDLE hSvc = OpenServiceW(hScm, SC_SERVICE_NAME, SERVICE_STOP | DELETE | SERVICE_QUERY_STATUS);
    if (!hSvc) { CloseServiceHandle(hScm); return false; }

    SERVICE_STATUS ss = {};
    if (ControlService(hSvc, SERVICE_CONTROL_STOP, &ss)) {
        for (int i = 0; i < 200; i++) {
            if (!QueryServiceStatus(hSvc, &ss)) break;
            if (ss.dwCurrentState == SERVICE_STOPPED) break;
            Sleep(50);
        }
    }

    bool ok = DeleteService(hSvc) != 0;
    CloseServiceHandle(hSvc);
    CloseServiceHandle(hScm);
    return ok;
}

} // namespace sc
