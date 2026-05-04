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

    QosProfile qos;
    qos.durability = durability;
    qos.history_depth = policy.queue_depth;

    switch (policy.delivery) {
        case core::DeliveryGuarantee::kBestEffort:
            qos.reliability = ReliabilityKind::kBestEffort;
            break;
        case core::DeliveryGuarantee::kReliable:
            qos.reliability = ReliabilityKind::kReliable;
            break;
    }

    switch (policy.retention) {
        case core::RetentionPolicy::kKeepLast:
            qos.history = HistoryKind::kKeepLast;
            break;
        case core::RetentionPolicy::kKeepAll:
            qos.history = HistoryKind::kKeepAll;
            break;
    }

    status = qos.Validate();
    if (!status.ok()) {
        return core::Result<QosProfile>::FromStatus(std::move(status));
    }

    return qos;
}

}  // namespace puppet_master::transport::fastdds
