#pragma once
// ============================================================
// ShieldCord — token_paths.h
// All sensitive file/directory paths to protect
// ============================================================
#include "common.h"
#include <string>
#include <vector>

namespace sc {

// Expand %APPDATA%, %LOCALAPPDATA% etc. at runtime
inline std::wstring ExpandPath(const std::wstring& tmpl) {
    wchar_t buf[MAX_PATH * 2] = {};
    ExpandEnvironmentStringsW(tmpl.c_str(), buf, _countof(buf));
    return buf;
}

// Describes one protected path entry
struct ProtectedPath {
    std::wstring rawPath;        // May contain env vars
    std::wstring expandedPath;   // Filled at startup
    bool         isDirectory;    // true = protect entire dir recursively
    std::wstring appTag;         // "discord", "chrome", "brave", etc.
};

inline std::vector<ProtectedPath> BuildProtectedPaths() {
    std::vector<ProtectedPath> p;

    auto add = [&](const wchar_t* raw, bool isDir, const wchar_t* tag) {
        ProtectedPath pp;
        pp.rawPath       = raw;
        pp.expandedPath  = ExpandPath(raw);
        pp.isDirectory   = isDir;
        pp.appTag        = tag;
        p.push_back(pp);
    };

    // ---- Discord -------------------------------------------------------
    add(L"%APPDATA%\\discord\\Local Storage\\leveldb",       true,  L"discord");
    add(L"%APPDATA%\\discordptb\\Local Storage\\leveldb",    true,  L"discordptb");
    add(L"%APPDATA%\\discordcanary\\Local Storage\\leveldb", true,  L"discordcanary");
    add(L"%APPDATA%\\discord\\Session Storage",              true,  L"discord");

    // ---- Google Chrome -------------------------------------------------
    add(L"%LOCALAPPDATA%\\Google\\Chrome\\User Data\\Default\\Network\\Cookies",     false, L"chrome");
    add(L"%LOCALAPPDATA%\\Google\\Chrome\\User Data\\Default\\Local Storage",        true,  L"chrome");
    add(L"%LOCALAPPDATA%\\Google\\Chrome\\User Data\\Default\\Session Storage",      true,  L"chrome");
    add(L"%LOCALAPPDATA%\\Google\\Chrome\\User Data\\Default\\Login Data",           false, L"chrome");

    // ---- Brave ---------------------------------------------------------
    add(L"%LOCALAPPDATA%\\BraveSoftware\\Brave-Browser\\User Data\\Default\\Network\\Cookies", false, L"brave");
    add(L"%LOCALAPPDATA%\\BraveSoftware\\Brave-Browser\\User Data\\Default\\Local Storage",    true,  L"brave");
    add(L"%LOCALAPPDATA%\\BraveSoftware\\Brave-Browser\\User Data\\Default\\Login Data",       false, L"brave");

    // ---- Microsoft Edge ------------------------------------------------
    add(L"%LOCALAPPDATA%\\Microsoft\\Edge\\User Data\\Default\\Network\\Cookies",    false, L"edge");
    add(L"%LOCALAPPDATA%\\Microsoft\\Edge\\User Data\\Default\\Local Storage",       true,  L"edge");
    add(L"%LOCALAPPDATA%\\Microsoft\\Edge\\User Data\\Default\\Login Data",          false, L"edge");

    // ---- Opera ---------------------------------------------------------
    add(L"%LOCALAPPDATA%\\Opera Software\\Opera Stable\\Network\\Cookies",           false, L"opera");
    add(L"%LOCALAPPDATA%\\Opera Software\\Opera Stable\\Local Storage",              true,  L"opera");

    // ---- Firefox -------------------------------------------------------
    // Firefox profile directories are dynamic; we protect the whole Profiles root
    add(L"%APPDATA%\\Mozilla\\Firefox\\Profiles",                                    true,  L"firefox");

    return p;
}

// Which process tags need protection for memory reads
inline const std::vector<std::wstring> PROTECTED_PROCESS_NAMES = {
    L"discord.exe",
    L"chrome.exe",
    L"brave.exe",
    L"msedge.exe",
    L"opera.exe",
    L"firefox.exe",
};

// ============================================================
// DEVELOPER TEST SANDBOX (--test mode)
// Fake token/cookie folders under %TEMP% — the real Discord /
// browser profiles are never touched. Deleted on clean exit.
// ============================================================

// Where the dev-mode DACL backup is written (crash safety)
inline std::wstring TestBackupFile() {
    return ExpandPath(L"%TEMP%\\ShieldCordDaclBackup.json");
}

inline std::wstring TestSandboxRoot() {
    return ExpandPath(L"%TEMP%\\ShieldCordTest");
}

// Same paths as the real discord/chrome entries, but inside the sandbox
inline std::vector<ProtectedPath> BuildTestProtectedPaths() {
    std::vector<ProtectedPath> p;

    auto add = [&](const wchar_t* raw, bool isDir, const wchar_t* tag) {
        ProtectedPath pp;
        pp.rawPath       = raw;
        pp.expandedPath  = ExpandPath(raw);
        pp.isDirectory   = isDir;
        pp.appTag        = tag;
        p.push_back(pp);
    };

    add(L"%TEMP%\\ShieldCordTest\\discord\\Local Storage\\leveldb", true,  L"discord");
    add(L"%TEMP%\\ShieldCordTest\\discord\\Session Storage",         true,  L"discord");
    add(L"%TEMP%\\ShieldCordTest\\chrome\\Default\\Network\\Cookies", false, L"chrome");

    return p;
}

// ---- small local helpers (kept header-only / self-contained) ----

inline void TestEnsureDir(const std::wstring& dir) {
    if (dir.empty()) return;
    DWORD a = GetFileAttributesW(dir.c_str());
    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) return;
    size_t pos = dir.find_last_of(L"\\/");
    if (pos != std::wstring::npos) TestEnsureDir(dir.substr(0, pos));
    CreateDirectoryW(dir.c_str(), nullptr);
}

