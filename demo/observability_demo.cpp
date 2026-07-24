#include <cassert>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#include <puppet_master/puppet_master.h>

#if defined(PUPPETMASTER_DEMO_WITH_GOODLOG)
#include <puppet_master/observability/goodlog_sink.h>
#endif

namespace core = puppet_master::core;
namespace observability = puppet_master::observability;
namespace runtime = puppet_master::runtime;
namespace scheduler = puppet_master::scheduler;
namespace transport = puppet_master::transport;

namespace {

int Fail(const std::string& message, const core::Status& status)
{
    std::cerr << message << ": " << status.ToString() << '\n';
    return 1;
}

core::ComponentName MakeComponentName(const std::string& value)
{
    auto name = core::ComponentName::Create(value);
    assert(name.ok());
    return name.value();
}

core::TopicName MakeTopicName(const std::string& value)
{
    auto name = core::TopicName::Create(value);
    assert(name.ok());
    return name.value();
}

transport::EndpointConfig MakeEndpoint()
{
    core::MessagePolicy policy;
    policy.freshness = core::FreshnessPolicy::kQueued;
    policy.retention = core::RetentionPolicy::kKeepLast;
    policy.queue_depth = 8;

    return transport::EndpointConfig {
        core::TopicSpec {
            MakeTopicName("/demo/vehicle_state"),
            core::TransportKind::kInMemory,
            policy,
        },
        transport::MessageDescriptor {"demo.VehicleState", "application/octet-stream"},
    };
}

void PrintMetrics(const observability::MetricsSnapshot& snapshot)
{
    std::cout << "\n=== middleware metrics ===\n";
    for (const auto& topic : snapshot.topics) {
        const auto average_latency_us =
            std::chrono::duration_cast<std::chrono::microseconds>(
                topic.average_latency).count();
        std::cout
            << "topic=" << topic.topic_name
            << " published=" << topic.messages_published
            << " received=" << topic.messages_received
            << " publish_hz=" << std::fixed << std::setprecision(1)
            << topic.publish_messages_per_second
            << " avg_latency_us=" << average_latency_us
            << " queue_depth=" << topic.queue_depth
            << " max_queue_depth=" << topic.max_queue_depth
            << '\n';
    }

    for (const auto& task : snapshot.tasks) {
        const auto average_execution_us =
            std::chrono::duration_cast<std::chrono::microseconds>(
                task.average_execution_time).count();
        std::cout
            << "task=" << task.task_name
            << " executions=" << task.executions
            << " failures=" << task.failures
            << " avg_execution_us=" << average_execution_us
            << " deadline_misses=" << task.deadline_misses
            << '\n';
    }
}

core::Status BringUp(runtime::RuntimeContext& context, const core::ComponentName& name)
{
    auto status = context.ConfigureComponent(name);
    if (!status.ok()) {
        return status;
    }

    status = context.InitializeComponent(name);
    if (!status.ok()) {
        return status;
    }

    return context.StartComponent(name);
}

class SlowPeriodicTask final : public runtime::Component {
public:
    SlowPeriodicTask()
        : spec_ {
            MakeComponentName("slow_localization"),
            "simulates a periodic task that exceeds its deadline",
            {},
            {},
            {core::TriggerSpec {
                core::TriggerKind::kPeriodic,
                std::chrono::milliseconds(5),
                {},
                {},
                {},
            }},
        }
    {
    }

    runtime::ComponentSpec Describe() const override
    {
        return spec_;
    }

    core::Status Execute(runtime::ComponentContext&) override
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
        return core::Status::Ok();
    }

private:
    runtime::ComponentSpec spec_;
};

}  // namespace

int main()
{
    runtime::RuntimeOptions options;
    options.observability_options.event_callback =
        [](const observability::Event& event) {
            if (event.kind == observability::EventKind::kDeadlineMiss) {
                std::cout
                    << "[trace] event=" << observability::ToString(event.kind)
                    << " resource=" << event.resource
                    << '\n';
            }
        };
    options.observability_options.metrics_callback = PrintMetrics;

    auto context = runtime::RuntimeContext::Create(std::move(options));
    if (!context.ok()) {
        return Fail("failed to create runtime context", context.status());
    }

#if defined(PUPPETMASTER_DEMO_WITH_GOODLOG)
    observability::goodlog::SinkOptions log_options;
    log_options.directory = "/tmp/puppet_master/";
    auto status = observability::goodlog::InstallSink(
        context.value()->observer(),
        log_options);
    if (!status.ok()) {
        return Fail("failed to install GoodLog sink", status);
    }
#else
    auto status = core::Status::Ok();
#endif

    const auto endpoint = MakeEndpoint();
    auto reader = context.value()->CreateReader(endpoint);
    auto writer = context.value()->CreateWriter(endpoint);
    if (!reader.ok()) {
        return Fail("failed to create reader", reader.status());
    }
    if (!writer.ok()) {
        return Fail("failed to create writer", writer.status());
    }

    const std::string payload = "vehicle-state";
    for (int index = 0; index < 3; ++index) {
        status = writer.value()->Write(
            transport::ByteView::From(payload.data(), payload.size()));
        if (!status.ok()) {
            return Fail("failed to publish message", status);
        }
    }

    for (int index = 0; index < 2; ++index) {
        auto message = reader.value()->Read();
        if (!message.ok()) {
            return Fail("failed to read message", message.status());
        }
    }

    auto component = std::make_shared<SlowPeriodicTask>();
    const auto component_name = component->Describe().name;
    status = context.value()->RegisterComponent(component);
    if (!status.ok()) {
        return Fail("failed to register component", status);
    }
    status = BringUp(*context.value(), component_name);
    if (!status.ok()) {
        return Fail("failed to start component", status);
    }

    scheduler::Scheduler sched(*context.value());
    status = sched.RegisterComponent(component_name);
    if (!status.ok()) {
        return Fail("failed to register scheduler component", status);
    }
    status = sched.Start();
    if (!status.ok()) {
        return Fail("failed to start scheduler", status);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    status = sched.Stop();
    if (!status.ok()) {
        return Fail("failed to stop scheduler", status);
    }

    context.value()->observer()->PublishMetrics();
    status = context.value()->StopComponent(component_name);
    if (!status.ok()) {
        return Fail("failed to stop component", status);
    }
    status = context.value()->ShutdownComponent(component_name);
    if (!status.ok()) {
        return Fail("failed to shut down component", status);
    }
    return 0;
}
