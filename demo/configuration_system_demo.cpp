#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

#include <puppet_master/puppet_master.h>

#ifdef PUPPETMASTER_DEMO_WITH_JSON_CONFIG
#include <puppet_master/configuration/json_loader.h>
#endif

namespace config = puppet_master::configuration;
namespace core = puppet_master::core;
namespace runtime = puppet_master::runtime;
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
        {speed_trigger},
        "speed"
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

int main(int argc, char* argv[])
{
    if (argc > 2) {
        std::cerr << "usage: " << argv[0] << " [config.json]\n";
        return 1;
    }

#ifdef PUPPETMASTER_DEMO_WITH_JSON_CONFIG
    auto project = argc == 2 ? config::LoadJsonConfig(argv[1]) : BuildProjectConfig();
#else
    if (argc == 2) {
        std::cerr << "JSON configuration support is disabled; rebuild with PUPPETMASTER_ENABLE_JSON_CONFIG=ON\n";
        return 1;
    }
    auto project = BuildProjectConfig();
#endif
    if (!project.ok()) {
        return Fail("failed to load project config", project.status());
    }

    auto endpoint = project.value().BuildEndpoint("speed");
    if (!endpoint.ok()) {
        return Fail("failed to build speed endpoint", endpoint.status());
    }

    auto monitor_spec = project.value().BuildComponentSpec("speed_monitor");
    if (!monitor_spec.ok()) {
        return Fail("failed to build speed monitor spec", monitor_spec.status());
    }
    const auto& spec = monitor_spec.value();
    if (project.value().components.size() != 1 || spec.readers.size() != 1 ||
        spec.readers.front().topic.name != endpoint.value().topic.name ||
        endpoint.value().topic.transport != core::TransportKind::kInMemory ||
        spec.triggers.size() != 1 || spec.triggers.front().kind != core::TriggerKind::kData ||
        spec.triggers.front().data_dependencies.size() != 1 ||
        spec.triggers.front().data_dependencies.front() != endpoint.value().topic.name) {
        std::cerr << "config must contain only speed_monitor with one reader and data trigger for an in-memory speed topic\n";
        return 1;
    }

    auto options = project.value().ToRuntimeOptions();
    if (!options.ok()) {
        return Fail("failed to build runtime options", options.status());
    }

    auto context = runtime::RuntimeContext::Create(options.value());
    if (!context.ok()) {
        return Fail("failed to create runtime context", context.status());
    }

    std::shared_ptr<ConfiguredSpeedMonitor> monitor;
    config::ComponentFactoryRegistry factories;
    auto status = factories.Register("speed",
        [&monitor](const runtime::ComponentSpec& component_spec) -> core::Result<runtime::ComponentPtr> {
            monitor = std::make_shared<ConfiguredSpeedMonitor>(component_spec);
            runtime::ComponentPtr component = monitor;
            return component;
        });
    if (!status.ok()) {
        return Fail("failed to register speed component factory", status);
    }

    auto sched = config::CreateScheduler(project.value(), *context.value(), factories);
    if (!sched.ok()) {
        return Fail("failed to create configured scheduler", sched.status());
    }

    const auto component_name = spec.name;
    status = BringUp(*context.value(), component_name);
    if (!status.ok()) {
        return Fail("failed to bring up component", status);
    }

    status = sched.value()->Start();
    if (!status.ok()) {
        return Fail("failed to start scheduler", status);
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

    status = sched.value()->WaitIdle(std::chrono::milliseconds(500));
    if (!status.ok()) {
        return Fail("scheduler did not become idle", status);
    }

    status = sched.value()->Stop();
    if (!status.ok()) {
        return Fail("failed to stop scheduler", status);
    }
    if (monitor->execute_count() != 1 || monitor->last_payload() != payload) {
        std::cerr << "unexpected speed monitor result: expected one execution with payload " << payload
                  << ", got " << monitor->execute_count() << " with payload " << monitor->last_payload() << '\n';
        return 1;
    }

    std::cout << "component: " << component_name.str() << '\n';
    std::cout << "executions: " << monitor->execute_count() << '\n';
    std::cout << "last payload: " << monitor->last_payload() << '\n';

    context.value()->StopComponent(component_name);
    context.value()->ShutdownComponent(component_name);
    return 0;
}
