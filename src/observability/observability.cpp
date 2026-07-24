#include <puppet_master/observability/observability.h>

#include <algorithm>
#include <chrono>
#include <map>
#include <mutex>
#include <sstream>
#include <utility>

namespace puppet_master::observability {

namespace {

core::TimePoint Now()
{
    return std::chrono::time_point_cast<core::Nanoseconds>(core::SteadyClock::now());
}

core::Nanoseconds AverageDuration(core::Nanoseconds total, std::uint64_t samples)
{
    if (samples == 0) {
        return core::Nanoseconds::zero();
    }

    return core::Nanoseconds(total.count() / static_cast<core::Nanoseconds::rep>(samples));
}

double PerSecond(std::uint64_t value, core::Nanoseconds elapsed)
{
    const auto seconds = std::chrono::duration<double>(elapsed).count();
    return seconds > 0.0 ? static_cast<double>(value) / seconds : 0.0;
}

template <typename Callback, typename Value>
void InvokeWithoutThrowing(const Callback& callback, const Value& value)
{
    if (!callback) {
        return;
    }

    try {
        callback(value);
    } catch (...) {
        // Observability hooks must never break middleware execution.
    }
}

}  // namespace

const char* ToString(LogLevel level) noexcept
{
    switch (level) {
        case LogLevel::kTrace:
            return "trace";
        case LogLevel::kDebug:
            return "debug";
        case LogLevel::kInfo:
            return "info";
        case LogLevel::kWarning:
            return "warning";
        case LogLevel::kError:
            return "error";
        case LogLevel::kFatal:
            return "fatal";
    }

    return "unknown";
}

const char* ToString(EventKind kind) noexcept
{
    switch (kind) {
        case EventKind::kTopicPublished:
            return "topic_published";
        case EventKind::kTopicReceived:
            return "topic_received";
        case EventKind::kQueueDepth:
            return "queue_depth";
        case EventKind::kTaskExecution:
            return "task_execution";
        case EventKind::kDeadlineMiss:
            return "deadline_miss";
    }

    return "unknown";
}

std::string FormatLogRecord(const LogRecord& record)
{
    std::ostringstream stream;
    stream << "[puppet_master]"
           << "[component=" << (record.component.empty() ? "runtime" : record.component) << ']'
           << "[event=" << (record.event.empty() ? "log" : record.event) << "] "
           << record.message;

    for (const auto& field : record.fields) {
        stream << ' ' << field.key << '=' << field.value;
    }

    return stream.str();
}

struct Observer::Impl {
    struct TopicAccumulator {
        TopicMetrics metrics;
        core::Nanoseconds total_latency {0};
    };

    struct TaskAccumulator {
        TaskMetrics metrics;
        core::Nanoseconds total_execution_time {0};
    };

    explicit Impl(Options observer_options)
        : options(std::move(observer_options)),
          started_at(Now())
    {
    }

    void Apply(const Event& event)
    {
        switch (event.kind) {
            case EventKind::kTopicPublished: {
                auto& topic = topics[event.resource];
                topic.metrics.topic_name = event.resource;
                ++topic.metrics.messages_published;
                topic.metrics.bytes_published += event.bytes;
                break;
            }
            case EventKind::kTopicReceived: {
                auto& topic = topics[event.resource];
                topic.metrics.topic_name = event.resource;
                ++topic.metrics.messages_received;
                topic.metrics.bytes_received += event.bytes;
                if (event.duration >= core::Nanoseconds::zero()) {
                    ++topic.metrics.latency_samples;
                    topic.metrics.last_latency = event.duration;
                    topic.total_latency += event.duration;
                    topic.metrics.max_latency =
                        std::max(topic.metrics.max_latency, event.duration);
                }
                break;
            }
            case EventKind::kQueueDepth: {
                auto& topic = topics[event.resource];
                topic.metrics.topic_name = event.resource;
                topic.metrics.queue_depth = event.queue_depth;
                topic.metrics.max_queue_depth =
                    std::max(topic.metrics.max_queue_depth, event.queue_depth);
                break;
            }
            case EventKind::kTaskExecution: {
                auto& task = tasks[event.resource];
                task.metrics.task_name = event.resource;
                ++task.metrics.executions;
                if (!event.success) {
                    ++task.metrics.failures;
                }
                task.metrics.last_execution_time = event.duration;
                task.total_execution_time += event.duration;
                task.metrics.max_execution_time =
                    std::max(task.metrics.max_execution_time, event.duration);
                break;
            }
            case EventKind::kDeadlineMiss: {
                auto& task = tasks[event.resource];
                task.metrics.task_name = event.resource;
                ++task.metrics.deadline_misses;
                break;
            }
        }
    }

