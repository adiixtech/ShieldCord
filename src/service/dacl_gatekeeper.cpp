// ============================================================
// ShieldCord — dacl_gatekeeper.cpp
// Layer 1: Set SYSTEM-only DACLs on token/cookie directories,
// dynamically granting the user read access only while
// a trusted app is running.
// ============================================================
#include "dacl_gatekeeper.h"
#include "utils.h"
#include "logger.h"
#include <aclapi.h>
#include <sddl.h>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

#pragma comment(lib, "advapi32.lib")

namespace sc {

DaclGatekeeper& DaclGatekeeper::Instance() {
    static DaclGatekeeper inst;
    return inst;
}

// -------------------------------------------------------
// Serialize a DACL as SDDL text ("D:P(A;...;...)" string)
// via a temporary security descriptor built around the ACL.
// -------------------------------------------------------
static std::wstring DaclBytesToSddl(const std::vector<BYTE>& bytes, bool hadDacl) {
    if (!hadDacl || bytes.empty()) return {};
    PACL acl = reinterpret_cast<PACL>(const_cast<BYTE*>(bytes.data()));
    // Build a minimal self-relative SD containing just the DACL, then
    // convert to SDDL. SECURITY_DESCRIPTOR_MIN_LENGTH is enough because
    // ConvertSecurityDescriptorToStringSecurityDescriptorW only needs the
    // layout, not valid owner/group.
    SECURITY_DESCRIPTOR sd;
    if (!InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION)) return {};
    if (!SetSecurityDescriptorDacl(&sd, TRUE, acl, FALSE)) return {};
    LPWSTR sddl = nullptr;
    ULONG  len  = 0;
    if (!ConvertSecurityDescriptorToStringSecurityDescriptorW(
            &sd, SDDL_REVISION_1, DACL_SECURITY_INFORMATION, &sddl, &len))
        return {};
    std::wstring result = sddl ? sddl : L"";
    if (sddl) LocalFree(sddl);
    return result;
}

static bool SddlToDaclBytes(const std::wstring& sddl, std::vector<BYTE>* out, bool* hadDacl) {
    *hadDacl = false;
    if (sddl.empty()) return true;
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl.c_str(), SDDL_REVISION_1, &sd, nullptr))
        return false;
    BOOL present = FALSE, def = FALSE;
    PACL acl = nullptr;
    GetSecurityDescriptorDacl(sd, &present, &acl, &def);
    if (present && acl) {
        *hadDacl = true;
        out->assign(reinterpret_cast<BYTE*>(acl),
                    reinterpret_cast<BYTE*>(acl) + acl->AclSize);
    }
    LocalFree(sd);
    return true;
}

// -------------------------------------------------------
// Raw Win32 file IO.
//
// NOT std::fstream: writes and reads from a Windows service (SYSTEM, session 0)
// have proven unreliable here — that is the same failure that silently stopped
// the engine log for a whole release. The visible symptom of it in this file
// was a legacy dacl_backup.json that could never be read, so it was never
// restored and never deleted, and the "Legacy DACL backup found" line repeated
// on every single service start.
// -------------------------------------------------------

// -------------------------------------------------------
// Persist / load the backup map as JSON { path: sddl }
// -------------------------------------------------------
static bool SaveBackupToFile(const std::wstring& path,
                             const std::map<std::wstring, DaclGatekeeper::DaclBackup>& backups)
{
    if (path.empty()) return true;
    try {
        json j = json::object();
        for (const auto& [p, bk] : backups) {
            std::string sddl = utils::ToNarrow(DaclBytesToSddl(bk.daclBytes, bk.hadDacl));
            j[utils::ToNarrow(p)] = sddl;
        }
        return utils::WriteFileUtf8(path, j.dump(2));
    } catch (...) {
        return false;
    }
}

