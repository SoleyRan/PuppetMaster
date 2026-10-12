#include <cassert>
#include <chrono>
#include <cstdint>
#include <limits>
#include <string>

#include <puppet_master/observability/prometheus_exporter.h>

namespace observability = puppet_master::observability;
namespace core = puppet_master::core;

namespace {

void RendersEmptySnapshot()
{
    const auto text = observability::PrometheusExporter::Render({});
    assert(text.find("# TYPE puppet_master_uptime_seconds gauge\n") != std::string::npos);
    assert(text.find("puppet_master_uptime_seconds 0\n") != std::string::npos);
    assert(text.find("# TYPE puppet_master_topic_messages_published_total counter\n") != std::string::npos);
    assert(text.find("# TYPE puppet_master_task_executions_total counter\n") != std::string::npos);
    assert(text.find("{topic=") == std::string::npos);
    assert(text.find("{task=") == std::string::npos);
}

void RendersMetricsAndEscapesLabels()
{
    observability::MetricsSnapshot snapshot;
    snapshot.uptime = std::chrono::milliseconds(1250);
    observability::TopicMetrics topic;
    topic.topic_name = "a\\\"\nb";
    topic.messages_published = std::numeric_limits<std::uint64_t>::max();
    topic.bytes_received = 42;
    topic.queue_depth = 3;
    topic.max_queue_depth = 5;
    topic.latency_samples = 2;
    topic.last_latency = std::chrono::milliseconds(250);
    topic.average_latency = std::chrono::milliseconds(125);
    topic.publish_messages_per_second = 2.5;
    snapshot.topics.push_back(topic);

    observability::TaskMetrics task;
    task.task_name = "worker";
    task.executions = 2;
    task.failures = 1;
    task.deadline_misses = 1;
    task.last_execution_time = std::chrono::milliseconds(500);
    task.last_queue_wait = std::chrono::milliseconds(125);
    task.last_response_time = std::chrono::milliseconds(625);
    snapshot.tasks.push_back(task);

    const auto text = observability::PrometheusExporter::Render(snapshot);
    assert(text.find("puppet_master_uptime_seconds 1.25\n") != std::string::npos);
    assert(text.find("puppet_master_topic_messages_published_total{topic=\"a\\\\\\\"\\nb\"} 18446744073709551615\n") != std::string::npos);
    assert(text.find("puppet_master_topic_bytes_received_total{topic=\"a\\\\\\\"\\nb\"} 42\n") != std::string::npos);
    assert(text.find("puppet_master_topic_queue_depth{topic=\"a\\\\\\\"\\nb\"} 3\n") != std::string::npos);
    assert(text.find("puppet_master_topic_max_queue_depth{topic=\"a\\\\\\\"\\nb\"} 5\n") != std::string::npos);
    assert(text.find("puppet_master_topic_latency_samples_total{topic=\"a\\\\\\\"\\nb\"} 2\n") != std::string::npos);
    assert(text.find("puppet_master_topic_last_latency_seconds{topic=\"a\\\\\\\"\\nb\"} 0.25\n") != std::string::npos);
    assert(text.find("puppet_master_topic_average_latency_seconds{topic=\"a\\\\\\\"\\nb\"} 0.125\n") != std::string::npos);
    assert(text.find("puppet_master_topic_publish_messages_per_second{topic=\"a\\\\\\\"\\nb\"} 2.5\n") != std::string::npos);
    assert(text.find("puppet_master_task_executions_total{task=\"worker\"} 2\n") != std::string::npos);
    assert(text.find("puppet_master_task_failures_total{task=\"worker\"} 1\n") != std::string::npos);
    assert(text.find("puppet_master_task_deadline_misses_total{task=\"worker\"} 1\n") != std::string::npos);
    assert(text.find("puppet_master_task_last_execution_time_seconds{task=\"worker\"} 0.5\n") != std::string::npos);
    assert(text.find("puppet_master_task_last_queue_wait_seconds{task=\"worker\"} 0.125\n") != std::string::npos);
    assert(text.find("puppet_master_task_last_response_time_seconds{task=\"worker\"} 0.625\n") != std::string::npos);
}

void IntegratesWithObserverCallback()
{
    std::string latest;
    observability::Observer observer;
    observer.SetMetricsCallback([&latest](const observability::MetricsSnapshot& snapshot) {
        latest = observability::PrometheusExporter::Render(snapshot);
    });
    observer.RecordTopicPublished(core::TopicName::Create("updates").value(), 12);
    observer.RecordTaskExecution(core::ComponentName::Create("worker").value(),
                                 std::chrono::milliseconds(2));
    observer.PublishMetrics();
    assert(latest.find("puppet_master_topic_messages_published_total{topic=\"updates\"} 1\n") != std::string::npos);
    assert(latest.find("puppet_master_task_executions_total{task=\"worker\"} 1\n") != std::string::npos);
}

}  // namespace

int main()
{
    RendersEmptySnapshot();
    RendersMetricsAndEscapesLabels();
    IntegratesWithObserverCallback();
    return 0;
}
