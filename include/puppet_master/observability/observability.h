#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <puppet_master/core/types.h>

namespace puppet_master::observability {

enum class LogLevel : std::uint8_t {
    kTrace = 0,
    kDebug,
    kInfo,
    kWarning,
    kError,
    kFatal,
};

enum class EventKind : std::uint8_t {
    kTopicPublished = 0,
    kTopicReceived,
    kQueueDepth,
    kTaskExecution,
    kDeadlineMiss,
};

const char* ToString(LogLevel level) noexcept;
const char* ToString(EventKind kind) noexcept;

struct LogField {
    std::string key;
    std::string value;
};

struct LogRecord {
    LogLevel level {LogLevel::kInfo};
    std::string component;
    std::string event;
    std::string message;
    std::vector<LogField> fields;
};

std::string FormatLogRecord(const LogRecord& record);

struct Event {
    EventKind kind {EventKind::kTopicPublished};
    core::TimePoint timestamp {};
    std::string resource;
    std::size_t bytes {0};
    std::size_t queue_depth {0};
    core::Nanoseconds duration {0};
    core::Nanoseconds deadline {0};
    bool success {true};
};

struct TopicMetrics {
    std::string topic_name;
    std::uint64_t messages_published {0};
    std::uint64_t bytes_published {0};
    std::uint64_t messages_received {0};
    std::uint64_t bytes_received {0};
    std::uint64_t latency_samples {0};
    std::size_t queue_depth {0};
    std::size_t max_queue_depth {0};
    core::Nanoseconds last_latency {0};
    core::Nanoseconds average_latency {0};
    core::Nanoseconds max_latency {0};
    double publish_messages_per_second {0.0};
    double publish_bytes_per_second {0.0};
    double receive_messages_per_second {0.0};
    double receive_bytes_per_second {0.0};
};

struct TaskMetrics {
    std::string task_name;
    std::uint64_t executions {0};
    std::uint64_t failures {0};
    std::uint64_t deadline_misses {0};
    core::Nanoseconds last_execution_time {0};
    core::Nanoseconds average_execution_time {0};
    core::Nanoseconds max_execution_time {0};
};

struct MetricsSnapshot {
    core::TimePoint captured_at {};
    core::Nanoseconds uptime {0};
    std::vector<TopicMetrics> topics;
    std::vector<TaskMetrics> tasks;
};

using EventCallback = std::function<void(const Event&)>;
using MetricsCallback = std::function<void(const MetricsSnapshot&)>;
using LogCallback = std::function<void(const LogRecord&)>;

struct Options {
    bool enabled {true};
    EventCallback event_callback;
    MetricsCallback metrics_callback;
    LogCallback log_callback;
};

// Observer is the runtime-wide event hub and in-process metrics registry.
// Exporters should consume callbacks or snapshots instead of coupling runtime
// code to a specific metrics, logging, or tracing backend.
class Observer final {
public:
    explicit Observer(Options options = {});
    ~Observer();

    Observer(const Observer&) = delete;
    Observer& operator=(const Observer&) = delete;
    Observer(Observer&&) = delete;
    Observer& operator=(Observer&&) = delete;

    bool enabled() const noexcept;
    void SetEnabled(bool enabled) noexcept;

    void SetEventCallback(EventCallback callback);
    void SetMetricsCallback(MetricsCallback callback);
    void SetLogCallback(LogCallback callback);

    void Emit(Event event);
    void Log(LogRecord record) const;

    void RecordTopicPublished(const core::TopicName& topic, std::size_t bytes);
    void RecordTopicReceived(
        const core::TopicName& topic,
        std::size_t bytes,
        core::Nanoseconds latency);
    void RecordQueueDepth(const core::TopicName& topic, std::size_t queue_depth);
    void RecordTaskExecution(
        const core::ComponentName& component,
        core::Nanoseconds duration,
        core::Nanoseconds deadline = core::Nanoseconds::zero(),
        bool success = true);

    MetricsSnapshot Snapshot() const;
    void PublishMetrics() const;

private:
    struct Impl;

    std::unique_ptr<Impl> impl_;
};

using ObserverPtr = std::shared_ptr<Observer>;

}  // namespace puppet_master::observability
