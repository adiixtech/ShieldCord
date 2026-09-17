// ============================================================
// ShieldCord — install.cpp
// The engine-side installer: driver + service + data directories.
//
// Run as Administrator. Ships inside the Inno Setup package, which calls it
// with --install --no-driver; it is also usable standalone in a dev/VM layout.
//
// It deliberately DELEGATES the driver work to shieldcord_driver_setup.exe
// rather than repeating it — one implementation of "install the driver", used
// by both the installer and the uninstaller.
//
// --no-driver skips the kernel driver entirely, leaving only the service. The
// packaged installer uses it because the driver CANNOT simply be installed: it
// is signed with ShieldCord's test certificate, so Windows will not load it
// until test signing is on, and turning that on is a security change for the
// whole PC that has to be explained and consented to. That conversation happens
// in the app's setup screen, which also has a user interface to report the
// failure in — this program runs hidden, so a driver that failed to load here
// reached nobody.
// ============================================================
#include "installer_common.h"

#include <string>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shlwapi.lib")

using sci::EnsureDir;
using sci::ModuleDirectory;

/// Runs a child process to completion. Returns its exit code, or -1.
static int RunChild(const std::wstring& exe, const std::wstring& args, DWORD timeoutMs) {
    std::wstring cmd = L"\"" + exe + L"\" " + args;

    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};

    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
    {
        return -1;
    }

    DWORD waited = WaitForSingleObject(pi.hProcess, timeoutMs);
    DWORD code = (DWORD)-1;
    if (waited == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
    else                         TerminateProcess(pi.hProcess, (UINT)-1);

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)code;
}

/// Finds shieldcord_driver_setup.exe next to us, in {app}, or in our own dir.
static bool FindDriverSetup(std::wstring& out) {
    if (GetFileAttributesW(SC_DRIVER_SETUP) != INVALID_FILE_ATTRIBUTES) {
        out = SC_DRIVER_SETUP;
        return true;
    }

    wchar_t dir[MAX_PATH] = {};
    if (ModuleDirectory(dir, MAX_PATH)) {
        std::wstring beside = std::wstring(dir) + L"\\shieldcord_driver_setup.exe";
        if (GetFileAttributesW(beside.c_str()) != INVALID_FILE_ATTRIBUTES) {
            out = beside;
            return true;
        }
    }
    return false;
}

