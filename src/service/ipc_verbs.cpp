// ============================================================
// ShieldCord — ipc_verbs.cpp
// The single implementation of every named-pipe verb.
// ============================================================
#include "ipc_verbs.h"
#include "config.h"
#include "driver_client.h"
#include "ipc_server.h"
#include "process_validator.h"
#include "app_watcher.h"
#include "utils.h"
#include "logger.h"
#include "../shared/ipc_protocol.h"
#include "../shared/whitelist.h"
#include "../shared/token_paths.h"
#include <tlhelp32.h>
#include <algorithm>
#include <set>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace sc {

namespace {

// ─── reply helpers ───────────────────────────────────────────

/*
 * Echo the request's "id" on the reply.
 *
 * The pipe is duplex and carries unsolicited `alert` broadcasts, so a client
 * cannot assume the next line it reads answers what it just sent. Echoing the
 * id is what makes correlation exact. Absent id => no echo, which is fine for
 * one-shot CLI use.
 */
std::wstring Reply(const json& req, json reply) {
    if (req.contains(ipc::field::ID))
        reply[ipc::field::ID] = req[ipc::field::ID];
    return utils::ToWide(reply.dump());
}

std::wstring ErrorReply(const json& req, const char* code) {
    json r;
    r[ipc::field::TYPE]  = ipc::MSG_ERROR;
    r[ipc::field::ERR] = code;
    return Reply(req, r);
}

/*
 * Gate a state-changing verb on the caller's elevation.
 *
 * The pipe ACL deliberately lets Authenticated Users connect (so the
 * non-elevated tray UI can read status and receive alerts), which means the
 * ACL is NOT the authorization boundary — this check is. Returns false and
 * fills 'out' when the caller is not authorized.
 */
bool Authorized(const json& req, bool privileged, const char* verb, std::wstring* out) {
    if (privileged) return true;
    LOG_WARN(L"IPC", L"Rejected privileged verb '" + utils::ToWide(verb) +
                     L"' from a non-elevated client.");
    *out = ErrorReply(req, ipc::ERR_NOT_AUTHORIZED);
    return false;
}

// ─── snapshot builders ───────────────────────────────────────

json BuildStatusJson(IpcContext& ctx) {
    const Config& c = ConfigManager::Instance().Get();

    json r;
    r[ipc::field::TYPE]        = ipc::MSG_STATUS_UPDATE;
    r["driver_connected"]      = DriverClient::Instance().IsConnected();
    r["driver_enforcement"]    = c.driverEnforcementEnabled;
    r["driver_file_block"]     = c.driverFileBlockEnabled;
    r["driver_alerts"]         = c.driverAlertsEnabled;
    r["memory_monitor_active"] = c.memoryMonitorEnabled;
    r["decoy_enabled"]         = c.decoyFolderEnabled;
    r["decoy_folder_path"]     = utils::ToNarrow(c.decoyFolderPath);
    r["uptime_seconds"]        = (long long)((GetTickCount64() - ctx.startTick) / 1000);
    r["threats_blocked"]       = (long long)AlertHistory::Instance().Total();
    r["trusted_process_count"] = (int)AppWatcher::Instance().TrustedCount();
    r["version"]               = utils::ToNarrow(SC_VERSION);
    r["service_mode"]          = utils::ToNarrow(ctx.mode);
    return r;
}

json BuildConfigJson() {
    const Config& c = ConfigManager::Instance().Get();

    json r;
    r[ipc::field::TYPE]        = ipc::MSG_CONFIG;
    r["enforcement"]           = c.driverEnforcementEnabled;
    r["file_block"]            = c.driverFileBlockEnabled;
    r["alerts"]                = c.driverAlertsEnabled;
    r["decoy_folder"]          = c.decoyFolderEnabled;
    r["memory_monitor_active"] = c.memoryMonitorEnabled;
    r["decoy_folder_path"]     = utils::ToNarrow(c.decoyFolderPath);
    return r;
}

// Friendly app name for a protected-path suffix, so the UI can group the
// list by product instead of showing raw path fragments.
std::wstring LabelForSuffix(const std::wstring& suffix) {
    struct { const wchar_t* needle; const wchar_t* label; } kMap[] = {
        { L"\\discordptb\\",       L"Discord PTB"    },
        { L"\\discordcanary\\",    L"Discord Canary" },
        { L"\\discord\\",          L"Discord"        },
        { L"\\Google\\Chrome\\",   L"Google Chrome"  },
        { L"\\BraveSoftware\\",    L"Brave"          },
        { L"\\Microsoft\\Edge\\",  L"Microsoft Edge" },
        { L"\\Opera Software\\",   L"Opera"          },
        { L"\\Mozilla\\Firefox\\", L"Firefox"        },
        { L"ShieldCord\\Decoy",    L"ShieldCord decoy (bait)" },
    };

    std::wstring low = utils::ToLower(suffix);
    for (const auto& m : kMap) {
        if (low.find(utils::ToLower(m.needle)) != std::wstring::npos)
            return m.label;
    }
    return L"Protected";
}

json BuildProtectedPathsJson() {
    json arr = json::array();
    for (const auto& s : PROTECTED_PATH_SUFFIXES) {
        json e;
        e["suffix"] = utils::ToNarrow(s);
        e["label"]  = utils::ToNarrow(LabelForSuffix(s));
        arr.push_back(e);
    }

    json r;
    r[ipc::field::TYPE] = ipc::MSG_PROTECTED_PATHS;
    r["paths"] = arr;
    return r;
}

json BuildProtectedAppsJson() {
    // One process snapshot for the whole list — cheaper than asking the OS
    // per entry, and this verb is only called when a UI opens the view.
    std::set<std::wstring> running;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe = {};
        pe.dwSize = sizeof(pe);
        if (Process32FirstW(snap, &pe)) {
            do { running.insert(utils::ToLower(std::wstring(pe.szExeFile))); }
            while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
    }

    json arr = json::array();

    // Built-in whitelist: matched by image name AND signature publisher.
    for (const auto& ta : TRUSTED_APPS) {
        json e;
        e["process_name"] = utils::ToNarrow(ta.processName);
        e["publisher"]    = utils::ToNarrow(PrimaryPublisher(ta));
        e["app_tag"]      = utils::ToNarrow(ta.appTag);
        e["builtin"]      = true;
        e["running"]      = running.count(utils::ToLower(ta.processName)) > 0;
        e["trusted"]      = AppWatcher::Instance().IsTagTrusted(ta.appTag);
        arr.push_back(e);
    }

    // Publishers the user trusted at runtime. These carry no process name or
    // tag by design — they apply to whatever that publisher signs.
    for (const auto& p : ConfigManager::Instance().Get().trustedPublishers) {
        json e;
        e["process_name"] = "";
        e["publisher"]    = utils::ToNarrow(p);
        e["app_tag"]      = "";
        e["builtin"]      = false;
        e["running"]      = false;
        e["trusted"]      = false;
        arr.push_back(e);
    }

    json r;
    r[ipc::field::TYPE] = ipc::MSG_PROTECTED_APPS;
    r["apps"] = arr;
    return r;
}

// ─── publisher trust list ────────────────────────────────────

bool AddTrustedPublisher(const std::wstring& publisher) {
    auto& list = ConfigManager::Instance().GetMut().trustedPublishers;

    std::wstring low = utils::ToLower(publisher);
    for (const auto& p : list)
        if (utils::ToLower(p) == low) return false;      // already present

    list.push_back(publisher);
    return true;
}

bool RemoveTrustedPublisher(const std::wstring& publisher) {
    auto& list = ConfigManager::Instance().GetMut().trustedPublishers;
    std::wstring low = utils::ToLower(publisher);

    for (auto it = list.begin(); it != list.end(); ++it) {
        if (utils::ToLower(*it) == low) { list.erase(it); return true; }
    }
    return false;
}

} // anonymous namespace

