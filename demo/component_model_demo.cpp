#include <iostream>
#include <cassert>
#include <memory>
#include <string>
#include <utility>

#include <puppet_master/puppet_master.h>

namespace core = puppet_master::core;
namespace runtime = puppet_master::runtime;
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
    policy.queue_depth = 8;

    return transport::EndpointConfig {
        core::TopicSpec {MakeTopicName("/vehicle/speed"), core::TransportKind::kInMemory, policy},
        transport::MessageDescriptor {"demo.ComponentSpeed", "text/plain"}
    };
}

class SpeedAlgorithm final : public runtime::Component {
public:
    SpeedAlgorithm()
        : endpoint_(MakeSpeedEndpoint()),
          spec_ {
              MakeComponentName("speed_algorithm"),
              "publishes a calculated speed sample",
              {},
              {endpoint_},
              {core::TriggerSpec {core::TriggerKind::kManual, {}, {}, {}, {}}}
          }
    {
    }

    runtime::ComponentSpec Describe() const override
    {
        return spec_;
    }

    core::Status Configure(runtime::ComponentContext& context) override
    {
        auto writer = context.CreateWriter(endpoint_);
        if (!writer.ok()) {
            return writer.status();
        }

        writer_ = writer.value();
        return core::Status::Ok();
    }

    core::Status Execute(runtime::ComponentContext&) override
    {
        const std::string payload = "speed=12.5";
        return writer_->Write(transport::ByteView::From(payload.data(), payload.size()));
    }

private:
    transport::EndpointConfig endpoint_;
    runtime::ComponentSpec spec_;
    transport::WriterPtr writer_;
};

std::string PayloadToString(const transport::ByteBuffer& payload)
{
    if (payload.empty()) {
        return {};
    }

    return std::string(reinterpret_cast<const char*>(payload.data()), payload.size());
}

}  // namespace

int main()
{
    auto context = runtime::RuntimeContext::Create();
    if (!context.ok()) {
        return Fail("failed to create runtime context", context.status());
    }

    auto algorithm = std::make_shared<SpeedAlgorithm>();
    const auto spec = algorithm->Describe();
    auto status = context.value()->RegisterComponent(algorithm);
    if (!status.ok()) {
        return Fail("failed to register algorithm", status);
    }

    auto reader = context.value()->CreateReader(spec.writers.front());
    if (!reader.ok()) {
        return Fail("failed to create inspection reader", reader.status());
    }

    status = context.value()->ConfigureComponent(spec.name);
    if (!status.ok()) {
        return Fail("failed to configure algorithm", status);
    }
    status = context.value()->InitializeComponent(spec.name);
    if (!status.ok()) {
        return Fail("failed to initialize algorithm", status);
    }
    status = context.value()->StartComponent(spec.name);
    if (!status.ok()) {
        return Fail("failed to start algorithm", status);
    }
    status = context.value()->ExecuteComponent(spec.name);
    if (!status.ok()) {
        return Fail("failed to execute algorithm", status);
    }

    auto message = reader.value()->Read();
    if (!message.ok()) {
        return Fail("failed to read algorithm output", message.status());
    }

    std::cout << "component: " << spec.name.str() << '\n';
    std::cout << "state: " << runtime::ToString(context.value()->GetComponentState(spec.name).value()) << '\n';
    std::cout << "output: " << PayloadToString(message.value().payload) << '\n';

    context.value()->StopComponent(spec.name);
    context.value()->ShutdownComponent(spec.name);
    return 0;
}
