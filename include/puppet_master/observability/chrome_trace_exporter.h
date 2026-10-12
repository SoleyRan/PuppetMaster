#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include <puppet_master/export.h>
#include <puppet_master/observability/observability.h>

namespace puppet_master::observability {

class PUPPET_MASTER_API ChromeTraceExporter final {
public:
    ChromeTraceExporter() = default;
    ChromeTraceExporter(const ChromeTraceExporter&) = delete;
    ChromeTraceExporter& operator=(const ChromeTraceExporter&) = delete;

    // Pass Record through an Observer event callback; no I/O occurs here.
    void Record(const Event& event);
    // Returns a complete, time-sorted trace with steady-clock epoch microseconds.
    std::string ToJson() const;
    void Clear();

private:
    struct RecordedEvent {
        Event event;
        std::uint64_t sequence {0};
    };

    mutable std::mutex mutex_;
    std::vector<RecordedEvent> events_;
    std::uint64_t next_sequence_ {0};
};

}  // namespace puppet_master::observability
