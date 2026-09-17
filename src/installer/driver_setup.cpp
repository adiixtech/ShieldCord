// ============================================================
// ShieldCord — driver_setup.cpp
// Installs / uninstalls the ShieldCordFilter minifilter driver.
//
// Why this exists as a program rather than the .bat it replaces:
//   install_driver.bat shelled out to rundll32 setupapi, pnputil, sc and reg.
//   That works by hand but cannot be shipped — it has no error handling, it
//   silently tried three different installation methods in a row, and it left
//   the driver registered even when the load failed. This does the same work
//   through documented APIs and reports what actually happened.
//
// Usage:
//   shieldcord_driver_setup.exe --install     (payload found next to the exe)
//   shieldcord_driver_setup.exe --uninstall
// Exit codes: 0 success, 1 failure, 2 not elevated, 3 bad usage.
// ============================================================
#include "installer_common.h"

#include <fltUser.h>       // FilterLoad / FilterUnload
#include <wincrypt.h>
#include <string>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "fltlib.lib")

using sci::EnsureDir;
using sci::FindPayload;
using sci::RequireElevation;

// -------------------------------------------------------
// Registry helpers
// -------------------------------------------------------
static bool SetRegValue(HKEY root, const std::wstring& subkey,
                        const wchar_t* name, DWORD type,
                        const BYTE* data, DWORD bytes)
{
    HKEY hKey = nullptr;
    LSTATUS st = RegCreateKeyExW(root, subkey.c_str(), 0, nullptr,
                                 REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr,
                                 &hKey, nullptr);
    if (st != ERROR_SUCCESS) return false;

    st = RegSetValueExW(hKey, name, 0, type, data, bytes);
    RegCloseKey(hKey);
    return st == ERROR_SUCCESS;
}

static bool SetRegDword(HKEY root, const std::wstring& subkey, const wchar_t* name, DWORD v) {
    return SetRegValue(root, subkey, name, REG_DWORD,
                       reinterpret_cast<const BYTE*>(&v), sizeof(v));
}

static bool SetRegSz(HKEY root, const std::wstring& subkey, const wchar_t* name,
                     const wchar_t* v) {
    return SetRegValue(root, subkey, name, REG_SZ,
                       reinterpret_cast<const BYTE*>(v),
                       (DWORD)((wcslen(v) + 1) * sizeof(wchar_t)));
}

// -------------------------------------------------------
// Certificate store
// -------------------------------------------------------

/// Loads the certificate from a .cer file (DER or PKCS#7).
static bool LoadCertificate(const wchar_t* cerPath, PCCERT_CONTEXT* out) {
    *out = nullptr;

    HCERTSTORE hStore = nullptr;
    HCRYPTMSG  hMsg   = nullptr;
    DWORD enc = 0, ct = 0, ft = 0;

    if (!CryptQueryObject(CERT_QUERY_OBJECT_FILE, cerPath,
                          CERT_QUERY_CONTENT_FLAG_CERT |
                          CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED,
                          CERT_QUERY_FORMAT_FLAG_ALL, 0,
                          &enc, &ct, &ft, &hStore, &hMsg, nullptr))
    {
        return false;
    }

    PCCERT_CONTEXT found = CertEnumCertificatesInStore(hStore, nullptr);
    if (found) *out = CertDuplicateCertificateContext(found);

    if (hMsg)   CryptMsgClose(hMsg);
    if (hStore) CertCloseStore(hStore, 0);
    return *out != nullptr;
}

static bool AddCertToStore(PCCERT_CONTEXT cert, LPCWSTR storeName) {
    HCERTSTORE h = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, NULL,
                                 CERT_SYSTEM_STORE_LOCAL_MACHINE, storeName);
    if (!h) return false;

    BOOL ok = CertAddCertificateContextToStore(h, cert,
                                               CERT_STORE_ADD_REPLACE_EXISTING, nullptr);
    CertCloseStore(h, 0);
    return ok != FALSE;
}

