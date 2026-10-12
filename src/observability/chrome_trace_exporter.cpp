#include <puppet_master/observability/chrome_trace_exporter.h>

#include <algorithm>
#include <chrono>
#include <sstream>

namespace puppet_master::observability {

namespace {

void WriteJsonString(std::ostream& output, const std::string& value)
{
    constexpr char hex[] = "0123456789abcdef";
    output << '"';
    for (unsigned char character : value) {
        switch (character) {
            case '"': output << "\\\""; break;
            case '\\': output << "\\\\"; break;
            case '\b': output << "\\b"; break;
            case '\f': output << "\\f"; break;
            case '\n': output << "\\n"; break;
            case '\r': output << "\\r"; break;
            case '\t': output << "\\t"; break;
            default:
                if (character < 0x20) {
                    output << "\\u00" << hex[character >> 4] << hex[character & 0x0f];
                } else {
                    output << static_cast<char>(character);
                }
                break;
        }
    }
    output << '"';
}

core::TimePoint TraceStart(const Event& event)
{
    return event.kind == EventKind::kTaskExecution
        ? event.timestamp - event.duration
        : event.timestamp;
}

std::int64_t Microseconds(core::Nanoseconds duration)
{
    return std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
}

}  // namespace

void ChromeTraceExporter::Record(const Event& event)
{
    if (event.kind != EventKind::kTaskExecution && event.kind != EventKind::kDeadlineMiss) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    events_.push_back({event, next_sequence_++});
}

std::string ChromeTraceExporter::ToJson() const
{
    std::vector<RecordedEvent> events;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        events = events_;
    }

    std::sort(events.begin(), events.end(), [](const RecordedEvent& left, const RecordedEvent& right) {
        const auto left_time = TraceStart(left.event);
        const auto right_time = TraceStart(right.event);
        return left_time == right_time
            ? left.sequence < right.sequence
            : left_time < right_time;
    });

    std::ostringstream output;
    output << "{\"traceEvents\":[";
    for (std::size_t index = 0; index < events.size(); ++index) {
        if (index != 0) {
            output << ',';
        }

        const auto& event = events[index].event;
        output << "{\"name\":";
        WriteJsonString(output, event.kind == EventKind::kTaskExecution
            ? event.resource : "deadline_miss");
        output << ",\"ph\":\"" << (event.kind == EventKind::kTaskExecution ? 'X' : 'i')
               << "\",\"pid\":1,\"tid\":0,\"ts\":"
               << Microseconds(TraceStart(event).time_since_epoch());
        if (event.kind == EventKind::kTaskExecution) {
            output << ",\"dur\":" << Microseconds(event.duration);
        } else {
            output << ",\"s\":\"t\"";
        }
        output << ",\"args\":{\"resource\":";
        WriteJsonString(output, event.resource);
        output << ",\"success\":" << (event.success ? "true" : "false")
               << ",\"queue_wait_us\":" << Microseconds(event.queue_wait)
               << ",\"deadline_us\":" << Microseconds(event.deadline)
               << "}}";
    }
    output << "]}";
    return output.str();
}

void ChromeTraceExporter::Clear()
{
    std::lock_guard<std::mutex> lock(mutex_);
    events_.clear();
    next_sequence_ = 0;
}

}  // namespace puppet_master::observability
