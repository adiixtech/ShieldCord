#pragma once
// ============================================================
// ShieldCord — alert_history.h
// Bounded, ordered history of blocked/killed events.
//
// Why this exists: the engine used to broadcast each alert and forget
// it, so a UI that connected a second later saw an empty list, and the
// "threats blocked" counter lived in two separate statics that had
// already drifted apart between service and console mode. This is now
// the single owner of both the records and the count.
// ============================================================
#include "../shared/common.h"
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace sc {

// One blocked / killed / detected event.
//
// These field names are C++-side; AlertRecordToJson() is the ONLY place
// that maps them onto the protocol's snake_case wire keys, so the kernel
// alert path and the memory-monitor path cannot drift apart.
struct AlertRecord {
    long long     id = 0;
    std::wstring  time;          // "YYYY-MM-DDTHH:MM:SS", filled by Append()
    std::wstring  severity;      // ipc::SEV_*
    std::wstring  source;        // ipc::SOURCE_DRIVER | ipc::SOURCE_MEMORY
    std::wstring  message;
    unsigned long pid = 0;
    std::wstring  processName;   // the offending process
    std::wstring  publisher;     // its Authenticode signer CN, "" if unknown
    std::wstring  path;          // protected path it tried to open (driver alerts)
    std::wstring  target;        // protected process it read (memory alerts)
    std::wstring  action;        // ipc::ACTION_*
};

class AlertHistory {
public:
    static AlertHistory& Instance();

    // Stamps the record with the next id + the current local time, stores it,
    // and returns the stored copy (so the caller broadcasts exactly what was
    // recorded). Thread-safe; called from the driver receiver thread, the
    // memory-monitor thread, and the app-watcher thread.
    AlertRecord Append(AlertRecord rec);

    // Records with id > afterId, oldest first. Callers page the UI with the
    // highest id they have seen.
    std::vector<AlertRecord> Since(long long afterId) const;

    // All-time number of appended records. Never decreases and is unaffected
    // by the in-memory cap, so it is the honest "threats blocked" total.
    long long Total() const;

private:
    AlertHistory() = default;
    SC_DISALLOW_COPY(AlertHistory)

    mutable std::mutex      m_mutex;
    std::deque<AlertRecord> m_records;
    long long               m_nextId = 1;

    // A long-running engine must not grow without bound. The UI reads
    // Since(0) once on connect and then follows the live broadcasts, so a
    // few hundred records is far more than it can ever display.
    static constexpr size_t kMaxRecords = 500;
};

// Serialize one record to the protocol JSON (wide, single line).
// See the wire shape in shared/ipc_protocol.h.
std::wstring AlertRecordToJson(const AlertRecord& rec);

} // namespace sc