static bool RemoveCertFromStore(PCCERT_CONTEXT cert, LPCWSTR storeName) {
    HCERTSTORE h = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, NULL,
                                 CERT_SYSTEM_STORE_LOCAL_MACHINE, storeName);
    if (!h) return false;

    // The context we hold is a copy; find the store's own before deleting.
    // CertDeleteCertificateFromStore frees the context it is given.
    PCCERT_CONTEXT found = CertFindCertificateInStore(
        h, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0,
        CERT_FIND_EXISTING, cert, nullptr);

    bool ok = false;
    if (found) ok = CertDeleteCertificateFromStore(found) != FALSE;

    CertCloseStore(h, 0);
    return ok;
}

// -------------------------------------------------------
// Service helpers
// -------------------------------------------------------
static bool IsServiceRunning(const wchar_t* name) {
    SC_HANDLE hScm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!hScm) return false;

    SC_HANDLE hSvc = OpenServiceW(hScm, name, SERVICE_QUERY_STATUS);
    bool running = false;
    if (hSvc) {
        SERVICE_STATUS ss = {};
        if (QueryServiceStatus(hSvc, &ss)) running = (ss.dwCurrentState == SERVICE_RUNNING);
        CloseServiceHandle(hSvc);
    }
    CloseServiceHandle(hScm);
    return running;
}

static void StopAndDeleteService(const wchar_t* name) {
    SC_HANDLE hScm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!hScm) return;

    SC_HANDLE hSvc = OpenServiceW(hScm, name, SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE);
    if (hSvc) {
        SERVICE_STATUS ss = {};
        ControlService(hSvc, SERVICE_CONTROL_STOP, &ss);

        for (int i = 0; i < 20; i++) {                 // up to ~4 s
            Sleep(200);
            if (!QueryServiceStatus(hSvc, &ss)) break;
            if (ss.dwCurrentState == SERVICE_STOPPED) break;
        }

        DeleteService(hSvc);
        CloseServiceHandle(hSvc);
    }
    CloseServiceHandle(hScm);
}

