// ============================================================
// ShieldCord — ipc_server.cpp
// Named pipe server — newline-delimited JSON protocol
// ============================================================
#include "ipc_server.h"
#include "utils.h"
#include "logger.h"
#include "../shared/ipc_protocol.h"
#include <sddl.h>
#include <thread>
#include <algorithm>
#include <chrono>

namespace sc {

IpcServer& IpcServer::Instance() {
    static IpcServer inst;
    return inst;
}

void IpcServer::Start(std::function<std::wstring(const std::wstring&, bool)> onMessage) {
    m_onMessage = std::move(onMessage);
    m_running   = true;
    m_acceptThread = std::thread([this]() { AcceptLoop(); });
    LOG_INFO(L"IPC", L"Named pipe server started: " SC_PIPE_NAME);
}

void IpcServer::Stop() {
    m_running = false;
    // Connect a dummy client to unblock ConnectNamedPipe
    HANDLE h = CreateFileW(SC_PIPE_NAME, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    if (m_acceptThread.joinable()) m_acceptThread.join();

    /*
     * BOTH waits below are deliberately BOUNDED, and that is load-bearing.
     *
     * This function is the only place in the whole stop path that can block
     * indefinitely, and the SCM's patience is finite. If it never returns,
     * ServiceMain never reaches SetStatus(SERVICE_STOPPED), the SCM concludes
     * the service crashed, and its configured SC_ACTION_RESTART brings the
     * engine straight back up — a stop that does not stick, and a service that
     * looks like it ignored the user. A clean, prompt return here is what makes
     * an explicit stop stay stopped.
     *
     * Each pipe handle has a SINGLE owner — the ClientLoop that services it,
     * which erases it from m_clientPipes and closes it on exit. Stop must NOT
     * close these handles itself (that would double-close against a detached
     * ClientLoop still unwinding). Instead we abort each client's blocking
     * ReadFile with CancelIoEx and wait for every ClientLoop to drain.
     */
    {
        // Broadcast holds this same lock across a blocking WriteFile to a client
        // that may have stopped reading, so acquiring it is not guaranteed
        // either. std::mutex has no timed lock (that is std::timed_mutex, which
        // would force the condition variable below to change too), so retry
        // briefly and give up rather than block shutdown indefinitely.
        std::unique_lock<std::mutex> lk(m_clientsMutex, std::defer_lock);

        bool acquired = false;
        for (int attempt = 0; attempt < 40 && !acquired; ++attempt) {
            acquired = lk.try_lock();
            if (!acquired) Sleep(50);                    // ~2 s in total
        }

        if (acquired) {
            for (HANDLE hPipe : m_clientPipes)
                CancelIoEx(hPipe, nullptr);
        } else {
            LOG_WARN(L"IPC", L"Could not take the client lock; some clients were not cancelled.");
        }
    }
    {
        std::unique_lock<std::mutex> lk(m_clientsMutex);

        // CancelIoEx is the only thing that ends this wait, and it is expected
        // to abort each synchronous ReadFile — but a thread parked in a way it
        // does not cover must not be able to wedge shutdown. Give the drain a
        // few seconds, then carry on regardless: the process is exiting, so a
        // straggling reader thread costs nothing.
        m_clientsCv.wait_for(lk, std::chrono::seconds(3),
                             [this]() { return m_activeClients.load() == 0; });

        if (m_activeClients.load() != 0)
            LOG_WARN(L"IPC", L"Client threads did not drain in time — continuing shutdown anyway.");
    }
    LOG_INFO(L"IPC", L"Named pipe server stopped.");
}

// -------------------------------------------------------
// Broadcast: send JSON to all connected clients
// -------------------------------------------------------
void IpcServer::Broadcast(const std::wstring& jsonMsg) {
    std::string msg = utils::ToNarrow(jsonMsg) + "\n";
    std::lock_guard<std::mutex> lk(m_clientsMutex);
    for (HANDLE hPipe : m_clientPipes) {
        DWORD written = 0;
        // Best-effort. On failure, do NOT close/erase here: the owning
        // ClientLoop's ReadFile will fail and perform the erase+close. Closing
        // from Broadcast would race the single-owner model and double-close.
        WriteFile(hPipe, msg.c_str(), (DWORD)msg.size(), &written, nullptr);
    }
}

// -------------------------------------------------------
// Accept loop: creates a new pipe instance per connection
// -------------------------------------------------------
void IpcServer::AcceptLoop() {
    while (m_running) {
        // The pipe is reachable by Authenticated Users so the (non-elevated)
        // tray UI running as the logged-in user can connect for get_status and
        // receive broadcasts. State-changing verbs are NOT authorized by pipe
        // ACL — they are gated per-command on the caller's elevation in
        // ClientLoop (see ClientIsElevated) so a non-admin cannot kill/disable
        // the SYSTEM service.
        SECURITY_ATTRIBUTES sa = {};
        sa.nLength = sizeof(sa);
        PSECURITY_DESCRIPTOR pSD = nullptr;
        if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
                L"D:(A;;GRGW;;;AU)", SDDL_REVISION_1, &pSD, nullptr)) {
            sa.lpSecurityDescriptor = pSD;
        }

        HANDLE hPipe = CreateNamedPipeW(
            SC_PIPE_NAME,
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES,
            4096, 4096,
            0,
            pSD ? &sa : nullptr);

        if (pSD) LocalFree(pSD);

        if (hPipe == INVALID_HANDLE_VALUE) {
            Sleep(500);
            continue;
        }

        BOOL connected = ConnectNamedPipe(hPipe, nullptr)
            ? TRUE
            : (GetLastError() == ERROR_PIPE_CONNECTED ? TRUE : FALSE);

        if (!m_running) {
            CloseHandle(hPipe);
            break;
        }

        if (connected) {
            {
                std::lock_guard<std::mutex> lk(m_clientsMutex);
                m_clientPipes.push_back(hPipe);
                ++m_activeClients;   // count before the thread starts so Stop()
                                     // (which waits on this count) can't race a
                                     // not-yet-running ClientLoop.
            }
            try {
                std::thread([this, hPipe]() { ClientLoop(hPipe); }).detach();
            } catch (...) {
                // Thread creation failed — undo the bookkeeping and drop it.
                std::lock_guard<std::mutex> lk(m_clientsMutex);
                auto it = std::find(m_clientPipes.begin(), m_clientPipes.end(), hPipe);
                if (it != m_clientPipes.end()) m_clientPipes.erase(it);
                CloseHandle(hPipe);
                if (--m_activeClients == 0) m_clientsCv.notify_all();
            }
        } else {
            CloseHandle(hPipe);
        }
    }
}

