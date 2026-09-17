// ============================================================
// ShieldCord — memory_monitor.cpp
// Layer 3: Enumerate all system handles looking for untrusted
// processes that hold PROCESS_VM_READ on protected processes.
// Uses NtQuerySystemInformation(SystemHandleInformation).
// ============================================================
#include "memory_monitor.h"
#include "process_validator.h"
#include "utils.h"
#include "logger.h"
#include "../shared/token_paths.h"
#include "../shared/ipc_protocol.h"
#include <winternl.h>
#include <tlhelp32.h>
#include <algorithm>
#include <map>
#include <set>
#include <sstream>

// NT API typedefs
typedef NTSTATUS(NTAPI* NtQuerySystemInformation_t)(
    ULONG SystemInformationClass,
    PVOID SystemInformation,
    ULONG SystemInformationLength,
    PULONG ReturnLength);

#define SystemHandleInformation 16

typedef struct _SYSTEM_HANDLE {
    ULONG      ProcessId;
    BYTE       ObjectTypeNumber;
    BYTE       Flags;
    USHORT     Handle;
    PVOID      Object;
    ACCESS_MASK GrantedAccess;
} SYSTEM_HANDLE;

typedef struct _SYSTEM_HANDLE_INFORMATION {
    ULONG HandleCount;
    SYSTEM_HANDLE Handles[1];
} SYSTEM_HANDLE_INFORMATION;

