#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include <puppet_master/core/status.h>
#include <puppet_master/core/types.h>

namespace puppet_master::transport::fastdds {

enum class TransportMode : std::uint8_t {
    kDefault = 0,
    kUdp,
    kSharedMemory,
    kHybrid,
};

enum class ReliabilityKind : std::uint8_t {
    kBestEffort = 0,
    kReliable,
};

enum class HistoryKind : std::uint8_t {
    kKeepLast = 0,
    kKeepAll,
};

enum class DurabilityKind : std::uint8_t {
    kVolatile = 0,
    kTransientLocal,
};

struct QosProfile {
    ReliabilityKind reliability {ReliabilityKind::kBestEffort};
    HistoryKind history {HistoryKind::kKeepLast};
    std::size_t history_depth {1};
    DurabilityKind durability {DurabilityKind::kVolatile};

    core::Status Validate() const
    {
        if (history == HistoryKind::kKeepLast && history_depth == 0) {
            return core::Status::InvalidArgument("FastDDS history_depth must be greater than zero");
        }
        return core::Status::Ok();
    }
};

struct Options {
    core::DomainId domain_id {0};
    std::string participant_name {"PuppetMasterFastDDS"};
    TransportMode transport_mode {TransportMode::kDefault};
    DurabilityKind durability {DurabilityKind::kVolatile};
    bool data_sharing {true};
    bool async_publish {true};
    std::size_t udp_buffer_size {16 * 1024 * 1024};
    std::size_t shm_segment_size {160 * 1024 * 1024};

    core::Status Validate() const
    {
        if (participant_name.empty()) {
            return core::Status::InvalidArgument("FastDDS participant_name must not be empty");
        }

        if ((transport_mode == TransportMode::kUdp || transport_mode == TransportMode::kHybrid)
            && udp_buffer_size == 0) {
            return core::Status::InvalidArgument("FastDDS udp_buffer_size must be greater than zero");
        }

        if ((transport_mode == TransportMode::kSharedMemory || transport_mode == TransportMode::kHybrid)
            && shm_segment_size == 0) {
            return core::Status::InvalidArgument("FastDDS shm_segment_size must be greater than zero");
        }

        return core::Status::Ok();
    }
};

inline const char* ToString(TransportMode mode) noexcept
{
    switch (mode) {
        case TransportMode::kDefault:
            return "default";
        case TransportMode::kUdp:
            return "udp";
        case TransportMode::kSharedMemory:
            return "shared_memory";
        case TransportMode::kHybrid:
            return "hybrid";
    }
    return "unknown";
}

}  // namespace puppet_master::transport::fastdds
