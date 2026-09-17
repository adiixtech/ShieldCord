// ============================================================
// ShieldCord — main.cpp
// Entry point: run as Windows service, or handle
// command-line flags: --install, --uninstall, --console
// ============================================================
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <iostream>
#include "service_controller.h"
#include "logger.h"
#include "config.h"
#include "dacl_gatekeeper.h"
#include "app_watcher.h"
#include "memory_monitor.h"
#include "ipc_server.h"
#include "ipc_verbs.h"
#include "driver_client.h"
#include "process_validator.h"
#include "utils.h"
#include "../shared/common.h"
#include "../shared/ipc_protocol.h"
#include "../shared/token_paths.h"
#include <cwchar>
#include <cstdlib>    // _countof
#include <nlohmann/json.hpp>

// -------------------------------------------------------
// Console shutdown flag.
//
// Set by the console control handler (Ctrl+C / window close) and by the IPC
// "shutdown" verb, so in both cases the main loop runs its cleanup instead of
// the process being torn down mid-write.
// -------------------------------------------------------
static std::atomic<bool> g_consoleShutdown{ false };

static BOOL WINAPI ConsoleCtrlHandler(DWORD /*type*/) {
    g_consoleShutdown = true;
    // Return TRUE so the default handler does NOT terminate us — the main
    // loop performs the clean shutdown and exits on its own.
    return TRUE;
}

// -------------------------------------------------------
// Console-mode engine: IPC + reconcile + alert sinks.
//
// The verbs themselves live in ipc_verbs.cpp and are shared with the
// installed service. This file supplies only the parts that differ: the mode
// string, the stop path, and the reconcile hook. (These two used to be
// separate copy-pasted switch statements that had already drifted apart.)
// -------------------------------------------------------
static sc::IpcContext g_consoleCtx;

static void OnConsoleMemoryAlert(const sc::AlertRecord& rec) {
    // PublishAlert records + broadcasts + logs to the file; the console adds
    // a human-readable line so an interactive run shows threats as they happen.
    sc::PublishAlert(rec);
    wprintf(L"[ALERT] %s\n", rec.message.c_str());
}

static void OnConsoleDriverAlert(const SC_BLOCK_ALERT& alert) {
    // Self-heal first (a legitimate app can lose the trust race and be
    // denied before its trust lands), then report through the shared
    // path so console and service alerts are identical.
    // Same shared path as service mode, so console and service mode cannot
    // disagree about what is worth reporting. See PublishDriverBlock.
    sc::PublishDriverBlock(alert);
}

// Reconcile runtime state with the persisted config after a UI/CLI change.
static void ApplyConsoleSettings() {
    const sc::Config& cfg = sc::ConfigManager::Instance().Get();

    if (cfg.decoyFolderEnabled) {
        sc::CreateDecoyFolder(cfg.decoyFolderPath);
        wprintf(L"[+] Decoy token folder ready: %s\n", cfg.decoyFolderPath.c_str());
    } else if (sc::RemoveDecoyTree(cfg.decoyFolderPath)) {
        wprintf(L"[+] Decoy token folder removed.\n");
    }

    // Same as service mode — see MarkTokenStoresNotIndexed.
    sc::MarkTokenStoresNotIndexed();

    if (cfg.memoryMonitorEnabled) {
        sc::MemoryMonitor::Instance().Start(cfg.memoryPollIntervalMs, OnConsoleMemoryAlert);
    } else {
        sc::MemoryMonitor::Instance().Stop();
    }
}

static std::wstring HandleConsoleIpc(const std::wstring& wMsg, bool privileged) {
    return sc::HandleIpcVerb(wMsg, privileged, g_consoleCtx);
}

static void PrintBanner() {
    wprintf(L"\n");
    wprintf(L"  ███████╗██╗  ██╗██╗███████╗██╗     ██████╗  ██████╗ ██████╗ ██████╗ \n");
    wprintf(L"  ██╔════╝██║  ██║██║██╔════╝██║     ██╔════╝██╔═══██╗██╔══██╗██╔══██╗\n");
    wprintf(L"  ███████╗███████║██║█████╗  ██║     ██║     ██║   ██║██████╔╝██║  ██║\n");
    wprintf(L"  ╚════██║██╔══██║██║██╔══╝  ██║     ██║     ██║   ██║██╔══██╗██║  ██║\n");
    wprintf(L"  ███████║██║  ██║██║███████╗███████╗╚██████╗╚██████╔╝██║  ██║██████╔╝\n");
    wprintf(L"  ╚══════╝╚═╝  ╚═╝╚═╝╚══════╝╚══════╝ ╚═════╝ ╚═════╝ ╚═╝  ╚═╝╚═════╝ \n");
    wprintf(L"                         Token Protector v" SC_VERSION L"\n\n");
}

