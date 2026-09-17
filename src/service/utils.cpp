// ============================================================
// ShieldCord — utils.cpp
// Windows API helper utilities implementation
// ============================================================
#include "utils.h"
#include "logger.h"
#include <psapi.h>
#include <algorithm>
#include <cctype>
#include <vector>
#include <shlwapi.h>
#include <tlhelp32.h>
#include <wtsapi32.h>
#include <aclapi.h>

#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "advapi32.lib")

namespace sc::utils {

// Extract the user SID from an access token. Caller LocalFrees the result.
static PSID GetUserSidFromToken(HANDLE hToken) {
    DWORD sz = 0;
    GetTokenInformation(hToken, TokenUser, nullptr, 0, &sz);
    if (!sz) return nullptr;
    std::vector<BYTE> buf(sz);
    TOKEN_USER* ptu = reinterpret_cast<TOKEN_USER*>(buf.data());
    if (!GetTokenInformation(hToken, TokenUser, ptu, sz, &sz)) return nullptr;
    return DupSid(ptu->User.Sid);
}

std::wstring ToWide(const std::string& s) {
    if (s.empty()) return {};
    int sz = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(sz, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), sz);
    return w;
}

std::string ToNarrow(const std::wstring& w) {
    if (w.empty()) return {};
    int sz = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(sz, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), sz, nullptr, nullptr);
    return s;
}

std::wstring ToLower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), ::towlower);
    return s;
}

bool PathExists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool EnsureDirectoryExists(const std::wstring& dirPath) {
    if (PathExists(dirPath)) return true;
    // Create recursively
    std::wstring parent = dirPath.substr(0, dirPath.find_last_of(L"\\/"));
    if (!parent.empty() && parent != dirPath) {
        EnsureDirectoryExists(parent);
    }
    return CreateDirectoryW(dirPath.c_str(), nullptr) != 0;
}

bool DeleteTreeForce(const std::wstring& dirPath) {
    if (!PathExists(dirPath)) return true;

    // Reset the DACL to "no DACL" (everyone full access) so we can delete
    // even SYSTEM-locked objects — the owner implicitly holds WRITE_DAC.
    auto stripDacl = [](const std::wstring& p) {
        SetNamedSecurityInfoW(const_cast<LPWSTR>(p.c_str()),
            SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION,
            nullptr, nullptr, nullptr, nullptr);
    };

    DWORD attrs = GetFileAttributesW(dirPath.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) return false;

    if (!(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        stripDacl(dirPath);
        SetFileAttributesW(dirPath.c_str(), FILE_ATTRIBUTE_NORMAL);
        return DeleteFileW(dirPath.c_str()) != 0;
    }

    std::wstring pattern = dirPath + L"\\*";
    WIN32_FIND_DATAW fd = {};
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            std::wstring child = dirPath + L"\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                    stripDacl(child);
                    RemoveDirectoryW(child.c_str());
                } else {
                    DeleteTreeForce(child);
                }
            } else {
                stripDacl(child);
                SetFileAttributesW(child.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(child.c_str());
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    stripDacl(dirPath);
    return RemoveDirectoryW(dirPath.c_str()) != 0;
}

std::wstring GetProcessImagePath(DWORD pid) {
    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProc) return {};
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD sz = _countof(buf);
    QueryFullProcessImageNameW(hProc, 0, buf, &sz);
    CloseHandle(hProc);
    return buf;
}

std::wstring GetProcessName(DWORD pid) {
    std::wstring path = GetProcessImagePath(pid);
    if (path.empty()) return {};
    size_t pos = path.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? path : path.substr(pos + 1);
}

ULONGLONG GetProcessCreateTime(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return 0;
    FILETIME ct, et, kt, ut;
    ULONGLONG res = 0;
    if (GetProcessTimes(h, &ct, &et, &kt, &ut)) {
        res = ((ULONGLONG)ct.dwHighDateTime << 32) | ct.dwLowDateTime;
    }
    CloseHandle(h);
    return res;
}

