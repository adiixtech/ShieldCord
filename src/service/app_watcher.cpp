// ============================================================
// ShieldCord — app_watcher.cpp
// Detects trusted app start/stop and triggers DACL grant/revoke.
//
// Two detection paths feed the same grant logic:
//   1. ProcessNotifier (real-time WMI/ETW events, ~10-50 ms) —
//      primary path, exists so the vault unlocks BEFORE the app
//      opens its storage (fixes the "logged out on restart" race).
//   2. PollLoop (snapshot sweep) — fallback for missed events,
//      plus the revocation logic after apps exit.
// ============================================================
#include "app_watcher.h"
#include "process_validator.h"
#include "process_notifier.h"
#include "driver_client.h"
#include "utils.h"
#include "logger.h"
#include "../shared/whitelist.h"
#include <tlhelp32.h>
#include <map>
#include <set>
#include <vector>
#include <algorithm>

#pragma comment(lib, "advapi32.lib")

namespace sc {

AppWatcher& AppWatcher::Instance() {
    static AppWatcher inst;
    return inst;
}

// Lowercase trusted process names — cheap pre-filter applied to
// every process (both the poll sweep and every start event).
static const std::set<std::wstring>& TrustedNamesLower() {
    static const std::set<std::wstring> names = [] {
        std::set<std::wstring> s;
        for (const auto& ta : TRUSTED_APPS)
            s.insert(utils::ToLower(ta.processName));
        return s;
    }();
    return names;
}

// tag -> lowercase process names (used by the liveness guard)
static const std::map<std::wstring, std::vector<std::wstring>>& NamesByTag() {
    static const std::map<std::wstring, std::vector<std::wstring>> m = [] {
        std::map<std::wstring, std::vector<std::wstring>> mm;
        for (const auto& ta : TRUSTED_APPS)
            mm[ta.appTag].push_back(utils::ToLower(ta.processName));
        return mm;
    }();
    return m;
}

void AppWatcher::Start(int intervalMs, std::function<void(const std::wstring&)> onAlert) {
    m_intervalMs = intervalMs;
    m_onAlert   = onAlert;
    m_running   = true;
    m_thread    = std::thread([this]() { PollLoop(m_intervalMs); });

    // Real-time path. If WMI is unavailable (not elevated), this logs a
    // warning and we silently degrade to the polling fallback above.
    ProcessNotifier::Instance().Start([this](DWORD pid, const std::wstring& name) {
        OnProcessStart(pid, name);
    });

    LOG_INFO(L"AppWatcher", L"Watcher started (poll " + std::to_wstring(intervalMs) +
             L" ms + real-time notifier, grace " +
             std::to_wstring(SC_GRACE_PERIOD_MS / 1000) + L" s).");
}

void AppWatcher::Stop() {
    m_running = false;
    ProcessNotifier::Instance().Stop();
    if (m_thread.joinable()) m_thread.join();
    LOG_INFO(L"AppWatcher", L"Watcher stopped.");
}

// -------------------------------------------------------
// Real-time entry point — called by ProcessNotifier for every
// process creation in the system.
// -------------------------------------------------------
void AppWatcher::OnProcessStart(DWORD pid, const std::wstring& processName) {
    if (!m_running) return;

    std::wstring nameLow = utils::ToLower(processName);
    if (TrustedNamesLower().count(nameLow) == 0) return;   // cheap filter

    std::wstring tag = TryRegisterTrusted(pid, nameLow);
    if (!tag.empty())
        GrantIfInactive(tag, pid);
}

// -------------------------------------------------------
// Verify a process and, if trusted, remember it + refresh its
// tag timestamp. Signature verification happens OUTSIDE m_mutex.
// -------------------------------------------------------
std::wstring AppWatcher::TryRegisterTrusted(DWORD pid, const std::wstring& /*nameLow*/) {
    // Already known? Just refresh.
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        auto it = m_knownTrusted.find(pid);
        if (it != m_knownTrusted.end()) {
            m_tagLastSeen[it->second] = GetTickCount64();
            return it->second;
        }
    }

    // Signature check — slow on the very first run per binary, instant
    // afterwards thanks to the persistent signer cache.
    const TrustedApp* ta = ProcessValidator::FindTrustedApp(pid);

