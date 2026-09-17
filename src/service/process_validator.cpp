// ============================================================
// ShieldCord — process_validator.cpp
// Digital signature verification via WinVerifyTrust + CryptMsg
// ============================================================
#include "process_validator.h"
#include "config.h"
#include "utils.h"
#include "logger.h"
#include <wintrust.h>
#include <softpub.h>
#include <wincrypt.h>
#include <mscat.h>
#include <algorithm>
#include <cctype>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

#pragma comment(lib, "wintrust.lib")
#pragma comment(lib, "crypt32.lib")

namespace sc {

// -------------------------------------------------------
// Extract the signer Subject CN from a PE's certificate
// -------------------------------------------------------
std::wstring ProcessValidator::GetSignerName(const std::wstring& filePath) {
    HCERTSTORE      hStore  = nullptr;
    HCRYPTMSG       hMsg    = nullptr;
    PCCERT_CONTEXT  pCert   = nullptr;
    std::wstring    result;

    // Open the Authenticode signed message embedded in the PE
    DWORD enc = 0, ct = 0, ft = 0;
    if (!CryptQueryObject(CERT_QUERY_OBJECT_FILE,
                          filePath.c_str(),
                          CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED,
                          CERT_QUERY_FORMAT_FLAG_BINARY,
                          0, &enc, &ct, &ft,
                          &hStore, &hMsg, nullptr))
    {
        return {};
    }

    // Get signer info
    DWORD cbSignerInfo = 0;
    CryptMsgGetParam(hMsg, CMSG_SIGNER_INFO_PARAM, 0, nullptr, &cbSignerInfo);
    if (!cbSignerInfo) goto cleanup;

    {
        std::vector<BYTE> buf(cbSignerInfo);
        if (!CryptMsgGetParam(hMsg, CMSG_SIGNER_INFO_PARAM, 0, buf.data(), &cbSignerInfo))
            goto cleanup;

        CMSG_SIGNER_INFO* pSI = reinterpret_cast<CMSG_SIGNER_INFO*>(buf.data());

        CERT_INFO ci = {};
        ci.Issuer       = pSI->Issuer;
        ci.SerialNumber = pSI->SerialNumber;

        pCert = CertFindCertificateInStore(hStore,
                    PKCS_7_ASN_ENCODING | X509_ASN_ENCODING,
                    0, CERT_FIND_SUBJECT_CERT, &ci, nullptr);
        if (!pCert) goto cleanup;

        // Get Subject Name (CN field)
        wchar_t subj[256] = {};
        CertGetNameStringW(pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, subj, _countof(subj));
        result = subj;
    }

cleanup:
    if (pCert)  CertFreeCertificateContext(pCert);
    if (hStore) CertCloseStore(hStore, 0);
    if (hMsg)   CryptMsgClose(hMsg);
    return result;
}

// -------------------------------------------------------
// Verify Authenticode signature with WinVerifyTrust
// -------------------------------------------------------
bool ProcessValidator::IsSignedByTrustedPublisher(const std::wstring& imagePath,
                                                   std::wstring* outPublisher) {
    WINTRUST_FILE_INFO fi = {};
    fi.cbStruct       = sizeof(fi);
    fi.pcwszFilePath  = imagePath.c_str();

    GUID policyGuid = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    WINTRUST_DATA wd = {};
    wd.cbStruct            = sizeof(wd);
    wd.dwUIChoice          = WTD_UI_NONE;
    wd.fdwRevocationChecks = WTD_REVOKE_NONE;   // skip CRL for speed
    wd.dwUnionChoice       = WTD_CHOICE_FILE;
    wd.pFile               = &fi;
    wd.dwStateAction       = WTD_STATEACTION_VERIFY;

    LONG status = WinVerifyTrust(nullptr, &policyGuid, &wd);

    // Close state
    wd.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(nullptr, &policyGuid, &wd);

    if (status != ERROR_SUCCESS) return false;

    if (outPublisher) {
        *outPublisher = GetSignerName(imagePath);
    }
    return true;
}

// -------------------------------------------------------
// Match against our TRUSTED_APPS whitelist
// -------------------------------------------------------
// Persistent verification cache, keyed by "path|size|mtime" so a rebuilt or
// updated binary invalidates its entry automatically. WinVerifyTrust takes
// hundreds of ms per call; without this cache the unlock arrives AFTER the
// app already failed to open its storage — the "logged out after restart"
// race. Persistent across restarts, shared by all ShieldCord components.
//
// Tradeoff: a file replaced at an already-verified path AND same size AND
// same mtime is trusted until the cache is cleared. Defended in depth:
// real attackers must also forge the trusted publisher's signature, which
// WinVerifyTrust already validated when the entry was created.
struct SignerCache {
    std::mutex mtx;
    std::map<std::wstring, std::wstring> map;   // key -> signer CN
    bool loaded = false;
    bool dirty  = false;
};

static SignerCache g_signerCache;

static std::wstring MakeSignerCacheKey(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA fad = {};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad))
        return path + L"|missing";

    unsigned long long size =
        (unsigned long long)fad.nFileSizeLow |
        ((unsigned long long)fad.nFileSizeHigh << 32);
    unsigned long long mtime =
        (unsigned long long)fad.ftLastWriteTime.dwLowDateTime |
        ((unsigned long long)fad.ftLastWriteTime.dwHighDateTime << 32);

    return path + L"|" + std::to_wstring(size) + L"|" + std::to_wstring(mtime);
}

