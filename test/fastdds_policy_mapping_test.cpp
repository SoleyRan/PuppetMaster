#include <cassert>
#include <cstdint>
#include <limits>

#include <puppet_master/transport/fastdds/policy_mapping.h>

namespace core = puppet_master::core;
namespace fastdds = puppet_master::transport::fastdds;

int main()
{
    core::MessagePolicy policy;
    policy.delivery = core::DeliveryGuarantee::kReliable;
    policy.retention = core::RetentionPolicy::kKeepLast;
    policy.queue_depth = 8;

    auto qos = fastdds::MapMessagePolicy(policy, fastdds::DurabilityKind::kTransientLocal);
    assert(qos.ok());
    assert(qos.value().reliability == fastdds::ReliabilityKind::kReliable);
    assert(qos.value().history == fastdds::HistoryKind::kKeepLast);
    assert(qos.value().history_depth == 1);
    assert(qos.value().durability == fastdds::DurabilityKind::kTransientLocal);
    auto writer_qos = fastdds::MapWriterMessagePolicy(policy);
    assert(writer_qos.ok());
    assert(writer_qos.value().history == fastdds::HistoryKind::kKeepLast);
    assert(writer_qos.value().history_depth == 8);

    policy.freshness = core::FreshnessPolicy::kQueued;
    qos = fastdds::MapMessagePolicy(policy);
    assert(qos.ok());
    assert(qos.value().history == fastdds::HistoryKind::kKeepLast);
    assert(qos.value().history_depth == 8);

    policy.retention = core::RetentionPolicy::kKeepAll;
    qos = fastdds::MapMessagePolicy(policy);
    assert(qos.ok());
    assert(qos.value().history == fastdds::HistoryKind::kKeepAll);
    writer_qos = fastdds::MapWriterMessagePolicy(policy);
    assert(writer_qos.ok());
    assert(writer_qos.value().history == fastdds::HistoryKind::kKeepAll);

    policy.overflow = core::QueueOverflowPolicy::kDropNewest;
    qos = fastdds::MapMessagePolicy(policy);
    assert(!qos.ok());
    assert(qos.status().code() == core::StatusCode::kUnsupported);
    policy.overflow = core::QueueOverflowPolicy::kBlock;
    qos = fastdds::MapMessagePolicy(policy);
    assert(!qos.ok());
    assert(qos.status().code() == core::StatusCode::kUnsupported);
    policy.overflow = core::QueueOverflowPolicy::kReject;
    qos = fastdds::MapMessagePolicy(policy);
    assert(!qos.ok());
    assert(qos.status().code() == core::StatusCode::kUnsupported);
    policy.overflow = core::QueueOverflowPolicy::kDropOldest;

    policy.retention = core::RetentionPolicy::kKeepLast;
    policy.queue_depth = static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()) + 1;
    qos = fastdds::MapMessagePolicy(policy);
    assert(!qos.ok());
    assert(qos.status().code() == core::StatusCode::kInvalidArgument);

    policy.freshness = core::FreshnessPolicy::kLatest;
    qos = fastdds::MapMessagePolicy(policy);
    assert(qos.ok());
    writer_qos = fastdds::MapWriterMessagePolicy(policy);
    assert(!writer_qos.ok());
    assert(writer_qos.status().code() == core::StatusCode::kInvalidArgument);

    policy.queue_depth = 0;
    qos = fastdds::MapMessagePolicy(policy);
    assert(!qos.ok());
    assert(qos.status().code() == core::StatusCode::kInvalidArgument);

    fastdds::Options options;
    assert(options.Validate().ok());
    options.data_sharing = true;
    assert(options.Validate().code() == core::StatusCode::kUnsupported);
    options.data_sharing = false;
    options.transport_mode = fastdds::TransportMode::kUdp;
    options.udp_buffer_size = 0;
    assert(!options.Validate().ok());

    return 0;
}
