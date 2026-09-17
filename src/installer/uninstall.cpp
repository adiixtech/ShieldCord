// ============================================================
// ShieldCord — uninstall.cpp
// Removes the engine service, the kernel driver, and program files.
// Run as Administrator.
//
// Order matters: the service must stop BEFORE the driver is unloaded, the
// driver before Program Files is deleted, and the legacy DACL restore before
// anything is removed — otherwise a user upgrading from the old DACL-locking
// build could be left permanently locked out of their own Discord data.
// ============================================================
#include "installer_common.h"

#include <string>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shlwapi.lib")

using sci::ModuleDirectory;

// -------------------------------------------------------
// Recursive delete
// -------------------------------------------------------
static bool RemoveDirRecursive(const wchar_t* path) {
    // PathCombineW is bounded to MAX_PATH and returns NULL (rather than the
    // undefined behaviour a truncating swprintf_s can cause) if the combined
    // path would overflow — so an over-long path is skipped, never silently
    // truncated into a wrong target.
    wchar_t pattern[MAX_PATH];
    if (!PathCombineW(pattern, path, L"*")) return false;

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return true;

    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;

        wchar_t child[MAX_PATH];
        if (!PathCombineW(child, path, fd.cFileName)) continue;   // too long — skip safely

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                // Junction / symlink: remove the link ITSELF and never recurse
                // into it. Recursing would follow the reparse point and delete
                // the *target's* contents, which can live anywhere on disk
                // (e.g. a planted junction to C:\Windows).
                RemoveDirectoryW(child);
            } else {
                RemoveDirRecursive(child);
            }
        } else {
            // Clear read-only so the delete isn't blocked by the attribute.
            // DeleteFileW removes a file symlink without following it.
            SetFileAttributesW(child, FILE_ATTRIBUTE_NORMAL);
            DeleteFileW(child);
        }
    } while (FindNextFileW(h, &fd));

    FindClose(h);
    return RemoveDirectoryW(path) != 0;
}

// -------------------------------------------------------
// Service teardown
// -------------------------------------------------------
static void StopAndDeleteService(const wchar_t* name) {
    SC_HANDLE hScm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!hScm) {
        wprintf(L"[!] Cannot open the Service Control Manager. Run as Administrator.\n");
        return;
    }

    SC_HANDLE hSvc = OpenServiceW(hScm, name,
        SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE);

    if (hSvc) {
        wprintf(L"[*] Stopping %s...\n", name);
        SERVICE_STATUS ss = {};
        ControlService(hSvc, SERVICE_CONTROL_STOP, &ss);

        for (int i = 0; i < 20; i++) {                 // up to ~10 s
            Sleep(500);
            if (!QueryServiceStatus(hSvc, &ss)) break;
            if (ss.dwCurrentState == SERVICE_STOPPED) break;
        }

        if (DeleteService(hSvc)) wprintf(L"[+] %s removed.\n", name);
        else                     wprintf(L"[!] Could not delete %s (error %lu).\n", name, GetLastError());

        CloseServiceHandle(hSvc);
    } else {
        wprintf(L"[i] %s is not registered.\n", name);
    }

    CloseServiceHandle(hScm);
}

