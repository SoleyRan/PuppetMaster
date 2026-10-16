#include <cassert>
#include <memory>
#include <stdexcept>
#include <string>

#include <puppet_master/puppet_master.h>

namespace core = puppet_master::core;
namespace transport = puppet_master::transport;
namespace inmemory = puppet_master::transport::inmemory;

namespace {

core::TopicName MakeTopicName(const std::string& value)
{
    auto topic = core::TopicName::Create(value);
    assert(topic.ok());
    return topic.value();
}

core::TransportName MakeTransportName()
{
    auto name = core::TransportName::Create("inmemory");
    assert(name.ok());
    return name.value();
}

transport::MessageDescriptor TestDescriptor()
{
    return transport::MessageDescriptor {"test.Payload", "text/plain"};
}

transport::EndpointConfig MakeEndpoint(const std::string& topic_name, core::MessagePolicy policy)
{
    return transport::EndpointConfig {
        core::TopicSpec {MakeTopicName(topic_name), core::TransportKind::kInMemory, policy},
        TestDescriptor()
    };
}

std::shared_ptr<inmemory::InMemoryTransport> MakeOpenTransport()
{
    auto transport = std::make_shared<inmemory::InMemoryTransport>(MakeTransportName());
    assert(transport->Open().ok());
    assert(transport->is_open());
    return transport;
}

core::Status WriteString(const transport::WriterPtr& writer, const std::string& payload)
{
    return writer->Write(transport::ByteView::From(payload.data(), payload.size()));
}

std::string PayloadToString(const transport::ByteBuffer& payload)
{
    if (payload.empty()) {
        return {};
    }
    return std::string(reinterpret_cast<const char*>(payload.data()), payload.size());
}

std::string ReadString(const transport::ReaderPtr& reader)
{
    auto message = reader->Read();
    assert(message.ok());
    return PayloadToString(message.value().payload);
}

void PublishSubscribeInvokesCallback()
{
    auto transport = MakeOpenTransport();
    const auto endpoint = MakeEndpoint("/test/pubsub", core::MessagePolicy {});

    auto reader = transport->CreateReader(endpoint);
    auto writer = transport->CreateWriter(endpoint);
    assert(reader.ok());
    assert(writer.ok());

    int callback_count = 0;
    assert(reader.value()->SetDataAvailableCallback([&callback_count]() {
        ++callback_count;
    }).ok());

    assert(WriteString(writer.value(), "hello").ok());
    assert(callback_count == 1);

    auto message = reader.value()->Read();
    assert(message.ok());
    assert(PayloadToString(message.value().payload) == "hello");
    assert(message.value().metadata.sequence == 1);
    assert(message.value().metadata.source_timestamp != core::TimePoint {});
    assert(message.value().metadata.reception_timestamp != core::TimePoint {});
}

void ThrowingReaderCallbackDoesNotEscapeWrite()
{
    auto transport = MakeOpenTransport();
    auto endpoint = MakeEndpoint("/test/throwing_callback", core::MessagePolicy {});
    auto throwing_reader = transport->CreateReader(endpoint);
    auto other_reader = transport->CreateReader(endpoint);
    auto writer = transport->CreateWriter(endpoint);
    assert(throwing_reader.ok());
    assert(other_reader.ok());
    assert(writer.ok());
    assert(throwing_reader.value()->SetDataAvailableCallback([]() {
        throw std::runtime_error("callback failed");
    }).ok());

    auto status = WriteString(writer.value(), "hello");
    assert(status.code() == core::StatusCode::kInternal);
    assert(ReadString(throwing_reader.value()) == "hello");
    assert(ReadString(other_reader.value()) == "hello");
}

void LatestReaderKeepsOnlyNewestMessage()
{
    auto transport = MakeOpenTransport();

    core::MessagePolicy policy;
    policy.freshness = core::FreshnessPolicy::kLatest;
    policy.retention = core::RetentionPolicy::kKeepLast;
    policy.queue_depth = 1;

    const auto endpoint = MakeEndpoint("/test/latest", policy);
    auto reader = transport->CreateReader(endpoint);
    auto writer = transport->CreateWriter(endpoint);
    assert(reader.ok());
    assert(writer.ok());

    assert(WriteString(writer.value(), "one").ok());
    assert(WriteString(writer.value(), "two").ok());
    assert(WriteString(writer.value(), "three").ok());

    assert(ReadString(reader.value()) == "three");

    auto empty_read = reader.value()->Read();
    assert(!empty_read.ok());
    assert(empty_read.status().code() == core::StatusCode::kUnavailable);
}

void QueuedReaderKeepsPublishOrder()
{
    auto transport = MakeOpenTransport();

    core::MessagePolicy policy;
    policy.freshness = core::FreshnessPolicy::kQueued;
    policy.retention = core::RetentionPolicy::kKeepLast;
    policy.queue_depth = 4;

    const auto endpoint = MakeEndpoint("/test/queued", policy);
    auto reader = transport->CreateReader(endpoint);
    auto writer = transport->CreateWriter(endpoint);
    assert(reader.ok());
    assert(writer.ok());

    assert(WriteString(writer.value(), "one").ok());
    assert(WriteString(writer.value(), "two").ok());
    assert(WriteString(writer.value(), "three").ok());

    assert(ReadString(reader.value()) == "one");
    assert(ReadString(reader.value()) == "two");
    assert(ReadString(reader.value()) == "three");
}

void TopicFanOutKeepsReaderMailboxesIndependent()
{
    auto transport = MakeOpenTransport();

    core::MessagePolicy latest_policy;
    latest_policy.freshness = core::FreshnessPolicy::kLatest;
    latest_policy.queue_depth = 1;

    core::MessagePolicy queued_policy;
    queued_policy.freshness = core::FreshnessPolicy::kQueued;
    queued_policy.retention = core::RetentionPolicy::kKeepLast;
    queued_policy.queue_depth = 4;

    auto latest_reader = transport->CreateReader(MakeEndpoint("/test/fanout", latest_policy));
    auto queued_reader = transport->CreateReader(MakeEndpoint("/test/fanout", queued_policy));
    auto writer = transport->CreateWriter(MakeEndpoint("/test/fanout", latest_policy));
    assert(latest_reader.ok());
    assert(queued_reader.ok());
    assert(writer.ok());

    assert(WriteString(writer.value(), "one").ok());
    assert(WriteString(writer.value(), "two").ok());

    assert(ReadString(latest_reader.value()) == "two");
    assert(ReadString(queued_reader.value()) == "one");
    assert(ReadString(queued_reader.value()) == "two");
}

void FullReaderDoesNotPreventDeliveryToOtherReaders()
{
    auto transport = MakeOpenTransport();
    core::MessagePolicy policy;
    policy.freshness = core::FreshnessPolicy::kQueued;
    policy.retention = core::RetentionPolicy::kKeepLast;
    policy.overflow = core::QueueOverflowPolicy::kReject;
    policy.queue_depth = 1;

    const auto endpoint = MakeEndpoint("/test/rejected_fanout", policy);
    auto full_reader = transport->CreateReader(endpoint);
    auto other_reader = transport->CreateReader(endpoint);
    auto writer = transport->CreateWriter(endpoint);
    assert(full_reader.ok());
    assert(other_reader.ok());
    assert(writer.ok());

    assert(WriteString(writer.value(), "one").ok());
    assert(ReadString(other_reader.value()) == "one");
    auto status = WriteString(writer.value(), "two");
    assert(status.code() == core::StatusCode::kResourceExhausted);
    assert(ReadString(full_reader.value()) == "one");
    assert(ReadString(other_reader.value()) == "two");
}

void BoundedQueueDropsOldestMessage()
{
    auto transport = MakeOpenTransport();

    core::MessagePolicy policy;
    policy.freshness = core::FreshnessPolicy::kQueued;
    policy.retention = core::RetentionPolicy::kKeepLast;
    policy.overflow = core::QueueOverflowPolicy::kDropOldest;
    policy.queue_depth = 2;

    const auto endpoint = MakeEndpoint("/test/bounded", policy);
    auto reader = transport->CreateReader(endpoint);
    auto writer = transport->CreateWriter(endpoint);
    assert(reader.ok());
    assert(writer.ok());

    assert(WriteString(writer.value(), "one").ok());
    assert(WriteString(writer.value(), "two").ok());
    assert(WriteString(writer.value(), "three").ok());

    assert(ReadString(reader.value()) == "two");
    assert(ReadString(reader.value()) == "three");
}

void UnboundedQueueKeepsAllMessages()
{
    auto transport = MakeOpenTransport();

    core::MessagePolicy policy;
    policy.freshness = core::FreshnessPolicy::kQueued;
    policy.retention = core::RetentionPolicy::kKeepAll;
    policy.overflow = core::QueueOverflowPolicy::kReject;
    policy.queue_depth = 1;

    const auto endpoint = MakeEndpoint("/test/unbounded", policy);
    auto reader = transport->CreateReader(endpoint);
    auto writer = transport->CreateWriter(endpoint);
    assert(reader.ok());
    assert(writer.ok());

    assert(WriteString(writer.value(), "one").ok());
    assert(WriteString(writer.value(), "two").ok());
    assert(WriteString(writer.value(), "three").ok());

    assert(ReadString(reader.value()) == "one");
    assert(ReadString(reader.value()) == "two");
    assert(ReadString(reader.value()) == "three");
}

}  // namespace

int main()
{
    PublishSubscribeInvokesCallback();
    ThrowingReaderCallbackDoesNotEscapeWrite();
    LatestReaderKeepsOnlyNewestMessage();
    QueuedReaderKeepsPublishOrder();
    TopicFanOutKeepsReaderMailboxesIndependent();
    FullReaderDoesNotPreventDeliveryToOtherReaders();
    BoundedQueueDropsOldestMessage();
    UnboundedQueueKeepsAllMessages();
    return 0;
}