bool DaclGatekeeper::RestoreFromFile(const std::wstring& filePath) {
    std::string text;
    if (!utils::ReadFileUtf8(filePath, text)) {
        LOG_WARN(L"Dacl", L"No readable legacy backup at " + filePath);
        return false;
    }
    try {
        json j = json::parse(text);
        int restored = 0;
        for (auto it = j.begin(); it != j.end(); ++it) {
            std::wstring path   = utils::ToWide(it.key());
            std::wstring sddl   = utils::ToWide(it.value().get<std::string>());
            if (!utils::PathExists(path)) continue;

            std::vector<BYTE> bytes;
            bool hadDacl = false;
            if (!SddlToDaclBytes(sddl, &bytes, &hadDacl)) continue;

            if (hadDacl) {
                PACL acl = reinterpret_cast<PACL>(bytes.data());
                SetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()),
                    SE_FILE_OBJECT,
                    DACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION,
                    nullptr, nullptr, acl, nullptr);
                restored++;
            } else {
                SetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()),
                    SE_FILE_OBJECT,
                    DACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION,
                    nullptr, nullptr, nullptr, nullptr);
                restored++;
            }
        }
        DeleteFileW(filePath.c_str());
        LOG_INFO(L"Dacl", L"Legacy DACL restore complete: " +
                 std::to_wstring(restored) + L" path(s) restored, backup deleted.");
        return true;
    } catch (...) {
        LOG_ERROR(L"Dacl", L"Legacy DACL backup is corrupt — left in place at " + filePath);
        return false;
    }
}

// -------------------------------------------------------
// Save original DACL so we can restore it on shutdown
// -------------------------------------------------------
static bool BackupDacl(const std::wstring& path,
                        DaclGatekeeper* /*gk*/,
                        std::map<std::wstring, sc::DaclGatekeeper::DaclBackup>& backups)
{
    // Get existing security descriptor
    PACL  pDacl   = nullptr;
    PSECURITY_DESCRIPTOR pSD = nullptr;

    DWORD ret = GetNamedSecurityInfoW(
        path.c_str(), SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION,
        nullptr, nullptr, &pDacl, nullptr, &pSD);

    if (ret != ERROR_SUCCESS) return false;

    sc::DaclGatekeeper::DaclBackup bk;
    bk.hadDacl = (pDacl != nullptr);
    if (bk.hadDacl) {
        DWORD sz = pDacl->AclSize;
        bk.daclBytes.resize(sz);
        memcpy(bk.daclBytes.data(), pDacl, sz);
    }
    backups[path] = std::move(bk);
    LocalFree(pSD);
    return true;
}