static void RunConsoleMode() {
    PrintBanner();
    wprintf(L"[*] Running in console mode (Ctrl+C to stop)...\n\n");

    // Initialize logger to ALWAYS log to file for easier debugging
    sc::utils::EnsureDirectoryExists(SC_LOG_DIR);
    sc::Logger::Instance().Init(SC_LOG_FILE);
    LOG_INFO(L"Main", L"=== ShieldCord engine " SC_VERSION L" (console mode) ===");
    sc::ConfigManager::Instance().Load(SC_CONFIG_FILE);
    const sc::Config& cfg = sc::ConfigManager::Instance().Get();

    // Real command handling so shieldcord_ctl.exe / the tray UI can drive the
    // console engine — same verb set as the installed service.
    g_consoleCtx.mode              = sc::ipc::MODE_CONSOLE;
    g_consoleCtx.startTick         = GetTickCount64();
    g_consoleCtx.requestStop       = [] { g_consoleShutdown = true; };
    g_consoleCtx.onSettingsChanged = ApplyConsoleSettings;
    g_consoleCtx.onReconnectDriver = [] { return sc::BringUpDriver(OnConsoleDriverAlert); };
    sc::IpcServer::Instance().Start(HandleConsoleIpc);

    // One-time legacy cleanup: an OLD build may have locked token/cookie folders
    // with SYSTEM-only DACLs. Restore them now (the call deletes the backup) so
    // no folder is left locked. Production protection is driver-only.
    if (sc::utils::PathExists(SC_DACL_BACKUP_FILE)) {
        wprintf(L"[*] Restoring DACLs left by an older build...\n");
        sc::DaclGatekeeper::RestoreFromFile(SC_DACL_BACKUP_FILE);
    }

    // Layer 1 (SOLE file-protection layer): the kernel minifilter driver.
    // BringUpDriver loads and arms it in the one safe order; the
    // "reconnect_driver" verb calls the same function, so a driver installed
    // while this engine is already running is armed identically.
    if (sc::BringUpDriver(OnConsoleDriverAlert)) {
        wprintf(L"[+] Kernel driver connected — enforcement %s.\n",
                cfg.driverEnforcementEnabled ? L"ARMED (default-deny)" : L"disabled by config");
    } else {
        wprintf(L"[+] Kernel driver not available — NO file protection.\n");
        wprintf(L"    Finish setup in the ShieldCord app so tokens stay protected.\n");
    }

    // The watcher's alert callback is unused (it grants trust, it does not
    // report threats); alerts come from the driver and the memory monitor.
    sc::AppWatcher::Instance().Start(cfg.appWatcherIntervalMs,
                                     [](const std::wstring&) {});

    // Console/dev runs NEVER terminate processes — threats are logged only.
    // The kill switch stays exclusive to the installed service. Set BEFORE the
    // monitor starts so a UI-triggered reconcile cannot re-enable it.
    sc::MemoryMonitor::Instance().SetKillEnabled(false);
    ApplyConsoleSettings();
    if (cfg.memoryMonitorEnabled)
        wprintf(L"[+] Memory Monitor active (LOG ONLY in console mode — no kills).\n");

    wprintf(L"[+] Protection running. Press Enter to stop.\n");

    // Read input on a side thread: neither an IPC "shutdown" nor Ctrl+C
    // unblocks a blocking getchar() on the main thread.
    g_consoleShutdown = false;
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
    std::thread inputThread([] {
        int c;
        while ((c = getchar()) != '\n' && c != EOF) { /* skip */ }
        g_consoleShutdown = true;
    });
    inputThread.detach();

    while (!g_consoleShutdown.load()) Sleep(150);

    sc::MemoryMonitor::Instance().Stop();
    sc::AppWatcher::Instance().Stop();
    sc::DriverClient::Instance().SetEnforcement(false);   // disarm before disconnect
    sc::DriverClient::Instance().Disconnect();
    sc::IpcServer::Instance().Stop();
    wprintf(L"[*] Stopped.\n");
}

