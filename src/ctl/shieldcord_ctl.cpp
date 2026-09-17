// ============================================================
// shieldcord_ctl.exe — command-line control for ShieldCord
//
// Sends JSON commands over the ShieldCord named pipe so you can
// drive a RUNNING engine (installed service, or --console mode):
//
//   shieldcord_ctl status               show live protection status
//   shieldcord_ctl config               show current config
//   shieldcord_ctl history              show recent blocked events
//   shieldcord_ctl apps                 trusted apps + user-trusted publishers
//   shieldcord_ctl paths                the protected path list
//   shieldcord_ctl enforce on|off       arm/disarm driver default-deny
//   shieldcord_ctl file-block on|off    toggle SC_FEATURE_FILE_BLOCK
//   shieldcord_ctl alerts on|off        toggle SC_FEATURE_ALERTS
//   shieldcord_ctl decoy on|off         toggle the decoy token folder
//   shieldcord_ctl memmon on|off        toggle the memory monitor
//   shieldcord_ctl trust "<publisher>"  trust everything that publisher signs
//   shieldcord_ctl untrust "<pub>"      remove a publisher from that list
//   shieldcord_ctl kill <pid>           terminate a process
//   shieldcord_ctl features [file_block=on|off] [alerts=on|off]
//   shieldcord_ctl send <json>          send a raw command line
//
// NOTE: the set_* / kill / trust verbs require an ELEVATED shell
// (Administrator). status / config / history / apps / paths do not.
//
// Exit codes: 0 ok, 1 transport error, 2 server returned an error,
//             3 usage error.
//
// Self-contained (Win32 + stdlib only) — no project headers needed.
// ============================================================
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cctype>
#include <cstdio>
#include <string>

static const wchar_t* kPipe = L"\\\\.\\pipe\\ShieldCord";

static std::string ToLower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

static std::string TrimRight(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
        s.pop_back();
    return s;
}

static std::string WideToNarrow(const wchar_t* w) {
    if (!w || !*w) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}

static bool ParseOnOff(const std::string& v, bool& out) {
    std::string l = ToLower(v);
    if (l == "on" || l == "1" || l == "true" || l == "yes") { out = true;  return true; }
    if (l == "off" || l == "0" || l == "false" || l == "no") { out = false; return true; }
    return false;
}

static bool IsElevated() {
    bool elevated = false;
    HANDLE hTok = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hTok)) {
        TOKEN_ELEVATION te = {};
        DWORD cb = 0;
        if (GetTokenInformation(hTok, TokenElevation, &te, sizeof(te), &cb))
            elevated = (te.TokenIsElevated != 0);
        CloseHandle(hTok);
    }
    return elevated;
}

// Pull a boolean field out of the server's compact JSON reply (no JSON lib).
static bool JsonBool(const std::string& json, const std::string& key, bool fallback) {
    const std::string t1 = "\"" + key + "\":true";
    const std::string t2 = "\"" + key + "\": true";
    const std::string f1 = "\"" + key + "\":false";
    const std::string f2 = "\"" + key + "\": false";
    if (json.find(t1) != std::string::npos || json.find(t2) != std::string::npos) return true;
    if (json.find(f1) != std::string::npos || json.find(f2) != std::string::npos) return false;
    return fallback;
}

// Send one JSON command line; return {0|1|2, trimmed reply body}.
struct CtlResult {
    int         code;    // 0 ok, 1 transport, 2 server error reply
    std::string body;
};