// -------------------------------------------------------
// Determine whether the connected pipe client is an elevated
// (Administrators) caller. Used to gate state-changing verbs.
// Impersonation at identification level is sufficient to query the token's
// elevation; we always RevertToSelf before returning.
// -------------------------------------------------------
static bool ClientIsElevated(HANDLE hPipe) {
    if (!ImpersonateNamedPipeClient(hPipe))
        return false;

    bool elevated = false;
    HANDLE hTok = nullptr;
    if (OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &hTok)) {
        TOKEN_ELEVATION te = {};
        DWORD cb = 0;
        if (GetTokenInformation(hTok, TokenElevation, &te, sizeof(te), &cb))
            elevated = (te.TokenIsElevated != 0);
        CloseHandle(hTok);
    }
    RevertToSelf();
    return elevated;
}

// -------------------------------------------------------
// Per-client read loop
// -------------------------------------------------------
void IpcServer::ClientLoop(HANDLE hPipe) {
    char buf[4096];
    std::string accumulator;

    while (true) {
        DWORD bytesRead = 0;
        BOOL ok = ReadFile(hPipe, buf, sizeof(buf) - 1, &bytesRead, nullptr);
        if (!ok || bytesRead == 0) break;

        buf[bytesRead] = '\0';
        accumulator += buf;

        // Process complete newline-delimited messages
        size_t pos;
        while ((pos = accumulator.find('\n')) != std::string::npos) {
            std::string msg = accumulator.substr(0, pos);
            accumulator.erase(0, pos + 1);

            if (msg.empty()) continue;

            std::wstring reply;
            if (m_onMessage) {
                bool privileged = ClientIsElevated(hPipe);
                reply = m_onMessage(utils::ToWide(msg), privileged);
            }

            if (!reply.empty()) {
                std::string replyNarrow = utils::ToNarrow(reply) + "\n";
                DWORD written = 0;
                WriteFile(hPipe, replyNarrow.c_str(), (DWORD)replyNarrow.size(), &written, nullptr);
            }
        }
    }

    // Sole owner of this handle: remove it from the list (so Broadcast can no
    // longer reference it), then disconnect and close, then release our slot in
    // the active-client count and wake Stop() if it is draining. All under the
    // one mutex Broadcast/Stop also take, so no other thread can touch hPipe.
    {
        std::lock_guard<std::mutex> lk(m_clientsMutex);
        auto it = std::find(m_clientPipes.begin(), m_clientPipes.end(), hPipe);
        if (it != m_clientPipes.end()) m_clientPipes.erase(it);
        DisconnectNamedPipe(hPipe);
        CloseHandle(hPipe);
        if (--m_activeClients == 0) m_clientsCv.notify_all();
    }
}

} // namespace sc
