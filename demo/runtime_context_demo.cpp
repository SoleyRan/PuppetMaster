#include <iostream>
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

core::Result<core::ComponentName> MakeComponentName(const std::string& value)
{
    return core::ComponentName::Create(value);
}

core::Result<core::TopicName> MakeTopicName(const std::string& value)
{
    return core::TopicName::Create(value);
}

transport::EndpointConfig MakeEndpoint(core::TopicName topic)
{
    core::MessagePolicy policy;
    policy.freshness = core::FreshnessPolicy::kQueued;
    policy.queue_depth = 8;

    return transport::EndpointConfig {
        core::TopicSpec {std::move(topic), core::TransportKind::kInMemory, policy},
        transport::MessageDescriptor {"demo.RuntimeSpeed", "text/plain"}
    };
}

std::string PayloadToString(const transport::ByteBuffer& payload)
{
    if (payload.empty()) {
        return {};
    }
    return std::string(reinterpret_cast<const char*>(payload.data()), payload.size());
}

core::Status WriteText(const transport::WriterPtr& writer, const std::string& payload)
{
    return writer->Write(transport::ByteView::From(payload.data(), payload.size()));
}

}  // namespace

int main()
{
    auto context = runtime::RuntimeContext::Create();
    if (!context.ok()) {
        return Fail("failed to create runtime context", context.status());
    }

    auto producer_name = MakeComponentName("speed_producer");
    auto consumer_name = MakeComponentName("speed_consumer");
    auto topic_name = MakeTopicName("/vehicle/speed");
    if (!producer_name.ok()) {
        return Fail("invalid producer name", producer_name.status());
    }
    if (!consumer_name.ok()) {
        return Fail("invalid consumer name", consumer_name.status());
    }
    if (!topic_name.ok()) {
        return Fail("invalid topic name", topic_name.status());
    }

    const auto endpoint = MakeEndpoint(topic_name.value());
    auto status = context.value()->RegisterComponent(runtime::ComponentSpec {
        producer_name.value(),
        "publishes speed samples",
        {},
        {endpoint},
        {}
    });
    if (!status.ok()) {
        return Fail("failed to register producer", status);
    }

    status = context.value()->RegisterComponent(runtime::ComponentSpec {
        consumer_name.value(),
        "reads speed samples",
        {endpoint},
        {},
        {core::TriggerSpec {core::TriggerKind::kData, {}, {}, {endpoint.topic.name}, {}}}
    });
    if (!status.ok()) {
        return Fail("failed to register consumer", status);
    }

    auto reader = context.value()->CreateReader(endpoint);
    auto writer = context.value()->CreateWriter(endpoint);
    if (!reader.ok()) {
        return Fail("failed to create reader", reader.status());
    }
    if (!writer.ok()) {
        return Fail("failed to create writer", writer.status());
    }

    for (const auto& sample : {"speed=10.0", "speed=11.5", "speed=12.0"}) {
        status = WriteText(writer.value(), sample);
        if (!status.ok()) {
            return Fail("failed to publish sample", status);
        }
    }

    std::cout << "runtime components:";
    for (const auto& name : context.value()->ListComponentNames()) {
        std::cout << ' ' << name.str();
    }
    std::cout << '\n';

    std::cout << "runtime transports:";
    for (const auto& name : context.value()->ListTransportNames()) {
        std::cout << ' ' << name.str();
    }
    std::cout << '\n';

    std::cout << "received:";
    while (true) {
        auto message = reader.value()->Read();
        if (!message.ok()) {
            break;
        }
        std::cout << ' ' << PayloadToString(message.value().payload);
    }
    std::cout << '\n';

    return 0;
}