static CtlResult Talk(const std::string& jsonCmd) {
    CtlResult res{ 1, "" };

    HANDLE h = CreateFileW(kPipe, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        std::fprintf(stderr,
                     "shieldcord_ctl: cannot connect to ShieldCord (error %lu).\n"
                     "                Start the service, or run --console mode, first.\n",
                     GetLastError());
        return res;
    }

    // Non-blocking reads so we can (a) time out and (b) filter out live
    // "alert" broadcasts that share the pipe with command replies.
    DWORD mode = PIPE_READMODE_BYTE | PIPE_NOWAIT;
    SetNamedPipeHandleState(h, &mode, nullptr, nullptr);

    std::string cmd = jsonCmd + "\n";
    DWORD written = 0;
    if (!WriteFile(h, cmd.data(), (DWORD)cmd.size(), &written, nullptr)) {
        std::fprintf(stderr, "shieldcord_ctl: write failed (error %lu).\n", GetLastError());
        CloseHandle(h);
        return res;
    }

    /*
     * The pipe carries BOTH one command reply per request AND live broadcasts
     * of block alerts. Read newline-delimited lines until the reply to OUR
     * command arrives — the first line whose type is NOT "alert"
     * (ok / status_update / config / error / ...). Broadcasts are skipped.
     */
    std::string acc;
    const DWORD deadlineMs = GetTickCount() + 6000;
    char buf[4096];

    for (;;) {
        DWORD n = 0;
        BOOL got = ReadFile(h, buf, sizeof(buf), &n, nullptr);
        if (got && n > 0)
            acc.append(buf, n);

        BOOL done = FALSE;
        size_t pos;
        while ((pos = acc.find('\n')) != std::string::npos) {
            std::string line = TrimRight(acc.substr(0, pos));
            acc.erase(0, pos + 1);
            if (line.empty()) continue;
            if (line.find("\"type\":\"alert\"") != std::string::npos ||
                line.find("\"type\": \"alert\"") != std::string::npos)
                continue;                     // unsolicited broadcast — not our reply
            res.body = line;                  // our reply (or a server error line)
            res.code = (line.find("\"error\"") != std::string::npos) ? 2 : 0;
            done = TRUE;
            break;
        }
        if (done) break;

        if (GetTickCount() >= deadlineMs) {
            std::fprintf(stderr,
                         "shieldcord_ctl: timed out waiting for a reply "
                         "(only broadcasts were seen).\n");
            CloseHandle(h);
            return res;                       // code stays 1 (transport)
        }
        Sleep(25);
    }

    CloseHandle(h);
    return res;
}

// ─── generic verb helpers (see the dispatch table in wmain) ───

// Read-only verb: send it, print the raw reply.
static int ReadVerb(const char* verb) {
    CtlResult r = Talk(std::string("{\"type\":\"") + verb + "\"}");
    if (r.code == 1) return 1;
    std::printf("%s\n", r.body.c_str());
    return r.code;
}

// Verb whose payload is a bare {"enabled":bool}. Gated on elevation
// server-side, so warn here rather than let the server reject it silently.
static int ToggleVerb(const char* verb, const std::string& value) {
    if (!IsElevated())
        std::fprintf(stderr, "note: '%s' needs an Administrator shell — the server will reject it otherwise.\n", verb);

    bool on = true;
    if (!ParseOnOff(value, on)) {
        std::fprintf(stderr, "bad value '%s' (expected on|off)\n", value.c_str());
        return 3;
    }
    CtlResult r = Talk(std::string("{\"type\":\"") + verb +
                       "\",\"enabled\":" + (on ? "true" : "false") + "}");
    if (r.code == 1) return 1;
    std::printf("%s\n", r.body.c_str());
    return r.code;
}

// Verb taking a single "publisher" string.
static int PublisherVerb(const char* verb, const std::string& publisher) {
    if (!IsElevated())
        std::fprintf(stderr, "note: '%s' needs an Administrator shell — the server will reject it otherwise.\n", verb);

    CtlResult r = Talk(std::string("{\"type\":\"") + verb +
                       "\",\"publisher\":\"" + publisher + "\"}");
    if (r.code == 1) return 1;
    std::printf("%s\n", r.body.c_str());
    return r.code;
}

static int PrintUsage() {
    std::printf(
        "shieldcord_ctl — control a running ShieldCord engine over its named pipe\n"
        "\n"
        "Usage:\n"
        "  shieldcord_ctl status               show live protection status\n"
        "  shieldcord_ctl config               show current configuration\n"
        "  shieldcord_ctl history              show recent blocked events\n"
        "  shieldcord_ctl apps                 show trusted apps + user-trusted publishers\n"
        "  shieldcord_ctl paths                show the protected path list\n"
        "  shieldcord_ctl enforce on|off       arm/disarm driver default-deny\n"
        "  shieldcord_ctl file-block on|off    toggle file blocking (SC_FEATURE_FILE_BLOCK)\n"
        "  shieldcord_ctl alerts on|off        toggle block alerts  (SC_FEATURE_ALERTS)\n"
        "  shieldcord_ctl decoy on|off         toggle the decoy token folder\n"
        "  shieldcord_ctl memmon on|off        toggle the memory monitor\n"
        "  shieldcord_ctl trust \"<publisher>\"   trust everything signed by a publisher\n"
        "  shieldcord_ctl untrust \"<pub>\"       remove a publisher from that list\n"
        "  shieldcord_ctl kill <pid>           terminate a process\n"
        "  shieldcord_ctl features [file_block=on|off] [alerts=on|off]   (default = on)\n"
        "  shieldcord_ctl send <json>          send a raw command line\n"
        "\n"
        "The set_* / kill / trust verbs require an Administrator shell.\n"
        "status / config / history / apps / paths work from any shell.\n");
    return 3;
}