    std::wstring tag;
    if (ta) {
        tag = ta->appTag;          // may be empty (our own service entry)
    } else {
        // Not on the built-in whitelist — but the user may have explicitly
        // trusted this binary's publisher from a false-positive alert. That
        // is a real trust decision with the same consequences, so it takes
        // the same grant path; the tag is derived from the image name
        // because there is no whitelist entry to supply one.
        std::wstring publisher;
        if (!ProcessValidator::MatchesRuntimePublisher(pid, &publisher)) return {};

        tag = ProcessValidator::RuntimeTagFor(pid);
        if (tag.empty()) return {};

        LOG_WARN(L"AppWatcher", L"Granting driver trust to '" + tag +
                 L"' via user-trusted publisher '" + publisher + L"'.");
    }

    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_knownTrusted[pid] = tag;
        m_tagLastSeen[tag]  = GetTickCount64();
    }
    return tag;
}

// -------------------------------------------------------
// How many processes are currently known-trusted (reported to the UI).
// -------------------------------------------------------
size_t AppWatcher::TrustedCount() {
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_knownTrusted.size();
}

// -------------------------------------------------------
// Grant the vault for 'tag' if not already granted.
// -------------------------------------------------------
void AppWatcher::GrantIfInactive(const std::wstring& tag, DWORD samplePid) {
    if (tag.empty()) return;   // our own service entry — nothing to unlock

    bool newlyGranted = false;
    bool pidIsNew     = false;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (!m_activeGrants[tag]) {
            m_activeGrants[tag] = true;
            newlyGranted = true;
        }
        m_tagLastSeen[tag] = GetTickCount64();

        /* Look up the PID, not the tag.
         *
         * This used to `return` whenever the tag was already granted, which
         * pushed exactly ONE pid per tag for the whole session. The driver's
         * trust table is keyed by PID, so every other process of that app
         * stayed untrusted and was DENIED at pre-create until the driver
         * reported the block and the service self-healed that single pid — by
         * which point the open it cared about had already failed.
         *
         * Every browser here runs many processes under one image name (browser,
         * GPU, network, storage, renderer), so this fired on every restart.
         * Brave's network-data migration gave up on the first denial and never
         * loaded its cookies; Discord retried its leveldb open and recovered,
         * which is why only some apps looked broken. */
        if (m_pushedPids.insert(samplePid).second) pidIsNew = true;
    }
    if (!pidIsNew) return;

    // Tell the kernel driver to allow this PID — the driver is now the SOLE
    // file-protection layer (the DACL Gatekeeper was removed). DriverClient is
    // a no-op if the driver is not installed.
    DriverClient::Instance().AddTrustedPid(samplePid, tag + L" (trusted)");

    if (!newlyGranted) {
        // Another process of an app already granted. Worth a line: how often
        // this happens is exactly what the per-tag bug hid.
        LOG_INFO(L"AppWatcher", L"Trust extended to another '" + tag +
                 L"' process (PID " + std::to_wstring(samplePid) + L").");
        return;
    }

    // Instrumentation: how old was the process when the grant landed?
    // If this number is regularly > ~100 ms, the app raced us and lost.
    ULONGLONG latencyMs = 0;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, samplePid);
    if (h) {
        FILETIME creation = {}, exitT = {}, kernelT = {}, userT = {};
        if (GetProcessTimes(h, &creation, &exitT, &kernelT, &userT)) {
            FILETIME nowFt;
            GetSystemTimeAsFileTime(&nowFt);
            ULARGE_INTEGER c{}, n{};
            c.LowPart = creation.dwLowDateTime; c.HighPart = creation.dwHighDateTime;
            n.LowPart = nowFt.dwLowDateTime;    n.HighPart = nowFt.dwHighDateTime;
            if (n.QuadPart >= c.QuadPart)
                latencyMs = (n.QuadPart - c.QuadPart) / 10000ULL;
        }
        CloseHandle(h);
    }
    LOG_INFO(L"AppWatcher", L"Trust granted for '" + tag + L"' (PID " +
             std::to_wstring(samplePid) + L") — process age at grant: " +
             std::to_wstring((unsigned long long)latencyMs) + L" ms.");
}

