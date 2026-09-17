#pragma once
// ============================================================
// ShieldCord — dacl_gatekeeper.h
// Layer 1: OS-level DACL protection of token/cookie directories
// ============================================================
#include "../shared/common.h"
#include "../shared/token_paths.h"
#include <string>
#include <map>
#include <mutex>

namespace sc {

class DaclGatekeeper {
public:
    // Backup of original DACLs keyed by path
    struct DaclBackup {
        std::vector<BYTE> daclBytes;  // raw copy of original DACL
        bool              hadDacl;
    };

    static DaclGatekeeper& Instance();

    // Apply SYSTEM-only DACLs to all protected paths. Call at service start.
    // overridePaths: optional explicit path list (dev/test sandbox mode);
    // nullptr = use the real Discord/browser paths.
    bool LockAll(const std::vector<ProtectedPath>* overridePaths = nullptr);

    // Restore original DACLs. Call at service stop or uninstall.
    bool UnlockAll();

    // Where the on-disk DACL backup is written (crash safety).
    // Empty = do not persist backups (default until set).
    void SetBackupFilePath(const std::wstring& path) { m_backupFilePath = path; }
    const std::wstring& BackupFilePath() const { return m_backupFilePath; }

    // Emergency recovery: restore DACLs from a backup file written by a
    // previous run (e.g. after a crash). Deletes the file on success.
    // Static — works without a prior LockAll in this process.
    static bool RestoreFromFile(const std::wstring& filePath);

    // Grant the interactive user read access to paths tagged with 'appTag'.
    // Called when a trusted app (e.g. chrome.exe) starts.
    bool GrantUser(const std::wstring& appTag, PSID userSid);

    // Revoke the user grant for paths tagged with 'appTag'.
    // Called when all instances of a trusted app exit.
    bool RevokeUser(const std::wstring& appTag, PSID userSid);

private:
    DaclGatekeeper() = default;
    SC_DISALLOW_COPY(DaclGatekeeper)

    // Set SYSTEM-only DACL on a single path
    bool SetSystemOnlyDacl(const std::wstring& path, bool isDirectory);

    // Add user ACE to existing DACL
    bool AddUserReadAce(const std::wstring& path, PSID userSid, bool isDirectory);

    // Remove user ACE from DACL (restore SYSTEM-only)
    bool RemoveUserAce(const std::wstring& path, PSID userSid, bool isDirectory);

    std::map<std::wstring, DaclBackup> m_backups;
    std::mutex                          m_mutex;

    // ref-count per appTag so multiple instances don't revoke too early
    std::map<std::wstring, int> m_grantRefCount;

    std::vector<ProtectedPath> m_paths;

    // On-disk backup location (crash safety); empty = in-memory only
    std::wstring m_backupFilePath;
};

} // namespace sc
