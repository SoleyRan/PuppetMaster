#pragma once

#include <cstddef>
#include <cstdint>

#include <puppet_master/core/status.h>

namespace puppet_master::core {

// DeliveryGuarantee describes the runtime's delivery intent, not a backend
// contract. A transport adapter may map it directly, emulate it, or return
// StatusCode::kUnsupported when the requested behavior cannot be provided.
enum class DeliveryGuarantee : std::uint8_t {
    kBestEffort = 0,
    kReliable,
};

// RetentionPolicy describes how much unread data PuppetMaster should preserve
// for a reader. It maps cleanly to local queues and can be translated to richer
// backend concepts such as DDS history policies by transport adapters.
enum class RetentionPolicy : std::uint8_t {
    kKeepLast = 0,
    kKeepAll,
};

enum class FreshnessPolicy : std::uint8_t {
    kLatest = 0,
    kQueued,
};

enum class QueueOverflowPolicy : std::uint8_t {
    kDropOldest = 0,
    kDropNewest,
    kBlock,
    kReject,
};

struct MessagePolicy {
    DeliveryGuarantee delivery {DeliveryGuarantee::kBestEffort};
    RetentionPolicy retention {RetentionPolicy::kKeepLast};
    FreshnessPolicy freshness {FreshnessPolicy::kLatest};
    QueueOverflowPolicy overflow {QueueOverflowPolicy::kDropOldest};
    std::size_t queue_depth {1};

    Status Validate() const
    {
        if (queue_depth == 0) {
            return Status::InvalidArgument("message policy queue_depth must be greater than zero");
        }
        return Status::Ok();
    }
};

inline const char* ToString(DeliveryGuarantee guarantee) noexcept
{
    switch (guarantee) {
        case DeliveryGuarantee::kBestEffort:
            return "best_effort";
        case DeliveryGuarantee::kReliable:
            return "reliable";
    }
    return "unknown";
}

inline const char* ToString(RetentionPolicy policy) noexcept
{
    switch (policy) {
        case RetentionPolicy::kKeepLast:
            return "keep_last";
        case RetentionPolicy::kKeepAll:
            return "keep_all";
    }
    return "unknown";
}

inline const char* ToString(FreshnessPolicy policy) noexcept
{
    switch (policy) {
        case FreshnessPolicy::kLatest:
            return "latest";
        case FreshnessPolicy::kQueued:
            return "queued";
    }
    return "unknown";
}

inline const char* ToString(QueueOverflowPolicy policy) noexcept
{
    switch (policy) {
        case QueueOverflowPolicy::kDropOldest:
            return "drop_oldest";
        case QueueOverflowPolicy::kDropNewest:
            return "drop_newest";
        case QueueOverflowPolicy::kBlock:
            return "block";
        case QueueOverflowPolicy::kReject:
            return "reject";
    }
    return "unknown";
}

}  // namespace puppet_master::core
