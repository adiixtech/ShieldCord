#pragma once
// ============================================================
// ShieldCord — process_notifier.h
// Real-time process-creation notification.
//
// Subscribes to Win32_ProcessStartTrace (WMI, backed by ETW) so we
// learn about a trusted app's launch within ~10-50 ms instead of
// waiting for the next polling sweep. This is the primary defence
// against the startup race that logged Discord out: the vault must
// be unlocked BEFORE the app's first file open.
//
// Requires elevation (service = SYSTEM: fine; console mode: run as
// admin). If the subscription fails we log a warning and silently
// fall back to AppWatcher's polling loop.
// ============================================================
#include "../shared/common.h"
#include <functional>
#include <string>
#include <thread>
#include <atomic>

namespace sc {

class ProcessNotifier {
public:
    static ProcessNotifier& Instance();

    using Callback = std::function<void(DWORD pid, const std::wstring& processName)>;

    // Start the WMI listener thread. Returns false if it could not start.
    // Safe to call when already running (no-op).
    bool Start(Callback cb);

    // Stop the listener thread gracefully.
    void Stop();

private:
    ProcessNotifier() = default;
    SC_DISALLOW_COPY(ProcessNotifier)

    void Loop(Callback cb);

    std::atomic<bool> m_running{ false };
    std::thread       m_thread;
};

} // namespace sc