static void LoadSignerCache() {
    std::string text;
    if (!utils::ReadFileUtf8(SC_SIGNER_CACHE_FILE, text)) return; // first ever run — fine
    try {
        json j = json::parse(text);
        for (auto it = j.begin(); it != j.end(); ++it)
            g_signerCache.map[utils::ToWide(it.key())] = utils::ToWide(it.value().get<std::string>());
        LOG_INFO(L"Validator", L"Signer cache loaded: " +
                 std::to_wstring(g_signerCache.map.size()) + L" entries.");
    } catch (...) {
        // Corrupt cache — start fresh, everything re-verifies once.
    }
}

static void SaveSignerCache() {
    try {
        // Snapshot the map into a JSON object while holding the lock — iterating
        // g_signerCache.map unlocked races with concurrent inserts from other
        // verification threads (map iterator invalidation → crash). The slow
        // file write happens after the lock is released.
        json j = json::object();
        {
            std::lock_guard<std::mutex> lk(g_signerCache.mtx);
            for (const auto& kv : g_signerCache.map)
                j[utils::ToNarrow(kv.first)] = utils::ToNarrow(kv.second);
            g_signerCache.dirty = false;
        }
        utils::WriteFileUtf8Atomic(SC_SIGNER_CACHE_FILE, j.dump(2));
    } catch (...) { /* best effort */ }
}

void ProcessValidator::FlushSignerCache() {
    bool dirty = false;
    {
        std::lock_guard<std::mutex> lk(g_signerCache.mtx);
        dirty = g_signerCache.dirty;
    }
    if (dirty) SaveSignerCache();
}

// Returns the signer CN for a verified image, "" if not verifiable.
static std::wstring GetCachedSigner(const std::wstring& path) {
    std::wstring key = MakeSignerCacheKey(path);

    {
        std::lock_guard<std::mutex> lk(g_signerCache.mtx);
        if (!g_signerCache.loaded) {
            g_signerCache.loaded = true;
            LoadSignerCache();           // loads under the lock once
        }
        auto it = g_signerCache.map.find(key);
        if (it != g_signerCache.map.end()) return it->second;
    }

    // Miss — do the real (slow) verification
    std::wstring signer;
    if (!ProcessValidator::IsSignedByTrustedPublisher(path, &signer))
        signer.clear();

    {
        std::lock_guard<std::mutex> lk(g_signerCache.mtx);
        g_signerCache.map[key] = signer;
        g_signerCache.dirty = true;
    }
    SaveSignerCache();
    return signer;
}

// -------------------------------------------------------
// Publisher matching
//
// The rule is deliberately loose: a match is either string containing the
// other, so a CN carrying a suffix (", Inc.", " AS") still matches a shorter
// expected name. An app may now list more than one acceptable CN, because a
// vendor renaming its signing certificate must not be able to turn genuine,
// signed software into a kill target. Opera did exactly that — the entry said
// "Opera Software AS" while the binary signs as "Opera Norway AS", so the check
// failed, Opera was classified untrusted, and the memory monitor killed it.
// -------------------------------------------------------
static bool PublisherMatches(const std::vector<std::wstring>& expected,
                             const std::wstring& signerLow) {
    for (const auto& p : expected) {
        std::wstring pubLow = utils::ToLower(p);
        if (pubLow.empty()) continue;
        if (signerLow.find(pubLow) != std::wstring::npos ||
            pubLow.find(signerLow) != std::wstring::npos)
            return true;
    }
    return false;
}

// "A' or 'B" — so a mismatch log names every accepted CN, not just one.
static std::wstring JoinPublishers(const std::vector<std::wstring>& pubs) {
    std::wstring out;
    for (const auto& p : pubs) {
        if (!out.empty()) out += L"' or '";
        out += p;
    }
    return out;
}

