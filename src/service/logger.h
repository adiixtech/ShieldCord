#pragma once
// ============================================================
// ShieldCord — logger.h
// Thread-safe file logger using raw Win32 file handles.
// ============================================================
// We deliberately do NOT use std::fstream for the log: writes from a Windows
// service (SYSTEM, session 0) via fstream have proven unreliable here. A raw
// CreateFileW + WriteFile with explicit share flags is immune to those issues,
// self-heals the handle, and bounds the file per run.
#include "../shared/common.h"
#include <string>
#include <mutex>

namespace sc {

enum class LogLevel { INFO, WARN, ERROR_, THREAT };

class Logger {
public:
    static Logger& Instance();

    void Init(const std::wstring& logFilePath);
    void Log(LogLevel level, const std::wstring& module, const std::wstring& msg);

    void Info   (const std::wstring& module, const std::wstring& msg) { Log(LogLevel::INFO,   module, msg); }
    void Warn   (const std::wstring& module, const std::wstring& msg) { Log(LogLevel::WARN,   module, msg); }
    void Error  (const std::wstring& module, const std::wstring& msg) { Log(LogLevel::ERROR_, module, msg); }
    void Threat (const std::wstring& module, const std::wstring& msg) { Log(LogLevel::THREAT, module, msg); }

private:
    Logger() = default;
    SC_DISALLOW_COPY(Logger)

    // Opens/creates the log handle if not valid; truncates if a previous run
    // exceeded kMaxLogBytes. Caller must hold m_mutex.
    bool EnsureOpen();

    HANDLE       m_file     = INVALID_HANDLE_VALUE;
    std::mutex   m_mutex;
    std::wstring m_filePath;
    static const unsigned long long kMaxLogBytes = 2ull * 1024 * 1024;   // 2 MB
};

#define LOG_INFO(mod, msg)   sc::Logger::Instance().Info(mod, msg)
#define LOG_WARN(mod, msg)   sc::Logger::Instance().Warn(mod, msg)
#define LOG_ERROR(mod, msg)  sc::Logger::Instance().Error(mod, msg)
#define LOG_THREAT(mod, msg) sc::Logger::Instance().Threat(mod, msg)

} // namespace sc