namespace sc {

MemoryMonitor& MemoryMonitor::Instance() {
    static MemoryMonitor inst;
    return inst;
}

void MemoryMonitor::Start(int pollIntervalMs, MemoryAlertCallback alertCb) {
    // Reconcile-safe: the UI can toggle the monitor at runtime, and a second
    // Start() would otherwise spawn a second poll thread sharing this object's
    // state and double-report every detection.
    if (m_running.load()) {
        LOG_INFO(L"MemMon", L"Already running — Start() ignored.");
        return;
    }

    m_alertCb = std::move(alertCb);
    m_running = true;
    m_thread  = std::thread([this, pollIntervalMs]() {
        PollLoop(pollIntervalMs);
    });
    LOG_INFO(L"MemMon", L"Started with poll interval " +
             std::to_wstring(pollIntervalMs) + L" ms.");
}

void MemoryMonitor::Stop() {
    m_running = false;
    if (m_thread.joinable()) m_thread.join();
    LOG_INFO(L"MemMon", L"Stopped.");
}

bool MemoryMonitor::IsProtectedProcess(DWORD pid, std::wstring* outName) {
    std::wstring name = utils::ToLower(utils::GetProcessName(pid));
    for (const auto& pname : PROTECTED_PROCESS_NAMES) {
        if (utils::ToLower(pname) == name) {
            if (outName) *outName = name;
            return true;
        }
    }
    return false;
}

// -------------------------------------------------------
// SAFETY: can we terminate this process?
//
// We must NEVER kill Windows system processes. lsass.exe legitimately
// holds PROCESS_VM_READ handles on browsers (SSO / credential tracking),
// and Windows system binaries are CATALOG-signed (no embedded
// Authenticode), so the signer check cannot vouch for them. Killing
// lsass.exe terminates the whole machine with a forced restart
// (Event 1074 / status 0x50006) — observed in the wild on 2026-08-27.
// -------------------------------------------------------
static bool IsSafeToKill(DWORD pid) {
    std::wstring path = utils::GetProcessImagePath(pid);

    // Cannot determine where it runs from? Do NOT kill — fail safe.
    if (path.empty()) return false;

    std::wstring pathLow = utils::ToLower(path);

    wchar_t winDir[MAX_PATH] = {};
    GetWindowsDirectoryW(winDir, MAX_PATH);
    std::wstring winLow = utils::ToLower(std::wstring(winDir));
    if (winLow.back() == L'\\') winLow.pop_back();

    // Anything executing out of \Windows (lsass, csrss, svchost, dwm,
    // MsMpEng, ...) is off limits — log-only.
    if (pathLow.rfind(winLow + L"\\", 0) == 0) return false;

    return true;
}

void MemoryMonitor::PollLoop(int intervalMs) {
    // Load NtQuerySystemInformation dynamically
    HMODULE hNtDll = GetModuleHandleW(L"ntdll.dll");
    if (!hNtDll) {
        LOG_ERROR(L"MemMon", L"Cannot get ntdll.dll handle.");
        return;
    }
    auto NtQSI = reinterpret_cast<NtQuerySystemInformation_t>(
        GetProcAddress(hNtDll, "NtQuerySystemInformation"));
    if (!NtQSI) {
        LOG_ERROR(L"MemMon", L"Cannot find NtQuerySystemInformation.");
        return;
    }

    // Cache of already-reported attacker PIDs to avoid repeated kills
    std::map<DWORD, ULONGLONG> killed;
    // Cache of legitimate (system/unresolvable) holders we already logged —
    // svchost.exe legitimately holds VM_READ on browsers; without this we would
    // re-log the same benign process on every poll (~5 lines/sec) forever.
    std::map<DWORD, ULONGLONG> ignoredLogged;

    // Signed-but-untrusted holders already written to the log, keyed
    // "name|publisher" rather than PID. A service that restarts — Cloudflare
    // WARP did — gets a fresh PID every time, so a PID key would write a line
    // per respawn until the log rotated.
    std::set<std::wstring> quietLogged;

    // Get our own PID to exclude
    DWORD ourPid = GetCurrentProcessId();

    while (m_running) {
        // Dynamically size the buffer
        ULONG bufSize = 1024 * 1024; // start at 1 MB
        std::vector<BYTE> buf;
        NTSTATUS status;

        do {
            buf.resize(bufSize);
            ULONG retLen = 0;
            status = NtQSI(SystemHandleInformation,
                           buf.data(), bufSize, &retLen);
            if (status == 0xC0000004L /* STATUS_INFO_LENGTH_MISMATCH */) {
                bufSize *= 2;
            }
        } while (status == 0xC0000004L && bufSize < 64 * 1024 * 1024);

        if (status != 0) {
            Sleep(intervalMs);
            continue;
        }

        auto* hi = reinterpret_cast<SYSTEM_HANDLE_INFORMATION*>(buf.data());

        // Build a quick map of PID→process name for protected processes
        // (to avoid repeated GetProcessName calls)
        std::map<DWORD, std::wstring> protectedPids;
        {
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
            if (snap != INVALID_HANDLE_VALUE) {
                PROCESSENTRY32W pe = {};
                pe.dwSize = sizeof(pe);
                if (Process32FirstW(snap, &pe)) {
                    do {
                        std::wstring nm = utils::ToLower(std::wstring(pe.szExeFile));
                        for (const auto& pp : PROTECTED_PROCESS_NAMES) {
                            if (utils::ToLower(pp) == nm) {
                                protectedPids[pe.th32ProcessID] = nm;
                                break;
                            }
                        }
                    } while (Process32NextW(snap, &pe));
                }
                CloseHandle(snap);
            }
        }

        for (ULONG i = 0; i < hi->HandleCount; i++) {
            const SYSTEM_HANDLE& h = hi->Handles[i];

            // We only care about handles with VM_READ access
            if (!(h.GrantedAccess & PROCESS_VM_READ)) continue;

            // Skip our own process
            DWORD ownerPid = h.ProcessId;
            if (ownerPid == ourPid || ownerPid == 0 || ownerPid == 4) continue;

            ULONGLONG ownerCreateTime = utils::GetProcessCreateTime(ownerPid);
            if (ownerCreateTime == 0) continue; // Process died already

            // Skip already-killed
            auto itKilled = killed.find(ownerPid);
            if (itKilled != killed.end() && itKilled->second == ownerCreateTime) continue;

            // To find the target of this handle, open the owner process and duplicate the handle
            HANDLE hOwner = OpenProcess(
                PROCESS_DUP_HANDLE | PROCESS_QUERY_INFORMATION,
                FALSE, ownerPid);
            if (!hOwner) continue;

            HANDLE hDup = nullptr;
            if (!DuplicateHandle(hOwner, reinterpret_cast<HANDLE>((ULONG_PTR)h.Handle),
                                 GetCurrentProcess(), &hDup,
                                 PROCESS_QUERY_LIMITED_INFORMATION, FALSE, 0))
            {
                CloseHandle(hOwner);
                continue;
            }

            // What process does the duplicated handle refer to?
            DWORD targetPid = GetProcessId(hDup);
            CloseHandle(hDup);
            CloseHandle(hOwner);

            if (targetPid == 0) continue;
            if (protectedPids.find(targetPid) == protectedPids.end()) continue;

            // The owner holds VM_READ on a protected process.
            //
            // Decide on what can be PROVEN about the binary, not on its name.
            // The hit above is a capability, not an act — nothing has observed
            // a token being read — so it carries a high false-positive rate and
            // the classification is what keeps the response proportional.
            std::wstring ownerName = utils::GetProcessName(ownerPid);

            ProcessValidator::SignerTrust trust =
                ProcessValidator::ClassifySigner(ownerPid);

            // 1. Whitelisted, a publisher the user trusted, or a path on their
            //    own allow list. Nothing to do.
            if (trust == ProcessValidator::SignerTrust::Trusted) continue;

            // 2. Validly signed, publisher not trusted — NOT killed and NOT
            //    raised as an alert. This is the Cloudflare WARP case: real,
            //    signed software that legitimately opens browser processes for
            //    traffic inspection. Killing it broke the VPN and, because it
            //    respawns under a fresh PID each time, produced an unbounded
            //    kill/alert loop made entirely of false positives. A valid
            //    Authenticode signature is not proof of good intent, but it is
            //    the one clean line separating this from the unsigned-stealer
            //    case, and logging it keeps the decision auditable.
            if (trust == ProcessValidator::SignerTrust::SignedUntrusted) {
                std::wstring publisher = ProcessValidator::GetPublisher(ownerPid);
                std::wstring key = utils::ToLower(ownerName) + L"|" +
                                   utils::ToLower(publisher);

                if (quietLogged.insert(key).second) {
                    LOG_INFO(L"MemMon",
                        L"Signed but untrusted '" + ownerName + L"' (publisher '" +
                        publisher + L"') holds VM_READ on '" +
                        protectedPids[targetPid] + L"' — logged only, not killed.");
                }
                continue;
            }

            // 3. Unsigned, or the signature did not verify. This is the class a
            //    token stealer falls into, and the only one we ever terminate.

            // HARD SAFETY: never terminate Windows system processes or
            // processes whose image path we cannot resolve. Killing lsass
            // force-restarts the entire machine (this actually happened).
            if (!IsSafeToKill(ownerPid)) {
                // Log each unique legitimate holder ONCE (not every poll).
                auto it = ignoredLogged.find(ownerPid);
                if (it == ignoredLogged.end() || it->second != ownerCreateTime) {
                    ignoredLogged[ownerPid] = ownerCreateTime;
                    LOG_INFO(L"MemMon",
                        L"System/unresolvable process '" + ownerName + L"' (PID " +
                        std::to_wstring(ownerPid) + L") holds VM_READ on '" +
                        protectedPids[targetPid] + L"' — legitimate, ignoring.");
                }
                continue;
            }

            std::wstring targetName = protectedPids[targetPid];

            bool killed_ok = false;
            if (m_killEnabled.load()) {
                killed_ok = utils::KillProcess(ownerPid);
            }
            killed[ownerPid] = ownerCreateTime;

            // Report through the shared alert path so this lands in the
            // history (and gets an id) exactly like a kernel block does.
            // The publisher is resolved here, while the process still exists —
            // a moment later it may be gone and the CN unrecoverable, which
            // would leave the UI's "trust this publisher" action with nothing
            // to act on.
            AlertRecord rec;
            rec.source      = utils::ToWide(ipc::SOURCE_MEMORY);
            rec.severity    = utils::ToWide(ipc::SEV_HIGH);
            rec.message     = L"Untrusted process attempted memory read on " + targetName;
            rec.pid         = ownerPid;
            rec.processName = ownerName;
            rec.publisher   = ProcessValidator::GetPublisher(ownerPid);
            rec.target      = targetName;
            rec.action      = utils::ToWide(!m_killEnabled.load() ? ipc::ACTION_DETECTED_ONLY
                            : (killed_ok ? ipc::ACTION_KILLED
                                         : ipc::ACTION_KILL_FAILED));
            if (m_alertCb) m_alertCb(rec);
        }

        // Sweep caches periodically to remove dead PIDs
        static ULONGLONG lastSweepTick = GetTickCount64();
        if (GetTickCount64() - lastSweepTick > 5000) {
            auto sweepMap = [](std::map<DWORD, ULONGLONG>& m) {
                for (auto it = m.begin(); it != m.end(); ) {
                    if (utils::GetProcessCreateTime(it->first) != it->second) {
                        it = m.erase(it);
                    } else {
                        ++it;
                    }
                }
            };
            sweepMap(killed);
            sweepMap(ignoredLogged);
            if (quietLogged.size() > 1024) quietLogged.clear();
            lastSweepTick = GetTickCount64();
        }

        Sleep(intervalMs);
    }
}

} // namespace sc
