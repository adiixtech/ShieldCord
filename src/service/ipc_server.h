#pragma once
// ============================================================
// ShieldCord — ipc_server.h
// Named-pipe server for tray UI / CLI communication
// ============================================================
#include "../shared/common.h"
#include <functional>
#include <string>
#include <thread>
#include <atomic>
#include <vector>
#include <mutex>
#include <condition_variable>

namespace sc {

// Called when service status changes or a threat alert fires
using IpcBroadcastFn = std::function<void(const std::wstring& jsonMsg)>;

class IpcServer {
public:
    static IpcServer& Instance();

    // Start listening on the named pipe.
    // onMessage: callback for incoming client commands. The bool argument is
    // true when the connected client is an elevated (Administrators) caller —
    // state-changing verbs (kill_process/shutdown) must require it. Returns a
    // JSON reply or empty.
    void Start(std::function<std::wstring(const std::wstring&, bool)> onMessage);
    void Stop();

    // Push a message to all connected clients (e.g. alerts)
    void Broadcast(const std::wstring& jsonMsg);

private:
    IpcServer() = default;
    SC_DISALLOW_COPY(IpcServer)

    void AcceptLoop();
    void ClientLoop(HANDLE hPipe);

    std::function<std::wstring(const std::wstring&, bool)> m_onMessage;

    std::atomic_bool m_running{ false };
    std::thread      m_acceptThread;

    std::vector<HANDLE>     m_clientPipes;
    std::mutex              m_clientsMutex;
    std::condition_variable m_clientsCv;      // signaled when a client thread exits
    std::atomic<int>        m_activeClients{ 0 };
};

} // namespace sc
