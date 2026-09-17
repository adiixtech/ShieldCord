#pragma once

// ============================================================
// ShieldCord — common.h
// Shared macros, typedefs, and includes used across all modules
// ============================================================

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <atomic>
#include <mutex>
#include <thread>

// Service identity
#define SC_SERVICE_NAME     L"ShieldCordSvc"
#define SC_DISPLAY_NAME     L"ShieldCord Token Protector"
#define SC_DESCRIPTION      L"Protects Discord and browser tokens from grabber malware."
#define SC_PIPE_NAME        L"\\\\.\\pipe\\ShieldCord"
#define SC_LOG_DIR          L"C:\\ProgramData\\ShieldCord\\logs"
#define SC_LOG_FILE         L"C:\\ProgramData\\ShieldCord\\logs\\shieldcord.log"
#define SC_CONFIG_FILE      L"C:\\ProgramData\\ShieldCord\\config.json"
#define SC_INSTALL_DIR      L"C:\\Program Files\\ShieldCord"

// Kernel minifilter communication port — must match SC_FILTER_PORT_NAME in driver_protocol.h
#define SC_FILTER_PORT_NAME L"\\ShieldCordFilterPort"

// Kernel minifilter driver service — registered by install_driver.bat. DEMAND-start,
// so the service must load it at startup for protection to survive a reboot.
#define SC_FILTER_SERVICE_NAME L"ShieldCordFilter"

// Engine version — logged at every startup so stale binaries are detectable.
// SINGLE SOURCE OF TRUTH for the product version: build_all.ps1 parses this
// line and passes it to Inno Setup and the UI project. Change it here only.
#define SC_VERSION          L"1.2.0"

// On-disk backup of original DACLs — written on lock, deleted on clean
// unlock. Lets `--restore` recover everything even after a crash.
#define SC_DACL_BACKUP_FILE L"C:\\ProgramData\\ShieldCord\\dacl_backup.json"

// Persistent signature-verification cache (path|size|mtime -> signer).
// Makes trusted-app verification instant after the first ever run.
#define SC_SIGNER_CACHE_FILE L"C:\\ProgramData\\ShieldCord\\signer_cache.json"

// How long the vault stays unlocked after the last trusted process exits.
// Long enough that an in-app restart (quit -> relaunch) never sees a
// locked vault; short enough to matter as protection.
#define SC_GRACE_PERIOD_MS  20000

// Convenience macros
#define SC_DISALLOW_COPY(T) T(const T&) = delete; T& operator=(const T&) = delete;