// -------------------------------------------------------
// Install
// -------------------------------------------------------
static int InstallDriver() {
    wchar_t sysSrc[MAX_PATH] = {};
    wchar_t cerSrc[MAX_PATH] = {};

    if (!FindPayload(SC_FILTER_SYS, sysSrc, MAX_PATH)) {
        wprintf(L"[-] %s not found next to this program or in .\\driver\\.\n", SC_FILTER_SYS);
        return 1;
    }

    bool haveCert = FindPayload(SC_FILTER_CER, cerSrc, MAX_PATH);

    // ── 1. driver binary → System32\drivers ─────────────────
    wchar_t sysRoot[MAX_PATH] = {};
    GetSystemDirectoryW(sysRoot, MAX_PATH);

    std::wstring sysDst = std::wstring(sysRoot) + L"\\drivers\\" + SC_FILTER_SYS;

    wprintf(L"[*] Installing driver binary...\n");
    wprintf(L"    %s\n    -> %s\n", sysSrc, sysDst.c_str());

    if (!CopyFileW(sysSrc, sysDst.c_str(), FALSE)) {
        DWORD err = GetLastError();
        // A running filter holds its .sys open; stop it and retry once.
        if (err == ERROR_SHARING_VIOLATION || err == ERROR_ACCESS_DENIED) {
            wprintf(L"[*] Driver file in use — unloading the current filter first...\n");
            FilterUnload(SC_FILTER_NAME);
            StopAndDeleteService(SC_FILTER_NAME);
            Sleep(500);
            if (!CopyFileW(sysSrc, sysDst.c_str(), FALSE)) {
                wprintf(L"[-] Copy failed. Error: %lu\n", GetLastError());
                return 1;
            }
        } else {
            wprintf(L"[-] Copy failed. Error: %lu\n", err);
            return 1;
        }
    }
    wprintf(L"[+] Driver binary installed.\n");

    // ── 2. certificate into the machine stores ──────────────
    if (haveCert) {
        PCCERT_CONTEXT cert = nullptr;
        if (LoadCertificate(cerSrc, &cert)) {
            bool root = AddCertToStore(cert, L"Root");
            bool pub  = AddCertToStore(cert, L"TrustedPublisher");
            CertFreeCertificateContext(cert);
            wprintf(root && pub
                ? L"[+] Certificate trusted (Root + TrustedPublisher).\n"
                : L"[!] Certificate could not be added to every store.\n");
        } else {
            wprintf(L"[!] %s could not be read — relying on test signing.\n", SC_FILTER_CER);
        }
    } else {
        wprintf(L"[!] %s not found — relying on test signing mode.\n", SC_FILTER_CER);
    }

    // ── 3. service registration ─────────────────────────────
    wprintf(L"[*] Registering the filter service...\n");

    // Remove any previous registration first, so an altitude or group change
    // actually takes effect instead of silently keeping the old values.
    StopAndDeleteService(SC_FILTER_NAME);
    Sleep(300);

    SC_HANDLE hScm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!hScm) {
        wprintf(L"[-] OpenSCManager failed: %lu\n", GetLastError());
        return 1;
    }

    SC_HANDLE hSvc = CreateServiceW(
        hScm,
        SC_FILTER_NAME,
        SC_FILTER_NAME,
        SERVICE_ALL_ACCESS,
        SERVICE_FILE_SYSTEM_DRIVER,
        SERVICE_DEMAND_START,            // the engine service loads it at boot
        SERVICE_ERROR_NORMAL,
        sysDst.c_str(),
        SC_FILTER_GROUP,                 // load-order group
        nullptr,                         // no tag id
        L"FltMgr",                       // dependency: Filter Manager
        nullptr, nullptr);

    if (!hSvc) {
        DWORD err = GetLastError();
        wprintf(L"[-] CreateService failed: %lu\n", err);
        CloseServiceHandle(hScm);
        return 1;
    }
    CloseServiceHandle(hSvc);
    CloseServiceHandle(hScm);

    // The Filter Manager needs these to recognise the service as a filter.
    // CreateService cannot express them, so they are written directly.
    const std::wstring key = std::wstring(L"System\\CurrentControlSet\\Services\\") + SC_FILTER_NAME;
    const std::wstring inst = key + L"\\Instances";
    const std::wstring instCfg = inst + L"\\" + SC_FILTER_INSTANCE;

    SetRegDword(HKEY_LOCAL_MACHINE, key, L"SupportedFeatures", 3);
    SetRegSz   (HKEY_LOCAL_MACHINE, inst, L"DefaultInstance", SC_FILTER_INSTANCE);
    SetRegSz   (HKEY_LOCAL_MACHINE, instCfg, L"Altitude", SC_FILTER_ALTITUDE);
    SetRegDword(HKEY_LOCAL_MACHINE, instCfg, L"Flags", 0);

    wprintf(L"[+] Filter service registered at altitude %s.\n", SC_FILTER_ALTITUDE);

    // ── 4. load it ──────────────────────────────────────────
    wprintf(L"[*] Loading the filter...\n");
    HRESULT hr = FilterLoad(SC_FILTER_NAME);

    if (FAILED(hr)) {
        // Fall back to the SCM, which is what `sc start` does.
        SC_HANDLE hScm2 = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        if (hScm2) {
            SC_HANDLE hSvc2 = OpenServiceW(hScm2, SC_FILTER_NAME, SERVICE_START);
            if (hSvc2) {
                StartServiceW(hSvc2, 0, nullptr);
                CloseServiceHandle(hSvc2);
            }
            CloseServiceHandle(hScm2);
        }
    }

    for (int i = 0; i < 25 && !IsServiceRunning(SC_FILTER_NAME); i++) Sleep(200);

    if (!IsServiceRunning(SC_FILTER_NAME)) {
        wprintf(L"\n[-] The driver was registered but did NOT load.\n");
        if (!sci::IsTestSigningEnabled()) {
            wprintf(L"[!] Test signing is off and this driver is not\n");
            wprintf(L"    attestation-signed by Microsoft, so Windows refused it.\n");
            wprintf(L"    During development: bcdedit /set testsigning on, then reboot.\n");
        } else {
            wprintf(L"[!] Test signing is on, so the refusal is something else —\n");
            wprintf(L"    check Event Viewer -> System for a CodeIntegrity entry.\n");
        }
        return 1;
    }

    wprintf(L"[+] ShieldCordFilter is loaded and running.\n");
    return 0;
}