int wmain(int argc, wchar_t* argv[]) {
    if (argc < 2) return PrintUsage();

    std::string cmd = ToLower(WideToNarrow(argv[1]));

    if (cmd == "help" || cmd == "-h" || cmd == "/h" || cmd == "--help" || cmd == "?") {
        PrintUsage();
        return 0;
    }

    if (cmd == "status") {
        CtlResult r = Talk("{\"type\":\"get_status\"}");
        if (r.code == 1) return 1;
        std::printf("%s\n", r.body.c_str());
        return r.code;
    }

    if (cmd == "config") {
        CtlResult r = Talk("{\"type\":\"get_config\"}");
        if (r.code == 1) return 1;
        std::printf("%s\n", r.body.c_str());
        return r.code;
    }

    if (cmd == "enforce" || cmd == "enforcement") {
        if (argc < 3) { std::fprintf(stderr, "usage: shieldcord_ctl enforce on|off\n"); return 3; }
        if (!IsElevated())
            std::fprintf(stderr, "note: set_enforcement needs an Administrator shell — run this prompt elevated or the server will reject it.\n");
        bool on = true;
        if (!ParseOnOff(WideToNarrow(argv[2]), on)) {
            std::fprintf(stderr, "bad value '%ls' (expected on|off)\n", argv[2]);
            return 3;
        }
        CtlResult r = Talk(std::string("{\"type\":\"set_enforcement\",\"enabled\":") +
                           (on ? "true" : "false") + "}");
        if (r.code == 1) return 1;
        std::printf("%s\n", r.body.c_str());
        return r.code;
    }

    if (cmd == "file-block" || cmd == "fileblock" || cmd == "file_block") {
        if (argc < 3) { std::fprintf(stderr, "usage: shieldcord_ctl file-block on|off\n"); return 3; }
        if (!IsElevated())
            std::fprintf(stderr, "note: set_features needs an Administrator shell — run this prompt elevated or the server will reject it.\n");
        bool on = true;
        if (!ParseOnOff(WideToNarrow(argv[2]), on)) {
            std::fprintf(stderr, "bad value '%ls' (expected on|off)\n", argv[2]);
            return 3;
        }
        // set_features sets BOTH bits server-side, so read current alerts first.
        CtlResult cfg = Talk("{\"type\":\"get_config\"}");
        if (cfg.code != 0) return cfg.code;
        bool al = JsonBool(cfg.body, "alerts", true);
        CtlResult r = Talk(std::string("{\"type\":\"set_features\",\"file_block\":") +
                           (on ? "true" : "false") + ",\"alerts\":" + (al ? "true" : "false") + "}");
        if (r.code == 1) return 1;
        std::printf("%s\n", r.body.c_str());
        return r.code;
    }

    if (cmd == "alerts") {
        if (argc < 3) { std::fprintf(stderr, "usage: shieldcord_ctl alerts on|off\n"); return 3; }
        if (!IsElevated())
            std::fprintf(stderr, "note: set_features needs an Administrator shell — run this prompt elevated or the server will reject it.\n");
        bool on = true;
        if (!ParseOnOff(WideToNarrow(argv[2]), on)) {
            std::fprintf(stderr, "bad value '%ls' (expected on|off)\n", argv[2]);
            return 3;
        }
        CtlResult cfg = Talk("{\"type\":\"get_config\"}");
        if (cfg.code != 0) return cfg.code;
        bool fb = JsonBool(cfg.body, "file_block", true);
        CtlResult r = Talk(std::string("{\"type\":\"set_features\",\"file_block\":") +
                           (fb ? "true" : "false") + ",\"alerts\":" + (on ? "true" : "false") + "}");
        if (r.code == 1) return 1;
        std::printf("%s\n", r.body.c_str());
        return r.code;
    }

    if (cmd == "features") {
        if (!IsElevated())
            std::fprintf(stderr, "note: set_features needs an Administrator shell — run this prompt elevated or the server will reject it.\n");
        CtlResult cfg = Talk("{\"type\":\"get_config\"}");
        if (cfg.code != 0) return cfg.code;
        bool fb = JsonBool(cfg.body, "file_block", true);
        bool al = JsonBool(cfg.body, "alerts",     true);
        for (int i = 2; i < argc; i++) {
            std::string kv = ToLower(WideToNarrow(argv[i]));
            bool v = false;
            if (kv.rfind("file_block=", 0) == 0 && ParseOnOff(kv.substr(11), v)) fb = v;
            else if (kv.rfind("file-block=", 0) == 0 && ParseOnOff(kv.substr(11), v)) fb = v;
            else if (kv.rfind("alerts=", 0) == 0 && ParseOnOff(kv.substr(7), v)) al = v;
            else { std::fprintf(stderr, "bad argument '%s' (expected file_block=on|off alerts=on|off)\n", kv.c_str()); return 3; }
        }
        CtlResult r = Talk(std::string("{\"type\":\"set_features\",\"file_block\":") +
                           (fb ? "true" : "false") + ",\"alerts\":" + (al ? "true" : "false") + "}");
        if (r.code == 1) return 1;
        std::printf("%s\n", r.body.c_str());
        return r.code;
    }

    if (cmd == "history" || cmd == "alerts-list") {
        // since=0 returns the whole retained history
        CtlResult r = Talk("{\"type\":\"get_alerts\",\"since\":0}");
        if (r.code == 1) return 1;
        std::printf("%s\n", r.body.c_str());
        return r.code;
    }

    if (cmd == "apps")   return ReadVerb("get_protected_apps");
    if (cmd == "paths")  return ReadVerb("get_protected_paths");

    if (cmd == "decoy") {
        if (argc < 3) { std::fprintf(stderr, "usage: shieldcord_ctl decoy on|off\n"); return 3; }
        return ToggleVerb("set_decoy", WideToNarrow(argv[2]));
    }

    if (cmd == "memmon" || cmd == "memory-monitor") {
        if (argc < 3) { std::fprintf(stderr, "usage: shieldcord_ctl memmon on|off\n"); return 3; }
        return ToggleVerb("set_memory_monitor", WideToNarrow(argv[2]));
    }

    if (cmd == "trust") {
        if (argc < 3) { std::fprintf(stderr, "usage: shieldcord_ctl trust \"<publisher CN>\"\n"); return 3; }
        return PublisherVerb("trust_publisher", WideToNarrow(argv[2]));
    }

    if (cmd == "untrust") {
        if (argc < 3) { std::fprintf(stderr, "usage: shieldcord_ctl untrust \"<publisher CN>\"\n"); return 3; }
        return PublisherVerb("untrust_publisher", WideToNarrow(argv[2]));
    }

    if (cmd == "kill") {
        if (argc < 3) { std::fprintf(stderr, "usage: shieldcord_ctl kill <pid>\n"); return 3; }
        if (!IsElevated())
            std::fprintf(stderr, "note: kill_process needs an Administrator shell.\n");
        CtlResult r = Talk(std::string("{\"type\":\"kill_process\",\"pid\":") +
                           WideToNarrow(argv[2]) + "}");
        if (r.code == 1) return 1;
        std::printf("%s\n", r.body.c_str());
        return r.code;
    }

    if (cmd == "send") {
        if (argc < 3) { std::fprintf(stderr, "usage: shieldcord_ctl send <json>\n"); return 3; }
        std::string json;
        for (int i = 2; i < argc; i++) {
            if (i > 2) json += ' ';
            json += WideToNarrow(argv[i]);
        }
        CtlResult r = Talk(json);
        if (r.code == 1) return 1;
        std::printf("%s\n", r.body.c_str());
        return r.code;
    }

    std::fprintf(stderr, "shieldcord_ctl: unknown command '%ls'\n\n", argv[1]);
    return PrintUsage();
}
