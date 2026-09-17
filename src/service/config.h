#pragma once
// ============================================================
// ShieldCord — config.h
// JSON config file read/write (uses nlohmann/json)
// ============================================================
#include "../shared/common.h"
#include <string>

namespace sc {

struct Config {
    bool memoryMonitorEnabled    = true;
    bool tokenVaultEnabled       = false;   // advanced, off by default
    int  memoryPollIntervalMs    = 200;     // how often to scan handles
    int  appWatcherIntervalMs    = 1000;    // process POLL interval (fallback +
                                            // revocation only; the real-time WMI
                                            // notifier does the actual granting)
    bool logToFile               = true;
    bool logToEventLog           = false;

    // Kernel driver control (armed after the service connects + trusts).
    // The driver is now the SOLE file-protection layer (the DACL Gatekeeper
    // was removed — it silently locked nothing when running as SYSTEM).
    bool driverEnforcementEnabled = true;   // arm default-deny on the driver
    bool driverFileBlockEnabled   = true;   // SC_FEATURE_FILE_BLOCK
    bool driverAlertsEnabled      = true;   // SC_FEATURE_ALERTS

    // Decoy token folder — fake tokens that bait grabbers; only ShieldCord's
    // own processes may touch it (driver protects it, we trust ourselves).
    bool         decoyFolderEnabled = true;
    std::wstring decoyFolderPath    = L"C:\\ProgramData\\ShieldCord\\Decoy";

    // Publishers the USER added at runtime, typically from an alert that was a
    // false positive. These widen trust beyond the built-in whitelist, so they
    // are deliberately session-visible and removable from the UI.
    std::vector<std::wstring> trustedPublishers;

    // Image paths the USER allow-listed for the memory monitor — their own
    // binaries: a test harness, a debugger, an in-house stealer probe. An entry
    // must match the owner's image path exactly, case-insensitively.
    //
    // SCOPE is deliberately narrow: this suppresses the memory monitor's KILL
    // only. It does NOT grant file access — the kernel driver still
    // default-denies these binaries at the token files. That separation is the
    // point when the binary is a probe you wrote: you want it to run and be
    // observed, while the file layer keeps refusing it.
    //
    // A path is weaker than a publisher — anything able to overwrite that file
    // inherits the trust — so use it only for binaries you control.
    std::vector<std::wstring> trustedPaths;
};

class ConfigManager {
public:
    static ConfigManager& Instance();
    bool Load(const std::wstring& path);
    bool Save(const std::wstring& path);
    const Config& Get() const { return m_config; }
    Config& GetMut()          { return m_config; }

private:
    ConfigManager() = default;
    SC_DISALLOW_COPY(ConfigManager)
    Config m_config;
};

} // namespace sc