// -------------------------------------------------------
// Uninstall
// -------------------------------------------------------
static int UninstallDriver() {
    bool didSomething = false;

    // ── 1. unload ───────────────────────────────────────────
    bool wasRunning = IsServiceRunning(SC_FILTER_NAME);
    if (wasRunning) {
        wprintf(L"[*] Unloading the filter...\n");
        FilterUnload(SC_FILTER_NAME);

        for (int i = 0; i < 25 && IsServiceRunning(SC_FILTER_NAME); i++) Sleep(200);

        if (IsServiceRunning(SC_FILTER_NAME)) {
            wprintf(L"[*] Still loaded — asking the Service Control Manager...\n");
            StopAndDeleteService(SC_FILTER_NAME);
        }
        didSomething = true;
    }

    // ── 2. service registration ─────────────────────────────
    wprintf(L"[*] Removing the filter service...\n");
    StopAndDeleteService(SC_FILTER_NAME);

    const std::wstring key = std::wstring(L"System\\CurrentControlSet\\Services\\") + SC_FILTER_NAME;
    if (RegDeleteTreeW(HKEY_LOCAL_MACHINE, key.c_str()) == ERROR_SUCCESS) {
        wprintf(L"[+] Service registration removed.\n");
        didSomething = true;
    }

    // ── 3. driver binary ────────────────────────────────────
    wchar_t sysRoot[MAX_PATH] = {};
    GetSystemDirectoryW(sysRoot, MAX_PATH);
    std::wstring sysDst = std::wstring(sysRoot) + L"\\drivers\\" + SC_FILTER_SYS;

    if (GetFileAttributesW(sysDst.c_str()) != INVALID_FILE_ATTRIBUTES) {
        // Only remove the copy Windows is not using. If the filter is somehow
        // still loaded this fails harmlessly, which is the correct outcome.
        if (DeleteFileW(sysDst.c_str())) {
            wprintf(L"[+] Driver binary removed.\n");
            didSomething = true;
        } else {
            wprintf(L"[!] Driver binary is still in use — it will be removed on the next boot.\n");
            MoveFileExW(sysDst.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
        }
    }

    // ── 4. certificate ──────────────────────────────────────
    wchar_t cerSrc[MAX_PATH] = {};
    if (FindPayload(SC_FILTER_CER, cerSrc, MAX_PATH)) {
        PCCERT_CONTEXT cert = nullptr;
        if (LoadCertificate(cerSrc, &cert)) {
            bool root = RemoveCertFromStore(cert, L"Root");
            bool pub  = RemoveCertFromStore(cert, L"TrustedPublisher");
            CertFreeCertificateContext(cert);
            if (root || pub) {
                wprintf(L"[+] Certificate removed from the machine stores.\n");
                didSomething = true;
            }
        }
    } else {
        wprintf(L"[i] Certificate file not present — nothing to remove from the stores.\n");
    }

    if (!didSomething) wprintf(L"[i] Nothing was installed.\n");
    wprintf(L"[+] Driver uninstall complete.\n");
    return 0;
}

// -------------------------------------------------------
int wmain(int argc, wchar_t* argv[]) {
    wprintf(L"\n=== ShieldCord Driver Setup ===\n\n");

    if (argc < 2) {
        wprintf(L"Usage: shieldcord_driver_setup.exe --install | --uninstall\n");
        return 3;
    }

    if (!RequireElevation(L"shieldcord_driver_setup.exe")) return 2;

    std::wstring mode = argv[1];
    if (mode == L"--install"   || mode == L"-i") return InstallDriver();
    if (mode == L"--uninstall" || mode == L"-u") return UninstallDriver();

    wprintf(L"Unknown option: %s\n", argv[1]);
    wprintf(L"Usage: shieldcord_driver_setup.exe --install | --uninstall\n");
    return 3;
}
