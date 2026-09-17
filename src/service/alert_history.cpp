// ============================================================
// ShieldCord — alert_history.cpp
// ============================================================
#include "alert_history.h"
#include "utils.h"
#include "../shared/ipc_protocol.h"
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace sc {

AlertHistory& AlertHistory::Instance() {
    static AlertHistory inst;
    return inst;
}

AlertRecord AlertHistory::Append(AlertRecord rec) {
    SYSTEMTIME st = {};
    GetLocalTime(&st);

    wchar_t ts[32] = {};
    swprintf_s(ts, L"%04d-%02d-%02dT%02d:%02d:%02d",
               (int)st.wYear, (int)st.wMonth, (int)st.wDay,
               (int)st.wHour, (int)st.wMinute, (int)st.wSecond);

    // Default anything the caller left unset, so a record is always a
    // complete wire object rather than a partial one the UI must guess at.
    if (rec.severity.empty()) rec.severity = utils::ToWide(ipc::SEV_HIGH);
    if (rec.source.empty())   rec.source   = utils::ToWide(ipc::SOURCE_DRIVER);
    if (rec.action.empty())   rec.action   = utils::ToWide(ipc::ACTION_BLOCKED);

    std::lock_guard<std::mutex> lk(m_mutex);
    rec.id   = m_nextId++;
    rec.time = ts;

    m_records.push_back(rec);
    while (m_records.size() > kMaxRecords) m_records.pop_front();

    return rec;
}

std::vector<AlertRecord> AlertHistory::Since(long long afterId) const {
    std::lock_guard<std::mutex> lk(m_mutex);
    std::vector<AlertRecord> out;
    for (const auto& r : m_records) {
        if (r.id > afterId) out.push_back(r);
    }
    return out;
}

long long AlertHistory::Total() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_nextId - 1;
}

std::wstring AlertRecordToJson(const AlertRecord& rec) {
    json j;
    j[ipc::field::ID]     = rec.id;
    j["time"]             = utils::ToNarrow(rec.time);
    j["severity"]         = utils::ToNarrow(rec.severity);
    j["source"]           = utils::ToNarrow(rec.source);
    j["message"]          = utils::ToNarrow(rec.message);
    j["pid"]              = rec.pid;
    j["process_name"]     = utils::ToNarrow(rec.processName);
    j["publisher"]        = utils::ToNarrow(rec.publisher);
    j["path"]             = utils::ToNarrow(rec.path);
    j["target"]           = utils::ToNarrow(rec.target);
    j["action"]           = utils::ToNarrow(rec.action);
    j["type"]             = ipc::MSG_ALERT;   // broadcast shape
    return utils::ToWide(j.dump());
}

} // namespace sc