// -------------------------------------------------------
// Apply SYSTEM-only DACL to a path
// -------------------------------------------------------
bool DaclGatekeeper::SetSystemOnlyDacl(const std::wstring& path, bool /*isDirectory*/) {
    if (!utils::PathExists(path)) {
        LOG_WARN(L"DACL", L"Path does not exist, skipping: " + path);
        return false;
    }

    // Get SYSTEM SID
    SID_IDENTIFIER_AUTHORITY ntAuth = SECURITY_NT_AUTHORITY;
    PSID pSystemSid = nullptr;
    if (!AllocateAndInitializeSid(&ntAuth, 1,
        SECURITY_LOCAL_SYSTEM_RID, 0,0,0,0,0,0,0, &pSystemSid))
    {
        return false;
    }

    // Build ACE: SYSTEM full control
    EXPLICIT_ACCESSW ea = {};
    ea.grfAccessPermissions = GENERIC_ALL;
    ea.grfAccessMode        = SET_ACCESS;
    ea.grfInheritance       = SUB_CONTAINERS_AND_OBJECTS_INHERIT | CONTAINER_INHERIT_ACE | OBJECT_INHERIT_ACE;
    ea.Trustee.TrusteeForm  = TRUSTEE_IS_SID;
    ea.Trustee.TrusteeType  = TRUSTEE_IS_WELL_KNOWN_GROUP;
    ea.Trustee.ptstrName    = reinterpret_cast<LPWCH>(pSystemSid);

    PACL pNewAcl = nullptr;
    DWORD ret = SetEntriesInAclW(1, &ea, nullptr, &pNewAcl);
    FreeSid(pSystemSid);

    if (ret != ERROR_SUCCESS || !pNewAcl) {
        LOG_ERROR(L"DACL", L"SetEntriesInAcl failed: " + utils::FormatError(ret));
        return false;
    }

    // Also grant Administrators read access so we can still debug
    // (optional — remove if you want fully SYSTEM-only)
    SID_IDENTIFIER_AUTHORITY ntAuth2 = SECURITY_NT_AUTHORITY;
    PSID pAdminSid = nullptr;
    AllocateAndInitializeSid(&ntAuth2, 2,
        SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
        0,0,0,0,0,0, &pAdminSid);

    if (pAdminSid) {
        EXPLICIT_ACCESSW eaAdmin = {};
        eaAdmin.grfAccessPermissions = GENERIC_READ | GENERIC_EXECUTE;
        eaAdmin.grfAccessMode        = GRANT_ACCESS;
        eaAdmin.grfInheritance       = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
        eaAdmin.Trustee.TrusteeForm  = TRUSTEE_IS_SID;
        eaAdmin.Trustee.TrusteeType  = TRUSTEE_IS_GROUP;
        eaAdmin.Trustee.ptstrName    = reinterpret_cast<LPWCH>(pAdminSid);

        PACL pAclWithAdmin = nullptr;
        SetEntriesInAclW(1, &eaAdmin, pNewAcl, &pAclWithAdmin);
        LocalFree(pNewAcl);
        pNewAcl = pAclWithAdmin;
        FreeSid(pAdminSid);
    }

    // Apply new DACL (protected = disable inheritance)
    ret = SetNamedSecurityInfoW(
        const_cast<LPWSTR>(path.c_str()),
        SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
        nullptr, nullptr, pNewAcl, nullptr);

    LocalFree(pNewAcl);

    if (ret != ERROR_SUCCESS) {
        LOG_ERROR(L"DACL", L"SetNamedSecurityInfo failed on '" + path + L"': " + utils::FormatError(ret));
        return false;
    }

    LOG_INFO(L"DACL", L"Locked: " + path);
    return true;
}

// -------------------------------------------------------
// Add user read ACE to an existing DACL
// -------------------------------------------------------
bool DaclGatekeeper::AddUserReadAce(const std::wstring& path, PSID userSid, bool /*isDirectory*/) {
    if (!utils::PathExists(path)) return false;

    // Get current DACL
    PACL pOldAcl = nullptr;
    PSECURITY_DESCRIPTOR pSD = nullptr;
    DWORD ret = GetNamedSecurityInfoW(
        path.c_str(), SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION,
        nullptr, nullptr, &pOldAcl, nullptr, &pSD);

    if (ret != ERROR_SUCCESS) return false;

    // Build new ACE for user.
    // NOTE: read-only is NOT enough — Discord/Chromium must WRITE to
    // Local Storage\leveldb (LOCK/.log/.ldb) and Session Storage, otherwise
    // the login session can never be persisted and the user is logged out
    // on every restart. GENERIC_WRITE is required; WRITE_DAC is not granted
    // so an attacker running as this user still cannot alter the lock itself.
    EXPLICIT_ACCESSW ea = {};
    ea.grfAccessPermissions = GENERIC_READ | GENERIC_WRITE | GENERIC_EXECUTE | DELETE;
    ea.grfAccessMode        = GRANT_ACCESS;
    ea.grfInheritance       = SUB_CONTAINERS_AND_OBJECTS_INHERIT | CONTAINER_INHERIT_ACE | OBJECT_INHERIT_ACE;
    ea.Trustee.TrusteeForm  = TRUSTEE_IS_SID;
    ea.Trustee.TrusteeType  = TRUSTEE_IS_USER;
    ea.Trustee.ptstrName    = reinterpret_cast<LPWCH>(userSid);

    PACL pNewAcl = nullptr;
    ret = SetEntriesInAclW(1, &ea, pOldAcl, &pNewAcl);
    LocalFree(pSD);

    if (ret != ERROR_SUCCESS || !pNewAcl) return false;

    ret = SetNamedSecurityInfoW(
        const_cast<LPWSTR>(path.c_str()),
        SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
        nullptr, nullptr, pNewAcl, nullptr);

    LocalFree(pNewAcl);
    return ret == ERROR_SUCCESS;
}