inline void TestWriteFile(const std::wstring& path, const wchar_t* content) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(h, content, (DWORD)(wcslen(content) * sizeof(wchar_t)), &written, nullptr);
    CloseHandle(h);
}

// ============================================================
// Mark a folder tree as "do not index the contents".
//
// WHY THE DECOY NEEDS THIS
//
//   Windows Search (SearchIndexer.exe) crawls files it sees appear. The decoy
//   tree is bait: writing it at startup wakes the indexer, which immediately
//   tries to open the new files — and by then the driver is armed and default-
//   deny, so the attempt is BLOCKED and reported as a threat.
//
//   The block is correct, but the alert is noise, and noise in a security tool
//   is dangerous: it teaches people to ignore the one alert that matters. It
//   also reads as "publisher unknown, so it cannot be trusted" about Windows'
//   own indexer, which misrepresents what happened.
//
//   Whitelisting SearchIndexer is NOT the fix and must not be: the whitelist
//   grants access to EVERY protected path, so trusting it would also hand the
//   indexer the real Discord and browser token stores. Setting
//   FILE_ATTRIBUTE_NOT_CONTENT_INDEXED is the honest signal — the bait should
//   never be indexed by anyone — and it leaves the block in place for anything
//   that is actually looking for tokens.
//
//   FILE_ATTRIBUTE_NOT_CONTENT_INDEXED is what the Explorer checkbox "Allow
//   files in this folder to have contents indexed" controls.
// ============================================================
inline void MarkTreeNotContentIndexed(const std::wstring& path) {
    DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) return;

    if (!(attrs & FILE_ATTRIBUTE_NOT_CONTENT_INDEXED)) {
        // Best effort. A failure here costs alert noise, not protection, so it
        // is deliberately not fatal and not reported.
        SetFileAttributesW(path.c_str(), attrs | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED);
    }

    if (!(attrs & FILE_ATTRIBUTE_DIRECTORY)) return;

    WIN32_FIND_DATAW fd = {};
    HANDLE h = FindFirstFileW((path + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        MarkTreeNotContentIndexed(path + L"\\" + fd.cFileName);
    } while (FindNextFileW(h, &fd));

    FindClose(h);
}

// ============================================================
// Keep Windows Search out of the REAL token stores too.
//
// Same reasoning as the decoy above, and the same reason NOT to whitelist
// SearchIndexer: the whitelist grants access to EVERY protected path, so
// trusting the indexer would hand it the real Discord and browser token stores
// as well — the exact opposite of the intent. It has no use for a cookie
// database; marking the trees keeps it from waking the driver on every crawl.
//
// Measured on 2026-09-12: SearchIndexer.exe produced 14 of the 20 alerts in one
// session, all high severity, against the decoy and every browser store.
//
// This list MIRRORS the driver's PROTECTED_PATH_SUFFIXES below on purpose: the
// indexer is quieted for exactly the set the driver guards, so the two cannot
// drift into guarding one set while silencing another.
//
// Runs on every start, not once, because browsers recreate these files
// constantly and a newly created file does not inherit the attribute.
// ============================================================
inline void MarkTokenStoresNotIndexed() {
    for (const auto& tmpl : {
             L"%APPDATA%\\discord\\Local Storage\\leveldb",
             L"%APPDATA%\\discord\\Session Storage",
             L"%APPDATA%\\discordptb\\Local Storage\\leveldb",
             L"%APPDATA%\\discordcanary\\Local Storage\\leveldb",

             L"%LOCALAPPDATA%\\Google\\Chrome\\User Data\\Default\\Network",
             L"%LOCALAPPDATA%\\Google\\Chrome\\User Data\\Default\\Local Storage",
             L"%LOCALAPPDATA%\\Google\\Chrome\\User Data\\Default\\Session Storage",

             L"%LOCALAPPDATA%\\BraveSoftware\\Brave-Browser\\User Data\\Default\\Network",
             L"%LOCALAPPDATA%\\BraveSoftware\\Brave-Browser\\User Data\\Default\\Local Storage",

             L"%LOCALAPPDATA%\\Microsoft\\Edge\\User Data\\Default\\Network",
             L"%LOCALAPPDATA%\\Microsoft\\Edge\\User Data\\Default\\Local Storage",

             L"%LOCALAPPDATA%\\Opera Software\\Opera Stable\\Network",
             L"%LOCALAPPDATA%\\Opera Software\\Opera Stable\\Local Storage",

             L"%APPDATA%\\Mozilla\\Firefox\\Profiles",
         })
    {
        // Missing paths are skipped inside — a browser that is not installed
        // costs nothing here.
        MarkTreeNotContentIndexed(ExpandPath(tmpl));
    }
}

