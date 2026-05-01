#pragma once

#include <cstdint>
#include <string>

namespace puppet_master::core {

// TransportKind names the communication backend selected for a topic. The
// runtime should depend on this enum and the transport interface, not on any
// concrete middleware headers.
enum class TransportKind : std::uint8_t {
    kInMemory = 0,
    kFastDds,
    kZmq,
    kIpc,
};

enum class EndpointKind : std::uint8_t {
    kReader = 0,
    kWriter,
};

enum class TriggerKind : std::uint8_t {
    kPeriodic = 0,
    kData,
    kTaskDependency,
    kManual,
};

enum class FreshnessPolicy : std::uint8_t {
    kLatest = 0,
    kQueued,
};

enum class QueueOverflowPolicy : std::uint8_t {
    kDropOldest = 0,
    kDropNewest,
    kBlock,
};

using ComponentName = std::string;
using TaskName = std::string;
using TopicName = std::string;

}  // namespace puppet_master::core
