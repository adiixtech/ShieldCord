#pragma once
// ============================================================
// ShieldCord — installer_common.h
// Shared constants + helpers for install/uninstall/driver_setup.
//
// These three programs must agree on every path and service name; before this
// header each one hard-coded its own copy, which is how the installer and the
// uninstaller drift apart.
// ============================================================
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlwapi.h>
#include <stdio.h>

// ── engine service ───────────────────────────────────────────
#define SC_SVC_NAME      L"ShieldCordSvc"
#define SC_SVC_DISPLAY   L"ShieldCord Token Protector"
#define SC_SVC_DESC      L"Protects Discord and browser tokens from grabber malware."

// ── kernel minifilter ────────────────────────────────────────
#define SC_FILTER_NAME     L"ShieldCordFilter"
#define SC_FILTER_ALTITUDE L"385200"
#define SC_FILTER_GROUP    L"FSFilter Activity Monitor"
#define SC_FILTER_INSTANCE L"ShieldCordFilter Instance"
#define SC_FILTER_SYS      L"shieldcord_filter.sys"
#define SC_FILTER_CER      L"ShieldCordCert.cer"
#define SC_FILTER_PORT     L"\\ShieldCordFilterPort"

// ── layout ───────────────────────────────────────────────────
//
// DO NOT USE SC_INSTALL_DIR OR SC_SVC_EXE TO LOCATE ANYTHING.
//
// These are the DEFAULT install location, which is all they have ever been able
// to describe. The Inno wizard lets the user choose the folder, so the real
// location is wherever the running installer or uninstaller happens to live —
// ask sci::ModuleDirectory() for it.
//
// They were used to register the service, which produced an install that copied
// the files to one folder, registered the service against another, reported
// success and had no engine at all. Kept only so the default path is written
// down somewhere.
#define SC_INSTALL_DIR   L"C:\\Program Files\\ShieldCord"
#define SC_DATA_DIR      L"C:\\ProgramData\\ShieldCord"
#define SC_LOG_DIR       L"C:\\ProgramData\\ShieldCord\\logs"
#define SC_SVC_EXE       L"C:\\Program Files\\ShieldCord\\shieldcord_svc.exe"
#define SC_UI_EXE        L"C:\\Program Files\\ShieldCord\\ui\\shieldcordui.exe"
#define SC_DRIVER_SETUP  L"C:\\Program Files\\ShieldCord\\shieldcord_driver_setup.exe"

// Where the installer stages the driver payload before driver_setup consumes it.
#define SC_DRIVER_STAGE  L"C:\\Program Files\\ShieldCord\\driver"

// Legacy crash-recovery file from the removed DACL layer.
#define SC_DACL_BACKUP   L"C:\\ProgramData\\ShieldCord\\dacl_backup.json"

namespace sci {

// ── elevation ────────────────────────────────────────────────

inline bool IsRunningElevated() {
    BOOL isAdmin = FALSE;
    SID_IDENTIFIER_AUTHORITY auth = SECURITY_NT_AUTHORITY;
    PSID adminSid = nullptr;
    if (AllocateAndInitializeSid(&auth, 2, SECURITY_BUILTIN_DOMAIN_RID,
                                 DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &adminSid)) {
        CheckTokenMembership(nullptr, adminSid, &isAdmin);
        FreeSid(adminSid);
    }
    return isAdmin != FALSE;
}

/// Prints a standard refusal and returns false when not elevated.
inline bool RequireElevation(const wchar_t* programName) {
    if (IsRunningElevated()) return true;
    wprintf(L"[!] %s must be run as Administrator.\n", programName);
    return false;
}

// ── filesystem ───────────────────────────────────────────────

inline bool EnsureDir(const wchar_t* path) {
    if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) return true;
    return CreateDirectoryW(path, nullptr) != 0;
}

/// Directory this executable lives in, without a trailing backslash.
inline bool ModuleDirectory(wchar_t* out, size_t cch) {
    if (!GetModuleFileNameW(nullptr, out, (DWORD)cch)) return false;
    PathRemoveFileSpecW(out);
    return true;
}

/// Finds a payload file next to the exe, or in ..\driver / driver\.
/// The installer stages driver files in {app}\driver; a dev run usually has
/// them beside the binary. Checking both keeps one binary usable in each.
inline bool FindPayload(const wchar_t* fileName, wchar_t* out, size_t cch) {
    wchar_t dir[MAX_PATH] = {};
    if (!ModuleDirectory(dir, MAX_PATH)) return false;

    const wchar_t* candidates[] = { L"%s\\%s", L"%s\\driver\\%s", L"%s\\..\\driver\\%s" };
    for (const wchar_t* fmt : candidates) {
        swprintf_s(out, cch, fmt, dir, fileName);
        if (GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES) return true;
    }
    return false;
}

// ── boot configuration ───────────────────────────────────────

/// Test-signing lets a self-signed driver load. It must be ON during
/// development and WILL be off on a customer machine (where the driver has to
/// be attestation-signed instead). We warn rather than refuse, so the same
/// installer works in both worlds.
inline bool IsTestSigningEnabled() {
    wchar_t out[4096] = {};
    // `bcdedit` is the only supported way to read this from user mode.
    FILE* f = _wpopen(L"bcdedit /enum {current} 2>nul", L"r");
    if (!f) return false;

    bool enabled = false;
    while (fgetws(out, _countof(out), f)) {
        if (wcsstr(out, L"testsigning") && (wcsstr(out, L"Yes") || wcsstr(out, L"yes"))) {
            enabled = true;
            break;
        }
    }
    _pclose(f);
    return enabled;
}

} // namespace sci
