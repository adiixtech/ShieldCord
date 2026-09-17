#pragma once
// ============================================================
// ShieldCord — process_validator.h
// Verify process digital signatures using WinVerifyTrust
// ============================================================
#include "../shared/common.h"
#include "../shared/whitelist.h"
#include <string>
#include <optional>

namespace sc {

class ProcessValidator {
public:
    // Returns true if the process at 'imagePath' is signed by any trusted publisher
    static bool IsSignedByTrustedPublisher(const std::wstring& imagePath,
                                           std::wstring* outPublisher = nullptr);

    // Full trust check: signature + path prefix match against TRUSTED_APPS
    // Returns pointer to matching TrustedApp or nullptr
    static const TrustedApp* FindTrustedApp(DWORD pid);

    // Convenience: is this PID a trusted process?
    static bool IsTrusted(DWORD pid);

    // The Authenticode signer CN of a process's image, or "" when the image
    // is unsigned or cannot be verified. Note this returns "" for unsigned
    // binaries (including malware), which is correct: there is no publisher
    // to vouch for them and none to offer in a "trust this publisher" action.
    static std::wstring GetPublisher(DWORD pid);

    // True when this process's signer matches a publisher the USER added at
    // runtime (Config::trustedPublishers). 'outPublisher' receives the CN.
    // Matching is exact and case-insensitive — unlike the built-in whitelist
    // (which matches loosely), user-entered trust must not match by substring.
    static bool MatchesRuntimePublisher(DWORD pid, std::wstring* outPublisher = nullptr);

    // Bookkeeping tag for a process trusted via a runtime publisher. There is
    // no whitelist entry to supply an appTag, so the image base name is used.
    static std::wstring RuntimeTagFor(DWORD pid);

    // What the memory monitor needs in order to choose between killing and not.
    //
    // The monitor's signal — "holds a PROCESS_VM_READ handle on a protected
    // process" — is a CAPABILITY, not an act. Nothing in that check observes a
    // token being read, and legitimate software opens such handles constantly
    // for crash reporting, traffic inspection and process identification. The
    // raw signal therefore has a high false-positive rate, and how well the
    // binary can be ATTRIBUTED is the only sound line available to split it on.
    enum class SignerTrust {
        Trusted,          // whitelist, user-trusted publisher, or allow-listed path
        SignedUntrusted,  // valid Authenticode signature, publisher not trusted
        Unverifiable,     // unsigned, tampered, or the signature did not verify
    };

    // Classify a process for the memory monitor's kill decision. This is the
    // only caller — file access is decided by FindTrustedApp, so an
    // allow-listed path here must NOT leak into the driver's trust list.
    static SignerTrust ClassifySigner(DWORD pid);

    // Save the signer cache if it has been modified during this session.
    static void FlushSignerCache();

private:
    // Extract signer common name from a file's Authenticode signature
    static std::wstring GetSignerName(const std::wstring& filePath);
};

} // namespace sc
