#pragma once

#include <puppet_master/core/message_policy.h>
#include <puppet_master/core/result.h>
#include <puppet_master/core/status.h>
#include <puppet_master/transport/fastdds/options.h>

#include <utility>

namespace puppet_master::transport::fastdds {

inline core::Result<QosProfile> MapMessagePolicy(
    const core::MessagePolicy& policy,
    DurabilityKind durability = DurabilityKind::kVolatile)
{
    auto status = policy.Validate();
    if (!status.ok()) {
        return core::Result<QosProfile>::FromStatus(std::move(status));
    }
    if (policy.overflow != core::QueueOverflowPolicy::kDropOldest) {
        return core::Result<QosProfile>::FromStatus(
            core::Status::Unsupported("FastDDS supports only drop-oldest reader history overflow"));
    }

    QosProfile qos;
    qos.durability = durability;
    qos.history_depth = policy.freshness == core::FreshnessPolicy::kLatest ? 1 : policy.queue_depth;

    switch (policy.delivery) {
        case core::DeliveryGuarantee::kBestEffort:
            qos.reliability = ReliabilityKind::kBestEffort;
            break;
        case core::DeliveryGuarantee::kReliable:
            qos.reliability = ReliabilityKind::kReliable;
            break;
    }

    switch (policy.freshness) {
        case core::FreshnessPolicy::kLatest:
            qos.history = HistoryKind::kKeepLast;
            break;
        case core::FreshnessPolicy::kQueued:
            qos.history = policy.retention == core::RetentionPolicy::kKeepAll
                ? HistoryKind::kKeepAll : HistoryKind::kKeepLast;
            break;
    }

    status = qos.Validate();
    if (!status.ok()) {
        return core::Result<QosProfile>::FromStatus(std::move(status));
    }

    return qos;
}

inline core::Result<QosProfile> MapWriterMessagePolicy(
    const core::MessagePolicy& policy,
    DurabilityKind durability = DurabilityKind::kVolatile)
{
    auto writer_policy = policy;
    writer_policy.freshness = core::FreshnessPolicy::kQueued;
    return MapMessagePolicy(writer_policy, durability);
}

}  // namespace puppet_master::transport::fastdds