// -------------------------------------------------------
// Remove user ACE (restore SYSTEM-only)
// -------------------------------------------------------
bool DaclGatekeeper::RemoveUserAce(const std::wstring& path, PSID /*userSid*/, bool isDirectory) {
    // Simply re-apply SYSTEM-only DACL (easiest correct approach)
    return SetSystemOnlyDacl(path, isDirectory);
}

// -------------------------------------------------------
// Public: LockAll — called at service startup
// -------------------------------------------------------
bool DaclGatekeeper::LockAll(const std::vector<ProtectedPath>* overridePaths) {
    std::lock_guard<std::mutex> lock(m_mutex);

    // Acquire the privilege to change file ownership / security
    {
        HANDLE hToken = nullptr;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
            TOKEN_PRIVILEGES tp = {};
            tp.PrivilegeCount = 1;
            LookupPrivilegeValueW(nullptr, L"SeTakeOwnershipPrivilege", &tp.Privileges[0].Luid);
            tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
            AdjustTokenPrivileges(hToken, FALSE, &tp, 0, nullptr, nullptr);

            TOKEN_PRIVILEGES tp2 = {};
            tp2.PrivilegeCount = 1;
            LookupPrivilegeValueW(nullptr, L"SeSecurityPrivilege", &tp2.Privileges[0].Luid);
            tp2.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
            AdjustTokenPrivileges(hToken, FALSE, &tp2, 0, nullptr, nullptr);

            CloseHandle(hToken);
        }
    }

    // Crash-recovery guard (must run BEFORE we snapshot any DACLs below).
    // If a backup file already exists at startup, a previous lock session ended
    // WITHOUT a clean UnlockAll — the service crashed and auto-restarted. The
    // protected paths on disk therefore still hold the SYSTEM-only *locked*
    // DACL, while the existing file holds the TRUE originals. If we let
    // BackupDacl run now it would capture the locked DACL as the "original" and
    // the SaveBackupToFile below would overwrite the file with it — permanently
    // destroying the real DACLs and locking the user out of their own tokens.
    // Restore from the existing file first (it deletes the file on success) so
    // the snapshot below captures the genuine originals. RestoreFromFile is
    // static and does not take m_mutex, so calling it under the lock is safe.
    if (!m_backupFilePath.empty() && utils::PathExists(m_backupFilePath)) {
        LOG_WARN(L"DACL", L"Existing DACL backup found at startup — a prior lock "
                 L"was not cleanly released. Restoring true originals before "
                 L"re-locking to avoid clobbering them.");
        RestoreFromFile(m_backupFilePath);
    }

    m_paths = overridePaths ? *overridePaths : BuildProtectedPaths();
    int locked = 0, skipped = 0;

    for (auto& pp : m_paths) {
        if (pp.expandedPath.empty()) { skipped++; continue; }

        // Backup original DACL first
        BackupDacl(pp.expandedPath, this, m_backups);

        if (SetSystemOnlyDacl(pp.expandedPath, pp.isDirectory))
            locked++;
        else
            skipped++;
    }

    // Crash safety: persist the backups so `--restore` can recover them
    // even if this process is killed before a clean UnlockAll.
    if (!m_backupFilePath.empty()) {
        if (SaveBackupToFile(m_backupFilePath, m_backups))
            LOG_INFO(L"DACL", L"Backups persisted to: " + m_backupFilePath);
        else
            LOG_WARN(L"DACL", L"Failed to persist backups to: " + m_backupFilePath);
    }

    LOG_INFO(L"DACL", L"LockAll complete: " +
             std::to_wstring(locked) + L" locked, " +
             std::to_wstring(skipped) + L" skipped/not found.");
    return locked > 0;
}

