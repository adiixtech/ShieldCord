// ============================================================
// ShieldCord — logger.cpp
// Thread-safe file logger using raw Win32 file handles.
// ============================================================
#include "logger.h"
#include <windows.h>

namespace sc {

Logger& Logger::Instance() {
    static Logger inst;
    return inst;
}

static const wchar_t* LevelStr(LogLevel lv) {
    switch (lv) {
        case LogLevel::INFO:   return L"INFO  ";
        case LogLevel::WARN:   return L"WARN  ";
        case LogLevel::ERROR_: return L"ERROR ";
        case LogLevel::THREAT: return L"THREAT";
        default:               return L"?     ";
    }
}

void Logger::Init(const std::wstring& logFilePath) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_filePath = logFilePath;

    std::wstring dir = logFilePath.substr(0, logFilePath.find_last_of(L"\\/"));
    CreateDirectoryW(dir.c_str(), nullptr);

    EnsureOpen();
}

bool Logger::EnsureOpen() {
    if (m_file != INVALID_HANDLE_VALUE) return true;
    if (m_filePath.empty()) return false;

    WIN32_FILE_ATTRIBUTE_DATA attr = {};
    if (GetFileAttributesExW(m_filePath.c_str(), GetFileExInfoStandard, &attr)) {
        LARGE_INTEGER sz;
        sz.LowPart = attr.nFileSizeLow;
        sz.HighPart = attr.nFileSizeHigh;
        if (sz.QuadPart >= (LONGLONG)kMaxLogBytes) {
            std::wstring backupPath = m_filePath + L".1";
            MoveFileExW(m_filePath.c_str(), backupPath.c_str(), MOVEFILE_REPLACE_EXISTING);
        }
    }

    m_file = CreateFileW(
        m_filePath.c_str(),
        FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (m_file == INVALID_HANDLE_VALUE) return false;
    return true;
}

void Logger::Log(LogLevel level, const std::wstring& module, const std::wstring& msg) {
    SYSTEMTIME st = {};
    GetLocalTime(&st);

    wchar_t ts[64];
    swprintf_s(ts, L"%04d-%02d-%02d %02d:%02d:%02d",
               (int)st.wYear, (int)st.wMonth, (int)st.wDay,
               (int)st.wHour, (int)st.wMinute, (int)st.wSecond);

    std::wstring wline = L"[" + std::wstring(ts) + L"] [" + LevelStr(level) +
                         L"] [" + module + L"] " + msg + L"\r\n";

    // Convert to UTF-8 (this is what the log is read as).
    std::string line;
    int need = WideCharToMultiByte(CP_UTF8, 0, wline.c_str(), (int)wline.size(),
                                   nullptr, 0, nullptr, nullptr);
    if (need > 0) {
        line.resize(need);
        WideCharToMultiByte(CP_UTF8, 0, wline.c_str(), (int)wline.size(),
                            &line[0], need, nullptr, nullptr);
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    if (!EnsureOpen() || line.empty()) return;

    DWORD written = 0;
    if (!WriteFile(m_file, line.data(), (DWORD)line.size(), &written, nullptr)) {
        // Handle went bad (e.g. deleted underneath us) — drop it so the next
        // call self-heals by reopening.
        CloseHandle(m_file);
        m_file = INVALID_HANDLE_VALUE;
    }
}

} // namespace sc
