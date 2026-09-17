#pragma once
// ============================================================
// ShieldCord — whitelist.h
// Trusted process definitions — digital signature publishers
// and expected install-path prefixes.
// ============================================================
#include "common.h"
#include <vector>

namespace sc {

struct TrustedApp {
    std::wstring processName;        // e.g. L"chrome.exe"

    // Acceptable signer CNs — ANY one of them matches. This is a list rather
    // than a single string because a vendor can rename its signing
    // certificate: Opera signed as "Opera Software AS" and now signs as
    // "Opera Norway AS". With one string, a rename does not merely fail to
    // protect the app — the mismatch makes the app untrusted, and the memory
    // monitor then kills it as an attacker. Fail open on a rename, never closed.
    std::vector<std::wstring> publishers;

    std::wstring expectedPathPrefix; // lowercase prefix of executable path
    std::wstring appTag;             // matches ProtectedPath::appTag
    bool         isProtectedProcess; // true = memory-read monitor watches this
};

// The publisher to show a user for this app — the first entry, which is the
// current/primary signer. The UI renders one string, so a list must not leak
// into it as a concatenation.
inline std::wstring PrimaryPublisher(const TrustedApp& ta) {
    return ta.publishers.empty() ? std::wstring() : ta.publishers.front();
}

inline const std::vector<TrustedApp> TRUSTED_APPS = {
    // Discord Updater (runs before discord.exe, gives us a head start)
    { L"update.exe",   { L"Discord Inc." },              L"",  L"discord",   false },

    // Discord (installs under %LOCALAPPDATA%\Discord\app-X.Y.Z\)
    { L"discord.exe",  { L"Discord Inc." },              L"",  L"discord",   true  },

    // Discord PTB / Canary (their leveldb dirs have their own appTag)
    { L"discordptb.exe",    { L"Discord Inc." },         L"",  L"discordptb",    true },
    { L"discordcanary.exe", { L"Discord Inc." },         L"",  L"discordcanary", true },

    // Google Chrome
    { L"chrome.exe",   { L"Google LLC" },                L"",  L"chrome",    true  },

    // Brave
    { L"brave.exe",    { L"Brave Software, Inc." },      L"",  L"brave",     true  },

    // Microsoft Edge
    { L"msedge.exe",   { L"Microsoft Corporation" },     L"",  L"edge",      true  },

    // Opera. Both CNs accepted: current first (what the UI shows), then the
    // pre-rebrand one, so builds signed either way keep working.
    { L"opera.exe",    { L"Opera Norway AS", L"Opera Software AS" },
                                                         L"",  L"opera",     true  },

    // Firefox (Mozilla)
    { L"firefox.exe",  { L"Mozilla Corporation" },       L"",  L"firefox",   true  },

    // Our own service — always trust ourselves. An empty publisher list skips
    // the signature check entirely; this one is matched by name alone.
    { L"shieldcord_svc.exe", { },                        L"",  L"",          false },
};

// Tags that correspond to browser cookie paths
inline const std::vector<std::wstring> BROWSER_TAGS = {
    L"chrome", L"brave", L"edge", L"opera", L"firefox"
};

} // namespace sc
