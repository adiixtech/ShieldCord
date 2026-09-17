// ============================================================
// ShieldCord — process_notifier.cpp
// Real-time process-creation events via WMI Win32_ProcessStartTrace.
// ============================================================
#include "process_notifier.h"
#include "logger.h"
#include <comdef.h>
#include <wbemidl.h>

#pragma comment(lib, "wbemuuid.lib")

namespace sc {

ProcessNotifier& ProcessNotifier::Instance() {
    static ProcessNotifier inst;
    return inst;
}

bool ProcessNotifier::Start(Callback cb) {
    if (m_running.load()) return true;   // already listening

    m_running = true;
    m_thread  = std::thread([this, cb]() { Loop(cb); });
    return true;
}

void ProcessNotifier::Stop() {
    m_running = false;
    if (m_thread.joinable()) m_thread.join();
}

void ProcessNotifier::Loop(Callback cb) {
    bool comInited = false;

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr)) {
        comInited = true;
    } else {
        // RPC_E_CHANGED_MODE etc. — another thread already set a different
        // apartment model. Try to continue anyway; ConnectServer usually works.
        LOG_WARN(L"Notifier", L"CoInitializeEx returned 0x" +
                 [&] { wchar_t b[16]; swprintf_s(b, L"%08lx", (unsigned long)hr); return std::wstring(b); }() +
                 L" — continuing anyway.");
    }

    IWbemLocator* pLoc = nullptr;
    hr = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
                          IID_IWbemLocator, reinterpret_cast<LPVOID*>(&pLoc));
    if (FAILED(hr) || !pLoc) {
        LOG_WARN(L"Notifier", L"CoCreateInstance(WbemLocator) failed — falling back to polling.");
        if (comInited) CoUninitialize();
        m_running = false;
        return;
    }

    IWbemServices* pSvc = nullptr;
    hr = pLoc->ConnectServer(_bstr_t(L"ROOT\\CIMV2"),   // namespace
                             nullptr,                   // strUser (current)
                             nullptr,                   // strPassword
                             nullptr,                   // strLocale
                             0,                         // lSecurityFlags
                             nullptr,                   // strAuthority
                             nullptr,                   // pCtx
                             &pSvc);
    if (FAILED(hr) || !pSvc) {
        LOG_WARN(L"Notifier", L"ConnectServer failed — falling back to polling.");
        pLoc->Release();
        if (comInited) CoUninitialize();
        m_running = false;
        return;
    }

    CoSetProxyBlanket(pSvc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
                      RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE,
                      nullptr, EOAC_NONE);

    IEnumWbemClassObject* pEnum = nullptr;
    hr = pSvc->ExecNotificationQuery(
        _bstr_t(L"WQL"),
        _bstr_t(L"SELECT * FROM Win32_ProcessStartTrace"),
        WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
        nullptr, &pEnum);
    if (FAILED(hr) || !pEnum) {
        // Win32_ProcessStartTrace requires elevation (it is an audited
        // trace). Without admin we cannot get real-time events.
        LOG_WARN(L"Notifier", L"Win32_ProcessStartTrace unavailable (hr=0x" +
                 [&] { wchar_t b[16]; swprintf_s(b, L"%08lx", (unsigned long)hr); return std::wstring(b); }() +
                 L", needs elevation) — falling back to polling only.");
        pSvc->Release();
        pLoc->Release();
        if (comInited) CoUninitialize();
        m_running = false;
        return;
    }

    LOG_INFO(L"Notifier", L"Real-time process-start events active (Win32_ProcessStartTrace).");

    while (m_running.load()) {
        IWbemClassObject* pObj = nullptr;
        ULONG returned = 0;

        // 1-second timeout keeps Stop() responsive
        hr = pEnum->Next(1000, 1, &pObj, &returned);
        if (hr == WBEM_S_TIMEDOUT) continue;
        if (FAILED(hr)) break;           // stream broken — exit, poll takes over
        if (returned == 0 || !pObj) continue;

        VARIANT vPid, vName;
        VariantInit(&vPid);
        VariantInit(&vName);
        if (SUCCEEDED(pObj->Get(L"ProcessID", 0, &vPid, nullptr, nullptr)) &&
            SUCCEEDED(pObj->Get(L"ProcessName", 0, &vName, nullptr, nullptr)))
        {
            DWORD pid = static_cast<DWORD>(vPid.uintVal);
            std::wstring name = (vName.bstrVal ? vName.bstrVal : L"");
            if (pid != 0 && !name.empty()) {
                try { cb(pid, name); } catch (...) { /* never kill the listener */ }
            }
        }
        VariantClear(&vPid);
        VariantClear(&vName);
        pObj->Release();
    }

    pEnum->Release();
    pSvc->Release();
    pLoc->Release();
    if (comInited) CoUninitialize();
    LOG_INFO(L"Notifier", L"Real-time process-start listener stopped.");
}

} // namespace sc