// -------------------------------------------------------
// DEVELOPER TEST MODE (--test)
//
// Runs the full protection pipeline against a FAKE sandbox in
// %TEMP% instead of your real Discord/browser folders:
//   * Creates %TEMP%\ShieldCordTest with dummy "token" files
//   * Locks/unlocks only those paths (real apps unaffected)
//   * Memory Monitor runs in log-only mode (never kills anything)
//   * On Ctrl+C or window close: DACLs restored, sandbox deleted
//   * If the process crashes: run `--restore` to fix up DACLs,
//     then the sandbox folder can simply be deleted
// -------------------------------------------------------
static int RunTestMode() {
    PrintBanner();
    wprintf(L"[*] DEVELOPER TEST MODE — sandbox only, nothing real is touched.\n\n");

    sc::utils::EnsureDirectoryExists(SC_LOG_DIR);
    sc::Logger::Instance().Init(SC_LOG_FILE);
    LOG_INFO(L"Main", L"=== ShieldCord engine " SC_VERSION L" (TEST mode) ===");
    sc::ConfigManager::Instance().Load(SC_CONFIG_FILE);
    const sc::Config& cfg = sc::ConfigManager::Instance().Get();

    // 1. Clean up any leftover sandbox from a crashed previous run
    const std::wstring sandboxRoot = sc::TestSandboxRoot();
    const std::wstring backupFile  = sc::TestBackupFile();
    if (sc::utils::PathExists(sandboxRoot)) {
        wprintf(L"[*] Cleaning leftover sandbox from previous run...\n");
        sc::utils::DeleteTreeForce(sandboxRoot);
    }
    if (sc::utils::PathExists(backupFile)) {
        wprintf(L"[*] Found leftover DACL backup — restoring first...\n");
        sc::DaclGatekeeper::RestoreFromFile(backupFile);
    }

    // 2. Create the fresh sandbox with fake token files
    sc::CreateTestSandbox();
    wprintf(L"[+] Sandbox created at: %s\n\n", sandboxRoot.c_str());

    // 3. Ctrl+C and window-close safety: restore everything and clean up
    auto cleanup = [&sandboxRoot, &backupFile]() {
        sc::MemoryMonitor::Instance().Stop();
        sc::AppWatcher::Instance().Stop();
        sc::DaclGatekeeper::Instance().UnlockAll();      // restores DACLs + deletes backup file
        if (sc::utils::PathExists(sandboxRoot))          // belt & braces
            sc::utils::DeleteTreeForce(sandboxRoot);
    };

    g_consoleShutdown = false;
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);


    // 4. IPC server so the real UI can connect to the test engine too — the
    //    same shared verb handler the service and console engine use.
    g_consoleCtx.mode        = sc::ipc::MODE_CONSOLE;
    g_consoleCtx.startTick   = GetTickCount64();
    g_consoleCtx.requestStop = [] { g_consoleShutdown = true; };
    sc::IpcServer::Instance().Start(HandleConsoleIpc);

    // 5. Start the pipeline against the sandbox paths
    sc::DaclGatekeeper::Instance().SetBackupFilePath(backupFile);
    auto testPaths = sc::BuildTestProtectedPaths();
    sc::DaclGatekeeper::Instance().LockAll(&testPaths);
    sc::AppWatcher::Instance().Start(cfg.appWatcherIntervalMs,
                                     [](const std::wstring&) {});
    wprintf(L"[+] DACL Gatekeeper active on sandbox paths.\n");
    wprintf(L"[*] Launch real Discord/Chrome to watch grant/revoke happen.\n");
    wprintf(L"[*] Try opening a sandbox file now (should be ACCESS DENIED),\n");
    wprintf(L"    then again while Discord is running (should succeed).\n");

    sc::MemoryMonitor::Instance().SetKillEnabled(false);   // log-only in test mode
    sc::MemoryMonitor::Instance().Start(cfg.memoryPollIntervalMs, OnConsoleMemoryAlert);
    wprintf(L"[+] Memory Monitor active (LOG ONLY in test mode — no kills).\n\n");

    wprintf(L"[+] Test running. Press Enter (or Ctrl+C) to stop and clean up.\n");

    // Read input on a side thread so the main loop can react to Ctrl+C /
    // window-close (handled in TestCtrlHandler) even while blocked on input.
    std::thread inputThread([] {
        int c;
        while ((c = getchar()) != '\n' && c != EOF) { /* skip */ }
        g_consoleShutdown = true;   // Enter pressed
    });
    inputThread.detach();

    while (!g_consoleShutdown.load()) Sleep(150);

    wprintf(L"\n[*] Cleaning up (restoring DACLs, deleting sandbox)...\n");
    cleanup();
    SetConsoleCtrlHandler(ConsoleCtrlHandler, FALSE);
    sc::IpcServer::Instance().Stop();
    wprintf(L"[+] Done — sandbox removed, DACLs restored.\n");
    return 0;
}

