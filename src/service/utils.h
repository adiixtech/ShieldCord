#pragma once
// ============================================================
// ShieldCord — utils.h
// Windows API helper utilities
// ============================================================
#include "../shared/common.h"
#include <string>

namespace sc::utils {

// Convert narrow string to wide
std::wstring ToWide(const std::string& s);

// Convert wide string to narrow (UTF-8)
std::string  ToNarrow(const std::wstring& w);

// Lower-case a wide string
std::wstring ToLower(std::wstring s);

// Check if a path exists (file or directory)
bool PathExists(const std::wstring& path);

// Ensure all directories in path exist (like mkdir -p)
bool EnsureDirectoryExists(const std::wstring& dirPath);

// Recursively delete a directory tree. "Force" variant first resets the
// DACL on every object (works even on SYSTEM-locked files, because the
// owner implicitly holds WRITE_DAC). Used for sandbox cleanup.
bool DeleteTreeForce(const std::wstring& dirPath);

// Get the full image path of a process by PID
std::wstring GetProcessImagePath(DWORD pid);

// Get the process name (filename only) from a PID
std::wstring GetProcessName(DWORD pid);

// Get process creation time as a 64-bit FILETIME value (0 on failure)
ULONGLONG GetProcessCreateTime(DWORD pid);

// Get current interactive user SID (the logged-on user)
// Caller must LocalFree the returned PSID
PSID GetInteractiveUserSid();

// Get the user SID that the given process is running as.
// Returns nullptr on failure. Caller must LocalFree the returned PSID.
PSID GetProcessUserSid(DWORD pid);

// Duplicate a SID — caller must LocalFree result
PSID DupSid(PSID src);

// Kill a process by PID — returns true on success
bool KillProcess(DWORD pid);

// Get SYSTEM SID — caller must FreeSid the result
PSID GetSystemSid();

// Format a DWORD error code as a readable wide string
std::wstring FormatError(DWORD code);

// Read entire file to string using Win32 API
bool ReadFileUtf8(const std::wstring& path, std::string& out);

// Write entire string to file using Win32 API
bool WriteFileUtf8(const std::wstring& path, const std::string& text);

// Write string to file using a temporary file and atomic replace
bool WriteFileUtf8Atomic(const std::wstring& path, const std::string& text);

} // namespace sc::utils