PSID GetInteractiveUserSid() {
    // The service runs as SYSTEM, so our own token is useless here.
    // Query the token of the user logged into the active console session.
    DWORD sessionId = WTSGetActiveConsoleSessionId();
    if (sessionId != 0xFFFFFFFF) {
        HANDLE hUserToken = nullptr;
        // WTSQueryUserToken requires SE_TCB_NAME privilege (LocalSystem has it)
        if (WTSQueryUserToken(sessionId, &hUserToken)) {
            PSID sid = GetUserSidFromToken(hUserToken);
            CloseHandle(hUserToken);
            if (sid) return sid;
        }
    }

    // Fallback: find explorer.exe in the interactive session and take its SID
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return nullptr;

    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);
    PSID result = nullptr;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"explorer.exe") == 0) {
                HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
                if (hProc) {
                    HANDLE hToken = nullptr;
                    if (OpenProcessToken(hProc, TOKEN_QUERY, &hToken)) {
                        result = GetUserSidFromToken(hToken);
                        CloseHandle(hToken);
                    }
                    CloseHandle(hProc);
                }
                if (result) break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return result;
}

PSID GetProcessUserSid(DWORD pid) {
    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProc) return nullptr;

    PSID result = nullptr;
    HANDLE hToken = nullptr;
    if (OpenProcessToken(hProc, TOKEN_QUERY, &hToken)) {
        result = GetUserSidFromToken(hToken);
        CloseHandle(hToken);
    }
    CloseHandle(hProc);
    return result;
}

PSID DupSid(PSID src) {
    if (!src || !IsValidSid(src)) return nullptr;
    DWORD len = GetLengthSid(src);
    PSID dst = LocalAlloc(LMEM_FIXED, len);
    if (dst) CopySid(len, dst, src);
    return dst;
}

bool KillProcess(DWORD pid) {
    HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (!hProc) return false;
    BOOL ok = TerminateProcess(hProc, 1);
    CloseHandle(hProc);
    return ok != 0;
}

PSID GetSystemSid() {
    SID_IDENTIFIER_AUTHORITY ntAuth = SECURITY_NT_AUTHORITY;
    PSID sid = nullptr;
    AllocateAndInitializeSid(&ntAuth, 1,
        SECURITY_LOCAL_SYSTEM_RID, 0,0,0,0,0,0,0, &sid);
    return sid;
}

std::wstring FormatError(DWORD code) {
    wchar_t* msg = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                   FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, code, 0, reinterpret_cast<LPWSTR>(&msg), 0, nullptr);
    std::wstring result = msg ? msg : L"Unknown error";
    LocalFree(msg);
    // Remove trailing CR/LF
    while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n'))
        result.pop_back();
    return result;
}

bool ReadFileUtf8(const std::wstring& path, std::string& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(h, &size) || size.QuadPart <= 0 || size.QuadPart > 64 * 1024 * 1024) {
        CloseHandle(h);
        return false;
    }

    out.resize((size_t)size.QuadPart);
    DWORD read = 0;
    BOOL ok = ReadFile(h, out.data(), (DWORD)out.size(), &read, nullptr);
    CloseHandle(h);

    if (!ok) return false;
    out.resize(read);
    return true;
}

bool WriteFileUtf8(const std::wstring& path, const std::string& text) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    DWORD written = 0;
    BOOL ok = WriteFile(h, text.data(), (DWORD)text.size(), &written, nullptr);
    CloseHandle(h);
    return ok && written == (DWORD)text.size();
}

bool WriteFileUtf8Atomic(const std::wstring& path, const std::string& text) {
    std::wstring tmpPath = path + L".tmp";
    if (!WriteFileUtf8(tmpPath, text)) return false;
    return MoveFileExW(tmpPath.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

} // namespace sc::utils