    MetricsSnapshot BuildSnapshot(core::TimePoint captured_at) const
    {
        MetricsSnapshot snapshot;
        snapshot.captured_at = captured_at;
        snapshot.uptime = captured_at - started_at;
        snapshot.topics.reserve(topics.size());
        snapshot.tasks.reserve(tasks.size());

        for (const auto& entry : topics) {
            auto metrics = entry.second.metrics;
            metrics.average_latency =
                AverageDuration(entry.second.total_latency, metrics.latency_samples);
            metrics.publish_messages_per_second =
                PerSecond(metrics.messages_published, snapshot.uptime);
            metrics.publish_bytes_per_second =
                PerSecond(metrics.bytes_published, snapshot.uptime);
            metrics.receive_messages_per_second =
                PerSecond(metrics.messages_received, snapshot.uptime);
            metrics.receive_bytes_per_second =
                PerSecond(metrics.bytes_received, snapshot.uptime);
            snapshot.topics.push_back(std::move(metrics));
        }

        for (const auto& entry : tasks) {
            auto metrics = entry.second.metrics;
            metrics.average_execution_time =
                AverageDuration(entry.second.total_execution_time, metrics.executions);
            snapshot.tasks.push_back(std::move(metrics));
        }

        return snapshot;
    }

    mutable std::mutex mutex;
    Options options;
    core::TimePoint started_at;
    std::map<std::string, TopicAccumulator> topics;
    std::map<std::string, TaskAccumulator> tasks;
};

Observer::Observer(Options options)
    : impl_(std::make_unique<Impl>(std::move(options)))
{
}

Observer::~Observer() = default;

bool Observer::enabled() const noexcept
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->options.enabled;
}

void Observer::SetEnabled(bool enabled) noexcept
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->options.enabled = enabled;
}

void Observer::SetEventCallback(EventCallback callback)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->options.event_callback = std::move(callback);
}

void Observer::SetMetricsCallback(MetricsCallback callback)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->options.metrics_callback = std::move(callback);
}

void Observer::SetLogCallback(LogCallback callback)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->options.log_callback = std::move(callback);
}

void Observer::Emit(Event event)
{
    EventCallback callback;

    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (!impl_->options.enabled) {
            return;
        }

        if (event.timestamp == core::TimePoint {}) {
            event.timestamp = Now();
        }
        impl_->Apply(event);
        callback = impl_->options.event_callback;
    }

    InvokeWithoutThrowing(callback, event);
}

void Observer::Log(LogRecord record) const
{
    LogCallback callback;

    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (!impl_->options.enabled) {
            return;
        }
        callback = impl_->options.log_callback;
    }

    InvokeWithoutThrowing(callback, record);
}

void Observer::RecordTopicPublished(const core::TopicName& topic, std::size_t bytes)
{
    Emit(Event {EventKind::kTopicPublished, {}, topic.str(), bytes});
}

void Observer::RecordTopicReceived(
    const core::TopicName& topic,
    std::size_t bytes,
    core::Nanoseconds latency)
{
    Event event;
    event.kind = EventKind::kTopicReceived;
    event.resource = topic.str();
    event.bytes = bytes;
    event.duration = latency;
    Emit(std::move(event));
}

void Observer::RecordQueueDepth(const core::TopicName& topic, std::size_t queue_depth)
{
    Event event;
    event.kind = EventKind::kQueueDepth;
    event.resource = topic.str();
    event.queue_depth = queue_depth;
    Emit(std::move(event));
}

void Observer::RecordTaskExecution(
    const core::ComponentName& component,
    core::Nanoseconds duration,
    core::Nanoseconds deadline,
    bool success)
{
    Event execution;
    execution.kind = EventKind::kTaskExecution;
    execution.resource = component.str();
    execution.duration = duration;
    execution.deadline = deadline;
    execution.success = success;
    Emit(execution);

    if (deadline > core::Nanoseconds::zero() && duration > deadline) {
        Event miss = execution;
        miss.kind = EventKind::kDeadlineMiss;
        Emit(std::move(miss));
    }
}

MetricsSnapshot Observer::Snapshot() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->BuildSnapshot(Now());
}

void Observer::PublishMetrics() const
{
    MetricsCallback callback;
    MetricsSnapshot snapshot;

    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (!impl_->options.enabled) {
            return;
        }
        callback = impl_->options.metrics_callback;
        snapshot = impl_->BuildSnapshot(Now());
    }

    InvokeWithoutThrowing(callback, snapshot);
}

}  // namespace puppet_master::observability
