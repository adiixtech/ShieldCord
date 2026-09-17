// ============================================================
// ShieldCord — config.cpp
// JSON config using nlohmann/json
// ============================================================
#include "config.h"
#include "logger.h"
#include "utils.h"
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace sc {

ConfigManager& ConfigManager::Instance() {
    static ConfigManager inst;
    return inst;
}

bool ConfigManager::Load(const std::wstring& path) {
    if (!utils::PathExists(path)) {
        LOG_INFO(L"Config", L"Config file not found, using defaults: " + path);
        return false;
    }
    try {
        std::string text;
        if (!utils::ReadFileUtf8(path, text)) {
            LOG_ERROR(L"Config", L"Failed to read config file");
            return false;
        }
        json j = json::parse(text);

        m_config.memoryMonitorEnabled   = j.value("memoryMonitorEnabled",    true);
        m_config.tokenVaultEnabled      = j.value("tokenVaultEnabled",       false);
        m_config.memoryPollIntervalMs   = j.value("memoryPollIntervalMs",    200);
        m_config.appWatcherIntervalMs   = j.value("appWatcherIntervalMs",    1000);
        m_config.logToFile              = j.value("logToFile",               true);
        m_config.logToEventLog          = j.value("logToEventLog",           false);

        m_config.driverEnforcementEnabled = j.value("driverEnforcementEnabled", true);
        m_config.driverFileBlockEnabled   = j.value("driverFileBlockEnabled",   true);
        m_config.driverAlertsEnabled      = j.value("driverAlertsEnabled",      true);
        m_config.decoyFolderEnabled       = j.value("decoyFolderEnabled",       true);
        m_config.decoyFolderPath = utils::ToWide(
            j.value("decoyFolderPath", std::string("C:\\ProgramData\\ShieldCord\\Decoy")));

        m_config.trustedPublishers.clear();
        if (j.contains("trustedPublishers") && j["trustedPublishers"].is_array()) {
            for (const auto& p : j["trustedPublishers"]) {
                if (p.is_string()) m_config.trustedPublishers.push_back(utils::ToWide(p.get<std::string>()));
            }
        }

        m_config.trustedPaths.clear();
        if (j.contains("trustedPaths") && j["trustedPaths"].is_array()) {
            for (const auto& p : j["trustedPaths"]) {
                if (p.is_string()) m_config.trustedPaths.push_back(utils::ToWide(p.get<std::string>()));
            }
        }

        LOG_INFO(L"Config", L"Config loaded from: " + path);
        return true;
    }
    catch (const std::exception& e) {
        LOG_ERROR(L"Config", utils::ToWide(std::string("Config parse error: ") + e.what()));
        return false;
    }
}

bool ConfigManager::Save(const std::wstring& path) {
    try {
        json j;
        j["memoryMonitorEnabled"]   = m_config.memoryMonitorEnabled;
        j["tokenVaultEnabled"]      = m_config.tokenVaultEnabled;
        j["memoryPollIntervalMs"]   = m_config.memoryPollIntervalMs;
        j["appWatcherIntervalMs"]   = m_config.appWatcherIntervalMs;
        j["logToFile"]              = m_config.logToFile;
        j["logToEventLog"]          = m_config.logToEventLog;

        j["driverEnforcementEnabled"] = m_config.driverEnforcementEnabled;
        j["driverFileBlockEnabled"]   = m_config.driverFileBlockEnabled;
        j["driverAlertsEnabled"]      = m_config.driverAlertsEnabled;
        j["decoyFolderEnabled"]       = m_config.decoyFolderEnabled;
        j["decoyFolderPath"]          = utils::ToNarrow(m_config.decoyFolderPath);

        j["trustedPublishers"] = json::array();
        for (const auto& p : m_config.trustedPublishers)
            j["trustedPublishers"].push_back(utils::ToNarrow(p));

        j["trustedPaths"] = json::array();
        for (const auto& p : m_config.trustedPaths)
            j["trustedPaths"].push_back(utils::ToNarrow(p));

        return utils::WriteFileUtf8Atomic(path, j.dump(4));
    }
    catch (...) {
        return false;
    }
}

} // namespace sc
