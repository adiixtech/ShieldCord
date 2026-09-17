/*
 * driver_client.cpp — ShieldCord Service: Kernel Driver Client
 *
 * Connects to \ShieldCordFilterPort and sends/receives messages.
 *
 * FAIL-OPEN DESIGN:
 *   If Connect() returns false (driver not installed), every method
 *   becomes a no-op. The service runs in DACL-only mode transparently.
 *
 * INITIAL TRUSTED PID LIST:
 *   At startup, SendInitialTrustedPids() scans the running process list,
 *   applies the same ProcessValidator::FindTrustedApp check used by
 *   AppWatcher, and sends AddTrustedPid for each already-running
 *   trusted process. This handles the case where Discord was open
 *   before the service started.
 */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include "driver_client.h"
#include "process_validator.h"
#include "logger.h"
#include "utils.h"
#include "../shared/whitelist.h"
#include "../shared/token_paths.h"
#include <set>
#include <algorithm>
#include <cwchar>     // wcsnlen — bound the fixed-size kernel string fields
#include <cstdlib>    // _countof
#include <cstring>    // memcpy
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// Count of zeroed / spurious alert messages delivered on the port (diagnostic).
// A real block alert always has a non-zero PID and a path; any message that
// lacks both is not a genuine block and must not be counted or broadcast.
static int g_spuriousAlerts = 0;

namespace sc {

DriverClient& DriverClient::Instance() {
    static DriverClient inst;
    return inst;
}

/* ── Connect ─────────────────────────────────────────────────── */

bool DriverClient::Connect() {
    if (m_connected.load()) return true;

    HRESULT hr = FilterConnectCommunicationPort(
        SC_FILTER_PORT_NAME,
        0,
        NULL,
        0,
        NULL,
        &m_hPort);

    if (FAILED(hr)) {
        if (HRESULT_CODE(hr) == ERROR_FILE_NOT_FOUND ||
            HRESULT_CODE(hr) == ERROR_PATH_NOT_FOUND) {
            /* Driver not installed — DACL-only mode */
            LOG_INFO(L"DriverClient", L"Kernel driver not installed. Running in DACL-only mode.");
        } else {
            LOG_WARN(L"DriverClient",
                     L"FilterConnectCommunicationPort failed: HRESULT=0x" +
                     std::to_wstring((unsigned long)hr) +
                     L". Running in DACL-only mode.");
        }
        m_hPort = INVALID_HANDLE_VALUE;
        return false;
    }

    m_connected.store(true);
    LOG_INFO(L"DriverClient", L"Connected to kernel driver at " SC_FILTER_PORT_NAME L".");
    return true;
}

/* ── Disconnect ──────────────────────────────────────────────── */

void DriverClient::Disconnect() {
    /*
     * StopAlertReceiver() closes the port handle (which unblocks the receiver's
     * synchronous FilterGetMessage) and then joins the thread. Ordering matters:
     * closing before the join is what prevents the deadlock — joining first would
     * wait forever on a thread parked inside FilterGetMessage.
     */
    StopAlertReceiver();

    /* Defensive: if the receiver was never started, StopAlertReceiver still
     * closed m_hPort; this only fires when Connect() succeeded but no receiver
     * was attached. */
    if (m_hPort != INVALID_HANDLE_VALUE) {
        CloseHandle(m_hPort);
        m_hPort = INVALID_HANDLE_VALUE;
    }
    m_connected.store(false);
    LOG_INFO(L"DriverClient", L"Disconnected from kernel driver.");
}

/* ── Internal message send ───────────────────────────────────── */

bool DriverClient::SendMessage(const void* msg, DWORD msgSize) {
    if (!m_connected.load() || m_hPort == INVALID_HANDLE_VALUE) return false;

    std::lock_guard<std::mutex> lk(m_sendMutex);

    DWORD bytesReturned = 0;
    HRESULT hr = FilterSendMessage(
        m_hPort,
        const_cast<void*>(msg),
        msgSize,
        NULL,   /* no reply buffer */
        0,
        &bytesReturned);

    if (FAILED(hr)) {
        LOG_WARN(L"DriverClient",
                 L"FilterSendMessage failed: 0x" + std::to_wstring((unsigned long)hr));
        return false;
    }
    return true;
}

/* ── AddTrustedPid / RemoveTrustedPid ────────────────────────── */

void DriverClient::AddTrustedPid(DWORD pid, const std::wstring& imageName) {
    if (!m_connected.load()) return;

    SC_TRUSTED_PID_MSG msg = {};
    msg.Type = ScMsgAddTrustedPid;
    msg.Pid  = pid;

    /* Copy image name safely */
    size_t copyLen = imageName.size() < (size_t)63 ? imageName.size() : (size_t)63;
    wcsncpy_s(msg.ImageName, 64, imageName.c_str(), copyLen);

    if (SendMessage(&msg, sizeof(msg)))
        LOG_INFO(L"DriverClient", L"AddTrustedPid: pid=" + std::to_wstring(pid) +
                 L" img=" + imageName);
}

void DriverClient::RemoveTrustedPid(DWORD pid) {
    if (!m_connected.load()) return;

    SC_TRUSTED_PID_MSG msg = {};
    msg.Type = ScMsgRemoveTrustedPid;
    msg.Pid  = pid;

    if (SendMessage(&msg, sizeof(msg)))
        LOG_INFO(L"DriverClient", L"RemoveTrustedPid: pid=" + std::to_wstring(pid));
}

/* ── SendInitialTrustedPids ──────────────────────────────────── */

void DriverClient::SendInitialTrustedPids() {
    if (!m_connected.load()) return;

    /*
     * Scan the process list. For any process whose name matches a trusted
     * app and passes signature verification, send AddTrustedPid.
     *
     * This handles the case where Discord was already running when the
     * service started (e.g. service started mid-session).
     */
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;

    /* Collect names from the whitelist for quick pre-filtering */
    std::set<std::wstring> trustedNames;
    for (const auto& ta : TRUSTED_APPS)
        trustedNames.insert(utils::ToLower(ta.processName));

    int count = 0;
    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);