// -------------------------------------------------------
// Liveness guard: is ANY process whose name belongs to this tag
// still running? (Name-only on purpose — for the re-lock decision
// it is safer to keep the vault open than to wrongly re-lock.)
// -------------------------------------------------------
bool AppWatcher::TagStillAlive(const std::wstring& tag) {
    const auto& byTag = NamesByTag();
    auto it = byTag.find(tag);
    if (it == byTag.end()) return false;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return true;   // can't tell — assume alive

    bool alive = false;
    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            std::wstring nameLow = utils::ToLower(std::wstring(pe.szExeFile));
            for (const auto& n : it->second) {
                if (n == nameLow) { alive = true; break; }
            }
            if (alive) break;
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return alive;
}

// -------------------------------------------------------
// Re-lock the vault for a tag.
// -------------------------------------------------------
void AppWatcher::RevokeTag(const std::wstring& tag) {
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_activeGrants[tag] = false;
    }

    // The driver removes the PID from its own trust table automatically when
    // the process exits (PsSetCreateProcessNotifyRoutineEx callback), so there
    // is nothing to send here — this just marks the grant as expired.
    LOG_INFO(L"AppWatcher", L"Trust grant expired for '" + tag +
             L"' (grace period elapsed).");
}

// -------------------------------------------------------
// Poll loop — fallback sweep + revocation.
// -------------------------------------------------------
void AppWatcher::PollLoop(int intervalMs) {
    while (m_running) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) { Sleep(intervalMs); continue; }

        PROCESSENTRY32W pe = {};
        pe.dwSize = sizeof(pe);

        if (Process32FirstW(snap, &pe)) {
            do {
                std::wstring nameLow = utils::ToLower(std::wstring(pe.szExeFile));
                if (TrustedNamesLower().count(nameLow) == 0) continue;

                std::wstring tag = TryRegisterTrusted(pe.th32ProcessID, nameLow);
                if (!tag.empty())
                    GrantIfInactive(tag, pe.th32ProcessID);
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);

        // Clean up dead PIDs from m_knownTrusted
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            for (auto it = m_knownTrusted.begin(); it != m_knownTrusted.end(); ) {
                HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, it->first);
                bool dead = true;
                if (h) {
                    DWORD exitCode = 0;
                    if (GetExitCodeProcess(h, &exitCode) && exitCode == STILL_ACTIVE)
                        dead = false;
                    CloseHandle(h);
                }
                if (dead) {
                    // Forget the push too, so a recycled PID is pushed again
                    // rather than being suppressed by a stale entry.
                    m_pushedPids.erase(it->first);
                    
                    std::wstring deadTag = it->second;
                    it = m_knownTrusted.erase(it);

                    // Check if this was the last PID for this tag
                    bool tagStillHasPids = false;
                    for (const auto& kv : m_knownTrusted) {
                        if (kv.second == deadTag) {
                            tagStillHasPids = true;
                            break;
                        }
                    }
                    if (!tagStillHasPids) {
                        // Immediately make it eligible for the grace-period countdown
                        m_tagLastSeen[deadTag] = 0;
                    }
                }
                else      ++it;
            }
        }

        // Revoke check — grace period, then a HARD liveness re-check
        ULONGLONG now = GetTickCount64();
        std::vector<std::wstring> toRevoke;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            for (auto& kv : m_activeGrants) {
                if (!kv.second) continue;
                ULONGLONG lastSeen = 0;
                auto ls = m_tagLastSeen.find(kv.first);
                if (ls != m_tagLastSeen.end()) lastSeen = ls->second;
                if (now - lastSeen > SC_GRACE_PERIOD_MS)
                    toRevoke.push_back(kv.first);
            }
        }

        for (const auto& tag : toRevoke) {
            // HARD RULE: never re-lock while any process with this tag's
            // name is still alive — a missed detection must not lock out
            // a running app (that's a logout bug, not protection).
            if (TagStillAlive(tag)) {
                std::lock_guard<std::mutex> lk(m_mutex);
                m_tagLastSeen[tag] = GetTickCount64();
                LOG_INFO(L"AppWatcher", L"Grace expired for '" + tag +
                         L"' but a matching process is still alive — keeping vault open.");
                continue;
            }
            RevokeTag(tag);
        }

        Sleep(intervalMs);
    }
}

bool AppWatcher::IsTagTrusted(const std::wstring& tag) {
    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = m_activeGrants.find(tag);
    return (it != m_activeGrants.end() && it->second);
}

} // namespace sc