/// Removes the ShieldCordFilter driver via the shared helper, falling back to
/// a direct unload when the helper is not present.
static void RemoveDriver() {
    wprintf(L"[*] Removing the kernel driver...\n");

    std::wstring helper = SC_DRIVER_SETUP;
    if (GetFileAttributesW(helper.c_str()) == INVALID_FILE_ATTRIBUTES) {
        wchar_t dir[MAX_PATH] = {};
        if (ModuleDirectory(dir, MAX_PATH)) {
            std::wstring beside = std::wstring(dir) + L"\\shieldcord_driver_setup.exe";
            if (GetFileAttributesW(beside.c_str()) != INVALID_FILE_ATTRIBUTES) helper = beside;
        }
    }

    if (GetFileAttributesW(helper.c_str()) != INVALID_FILE_ATTRIBUTES) {
        std::wstring cmd = L"\"" + helper + L"\" --uninstall";
        STARTUPINFOW si = { sizeof(si) };
        PROCESS_INFORMATION pi = {};
        if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                           CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        {
            WaitForSingleObject(pi.hProcess, 60000);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            return;
        }
    }

    // Helper missing (partial install, or the user deleted it). Do the minimum
    // that stops the driver from staying resident after an uninstall.
    wprintf(L"[!] Driver helper not found — unloading directly.\n");
    StopAndDeleteService(SC_FILTER_NAME);

    wchar_t sysRoot[MAX_PATH] = {};
    GetSystemDirectoryW(sysRoot, MAX_PATH);
    std::wstring sysDst = std::wstring(sysRoot) + L"\\drivers\\" + SC_FILTER_SYS;
    if (DeleteFileW(sysDst.c_str())) {
        wprintf(L"[+] Driver binary removed.\n");
    } else if (GetFileAttributesW(sysDst.c_str()) != INVALID_FILE_ATTRIBUTES) {
        MoveFileExW(sysDst.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
        wprintf(L"[!] Driver binary in use — it will be removed on the next boot.\n");
    }
}

// -------------------------------------------------------
int wmain(int argc, wchar_t* argv[]) {
    wprintf(L"\n=== ShieldCord Uninstaller ===\n\n");

    // --keep-data: used by the Setup uninstaller when the user chose to keep
    // their logs and configuration. Without it, ProgramData is removed too.
    bool keepData = false;
    for (int i = 1; i < argc; i++) {
        if (wcscmp(argv[i], L"--keep-data") == 0) keepData = true;
    }

    if (!sci::RequireElevation(L"The ShieldCord uninstaller")) return 1;

    /*
     * Where this program lives is where the product lives. Read from the module
     * rather than assumed, because the installer wizard lets the user choose the
     * folder — a fixed C:\Program Files\ShieldCord would leave a service, its
     * files and its --restore helper behind on any other path.
     */
    wchar_t ownDir[MAX_PATH] = {};
    if (!sci::ModuleDirectory(ownDir, MAX_PATH)) ownDir[0] = L'\0';

    // ── 1. stop the engine first ────────────────────────────
    StopAndDeleteService(SC_SVC_NAME);

    // ── 2. crash-safety: legacy DACL restore ────────────────
    // An OLD build could leave the user's Discord/browser folders locked with
    // SYSTEM-only DACLs. Blindly deleting the data directory would destroy
    // dacl_backup.json — the only record of the original permissions — leaving
    // the user permanently locked out of their own data. Restore first, using
    // the service exe (which has --restore) while it still exists.
    if (GetFileAttributesW(SC_DACL_BACKUP) != INVALID_FILE_ATTRIBUTES) {
        wprintf(L"[*] A legacy DACL backup is present — restoring original permissions first...\n");

        // Found relative to where THIS program lives — see ownDir above.
        std::wstring svcPath = std::wstring(ownDir) + L"\\shieldcord_svc.exe";

        if (GetFileAttributesW(svcPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
            std::wstring cmd = L"\"" + svcPath + L"\" --restore";
            STARTUPINFOW si = { sizeof(si) };
            PROCESS_INFORMATION pi = {};
            if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                               CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
            {
                WaitForSingleObject(pi.hProcess, 15000);
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);
                wprintf(L"[+] Restore attempted.\n");
            } else {
                wprintf(L"[!] Could not launch the restore (error %lu).\n", GetLastError());
            }
        } else {
            wprintf(L"[!] Service exe not found — cannot auto-restore.\n");
        }
    }

    // ── 3. kernel driver ────────────────────────────────────
    RemoveDriver();

    // ── 4. program files ────────────────────────────────────
    // Our own directory. This program's exe is locked while it runs, so it
    // survives — which is what Inno's uninstaller expects, since it removes
    // {app} itself afterwards.
    wprintf(L"[*] Removing program files...\n");
    RemoveDirRecursive(ownDir);

    // ── 5. data files ───────────────────────────────────────
    // NEVER destroy a surviving DACL backup. If it is still here the restore
    // did not complete, and that file is the user's only recovery path.
    if (GetFileAttributesW(SC_DACL_BACKUP) != INVALID_FILE_ATTRIBUTES) {
        wprintf(L"\n[!] A DACL backup is still present at:\n    %s\n", SC_DACL_BACKUP);
        wprintf(L"[!] Your protected folders may still be locked. The data directory\n");
        wprintf(L"    was PRESERVED so you can recover. Restore the original permissions\n");
        wprintf(L"    (a ShieldCord build's --restore), then delete:\n");
        wprintf(L"    %s\n\n", SC_DATA_DIR);
        wprintf(L"[+] Service and driver removed; data preserved for recovery.\n\n");
        return 0;
    }

    if (keepData) {
        wprintf(L"[i] Keeping logs and configuration at %s (--keep-data).\n", SC_DATA_DIR);
        wprintf(L"\n[+] ShieldCord uninstalled; data kept.\n\n");
        return 0;
    }

    wprintf(L"[*] Removing logs and configuration...\n");
    RemoveDirRecursive(SC_DATA_DIR);

    wprintf(L"\n[+] ShieldCord uninstalled.\n\n");
    return 0;
}