// -------------------------------------------------------
// CRASH RECOVERY (--restore)
// Restores DACLs from the backup file written by a crashed
// test run. Works for both the dev backup and the service one.
// -------------------------------------------------------
static int RunRestoreMode() {
    PrintBanner();
    wprintf(L"[*] Restore mode — recovering DACLs from backup file...\n\n");

    bool any = false;
    if (sc::utils::PathExists(sc::TestBackupFile())) {
        wprintf(L"[*] Dev backup found: %s\n", sc::TestBackupFile().c_str());
        sc::DaclGatekeeper::RestoreFromFile(sc::TestBackupFile());
        any = true;
    }
    if (sc::utils::PathExists(SC_DACL_BACKUP_FILE)) {
        wprintf(L"[*] Service backup found: %s\n", SC_DACL_BACKUP_FILE);
        sc::DaclGatekeeper::RestoreFromFile(SC_DACL_BACKUP_FILE);
        any = true;
    }

    // Also remove any crash-orphaned test sandbox
    if (sc::utils::PathExists(sc::TestSandboxRoot())) {
        wprintf(L"[*] Removing leftover test sandbox: %s\n", sc::TestSandboxRoot().c_str());
        sc::utils::DeleteTreeForce(sc::TestSandboxRoot());
        any = true;
    }

    if (!any) {
        wprintf(L"[+] No backup files found — nothing to restore.\n");
    } else {
        wprintf(L"[+] Restore complete.\n");
    }
    return 0;
}

int wmain(int argc, wchar_t* argv[]) {
    // Parse command-line arguments
    bool install   = false;
    bool uninstall = false;
    bool console   = false;
    bool test      = false;
    bool restore   = false;

    for (int i = 1; i < argc; i++) {
        std::wstring arg = argv[i];
        if (arg == L"--install"   || arg == L"-i") install   = true;
        if (arg == L"--uninstall" || arg == L"-u") uninstall = true;
        if (arg == L"--console"   || arg == L"-c") console   = true;
        if (arg == L"--test"      || arg == L"-t") test      = true;
        if (arg == L"--restore"   || arg == L"-r") restore   = true;
    }

    if (restore) {
        return RunRestoreMode();
    }

    if (test) {
        return RunTestMode();
    }

    if (install) {
        PrintBanner();
        wchar_t exePath[MAX_PATH];
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);

        // Ensure data directory exists
        sc::utils::EnsureDirectoryExists(L"C:\\ProgramData\\ShieldCord\\logs");

        bool ok = sc::ServiceController::InstallService(exePath);
        if (ok) {
            wprintf(L"[+] ShieldCord service installed successfully.\n");
            // Start the service
            SC_HANDLE hScm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
            if (hScm) {
                SC_HANDLE hSvc = OpenServiceW(hScm, SC_SERVICE_NAME, SERVICE_START);
                if (hSvc) {
                    StartServiceW(hSvc, 0, nullptr);
                    wprintf(L"[+] Service started.\n");
                    CloseServiceHandle(hSvc);
                }
                CloseServiceHandle(hScm);
            }
        } else {
            wprintf(L"[-] Failed to install service. Error: %lu\n", GetLastError());
        }
        return ok ? 0 : 1;
    }

    if (uninstall) {
        PrintBanner();
        bool ok = sc::ServiceController::UninstallService();
        wprintf(ok ? L"[+] Service uninstalled.\n" : L"[-] Uninstall failed.\n");
        return ok ? 0 : 1;
    }

    if (console) {
        RunConsoleMode();
        return 0;
    }

    // Default: run as Windows service
    sc::RunService();
    return 0;
}
