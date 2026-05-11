#include <iostream>
#include <memory>
#include <string>

#include <puppet_master/puppet_master.h>

namespace core = puppet_master::core;
namespace transport = puppet_master::transport;
namespace inmemory = puppet_master::transport::inmemory;

namespace {

core::Result<core::TopicName> MakeTopicName(const std::string& value)
{
    return core::TopicName::Create(value);
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

transport::EndpointConfig MakeEndpoint(core::TopicName topic, core::MessagePolicy policy)
{
    return transport::EndpointConfig {
        core::TopicSpec {std::move(topic), core::TransportKind::kInMemory, policy},
        transport::MessageDescriptor {"demo.VehicleSpeed", "text/plain"}
    };
}

int Fail(const std::string& message, const core::Status& status)
{
    std::cerr << message << ": " << status.ToString() << '\n';
    return 1;
}

}  // namespace

int main()
{
    auto transport_name = core::TransportName::Create("inmemory");
    if (!transport_name.ok()) {
        return Fail("invalid transport name", transport_name.status());
    }

    auto topic = MakeTopicName("/vehicle/speed");
    if (!topic.ok()) {
        return Fail("invalid topic name", topic.status());
    }

    auto transport = std::make_shared<inmemory::InMemoryTransport>(transport_name.value());
    auto status = transport->Open();
    if (!status.ok()) {
        return Fail("failed to open transport", status);
    }

    core::MessagePolicy latest_policy;
    latest_policy.freshness = core::FreshnessPolicy::kLatest;
    latest_policy.queue_depth = 1;

    core::MessagePolicy queued_policy;
    queued_policy.freshness = core::FreshnessPolicy::kQueued;
    queued_policy.retention = core::RetentionPolicy::kKeepLast;
    queued_policy.queue_depth = 8;

    auto latest_reader = transport->CreateReader(MakeEndpoint(topic.value(), latest_policy));
    auto queued_reader = transport->CreateReader(MakeEndpoint(topic.value(), queued_policy));
    auto writer = transport->CreateWriter(MakeEndpoint(topic.value(), latest_policy));
    if (!latest_reader.ok()) {
        return Fail("failed to create latest reader", latest_reader.status());
    }
    if (!queued_reader.ok()) {
        return Fail("failed to create queued reader", queued_reader.status());
    }
    if (!writer.ok()) {
        return Fail("failed to create writer", writer.status());
    }

    int wakeups = 0;
    status = latest_reader.value()->SetDataAvailableCallback([&wakeups]() {
        ++wakeups;
    });
    if (!status.ok()) {
        return Fail("failed to set callback", status);
    }

    for (const auto& sample : {"speed=10.0", "speed=11.5", "speed=12.0"}) {
        status = WriteText(writer.value(), sample);
        if (!status.ok()) {
            return Fail("failed to publish sample", status);
        }
    }

    auto latest = latest_reader.value()->Read();
    if (!latest.ok()) {
        return Fail("failed to read latest sample", latest.status());
    }

    std::cout << "latest reader: " << PayloadToString(latest.value().payload) << '\n';
    std::cout << "latest callback wakeups: " << wakeups << '\n';

    std::cout << "queued reader:";
    while (true) {
        auto message = queued_reader.value()->Read();
        if (!message.ok()) {
            break;
        }
        std::cout << ' ' << PayloadToString(message.value().payload);
    }
    std::cout << '\n';

    return 0;
}
