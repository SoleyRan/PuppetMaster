#include <algorithm>
#include <cassert>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include <puppet_master/puppet_master.h>

namespace core = puppet_master::core;
namespace observability = puppet_master::observability;
namespace runtime = puppet_master::runtime;
namespace transport = puppet_master::transport;

namespace {

transport::EndpointConfig MakeQueuedEndpoint()
{
    core::MessagePolicy policy;
    policy.freshness = core::FreshnessPolicy::kQueued;
    policy.retention = core::RetentionPolicy::kKeepLast;
    policy.queue_depth = 4;

    auto topic = core::TopicName::Create("/observability/transport");
    assert(topic.ok());

    return transport::EndpointConfig {
        core::TopicSpec {
            topic.value(),
            core::TransportKind::kInMemory,
            policy,
        },
        transport::MessageDescriptor {"test.Payload", "text/plain"},
    };
}

void TransportOperationsUpdateMetricsAndEvents()
{
    std::vector<observability::EventKind> events;

    runtime::RuntimeOptions options;
    options.observability_options.event_callback =
        [&events](const observability::Event& event) {
            events.push_back(event.kind);
        };

    auto context = runtime::RuntimeContext::Create(std::move(options));
    assert(context.ok());

    const auto endpoint = MakeQueuedEndpoint();
    auto reader = context.value()->CreateReader(endpoint);
    auto writer = context.value()->CreateWriter(endpoint);
    assert(reader.ok());
    assert(writer.ok());

    std::size_t notifications = 0;
    assert(reader.value()->SetDataAvailableCallback([&notifications]() {
        ++notifications;
    }).ok());

    const std::string first_payload = "first";
    const std::string second_payload = "second";
    assert(writer.value()->Write(
        transport::ByteView::From(first_payload.data(), first_payload.size())).ok());
    assert(writer.value()->Write(
        transport::ByteView::From(second_payload.data(), second_payload.size())).ok());
    assert(notifications == 2);

    auto pending = reader.value()->PendingMessageCount();
    assert(pending.ok());
    assert(pending.value() == 2);

    auto message = reader.value()->Read();
    assert(message.ok());

    const auto snapshot = context.value()->observer()->Snapshot();
    assert(snapshot.topics.size() == 1);

    const auto& metrics = snapshot.topics.front();
    assert(metrics.topic_name == "/observability/transport");
    assert(metrics.messages_published == 2);
    assert(metrics.bytes_published == first_payload.size() + second_payload.size());
    assert(metrics.messages_received == 1);
    assert(metrics.bytes_received == first_payload.size());
    assert(metrics.latency_samples == 1);
    assert(metrics.queue_depth == 1);
    assert(metrics.max_queue_depth == 2);
    assert(metrics.publish_messages_per_second > 0.0);
    assert(metrics.receive_messages_per_second > 0.0);

    assert(std::count(
        events.begin(),
        events.end(),
        observability::EventKind::kTopicPublished) == 2);
    assert(std::count(
        events.begin(),
        events.end(),
        observability::EventKind::kTopicReceived) == 1);
    assert(std::count(
        events.begin(),
        events.end(),
        observability::EventKind::kQueueDepth) >= 3);
}

}  // namespace

int main()
{
    TransportOperationsUpdateMetricsAndEvents();
    return 0;
}