// ============================================================
// PublishAlert — record, broadcast, log. One path for every alert
// source so the id sequence and the UI's view stay consistent.
// ============================================================
void PublishAlert(AlertRecord rec) {
    AlertRecord stored = AlertHistory::Instance().Append(std::move(rec));
    std::wstring line = AlertRecordToJson(stored);
    IpcServer::Instance().Broadcast(line);
    LOG_THREAT(L"Alert", line);
}

// ============================================================
// RemoveDecoyTree — delete the bait token tree when the user turns
// the decoy folder off. Best-effort: a file still held open by another
// process is skipped, and the caller reports whatever remains.
// ============================================================
bool RemoveDecoyTree(const std::wstring& path) {
    DWORD attr = GetFileAttributesW(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) return true;              // already gone
    if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) return DeleteFileW(path.c_str()) != 0;

    WIN32_FIND_DATAW fd = {};
    std::wstring pattern = path + L"\\*";
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            std::wstring child = path + L"\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                    RemoveDirectoryW(child.c_str());
                } else {
                    RemoveDecoyTree(child);
                }
            } else {
                SetFileAttributesW(child.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(child.c_str());
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return RemoveDirectoryW(path.c_str()) != 0;
}

// ============================================================
// MakeDriverBlockRecord — one shape for a kernel block, whichever engine
// observed it. Static on purpose: a block reaches the UI only through
// PublishDriverBlock below, which is where the trust race is dealt with.
// ============================================================
static AlertRecord MakeDriverBlockRecord(const SC_BLOCK_ALERT& alert) {
    DWORD pid = (DWORD)alert.BlockedPid;

    // Bound every read: the kernel struct's fixed-size wchar_t arrays are not
    // guaranteed to be null-terminated when completely filled.
    size_t nameLen = wcsnlen(alert.BlockedImageName, _countof(alert.BlockedImageName));
    size_t pathLen = wcsnlen(alert.BlockedPath,      _countof(alert.BlockedPath));

    AlertRecord rec;
    rec.source      = utils::ToWide(ipc::SOURCE_DRIVER);
    rec.pid         = pid;
    rec.processName = std::wstring(alert.BlockedImageName, nameLen);
    rec.path        = std::wstring(alert.BlockedPath, pathLen);
    rec.action      = utils::ToWide(ipc::ACTION_BLOCKED);

    /*
     * This record only ever describes a block we could NOT repair, so it is
     * always the real thing and always high severity.
     *
     * A block that the trust-race repair resolved never gets here — see
     * PublishDriverBlock. It used to: this branch downgraded such a race to
     * info and still published it, which at 2-6 entries per browser restart was
     * the bulk of the timeline. A feed of "blocked" entries that all resolved
     * themselves is how a security tool trains someone to stop reading it, and
     * the entry that finally matters goes unread with the rest.
     */
    rec.severity = utils::ToWide(ipc::SEV_HIGH);
    rec.message  = L"Kernel blocked an untrusted process from opening a protected file.";

    // Resolve the signer while the process still exists — a moment later it
    // may be gone and the CN unrecoverable, leaving the UI's "trust this
    // publisher" action with nothing to act on. This is cached, so the cost
    // is one WinVerifyTrust for a binary we have not seen before.
    rec.publisher = ProcessValidator::GetPublisher(pid);
    return rec;
}

// ============================================================
// PublishDriverBlock — the ONE place a kernel block becomes an alert.
//
// Repair the trust race FIRST, before deciding anything: a legitimate app can
// open its storage in the window between its process starting and our trust
// message landing, and default-deny would otherwise leave it locked out. That
// is the "Discord logged me out" failure mode, and it must never ship.
//
// A race we then repaired is NOT published. It cost the app nothing permanent,
// its cause is our own verification latency rather than anything the user did,
// and it arrives 2-6 times per browser restart — so it is the bulk of what the
// timeline shows. It goes to the log instead, which keeps the event
// diagnosable without teaching anyone to ignore the feed.
//
// A block we could NOT repair is the real thing and IS published.
//
// Both engines call this rather than each doing the heal-then-publish dance,
// so console and service mode cannot drift apart on it.
// ============================================================
void PublishDriverBlock(const SC_BLOCK_ALERT& alert) {
    DWORD pid = (DWORD)alert.BlockedPid;

    if (SelfHealTrustedProcess(pid)) {
        // Bounded reads: the kernel struct's arrays are not guaranteed to be
        // null-terminated when completely filled.
        size_t nameLen = wcsnlen(alert.BlockedImageName, _countof(alert.BlockedImageName));
        size_t pathLen = wcsnlen(alert.BlockedPath,      _countof(alert.BlockedPath));

        LOG_INFO(L"Driver", L"Trust race repaired for '" +
                 std::wstring(alert.BlockedImageName, nameLen) + L"' (pid " +
                 std::to_wstring(pid) + L") on the protected path '" +
                 std::wstring(alert.BlockedPath, pathLen) +
                 L"' — resolved automatically, not reported as an alert.");
        return;
    }

    PublishAlert(MakeDriverBlockRecord(alert));
}

// ============================================================
// SelfHealTrustedProcess — see the header for why this exists.
// ============================================================
bool SelfHealTrustedProcess(DWORD pid) {
    if (pid == 0) return false;

    bool known = false;
    if (ProcessValidator::FindTrustedApp(pid) != nullptr) {
        known = true;
    } else if (ProcessValidator::MatchesRuntimePublisher(pid)) {
        known = true;
    }
    if (!known) return false;

    std::wstring name = utils::GetProcessName(pid);
    if (name.empty()) return false;

    DriverClient::Instance().AddTrustedPid(pid, name);
    LOG_WARN(L"IPC", L"Self-healed trusted process pid=" + std::to_wstring(pid) +
                     L" ('" + name + L"') — re-trusted after a trust race.");
    return true;
}

// ============================================================
// HandleIpcVerb
// ============================================================
std::wstring HandleIpcVerb(const std::wstring& wMsg, bool privileged, IpcContext& ctx) {
    json req;
    try {
        req = json::parse(utils::ToNarrow(wMsg));
    } catch (...) {
        json r;
        r[ipc::field::TYPE]  = ipc::MSG_ERROR;
        r[ipc::field::ERR] = ipc::ERR_PARSE_ERROR;
        return utils::ToWide(r.dump());
    }

    const std::string type = req.value(ipc::field::TYPE, "");
    std::wstring deny;

    // ── read verbs (no elevation) ────────────────────────────

    if (type == ipc::MSG_GET_STATUS)  return Reply(req, BuildStatusJson(ctx));
    if (type == ipc::MSG_GET_CONFIG)  return Reply(req, BuildConfigJson());
    if (type == ipc::MSG_GET_PROTECTED_PATHS) return Reply(req, BuildProtectedPathsJson());
    if (type == ipc::MSG_GET_PROTECTED_APPS)  return Reply(req, BuildProtectedAppsJson());

    if (type == ipc::MSG_GET_ALERTS) {
        long long since = req.value(ipc::field::SINCE, (long long)0);
        auto records = AlertHistory::Instance().Since(since);

        json arr = json::array();
        for (const auto& rec : records)
            arr.push_back(json::parse(utils::ToNarrow(AlertRecordToJson(rec))));

        json r;
        r[ipc::field::TYPE]    = ipc::MSG_ALERTS;
        r[ipc::field::RECORDS] = arr;
        return Reply(req, r);
    }

    // ── write verbs (elevation required) ─────────────────────

    if (type == ipc::MSG_SET_ENFORCEMENT) {
        if (!Authorized(req, privileged, ipc::MSG_SET_ENFORCEMENT, &deny)) return deny;

        bool enable = req.value(ipc::field::ENABLED, true);
        ConfigManager::Instance().GetMut().driverEnforcementEnabled = enable;
        ConfigManager::Instance().Save(SC_CONFIG_FILE);
        DriverClient::Instance().SetEnforcement(enable);
        LOG_THREAT(L"IPC", enable ? L"Enforcement ARMED via UI/CLI."
                                  : L"Enforcement DISARMED via UI/CLI — tokens are UNPROTECTED.");

        json r;
        r[ipc::field::TYPE]    = ipc::MSG_OK;
        r[ipc::field::ENABLED] = enable;
        return Reply(req, r);
    }

    if (type == ipc::MSG_SET_FEATURES) {
        if (!Authorized(req, privileged, ipc::MSG_SET_FEATURES, &deny)) return deny;

        bool fb = req.value(ipc::field::FILE_BLOCK, true);
        bool al = req.value(ipc::field::ALERTS,     true);
        ConfigManager::Instance().GetMut().driverFileBlockEnabled = fb;
        ConfigManager::Instance().GetMut().driverAlertsEnabled    = al;
        ConfigManager::Instance().Save(SC_CONFIG_FILE);
        DriverClient::Instance().SetFeatureFlags(
            (fb ? SC_FEATURE_FILE_BLOCK : 0) |
            (al ? SC_FEATURE_ALERTS     : 0));

        json r;
        r[ipc::field::TYPE]       = ipc::MSG_OK;
        r[ipc::field::FILE_BLOCK] = fb;
        r[ipc::field::ALERTS]     = al;
        return Reply(req, r);
    }

    if (type == ipc::MSG_SET_DECOY || type == ipc::MSG_SET_MEMORY_MONITOR) {
        if (!Authorized(req, privileged, type.c_str(), &deny)) return deny;

        bool enable = req.value(ipc::field::ENABLED, true);

        if (type == ipc::MSG_SET_DECOY) {
            ConfigManager::Instance().GetMut().decoyFolderEnabled = enable;
        } else {
            ConfigManager::Instance().GetMut().memoryMonitorEnabled = enable;
        }
        ConfigManager::Instance().Save(SC_CONFIG_FILE);

        // The engine owns the side effects (creating/removing the decoy tree,
        // starting/stopping the monitor thread) — the verb only records intent.
        if (ctx.onSettingsChanged) ctx.onSettingsChanged();

        json r;
        r[ipc::field::TYPE]    = ipc::MSG_OK;
        r[ipc::field::ENABLED] = enable;
        return Reply(req, r);
    }

    if (type == ipc::MSG_KILL_PROCESS) {
        if (!Authorized(req, privileged, ipc::MSG_KILL_PROCESS, &deny)) return deny;

        DWORD pid = req.value(ipc::field::PID, 0);
        bool ok   = (pid > 0) ? utils::KillProcess(pid) : false;

        json r;
        r[ipc::field::TYPE]    = ipc::MSG_KILL_RESULT;
        r[ipc::field::PID]     = (unsigned long)pid;
        r[ipc::field::SUCCESS] = ok;
        return Reply(req, r);
    }

    if (type == ipc::MSG_TRUST_PUBLISHER) {
        if (!Authorized(req, privileged, ipc::MSG_TRUST_PUBLISHER, &deny)) return deny;

        std::wstring publisher = utils::ToWide(req.value(ipc::field::PUBLISHER, std::string()));
        if (publisher.empty()) return ErrorReply(req, ipc::ERR_BAD_REQUEST);

        bool added = AddTrustedPublisher(publisher);
        ConfigManager::Instance().Save(SC_CONFIG_FILE);

        /*
         * Widening trust is exactly the kind of change that must be
         * reconstructible after the fact, so it is logged at THREAT level
         * even though it is a legitimate user action.
         */
        LOG_THREAT(L"IPC", L"Publisher TRUSTED via UI/CLI: '" + publisher + L"'" +
                           (added ? L"" : L" (already present)"));

        /*
         * If the caller named the process that was just blocked, trust it
         * immediately — otherwise the user would have to restart the app for
         * the decision to take effect, which reads as "the button did nothing".
         */
        DWORD pid = req.value(ipc::field::PID, 0);
        bool  pidTrusted = false;
        if (pid > 0) {
            std::wstring name = utils::GetProcessName(pid);
            if (!name.empty()) {
                DriverClient::Instance().AddTrustedPid(pid, name);
                pidTrusted = true;
            }
        }

        json r;
        r[ipc::field::TYPE]      = ipc::MSG_OK;
        r[ipc::field::PUBLISHER] = utils::ToNarrow(publisher);
        r["added"]               = added;
        r["pid_trusted"]         = pidTrusted;
        return Reply(req, r);
    }

    if (type == ipc::MSG_UNTRUST_PUBLISHER) {
        if (!Authorized(req, privileged, ipc::MSG_UNTRUST_PUBLISHER, &deny)) return deny;

        std::wstring publisher = utils::ToWide(req.value(ipc::field::PUBLISHER, std::string()));
        if (publisher.empty()) return ErrorReply(req, ipc::ERR_BAD_REQUEST);

        bool removed = RemoveTrustedPublisher(publisher);
        ConfigManager::Instance().Save(SC_CONFIG_FILE);

        LOG_THREAT(L"IPC", L"Publisher UNTRUSTED via UI/CLI: '" + publisher + L"'" +
                           (removed ? L"" : L" (was not present)"));

        json r;
        r[ipc::field::TYPE]      = ipc::MSG_OK;
        r[ipc::field::PUBLISHER] = utils::ToNarrow(publisher);
        r["removed"]             = removed;
        // Honest caveat: PIDs already trusted by the driver stay trusted
        // until they exit. Revoking them here would mean re-verifying every
        // running process, which is not worth the latency for a rare action.
        r["note"] = "Applies to processes started from now on; "
                    "already-running processes keep access until they exit.";
        return Reply(req, r);
    }

    if (type == ipc::MSG_SHUTDOWN) {
        if (!Authorized(req, privileged, ipc::MSG_SHUTDOWN, &deny)) return deny;

        LOG_INFO(L"IPC", L"Shutdown requested via IPC (authorized).");

        // Disarm SCM crash-recovery FIRST, before anything starts tearing down.
        // Teardown is not instantaneous and must not be able to trip the restart
        // policy that exists for crashes — see IpcContext::onDeliberateStop.
        if (ctx.onDeliberateStop) ctx.onDeliberateStop();

        // Signal only. The engine tears itself down on its own thread —
        // doing it inline would join the very client thread we are on.
        if (ctx.requestStop) ctx.requestStop();

        json r;
        r[ipc::field::TYPE] = ipc::MSG_OK;
        return Reply(req, r);
    }

    if (type == ipc::MSG_RECONNECT_DRIVER) {
        if (!Authorized(req, privileged, ipc::MSG_RECONNECT_DRIVER, &deny)) return deny;

        if (!ctx.onReconnectDriver) {
            // The DACL-only --test engine has no driver stream to reconnect.
            // Say which engine refused rather than reporting a bare bad_request.
            json r;
            r[ipc::field::TYPE]  = ipc::MSG_ERROR;
            r[ipc::field::ERR]   = ipc::ERR_BAD_REQUEST;
            r["message"]         = "This engine does not drive the kernel filter.";
            return Reply(req, r);
        }

        bool connected = ctx.onReconnectDriver();
        LOG_INFO(L"IPC", connected
            ? L"reconnect_driver: kernel filter connected and armed."
            : L"reconnect_driver: kernel filter unavailable — no file protection.");

        /*
         * "ok" answers whether the REQUEST was honored; driver_connected says
         * whether the driver actually came up. The two are different facts, and
         * collapsing them is how a caller ends up telling the user protection is
         * on over a driver that is not running. Same split as kill_result.
         */
        json r;
        r[ipc::field::TYPE]   = ipc::MSG_OK;
        r["driver_connected"] = connected;
        return Reply(req, r);
    }

    return ErrorReply(req, ipc::ERR_UNKNOWN_TYPE);
}

} // namespace sc
