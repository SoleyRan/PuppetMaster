#include <cassert>
#include <chrono>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <puppet_master/puppet_master.h>

namespace observability = puppet_master::observability;
namespace core = puppet_master::core;

namespace {

core::TopicName MakeTopicName(const std::string& value)
{
    auto name = core::TopicName::Create(value);
    assert(name.ok());
    return name.value();
}

core::ComponentName MakeComponentName(const std::string& value)
{
    auto name = core::ComponentName::Create(value);
    assert(name.ok());
    return name.value();
}

void FormatsStructuredLogRecords()
{
    observability::LogRecord record;
    record.level = observability::LogLevel::kInfo;
    record.component = "runtime";
    record.event = "started";
    record.message = "runtime started";
    record.fields = {{"transports", "1"}, {"components", "2"}};

    const auto formatted = observability::FormatLogRecord(record);
    assert(formatted.find("[puppet_master]") != std::string::npos);
    assert(formatted.find("[component=runtime]") != std::string::npos);
    assert(formatted.find("[event=started]") != std::string::npos);
    assert(formatted.find("transports=1") != std::string::npos);
}

void AggregatesTopicAndTaskMetrics()
{
    observability::Observer observer;
    const auto topic = MakeTopicName("/observability/speed");
    const auto component = MakeComponentName("speed_monitor");

    observer.RecordTopicPublished(topic, 64);
    observer.RecordTopicPublished(topic, 32);
    observer.RecordTopicReceived(topic, 64, std::chrono::microseconds(250));
    observer.RecordTopicReceived(topic, 16, std::nullopt);
    observer.RecordQueueDepth(topic, 3);
    observer.RecordQueueDepth(topic, 1);
    observer.RecordTaskExecution(
        component,
        std::chrono::milliseconds(12),
        std::chrono::milliseconds(10),
        true);

    const auto snapshot = observer.Snapshot();
    assert(snapshot.topics.size() == 1);
    assert(snapshot.tasks.size() == 1);

    const auto& topic_metrics = snapshot.topics.front();
    assert(topic_metrics.messages_published == 2);
    assert(topic_metrics.bytes_published == 96);
    assert(topic_metrics.messages_received == 2);
    assert(topic_metrics.bytes_received == 80);
    assert(topic_metrics.latency_samples == 1);
    assert(topic_metrics.queue_depth == 1);
    assert(topic_metrics.max_queue_depth == 3);
    assert(topic_metrics.average_latency == std::chrono::microseconds(250));

    const auto& task_metrics = snapshot.tasks.front();
    assert(task_metrics.executions == 1);
    assert(task_metrics.deadline_misses == 1);
    assert(task_metrics.last_execution_time == std::chrono::milliseconds(12));
    assert(task_metrics.last_queue_wait == core::Nanoseconds::zero());
    assert(task_metrics.last_response_time == std::chrono::milliseconds(12));
}

void TracksEndToEndDeadlines()
{
    std::vector<observability::Event> events;
    observability::Options options;
    options.event_callback = [&events](const observability::Event& event) {
        events.push_back(event);
    };
    observability::Observer observer(std::move(options));
    const auto component = MakeComponentName("queued_task");
    const core::TimePoint completed_at(std::chrono::nanoseconds(123456789));
    observer.RecordTaskExecution(component, std::chrono::milliseconds(2),
                                 std::chrono::milliseconds(5), true,
                                 std::chrono::milliseconds(3));
    observer.RecordTaskExecution(component, std::chrono::milliseconds(2),
                                 core::Nanoseconds::zero(), true,
                                 std::chrono::milliseconds(8));
    observer.RecordTaskExecution(component, std::chrono::milliseconds(2),
                                 std::chrono::milliseconds(5), true,
                                 std::chrono::milliseconds(4), completed_at);
    observer.RecordTaskExecution(component, std::chrono::milliseconds(4),
                                 std::chrono::milliseconds(5), false,
                                 std::chrono::milliseconds(7));

    assert(events.size() == 6);
    assert(events[0].kind == observability::EventKind::kTaskExecution);
    assert(events[0].queue_wait == std::chrono::milliseconds(3));
    assert(events[1].kind == observability::EventKind::kTaskExecution);
    assert(events[2].kind == observability::EventKind::kTaskExecution);
    assert(events[3].kind == observability::EventKind::kDeadlineMiss);
    assert(events[2].timestamp == completed_at);
    assert(events[2].timestamp == events[3].timestamp);
    assert(events[3].queue_wait == std::chrono::milliseconds(4));
    assert(events[3].duration == std::chrono::milliseconds(2));
    assert(events[3].deadline == std::chrono::milliseconds(5));
    assert(events[4].kind == observability::EventKind::kTaskExecution);
    assert(events[5].kind == observability::EventKind::kDeadlineMiss);
    assert(events[5].queue_wait == events[4].queue_wait);
    assert(events[5].duration == events[4].duration);
    assert(events[5].success == events[4].success);

    const auto snapshot = observer.Snapshot();
    assert(snapshot.tasks.size() == 1);
    const auto& metrics = snapshot.tasks.front();
    assert(metrics.executions == 4);
    assert(metrics.failures == 1);
    assert(metrics.deadline_misses == 2);
    assert(metrics.last_execution_time == std::chrono::milliseconds(4));
    assert(metrics.average_execution_time == std::chrono::milliseconds(2) + std::chrono::microseconds(500));
    assert(metrics.max_execution_time == std::chrono::milliseconds(4));
    assert(metrics.last_queue_wait == std::chrono::milliseconds(7));
    assert(metrics.average_queue_wait == std::chrono::milliseconds(5) + std::chrono::microseconds(500));
    assert(metrics.max_queue_wait == std::chrono::milliseconds(8));
    assert(metrics.last_response_time == std::chrono::milliseconds(11));
    assert(metrics.average_response_time == std::chrono::milliseconds(8));
    assert(metrics.max_response_time == std::chrono::milliseconds(11));
}

void CallbackDelayDoesNotChangeDeadlineTimestamp()
{
    std::vector<observability::Event> events;
    observability::Options options;
    options.event_callback = [&events](const observability::Event& event) {
        events.push_back(event);
        if (event.kind == observability::EventKind::kTaskExecution) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    };
    observability::Observer observer(std::move(options));
    observer.RecordTaskExecution(MakeComponentName("callback_delay"),
                                 std::chrono::milliseconds(2),
                                 std::chrono::milliseconds(1));

    assert(events.size() == 2);
    assert(events[0].kind == observability::EventKind::kTaskExecution);
    assert(events[1].kind == observability::EventKind::kDeadlineMiss);
    assert(events[0].timestamp == events[1].timestamp);
}

void PublishesTraceAndMetricsCallbacks()
{
    int event_count = 0;
    int metrics_count = 0;

    observability::Options options;
    options.event_callback = [&event_count](const observability::Event&) {
        ++event_count;
    };
    options.metrics_callback = [&metrics_count](const observability::MetricsSnapshot& snapshot) {
        ++metrics_count;
        assert(!snapshot.topics.empty());
    };

    observability::Observer observer(std::move(options));
    observer.RecordTopicPublished(MakeTopicName("/observability/callback"), 8);
    observer.PublishMetrics();

    assert(event_count == 1);
    assert(metrics_count == 1);
}

void CallbackExceptionsDoNotEscape()
{
    observability::Options options;
    options.event_callback = [](const observability::Event&) {
        throw 1;
    };
    options.log_callback = [](const observability::LogRecord&) {
        throw 2;
    };

    observability::Observer observer(std::move(options));
    observer.RecordQueueDepth(MakeTopicName("/observability/no_throw"), 1);
    observer.Log(observability::LogRecord {});
}

}  // namespace

int main()
{
    FormatsStructuredLogRecords();
    AggregatesTopicAndTaskMetrics();
    TracksEndToEndDeadlines();
    CallbackDelayDoesNotChangeDeadlineTimestamp();
    PublishesTraceAndMetricsCallbacks();
    CallbackExceptionsDoNotEscape();
    return 0;
}
