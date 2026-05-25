#include <cassert>
#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

#include <puppet_master/puppet_master.h>

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

transport::EndpointConfig MakeSpeedEndpoint()
{
    core::MessagePolicy policy;
    policy.freshness = core::FreshnessPolicy::kQueued;
    policy.retention = core::RetentionPolicy::kKeepLast;
    policy.queue_depth = 8;

    return transport::EndpointConfig {
        core::TopicSpec {MakeTopicName("/vehicle/speed"), core::TransportKind::kInMemory, policy},
        transport::MessageDescriptor {"demo.SchedulerSpeed", "text/plain"}
    };
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

class SpeedMonitor final : public runtime::Component {
public:
    explicit SpeedMonitor(transport::EndpointConfig endpoint)
        : spec_ {
            MakeComponentName("speed_monitor"),
            "runs when speed data arrives",
            {std::move(endpoint)},
            {},
            {}
        }
    {
        spec_.triggers = {core::TriggerSpec {
            core::TriggerKind::kData,
            {},
            core::DependencyPolicy::kAny,
            {spec_.readers.front().topic.name},
            {}
        }};
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

    const std::string& last_payload() const noexcept
    {
        return last_payload_;
    }

    int execute_count() const noexcept
    {
        return execute_count_;
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
    auto context = runtime::RuntimeContext::Create();
    if (!context.ok()) {
        return Fail("failed to create runtime context", context.status());
    }

    const auto endpoint = MakeSpeedEndpoint();
    auto monitor = std::make_shared<SpeedMonitor>(endpoint);
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

    auto writer = context.value()->CreateWriter(endpoint);
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