const TrustedApp* ProcessValidator::FindTrustedApp(DWORD pid) {
    std::wstring imagePath = utils::GetProcessImagePath(pid);
    if (imagePath.empty()) return nullptr;

    std::wstring lowerPath = utils::ToLower(imagePath);
    std::wstring lowerName = utils::GetProcessName(pid);
    std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::towlower);

    // Quick name pre-filter
    for (const auto& ta : TRUSTED_APPS) {
        std::wstring taNameLow = utils::ToLower(ta.processName);
        if (lowerName != taNameLow) continue;

        // Signature check (an empty list = our own service, matched by name
        // alone — it has no third-party publisher to verify)
        if (!ta.publishers.empty()) {
            std::wstring signerName = GetCachedSigner(lowerPath);
            if (signerName.empty()) continue;

            std::wstring signerLow = utils::ToLower(signerName);
            if (!PublisherMatches(ta.publishers, signerLow)) {
                LOG_WARN(L"Validator",
                    L"Signature mismatch for " + lowerName +
                    L": expected '" + JoinPublishers(ta.publishers) +
                    L"', got '" + signerName + L"'");
                continue;
            }
        }

        return &ta;
    }
    return nullptr;
}

// Exact, case-insensitive full-path match against Config::trustedPaths.
static bool MatchesTrustedPath(const std::wstring& imagePath) {
    const auto& list = ConfigManager::Instance().Get().trustedPaths;
    if (list.empty() || imagePath.empty()) return false;

    std::wstring pathLow = utils::ToLower(imagePath);
    for (const auto& p : list) {
        if (!p.empty() && utils::ToLower(p) == pathLow) return true;
    }
    return false;
}

ProcessValidator::SignerTrust ProcessValidator::ClassifySigner(DWORD pid) {
    std::wstring imagePath = utils::GetProcessImagePath(pid);

    // No resolvable path means no attribution at all — the class we act on.
    if (imagePath.empty()) return SignerTrust::Unverifiable;

    // The user's own binaries, checked before the signature so a probe they
    // built themselves needs no publisher behind it.
    if (MatchesTrustedPath(imagePath)) return SignerTrust::Trusted;

    std::wstring signer;
    if (!IsSignedByTrustedPublisher(imagePath, &signer)) {
        // WinVerifyTrust refused the chain: unsigned, tampered, or revoked.
        // This is the class a token stealer falls into.
        return SignerTrust::Unverifiable;
    }

    // The signature verified — now attribute it. Built-in whitelist publishers
    // match loosely and user-added ones exactly. That split now lives here
    // alone, so nothing can drift into disagreeing about who is trusted.
    std::wstring signerLow = utils::ToLower(signer);
    for (const auto& ta : TRUSTED_APPS) {
        if (PublisherMatches(ta.publishers, signerLow)) return SignerTrust::Trusted;
    }
    for (const auto& p : ConfigManager::Instance().Get().trustedPublishers) {
        if (utils::ToLower(p) == signerLow) return SignerTrust::Trusted;
    }

    // Validly signed by a publisher we do not trust. Note this stays
    // SignedUntrusted even when the CN could not be read: the signature itself
    // verified, and treating a valid signature as unverifiable would kill the
    // very software this split exists to spare.
    return SignerTrust::SignedUntrusted;
}

std::wstring ProcessValidator::GetPublisher(DWORD pid) {
    std::wstring imagePath = utils::GetProcessImagePath(pid);
    if (imagePath.empty()) return {};
    return GetCachedSigner(utils::ToLower(imagePath));
}

bool ProcessValidator::MatchesRuntimePublisher(DWORD pid, std::wstring* outPublisher) {
    std::wstring signer = GetPublisher(pid);
    if (signer.empty()) return false;

    std::wstring signerLow = utils::ToLower(signer);
    for (const auto& p : ConfigManager::Instance().Get().trustedPublishers) {
        if (utils::ToLower(p) == signerLow) {
            if (outPublisher) *outPublisher = signer;
            return true;
        }
    }
    return false;
}

std::wstring ProcessValidator::RuntimeTagFor(DWORD pid) {
    std::wstring name = utils::ToLower(utils::GetProcessName(pid));

    static const std::wstring kExt = L".exe";
    if (name.size() > kExt.size() &&
        name.compare(name.size() - kExt.size(), kExt.size(), kExt) == 0)
    {
        name.erase(name.size() - kExt.size());
    }
    return name;
}

bool ProcessValidator::IsTrusted(DWORD pid) {
    return FindTrustedApp(pid) != nullptr;
}

} // namespace sc