// -------------------------------------------------------
// Public: UnlockAll — restore original DACLs at shutdown
// -------------------------------------------------------
bool DaclGatekeeper::UnlockAll() {
    std::lock_guard<std::mutex> lock(m_mutex);

    // If we never locked anything this run (e.g. crash-recovery scenario),
    // fall back to the persisted backup file. RestoreFromFile is static and
    // does not take m_mutex, so calling it under the lock is safe.
    if (m_backups.empty() && !m_backupFilePath.empty()) {
        return RestoreFromFile(m_backupFilePath);
    }

    for (auto& [path, bk] : m_backups) {
        if (!utils::PathExists(path)) continue;

        if (bk.hadDacl && !bk.daclBytes.empty()) {
            PACL pOrigAcl = reinterpret_cast<PACL>(bk.daclBytes.data());
            SetNamedSecurityInfoW(
                const_cast<LPWSTR>(path.c_str()),
                SE_FILE_OBJECT,
                DACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION,
                nullptr, nullptr, pOrigAcl, nullptr);
        } else {
            // No original DACL — just remove protected flag and let inheritance work
            SetNamedSecurityInfoW(
                const_cast<LPWSTR>(path.c_str()),
                SE_FILE_OBJECT,
                DACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION,
                nullptr, nullptr, nullptr, nullptr);
        }
    }

    m_backups.clear();
    m_grantRefCount.clear();

    // Clean shutdown — the on-disk backup is no longer needed
    if (!m_backupFilePath.empty())
        DeleteFileW(m_backupFilePath.c_str());

    LOG_INFO(L"DACL", L"UnlockAll complete — DACLs restored.");
    return true;
}

// -------------------------------------------------------
// Public: GrantUser — when trusted app starts
// -------------------------------------------------------
bool DaclGatekeeper::GrantUser(const std::wstring& appTag, PSID userSid) {
    std::lock_guard<std::mutex> lock(m_mutex);

    m_grantRefCount[appTag]++;
    if (m_grantRefCount[appTag] > 1) {
        LOG_INFO(L"DACL", L"Grant refcount for '" + appTag + L"' = " +
                 std::to_wstring(m_grantRefCount[appTag]) + L" (already granted)");
        return true; // already granted
    }

    int granted = 0;
    for (const auto& pp : m_paths) {
        if (pp.appTag != appTag) continue;
        if (!utils::PathExists(pp.expandedPath)) continue;
        if (AddUserReadAce(pp.expandedPath, userSid, pp.isDirectory))
            granted++;
    }

    // Instrumentation: log exactly WHICH account was granted access —
    // if this ever shows S-1-5-18 (SYSTEM) while running as a service,
    // the unlock is useless and Discord will stay logged out.
    LPWSTR sidStr = nullptr;
    ConvertSidToStringSidW(userSid, &sidStr);
    LOG_INFO(L"DACL", L"Granted user read for app '" + appTag + L"': " +
             std::to_wstring(granted) + L" path(s), SID = " +
             (sidStr ? sidStr : L"<invalid>"));
    if (sidStr) LocalFree(sidStr);
    return granted > 0;
}

// -------------------------------------------------------
// Public: RevokeUser — when trusted app exits
// -------------------------------------------------------
bool DaclGatekeeper::RevokeUser(const std::wstring& appTag, PSID userSid) {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_grantRefCount.find(appTag);
    if (it == m_grantRefCount.end() || it->second <= 0) return false;

    it->second--;
    if (it->second > 0) {
        LOG_INFO(L"DACL", L"Revoke refcount for '" + appTag + L"' = " +
                 std::to_wstring(it->second) + L" (still running instances)");
        return true; // other instances still running
    }

    // All instances closed — revoke
    int revoked = 0;
    for (const auto& pp : m_paths) {
        if (pp.appTag != appTag) continue;
        if (!utils::PathExists(pp.expandedPath)) continue;
        if (RemoveUserAce(pp.expandedPath, userSid, pp.isDirectory))
            revoked++;
    }

    LOG_INFO(L"DACL", L"Revoked user read for app '" + appTag + L"': " +
             std::to_wstring(revoked) + L" path(s).");
    return revoked > 0;
}

} // namespace sc
