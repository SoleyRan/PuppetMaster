#include <cassert>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include <puppet_master/puppet_master.h>
#include <puppet_master/transport/fastdds/fastdds_transport.h>

namespace core = puppet_master::core;
namespace transport = puppet_master::transport;
namespace fastdds = puppet_master::transport::fastdds;

namespace {

transport::EndpointConfig MakeEndpoint(const std::string& topic_name, const std::string& type_name)
{
    auto topic = core::TopicName::Create(topic_name);
    assert(topic.ok());
    core::MessagePolicy policy;
    policy.delivery = core::DeliveryGuarantee::kReliable;
    policy.queue_depth = 16;
    return transport::EndpointConfig {
        core::TopicSpec {topic.value(), core::TransportKind::kFastDds, policy},
        transport::MessageDescriptor {type_name, "application/octet-stream"}
    };
}

std::unique_ptr<fastdds::FastDdsTransport> MakeTransport()
{
    auto name = core::TransportName::Create("fastdds");
    assert(name.ok());
    return std::make_unique<fastdds::FastDdsTransport>(name.value());
}

void RejectsEndpointsBeforeOpen()
{
    auto transport = MakeTransport();
    const auto endpoint = MakeEndpoint("/test/closed", "test.Bytes");
    auto reader = transport->CreateReader(endpoint);
    assert(!reader.ok());
    assert(reader.status().code() == core::StatusCode::kFailedPrecondition);
}

void RejectsForeignTransportKind()
{
    auto transport = MakeTransport();
    auto endpoint = MakeEndpoint("/test/foreign", "test.Bytes");
    endpoint.topic.transport = core::TransportKind::kInMemory;
    assert(transport->ValidateEndpoint(endpoint).code() == core::StatusCode::kInvalidArgument);
}

void PublishSubscribeRoundTrip()
{
    auto transport = MakeTransport();
    assert(transport->Open().ok());
    assert(transport->is_open());

    const auto endpoint = MakeEndpoint("/test/roundtrip", "test.Bytes");
    auto reader = transport->CreateReader(endpoint);
    auto writer = transport->CreateWriter(endpoint);
    assert(reader.ok());
    assert(writer.ok());

    const std::string payload = "hello fastdds";
    transport::ReadOptions options;
    options.wait = true;
    options.timeout = std::chrono::milliseconds(200);

    // Intra-participant matching is asynchronous; retry until delivered.
    bool received = false;
    for (int attempt = 0; attempt < 25 && !received; ++attempt) {
        assert(writer.value()->Write(transport::ByteView::From(payload.data(), payload.size())).ok());
        auto message = reader.value()->Read(options);
        if (message.ok()) {
            const auto& bytes = message.value().payload;
            assert(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()) == payload);
            assert(message.value().metadata.sequence >= 1);
            received = true;
        }
    }
    assert(received);

    // Same topic with a different descriptor is rejected.
    auto conflicting = transport->CreateReader(MakeEndpoint("/test/roundtrip", "test.Other"));
    assert(!conflicting.ok());
    assert(conflicting.status().code() == core::StatusCode::kInvalidArgument);

    // Handles outliving Close() report Unavailable instead of crashing.
    assert(transport->Close().ok());
    assert(!transport->is_open());
    auto after_close = reader.value()->Read();
    assert(after_close.status().code() == core::StatusCode::kUnavailable);
    assert(writer.value()->Write(transport::ByteView::From(payload.data(), payload.size())).code() ==
           core::StatusCode::kUnavailable);
}

}  // namespace

int main()
{
    RejectsEndpointsBeforeOpen();
    RejectsForeignTransportKind();
    PublishSubscribeRoundTrip();
    return 0;
}