    if (Process32FirstW(snap, &pe)) {
        do {
            std::wstring nameLow = utils::ToLower(std::wstring(pe.szExeFile));
            if (trustedNames.count(nameLow) == 0) continue;

            /* Run the same signature verification used by AppWatcher */
            const TrustedApp* ta = ProcessValidator::FindTrustedApp(pe.th32ProcessID);
            if (!ta) continue;

            AddTrustedPid(pe.th32ProcessID, std::wstring(pe.szExeFile));
            count++;
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);

    LOG_INFO(L"DriverClient",
             L"Sent initial trusted PID list: " + std::to_wstring(count) + L" process(es).");
}

/* ── TrustSelf ───────────────────────────────────────────────── */

void DriverClient::TrustSelf() {
    if (!m_connected.load()) return;

    /* The engine itself is ShieldCord — always trusted (owns the decoy folder). */
    AddTrustedPid(GetCurrentProcessId(), L"shieldcord_svc.exe");

    /* Also trust the tray UI if it is running (it reads the decoy folder too). */
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe = {};
        pe.dwSize = sizeof(pe);
        if (Process32FirstW(snap, &pe)) {
            do {
                std::wstring nameLow = utils::ToLower(std::wstring(pe.szExeFile));
                if (nameLow == L"shieldcordui.exe")
                    AddTrustedPid(pe.th32ProcessID, std::wstring(pe.szExeFile));
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
    }

    LOG_INFO(L"DriverClient", L"Trusted ShieldCord's own process(es).");
}

/* ── SendProtectedPaths ──────────────────────────────────────── */

void DriverClient::SendProtectedPaths() {
    if (!m_connected.load()) return;

    const auto& suffixes = PROTECTED_PATH_SUFFIXES;

    /*
     * Layout the driver's ScMsgSetProtectedPaths handler expects:
     *   [SC_MSG_TYPE header][path1\0][path2\0]...[pathN\0][\0]
     * The final extra NUL (double-null) terminates the array loop.
     */
    size_t wchars = sizeof(SC_MSG_TYPE) / sizeof(wchar_t);
    for (const auto& s : suffixes) wchars += s.size() + 1;
    wchars += 1;

    std::vector<wchar_t> buf(wchars, L'\0');
    SC_MSG_TYPE type = ScMsgSetProtectedPaths;
    memcpy(buf.data(), &type, sizeof(SC_MSG_TYPE));

    wchar_t* p = buf.data() + (sizeof(SC_MSG_TYPE) / sizeof(wchar_t));
    for (const auto& s : suffixes) {
        memcpy(p, s.c_str(), s.size() * sizeof(wchar_t));
        p += s.size();
        *p++ = L'\0';
    }
    /* trailing double-null already zeroed by the vector initializer */

    if (SendMessage(buf.data(), (DWORD)(buf.size() * sizeof(wchar_t))))
        LOG_INFO(L"DriverClient",
                 L"Sent protected path list: " + std::to_wstring(suffixes.size()) +
                 L" suffix(es).");
}

/* ── SetEnforcement ──────────────────────────────────────────── */

bool DriverClient::SetEnforcement(bool enable) {
    if (!m_connected.load()) return false;

    SC_ENFORCEMENT_MSG msg = {};
    msg.Type   = ScMsgSetEnforcementMode;
    msg.Enable = enable ? 1 : 0;

    if (!SendMessage(&msg, sizeof(msg))) return false;

    LOG_INFO(L"DriverClient", enable
                 ? L"Enforcement ARMED (default-deny active)."
                 : L"Enforcement DISABLED (fail-open).");
    return true;
}

/* ── SetFeatureFlags ─────────────────────────────────────────── */

void DriverClient::SetFeatureFlags(DWORD flags) {
    if (!m_connected.load()) return;

    SC_FEATURE_FLAGS_MSG msg = {};
    msg.Type  = ScMsgSetFeatureFlags;
    msg.Flags = flags;

    if (SendMessage(&msg, sizeof(msg)))
        LOG_INFO(L"DriverClient",
                 L"FeatureFlags=0x" + std::to_wstring(flags) + L" sent to driver.");
}

/* ── Alert receiver ──────────────────────────────────────────── */

void DriverClient::StartAlertReceiver(
    std::function<void(const SC_BLOCK_ALERT&)> alertCallback)
{
    if (m_receiverRunning.load()) return;
    m_alertCallback = alertCallback;
    m_receiverRunning.store(true);
    m_receiverThread = std::thread([this]() { AlertReceiverLoop(); });
}

void DriverClient::StopAlertReceiver() {
    m_receiverRunning.store(false);

    /*
     * FilterGetMessage is a blocking synchronous call; the only way to unblock
     * the receiver thread is to close the port handle out from under it, which
     * makes the in-flight FilterGetMessage return ERROR_INVALID_HANDLE /
     * ERROR_OPERATION_ABORTED. We MUST do this before join(), otherwise the
     * join deadlocks against a thread that is parked inside FilterGetMessage.
     *
     * The handle is captured-and-nulled first so the receiver loop (which reads
     * m_hPort each iteration) sees INVALID_HANDLE_VALUE rather than a stale one.
     */
    HANDLE h = m_hPort;
    m_hPort = INVALID_HANDLE_VALUE;
    if (h != INVALID_HANDLE_VALUE) {
        CloseHandle(h);
    }

    if (m_receiverThread.joinable()) m_receiverThread.join();
}

void DriverClient::AlertReceiverLoop() {
    /* FilterGetMessage requires a FILTER_MESSAGE_HEADER prepended to our struct */
#pragma pack(push, 8)
    struct AlignedMessage {
        FILTER_MESSAGE_HEADER Header;
        SC_BLOCK_ALERT        Alert;
    };
#pragma pack(pop)

    AlignedMessage buf = {};

    while (m_receiverRunning.load()) {
        HANDLE h = m_hPort;
        if (h == INVALID_HANDLE_VALUE) break;

        HRESULT hr = FilterGetMessage(
            h,
            &buf.Header,
            sizeof(buf),
            NULL   /* no overlapped — blocking call */
        );

        if (FAILED(hr)) {
            if (HRESULT_CODE(hr) == ERROR_INVALID_HANDLE ||
                HRESULT_CODE(hr) == ERROR_OPERATION_ABORTED) {
                break; /* port was closed — normal shutdown */
            }
            LOG_WARN(L"DriverClient",
                     L"FilterGetMessage error: 0x" + std::to_wstring((unsigned long)hr));
            Sleep(100); /* back off before retrying */
            continue;
        }

        /*
         * Guard against spurious messages. A genuine kernel block alert always
         * carries a non-zero PID and a path. Some messages on the port arrive
         * zeroed (observed correlating with our own FilterSendMessage calls);
         * treat them as noise — do NOT count them as threats or broadcast them.
         */
        const SC_BLOCK_ALERT& a = buf.Alert;
        if (a.BlockedPid == 0 && a.BlockedPath[0] == L'\0') {
            if (g_spuriousAlerts < 3) {
                g_spuriousAlerts++;
                LOG_WARN(L"DriverClient", L"Zeroed block-alert message ignored (spurious).");
            }
            RtlZeroMemory(&buf, sizeof(buf));
            continue;
        }

        if (m_alertCallback) {
            try {
                m_alertCallback(a);
            } catch (...) {
                /* Don't let a callback exception kill the receiver thread */
            }
        }

        /* Log the block */
        LOG_INFO(L"DriverClient",
                 L"KERNEL BLOCK — pid=" + std::to_wstring(a.BlockedPid) +
                 L" img=" + std::wstring(a.BlockedImageName, wcsnlen(a.BlockedImageName, _countof(a.BlockedImageName))) +
                 L" path=" + std::wstring(a.BlockedPath, wcsnlen(a.BlockedPath, _countof(a.BlockedPath))));

        /* Reset buffer for the next message */
        RtlZeroMemory(&buf, sizeof(buf));
    }
}

/* ── BuildAlertJson ──────────────────────────────────────────── */

std::wstring DriverClient::BuildAlertJson(const SC_BLOCK_ALERT& alert) {
    /*
     * The kernel struct's wchar_t members are FIXED-SIZE arrays that are not
     * guaranteed to be null-terminated when completely filled. Bound each read
     * with wcsnlen so we never run past the array into adjacent bytes.
     */
    size_t nameLen = wcsnlen(alert.BlockedImageName, _countof(alert.BlockedImageName));
    size_t pathLen = wcsnlen(alert.BlockedPath,      _countof(alert.BlockedPath));
    std::wstring imageName(alert.BlockedImageName, nameLen);
    std::wstring path(alert.BlockedPath, pathLen);

    /*
     * nlohmann handles all JSON escaping — critically the backslashes in the
     * NT-native path (e.g. \Device\HarddiskVolume3\...) — so the result is
     * always valid single-line JSON the C# client can parse.
     */
    json r;
    r["type"]         = "alert";
    r["severity"]     = "high";
    r["message"]      = "Kernel blocked an untrusted process from opening a protected file.";
    r["pid"]          = (unsigned long)alert.BlockedPid;
    r["process_name"] = utils::ToNarrow(imageName);
    r["path"]         = utils::ToNarrow(path);
    r["action"]       = "blocked";

    return utils::ToWide(r.dump());
}

} // namespace sc
