/*
 * driver_client.h — ShieldCord Service: Kernel Driver Client
 *
 * User-mode interface for communicating with shieldcord_filter.sys
 * via Filter Manager's FilterConnectCommunicationPort API.
 *
 * USAGE PATTERN (in main.cpp):
 *
 *   bool driverActive = DriverClient::Instance().Connect();
 *   if (driverActive) {
 * 
 *       DriverClient::Instance().StartAlertReceiver(alertCallback);
 *       DriverClient::Instance().SendInitialTrustedPids(alreadyRunningPids);
 *   }
 *   // ... service runs ...
 *   DriverClient::Instance().Disconnect();
 *
 * THREAD SAFETY:
 *   Connect/Disconnect are not thread-safe (call from main thread only).
 *   AddTrustedPid/RemoveTrustedPid are thread-safe (called from AppWatcher threads).
 */

#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <fltUser.h>
#include <functional>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include "../shared/common.h"
#include "../driver/filter/driver_protocol.h"

// fltUser.h is part of the WDK user-mode headers.
// It provides FilterConnectCommunicationPort, FilterSendMessage, FilterGetMessage.
// Link with: fltlib.lib
#include <fltUser.h>

namespace sc {

class DriverClient {
public:
    static DriverClient& Instance();

    /*
     * Connect
     * Connects to \ShieldCordFilterPort.
     * Returns false if the driver is not installed (ERROR_FILE_NOT_FOUND).
     * In that case, the service runs in DACL-only mode — no error is shown.
     * Returns true if the connection succeeds.
     */
    bool Connect();

    /*
     * Disconnect
     * Closes the connection and stops the alert receiver thread.
     * Safe to call even if Connect returned false.
     */
    void Disconnect();

    /*
     * IsConnected
     * Returns true if the driver is connected.
     * Thread-safe (atomic read).
     */
    bool IsConnected() const { return m_connected.load(); }

    /*
     * AddTrustedPid
     * Sends ScMsgAddTrustedPid to the driver.
     * Call this from AppWatcher::GrantIfInactive after DACL grant.
     * Thread-safe.
     */
    void AddTrustedPid(DWORD pid, const std::wstring& imageName);

    /*
     * RemoveTrustedPid
     * Sends ScMsgRemoveTrustedPid to the driver.
     * Call this from AppWatcher::RevokeTag after DACL revoke.
     * Thread-safe.
     */
    void RemoveTrustedPid(DWORD pid);

    /*
     * SendInitialTrustedPids
     * Sends the full list of already-running trusted PIDs at startup.
     * Call this immediately after Connect() returns true.
     * Builds the list from the current process snapshot.
     */
    void SendInitialTrustedPids();

    /*
     * TrustSelf
     * Trusts ShieldCord's own processes (this service + a running tray UI)
     * so they may access the decoy token folder the driver protects.
     * Call right after Connect() and before arming enforcement.
     */
    void TrustSelf();

    /*
     * SendProtectedPaths
     * Pushes the protected-path suffix list (PROTECTED_PATH_SUFFIXES) to the
     * driver, making the path list app-controllable. Call after Connect().
     */
    void SendProtectedPaths();

    /*
     * SetEnforcement
     * Arms (true) or disarms (false) default-deny enforcement on the driver.
     * When armed, only TrustAllowed PIDs may open protected paths.
     *
     * Returns whether the driver accepted it. This is the LAST step of arming,
     * so it doubles as a liveness check on the port: a handle left over from a
     * driver that has since been unloaded or replaced fails here, which is the
     * only way callers can tell "armed" from "believed to be connected to
     * something that is no longer there".
     */
    bool SetEnforcement(bool enable);

    /*
     * SetFeatureFlags
     * Sets the SC_FEATURE_* bitmask on the driver (file blocking, alerts).
     */
    void SetFeatureFlags(DWORD flags);

    /*
     * StartAlertReceiver
     * Starts a background thread that calls FilterGetMessage in a loop.
     * When ScAlertFileBlocked arrives, calls alertCallback.
     * alertCallback is called from the receiver thread — must be thread-safe.
     */
    void StartAlertReceiver(std::function<void(const SC_BLOCK_ALERT&)> alertCallback);

    /*
     * StopAlertReceiver
     * Signals the receiver thread to stop and joins it.
     * Called by Disconnect().
     */
    void StopAlertReceiver();

    /*
     * BuildAlertJson
     * Converts a kernel SC_BLOCK_ALERT into the newline-free JSON string the
     * service broadcasts to the UI. Static + pure (no instance state) so the
     * service controller can call it from its OnDriverAlert trampoline.
     *
     * Uses a real JSON serializer so the NT-native path in BlockedPath (which
     * is full of backslashes) is correctly escaped — hand-built concatenation
     * produced invalid JSON that the C# client silently dropped.
     */
    static std::wstring BuildAlertJson(const SC_BLOCK_ALERT& alert);

private:
    DriverClient() = default;
    SC_DISALLOW_COPY(DriverClient)

    bool SendMessage(const void* msg, DWORD msgSize);
    void AlertReceiverLoop();

    HANDLE              m_hPort   = INVALID_HANDLE_VALUE;
    std::atomic<bool>   m_connected{ false };
    std::atomic<bool>   m_receiverRunning{ false };
    std::thread         m_receiverThread;
    mutable std::mutex  m_sendMutex;

    std::function<void(const SC_BLOCK_ALERT&)> m_alertCallback;
};

} // namespace sc
