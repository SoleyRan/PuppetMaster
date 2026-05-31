#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

#include <puppet_master/puppet_master.h>

namespace config = puppet_master::configuration;
namespace core = puppet_master::core;
namespace runtime = puppet_master::runtime;
namespace scheduler = puppet_master::scheduler;
namespace transport = puppet_master::transport;

namespace {

int Fail(const std::string& message, const core::Status& status)
{
    std::cerr << message << ": " << status.ToString() << '\n';
    return 1;
}

core::Result<config::ProjectConfig> BuildProjectConfig()
{
    core::MessagePolicy queued_policy;
    queued_policy.freshness = core::FreshnessPolicy::kQueued;
    queued_policy.retention = core::RetentionPolicy::kKeepLast;
    queued_policy.queue_depth = 8;

    config::ProjectConfig project;
    auto status = project.AddTopic(config::TopicConfig {
        "speed",
        "/vehicle/speed",
        core::TransportKind::kInMemory,
        "demo.Speed",
        "text/plain",
        queued_policy
    });
    if (!status.ok()) {
        return core::Result<config::ProjectConfig>::FromStatus(std::move(status));
    }

    config::TriggerConfig speed_trigger;
    speed_trigger.kind = core::TriggerKind::kData;
    speed_trigger.dependency_policy = core::DependencyPolicy::kAny;
    speed_trigger.data_topics = {"speed"};

    status = project.AddComponent(config::ComponentConfig {
        "speed_monitor",
        "runs when speed data arrives",
        {"speed"},
        {},
        {speed_trigger}
    });
    if (!status.ok()) {
        return core::Result<config::ProjectConfig>::FromStatus(std::move(status));
    }

    status = project.Validate();
    if (!status.ok()) {
        return core::Result<config::ProjectConfig>::FromStatus(std::move(status));
    }

    return project;
}

std::string PayloadToString(const transport::ByteBuffer& payload)
{
    if (payload.empty()) {
        return {};
    }

    return std::string(reinterpret_cast<const char*>(payload.data()), payload.size());
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

class ConfiguredSpeedMonitor final : public runtime::Component {
public:
    explicit ConfiguredSpeedMonitor(runtime::ComponentSpec spec)
        : spec_(std::move(spec))
    {
    }

    runtime::ComponentSpec Describe() const override
    {
        return spec_;
    }

    core::Status Configure(runtime::ComponentContext& context) override
    {
        auto reader = context.CreateReader(spec_.readers.front());
        if (!reader.ok()) {
            return reader.status();
        }

        reader_ = reader.value();
        return core::Status::Ok();
    }

    core::Status Execute(runtime::ComponentContext&) override
    {
        auto message = reader_->Read();
        if (!message.ok()) {
            return message.status();
        }

        last_payload_ = PayloadToString(message.value().payload);
        ++execute_count_;
        return core::Status::Ok();
    }

    int execute_count() const noexcept
    {
        return execute_count_;
    }

    const std::string& last_payload() const noexcept
    {
        return last_payload_;
    }

private:
    runtime::ComponentSpec spec_;
    transport::ReaderPtr reader_;
    std::string last_payload_;
    int execute_count_ {0};
};

}  // namespace

int main()
{
    auto project = BuildProjectConfig();
    if (!project.ok()) {
        return Fail("failed to build project config", project.status());
    }

    auto options = project.value().ToRuntimeOptions();
    if (!options.ok()) {
        return Fail("failed to build runtime options", options.status());
    }

    auto context = runtime::RuntimeContext::Create(options.value());
    if (!context.ok()) {
        return Fail("failed to create runtime context", context.status());
    }

    auto monitor_spec = project.value().BuildComponentSpec("speed_monitor");
    if (!monitor_spec.ok()) {
        return Fail("failed to build component spec", monitor_spec.status());
    }

    auto monitor = std::make_shared<ConfiguredSpeedMonitor>(monitor_spec.value());
    auto status = context.value()->RegisterComponent(monitor);
    if (!status.ok()) {
        return Fail("failed to register component", status);
    }

    const auto component_name = monitor->Describe().name;
    status = BringUp(*context.value(), component_name);
    if (!status.ok()) {
        return Fail("failed to bring up component", status);
    }

    scheduler::Scheduler sched(*context.value());
    status = sched.RegisterAllComponents();
    if (!status.ok()) {
        return Fail("failed to register scheduler components", status);
    }

    status = sched.Start();
    if (!status.ok()) {
        return Fail("failed to start scheduler", status);
    }

    auto endpoint = project.value().BuildEndpoint("speed");
    if (!endpoint.ok()) {
        return Fail("failed to build speed endpoint", endpoint.status());
    }

    auto writer = context.value()->CreateWriter(endpoint.value());
    if (!writer.ok()) {
        return Fail("failed to create speed writer", writer.status());
    }

    const std::string payload = "speed=12.5";
    status = writer.value()->Write(transport::ByteView::From(payload.data(), payload.size()));
    if (!status.ok()) {
        return Fail("failed to publish speed", status);
    }

    status = sched.WaitIdle(std::chrono::milliseconds(500));
    if (!status.ok()) {
        return Fail("scheduler did not become idle", status);
    }

    std::cout << "component: " << component_name.str() << '\n';
    std::cout << "executions: " << monitor->execute_count() << '\n';
    std::cout << "last payload: " << monitor->last_payload() << '\n';

    sched.Stop();
    context.value()->StopComponent(component_name);
    context.value()->ShutdownComponent(component_name);
    return 0;
}
