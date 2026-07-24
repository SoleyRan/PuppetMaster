#include <cassert>
#include <chrono>
#include <optional>
#include <string>
#include <utility>

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
    PublishesTraceAndMetricsCallbacks();
    CallbackExceptionsDoNotEscape();
    return 0;
}
