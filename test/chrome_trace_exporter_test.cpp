#include <cassert>
#include <chrono>
#include <string>
#include <utility>

#include <puppet_master/observability/chrome_trace_exporter.h>

namespace observability = puppet_master::observability;
namespace core = puppet_master::core;

namespace {

void RecordsSupportedEventsAndProducesValidJson()
{
    observability::ChromeTraceExporter exporter;
    const core::TimePoint completed_at(std::chrono::nanoseconds(123456789));

    exporter.Record({observability::EventKind::kTopicPublished, completed_at, "ignored"});
    exporter.Record({observability::EventKind::kTaskExecution, completed_at, "task\"\\\n\v", 0, 0,
                     std::chrono::nanoseconds(2345678), true, std::chrono::nanoseconds(3456789),
                     false, std::chrono::nanoseconds(456789)});
    exporter.Record({observability::EventKind::kDeadlineMiss, completed_at, "task\"\\\n\v", 0, 0,
                     std::chrono::nanoseconds(2345678), true, std::chrono::nanoseconds(3456789),
                     false, std::chrono::nanoseconds(456789)});

    const auto json = exporter.ToJson();
    assert(json.rfind("{\"traceEvents\":[", 0) == 0);
    assert(json.back() == '}');
    assert(json.find("\\\"\\\\\\n") != std::string::npos);
    assert(json.find("\\u000b") != std::string::npos);
    assert(json.find("\"name\":\"deadline_miss\"") != std::string::npos);
    assert(json.find("\"ph\":\"X\"") != std::string::npos);
    assert(json.find("\"ph\":\"i\",\"pid\":1,\"tid\":0") != std::string::npos);
    assert(json.find("\"ts\":121111,\"dur\":2345") != std::string::npos);
    assert(json.find("\"ts\":123456,\"s\":\"t\"") != std::string::npos);
    assert(json.find("\"queue_wait_us\":456") != std::string::npos);
    assert(json.find("\"deadline_us\":3456") != std::string::npos);
    assert(json.find("ignored") == std::string::npos);
    assert(json.find("\"success\":false") != std::string::npos);
}

void SortsByTraceTimeAndKeepsEqualTimesStable()
{
    observability::ChromeTraceExporter exporter;
    exporter.Record({observability::EventKind::kTaskExecution,
                     core::TimePoint(std::chrono::nanoseconds(3000000)), "late", 0, 0,
                     std::chrono::nanoseconds(1000000)});
    exporter.Record({observability::EventKind::kDeadlineMiss,
                     core::TimePoint(std::chrono::nanoseconds(1000000)), "early"});
    exporter.Record({observability::EventKind::kTaskExecution,
                     core::TimePoint(std::chrono::nanoseconds(3000000)), "same", 0, 0,
                     std::chrono::nanoseconds(1000000)});

    const auto json = exporter.ToJson();
    assert(json.find("\"resource\":\"early\"") < json.find("\"name\":\"late\""));
    assert(json.find("\"name\":\"late\"") < json.find("\"name\":\"same\""));
}

void EmptyTraceIsValidAndClearRemovesEvents()
{
    observability::ChromeTraceExporter exporter;
    assert(exporter.ToJson() == "{\"traceEvents\":[]}");
    exporter.Record({observability::EventKind::kDeadlineMiss, {}, "event"});
    exporter.Clear();
    assert(exporter.ToJson() == "{\"traceEvents\":[]}");
}

void IntegratesWithObserverCallback()
{
    observability::ChromeTraceExporter exporter;
    observability::Options options;
    options.event_callback = [&exporter](const observability::Event& event) {
        exporter.Record(event);
    };
    observability::Observer observer(std::move(options));
    observer.Emit({observability::EventKind::kQueueDepth, {}, "ignored"});
    observer.RecordTaskExecution(
        core::ComponentName::Create("callback").value(),
        std::chrono::microseconds(2), std::chrono::microseconds(1), true,
        std::chrono::microseconds(1), core::TimePoint(std::chrono::microseconds(10)));
    const auto json = exporter.ToJson();
    assert(json.find("\"ts\":8,\"dur\":2") != std::string::npos);
    assert(json.find("\"ts\":10,\"s\":\"t\"") != std::string::npos);
    observer.SetEventCallback([&exporter](const observability::Event& event) {
        exporter.Record(event);
    });
    observer.Emit({observability::EventKind::kDeadlineMiss,
                   core::TimePoint(std::chrono::microseconds(20)), "second"});
    assert(exporter.ToJson().find("\"resource\":\"second\"") != std::string::npos);
}

}  // namespace

int main()
{
    RecordsSupportedEventsAndProducesValidJson();
    SortsByTraceTimeAndKeepsEqualTimesStable();
    EmptyTraceIsValidAndClearRemovesEvents();
    IntegratesWithObserverCallback();
    return 0;
}