// Create the sandbox folders with dummy "token" files to protect
inline void CreateTestSandbox() {
    const std::wstring root = TestSandboxRoot();

    const std::wstring ldb = root + L"\\discord\\Local Storage\\leveldb";
    TestEnsureDir(ldb);
    TestWriteFile(ldb + L"\\LOCK",     L"");
    TestWriteFile(ldb + L"\\CURRENT",  L"MANIFEST-000001");
    TestWriteFile(ldb + L"\\000003.log",
        L"FAKE-TEST-TOKEN: eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.SANDBOX.NOT-REAL");

    const std::wstring sess = root + L"\\discord\\Session Storage";
    TestEnsureDir(sess);
    TestWriteFile(sess + L"\\000003.log", L"FAKE-SESSION-DATA");

    const std::wstring cookies = root + L"\\chrome\\Default\\Network";
    TestEnsureDir(cookies);
    TestWriteFile(cookies + L"\\Cookies", L"FAKE-CHROME-COOKIE-DB");
}

// ============================================================
// DECOY TOKEN FOLDER
// Fake "tokens" placed where a grabber would look. The kernel driver
// protects this folder like a real token store; only ShieldCord's own
// processes (trusted via TrustSelf) may read/write it. A grabber that
// "succeeds" here only exfiltrates bait — and gets flagged for the attempt.
// ============================================================

inline void CreateDecoyFolder(const std::wstring& root) {
    if (root.empty()) return;

    const std::wstring ldb = root + L"\\discord\\Local Storage\\leveldb";
    TestEnsureDir(ldb);
    TestWriteFile(ldb + L"\\CURRENT", L"MANIFEST-000001");
    TestWriteFile(ldb + L"\\000003.log",
        L"FAKE-DECOY-TOKEN: eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.DECOY.NOT-A-REAL-TOKEN");

    const std::wstring sess = root + L"\\discord\\Session Storage";
    TestEnsureDir(sess);
    TestWriteFile(sess + L"\\000003.log", L"FAKE-DECOY-SESSION");

    const std::wstring cookies = root + L"\\chrome\\Default\\Network";
    TestEnsureDir(cookies);
    TestWriteFile(cookies + L"\\Cookies", L"FAKE-DECOY-CHROME-COOKIE-DB");

    // Keep Windows Search out of the bait — see MarkTreeNotContentIndexed for
    // why this is not merely tidiness. Marked after writing, because the files
    // just created carry default attributes.
    MarkTreeNotContentIndexed(root);
}

// ============================================================
// PROTECTED PATH SUFFIXES (sent to the kernel driver at startup)
// MUST mirror src\driver\filter\path_filter.c sc_Win32Paths exactly:
// the service pushes this list via ScMsgSetProtectedPaths, and the driver
// REPLACES its built-in list with it. Keep the two in sync.
//
// Each entry is matched by the driver as a case-insensitive substring of the
// NT-normalized path (e.g. \Device\HarddiskVolume2\Users\...\AppData\...).
// ============================================================
inline const std::vector<std::wstring> PROTECTED_PATH_SUFFIXES = {
    // Discord
    L"\\AppData\\Roaming\\discord\\Local Storage\\leveldb",
    L"\\AppData\\Roaming\\discord\\Session Storage",
    L"\\AppData\\Roaming\\discordptb\\Local Storage\\leveldb",
    L"\\AppData\\Roaming\\discordcanary\\Local Storage\\leveldb",

    // Google Chrome
    L"\\AppData\\Local\\Google\\Chrome\\User Data\\Default\\Network",
    L"\\AppData\\Local\\Google\\Chrome\\User Data\\Default\\Local Storage",
    L"\\AppData\\Local\\Google\\Chrome\\User Data\\Default\\Session Storage",

    // Brave
    L"\\AppData\\Local\\BraveSoftware\\Brave-Browser\\User Data\\Default\\Network",
    L"\\AppData\\Local\\BraveSoftware\\Brave-Browser\\User Data\\Default\\Local Storage",

    // Microsoft Edge
    L"\\AppData\\Local\\Microsoft\\Edge\\User Data\\Default\\Network",
    L"\\AppData\\Local\\Microsoft\\Edge\\User Data\\Default\\Local Storage",

    // Opera
    L"\\AppData\\Local\\Opera Software\\Opera Stable\\Network",
    L"\\AppData\\Local\\Opera Software\\Opera Stable\\Local Storage",

    // Firefox
    L"\\AppData\\Roaming\\Mozilla\\Firefox\\Profiles",

    // ShieldCord decoy token folder
    L"\\ProgramData\\ShieldCord\\Decoy",
};

} // namespace sc
