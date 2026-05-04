#include <cassert>

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
    assert(qos.value().history_depth == 8);
    assert(qos.value().durability == fastdds::DurabilityKind::kTransientLocal);

    policy.retention = core::RetentionPolicy::kKeepAll;
    qos = fastdds::MapMessagePolicy(policy);
    assert(qos.ok());
    assert(qos.value().history == fastdds::HistoryKind::kKeepAll);

    policy.queue_depth = 0;
    qos = fastdds::MapMessagePolicy(policy);
    assert(!qos.ok());
    assert(qos.status().code() == core::StatusCode::kInvalidArgument);

    fastdds::Options options;
    assert(options.Validate().ok());
    options.transport_mode = fastdds::TransportMode::kUdp;
    options.udp_buffer_size = 0;
    assert(!options.Validate().ok());

    return 0;
}
