#pragma once
// ============================================================
// ShieldCord — memory_monitor.h
// Layer 3: Detect any untrusted process that opens a protected
// process (discord.exe, chrome.exe, etc.) with PROCESS_VM_READ.
// ============================================================
#include "../shared/common.h"
#include "alert_history.h"      // AlertRecord
#include <functional>
#include <thread>
#include <atomic>

namespace sc {

// Distinct from AppWatcher's AlertCallback (which carries a ready-made JSON
// string): the memory monitor reports a structured record and lets the engine
// decide how to publish it, so both layers share one alert path.
using MemoryAlertCallback = std::function<void(const AlertRecord&)>;

class MemoryMonitor {
public:
    static MemoryMonitor& Instance();

    void Start(int pollIntervalMs, MemoryAlertCallback alertCb);

    // When false (dev/test mode), detected threats are logged and alerted
    // but processes are NOT terminated.
    void SetKillEnabled(bool enabled) { m_killEnabled = enabled; }

    void Stop();

private:
    MemoryMonitor() = default;
    SC_DISALLOW_COPY(MemoryMonitor)

    void PollLoop(int intervalMs);
    bool IsProtectedProcess(DWORD pid, std::wstring* outName = nullptr);

    std::thread      m_thread;
    std::atomic_bool m_running{ false };
    std::atomic_bool m_killEnabled{ true };
    MemoryAlertCallback m_alertCb;
};

} // namespace sc