int wmain(int argc, wchar_t* argv[]) {
    wprintf(L"\n=== ShieldCord Installer ===\n\n");

    if (!sci::RequireElevation(L"The ShieldCord installer")) return 1;

    bool skipDriver = false;
    for (int i = 1; i < argc; i++) {
        if (wcscmp(argv[i], L"--no-driver") == 0) skipDriver = true;
    }

    wchar_t srcDir[MAX_PATH] = {};
    if (!ModuleDirectory(srcDir, MAX_PATH)) {
        wprintf(L"[-] Cannot determine the installer's own directory.\n");
        return 1;
    }

    /*
     * Everything is placed and registered relative to where THIS program lives,
     * not relative to a hard-coded C:\Program Files\ShieldCord.
     *
     * That matters because the installer wizard lets the user choose the folder.
     * Registering the service with a fixed path while the files land somewhere
     * else produced an install that completed, reported success, and had no
     * engine at all — the service pointed at a directory that did not exist.
     * Reading our own directory means the two cannot disagree.
     */
    std::wstring installDir = srcDir;

    // ── 1. directories ──────────────────────────────────────
    wprintf(L"[*] Creating directories...\n");
    EnsureDir(installDir.c_str());
    EnsureDir(SC_DATA_DIR);
    EnsureDir(SC_LOG_DIR);

    // ── 2. binaries ─────────────────────────────────────────
    // Nothing to copy: this program RUNS from the install directory, so Inno has
    // already placed the binaries beside it. All that is left is to check the one
    // that matters, because a missing service binary makes everything below
    // pointless.
    std::wstring svcExe = installDir + L"\\shieldcord_svc.exe";

    if (GetFileAttributesW(svcExe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        wprintf(L"[-] shieldcord_svc.exe is missing from %s. Cannot install.\n",
                installDir.c_str());
        return 1;
    }
    wprintf(L"[+] Program files present in %s\n", installDir.c_str());

    // ── 3. kernel driver ────────────────────────────────────
    // Do this BEFORE registering the service so the engine finds a loaded
    // driver on its very first start rather than logging "driver not loaded".
    if (skipDriver) {
        wprintf(L"[*] Skipping the kernel driver (--no-driver).\n");
        wprintf(L"    Protection is not active yet: the driver is installed from the\n");
        wprintf(L"    ShieldCord app, so test signing can be explained and consented to.\n");
    } else {
        std::wstring driverSetup;
        if (FindDriverSetup(driverSetup)) {
            wprintf(L"[*] Installing the kernel driver...\n");
            int rc = RunChild(driverSetup, L"--install", 60000);
            if (rc != 0) {
                // Not fatal: the engine runs without the driver (no file
                // protection) and the service will say so. Refusing to install the
                // service entirely would leave the user worse off.
                wprintf(L"[!] Driver installation returned %d — the engine will run without file protection.\n", rc);
                wprintf(L"    Finish setup in the ShieldCord app, which reports what went wrong.\n");
            }
        } else {
            wprintf(L"[!] shieldcord_driver_setup.exe not found — skipping driver installation.\n");
        }
    }

    // ── 4. service registration ─────────────────────────────
    wprintf(L"[*] Registering the Windows service...\n");

    SC_HANDLE hScm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
    if (!hScm) {
        wprintf(L"[-] OpenSCManager failed: %lu\n", GetLastError());
        return 1;
    }

    // Quote the image path: it lives under "C:\Program Files\..." which
    // contains a space. An unquoted lpBinaryPathName is a classic
    // unquoted-service-path escalation weakness.
    // Built from OUR directory rather than the SC_SVC_EXE macro, so a user who
    // chose a different install folder gets a service that points at itself
    // rather than at C:\Program Files\ShieldCord. See the note at installDir.
    std::wstring quotedSvc = L"\"" + svcExe + L"\"";
    const wchar_t* quotedSvcExe = quotedSvc.c_str();

    SC_HANDLE hSvc = OpenServiceW(hScm, SC_SVC_NAME, SERVICE_ALL_ACCESS);
    if (hSvc) {
        // Upgrade in place: point an existing registration at the new binary.
        wprintf(L"[*] Existing service found — updating it in place.\n");
        ChangeServiceConfigW(hSvc, SERVICE_NO_CHANGE, SERVICE_AUTO_START,
                             SERVICE_NO_CHANGE, quotedSvcExe,
                             nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
    } else {
        hSvc = CreateServiceW(
            hScm, SC_SVC_NAME, SC_SVC_DISPLAY,
            SERVICE_ALL_ACCESS,
            SERVICE_WIN32_OWN_PROCESS,
            SERVICE_AUTO_START,
            SERVICE_ERROR_NORMAL,
            quotedSvcExe,
            nullptr, nullptr, nullptr, nullptr, nullptr);
    }

    if (!hSvc) {
        wprintf(L"[-] Failed to create the service. Error: %lu\n", GetLastError());
        CloseServiceHandle(hScm);
        return 1;
    }

    SERVICE_DESCRIPTIONW desc = {};
    desc.lpDescription = const_cast<LPWSTR>(SC_SVC_DESC);
    ChangeServiceConfig2W(hSvc, SERVICE_CONFIG_DESCRIPTION, &desc);

    // Restart twice on failure, then stay down rather than crash-looping.
    SC_ACTION acts[3] = {
        { SC_ACTION_RESTART, 5000  },
        { SC_ACTION_RESTART, 10000 },
        { SC_ACTION_NONE,    0     },
    };
    SERVICE_FAILURE_ACTIONSW fa = {};
    fa.dwResetPeriod = 86400;
    fa.cActions      = 3;
    fa.lpsaActions   = acts;
    ChangeServiceConfig2W(hSvc, SERVICE_CONFIG_FAILURE_ACTIONS, &fa);

    wprintf(L"[+] Service registered (auto-start).\n");

    // ── 5. start ────────────────────────────────────────────
    wprintf(L"[*] Starting the service...\n");
    if (StartServiceW(hSvc, 0, nullptr)) {
        wprintf(L"[+] Service started.\n");
    } else {
        DWORD err = GetLastError();
        if (err == ERROR_SERVICE_ALREADY_RUNNING)
            wprintf(L"[+] Service is already running.\n");
        else
            wprintf(L"[!] Could not start the service (error %lu). It will start at the next boot.\n", err);
    }

    CloseServiceHandle(hSvc);
    CloseServiceHandle(hScm);

    wprintf(L"\n[+] ShieldCord installed.\n");
    wprintf(L"    Service: %s\n", SC_SVC_DISPLAY);
    wprintf(L"    Logs:    %s\\shieldcord.log\n\n", SC_LOG_DIR);
    return 0;
}
