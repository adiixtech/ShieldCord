#pragma once
// ============================================================
// ShieldCord — app_watcher.h
// Watches for trusted app start/stop and triggers DACL
// grant/revoke accordingly.
//
// Two detection paths feed the same logic:
//   1. ProcessNotifier — real-time WMI/ETW process-start events
//      (~10-50 ms latency). This is the primary path; it exists
//      so the vault is unlocked BEFORE the app opens its storage.
//   2. PollLoop — a slow snapshot sweep that acts as fallback for
//      missed events and handles revocation after apps exit.
// ============================================================
#include "../shared/common.h"
#include <functional>
#include <set>
#include <map>
#include <string>
#include <thread>
#include <atomic>

namespace sc {

using AlertCallback = std::function<void(const std::wstring& json)>;

class AppWatcher {
public:
    static AppWatcher& Instance();

    // Start detection threads. Call after DACL is locked.
    void Start(int intervalMs, std::function<void(const std::wstring&)> onAlert);

    // Stop all threads gracefully.
    void Stop();

    // Called by ProcessNotifier (real-time path) when ANY process starts.
    // Performs the name check + signature verification + grant inline —
    // fast because signature results are cached. Thread-safe.
    void OnProcessStart(DWORD pid, const std::wstring& processName);

    // How many processes are currently known-trusted. Reported to the UI.
    size_t TrustedCount();

    // True if the tag is currently pushed to the driver as trusted
    bool IsTagTrusted(const std::wstring& tag);

private:
    // True if any process whose image name belongs to 'tag' is still alive.
    bool TagStillAlive(const std::wstring& tag);
    AppWatcher() = default;
    SC_DISALLOW_COPY(AppWatcher)

    void PollLoop(int intervalMs);

    // Verify a process and, if trusted, remember it and refresh its tag.
    // Returns the app tag (empty if not trusted). Expensive work is done
    // OUTSIDE m_mutex.
    std::wstring TryRegisterTrusted(DWORD pid, const std::wstring& nameLow);

    // Grant the vault for 'tag' if not already granted. Uses samplePid to
    // derive the user SID and logs the detection->grant latency.
    void GrantIfInactive(const std::wstring& tag, DWORD samplePid);

    // Revoke (re-lock) the vault for 'tag'.
    void RevokeTag(const std::wstring& tag);

    std::atomic<bool> m_running{false};
    std::thread m_thread;
    int m_intervalMs = 1000;
    std::function<void(const std::wstring&)> m_onAlert;

    std::mutex m_mutex;
    // Map of PID -> App Tag (e.g., 1234 -> L"discord")
    std::map<DWORD, std::wstring> m_knownTrusted;
    // PIDs already pushed into the driver's trust table.
    //
    // This exists because the two layers are keyed differently: our grants are
    // keyed by TAG, the driver's table is keyed by PID. An app that runs several
    // processes under one image name (every browser here) needs one push per
    // process, so the pushed set has to be tracked per PID or the extra
    // processes stay untrusted and get denied at pre-create.
    std::set<DWORD> m_pushedPids;
    // Map of App Tag -> Is Active (e.g., L"discord" -> true)
    std::map<std::wstring, bool> m_activeGrants;
    // Map of App Tag -> last-seen tick count (ms)
    std::map<std::wstring, ULONGLONG> m_tagLastSeen;
};

} // namespace sc
