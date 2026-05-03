#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <puppet_master/core/message_policy.h>
#include <puppet_master/core/result.h>
#include <puppet_master/core/status.h>

namespace puppet_master::core {

using Byte = std::uint8_t;
using DomainId = std::uint32_t;
using SequenceNumber = std::uint64_t;
using Nanoseconds = std::chrono::nanoseconds;
using SteadyClock = std::chrono::steady_clock;
using TimePoint = std::chrono::time_point<SteadyClock, Nanoseconds>;

inline constexpr std::size_t kMaxNameLength = 255;

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

enum class DependencyPolicy : std::uint8_t {
    kAll = 0,
    kAny,
};

namespace detail {

inline std::string CopyStringView(std::string_view value)
{
    return std::string(value.data(), value.size());
}

}  // namespace detail

inline const char* ToString(TransportKind kind) noexcept
{
    switch (kind) {
        case TransportKind::kInMemory:
            return "in_memory";
        case TransportKind::kFastDds:
            return "fastdds";
        case TransportKind::kZmq:
            return "zmq";
        case TransportKind::kIpc:
            return "ipc";
    }
    return "unknown";
}

inline const char* ToString(EndpointKind kind) noexcept
{
    switch (kind) {
        case EndpointKind::kReader:
            return "reader";
        case EndpointKind::kWriter:
            return "writer";
    }
    return "unknown";
}

inline const char* ToString(TriggerKind kind) noexcept
{
    switch (kind) {
        case TriggerKind::kPeriodic:
            return "periodic";
        case TriggerKind::kData:
            return "data";
        case TriggerKind::kTaskDependency:
            return "task_dependency";
        case TriggerKind::kManual:
            return "manual";
    }
    return "unknown";
}

inline Status ValidateName(std::string_view kind, std::string_view value)
{
    if (value.empty()) {
        return Status::InvalidArgument(detail::CopyStringView(kind) + " must not be empty");
    }

    if (value.size() > kMaxNameLength) {
        return Status::InvalidArgument(detail::CopyStringView(kind) + " exceeds max length");
    }

    for (char ch : value) {
        const auto byte = static_cast<unsigned char>(ch);
        if (byte <= 0x20 || byte == 0x7f) {
            return Status::InvalidArgument(
                detail::CopyStringView(kind) + " contains whitespace or control characters");
        }
    }

    return Status::Ok();
}

namespace detail {

template <typename Tag>
class NamedValue {
public:
    static Result<NamedValue> Create(std::string value)
    {
        auto status = ValidateName(Tag::label, std::string_view(value.data(), value.size()));
        if (!status.ok()) {
            return Result<NamedValue>::FromStatus(std::move(status));
        }
        return NamedValue(std::move(value));
    }

    // Unsafe construction is reserved for trusted internal paths such as
    // generated metadata. User-facing APIs should prefer Create().
    static NamedValue Unsafe(std::string value)
    {
        return NamedValue(std::move(value));
    }

    const std::string& str() const noexcept
    {
        return value_;
    }

    std::string_view view() const noexcept
    {
        return std::string_view(value_.data(), value_.size());
    }

    bool empty() const noexcept
    {
        return value_.empty();
    }

    friend bool operator==(const NamedValue& lhs, const NamedValue& rhs)
    {
        return lhs.value_ == rhs.value_;
    }

    friend bool operator!=(const NamedValue& lhs, const NamedValue& rhs)
    {
        return !(lhs == rhs);
    }

    friend bool operator<(const NamedValue& lhs, const NamedValue& rhs)
    {
        return lhs.value_ < rhs.value_;
    }

private:
    explicit NamedValue(std::string value)
        : value_(std::move(value))
    {
    }

    std::string value_;
};

struct ComponentNameTag {
    static constexpr std::string_view label {"component name"};
};

struct TaskNameTag {
    static constexpr std::string_view label {"task name"};
};

struct TopicNameTag {
    static constexpr std::string_view label {"topic name"};
};

struct TransportNameTag {
    static constexpr std::string_view label {"transport name"};
};

}  // namespace detail

using ComponentName = detail::NamedValue<detail::ComponentNameTag>;
using TaskName = detail::NamedValue<detail::TaskNameTag>;
using TopicName = detail::NamedValue<detail::TopicNameTag>;
using TransportName = detail::NamedValue<detail::TransportNameTag>;

struct TopicSpec {
    TopicName name;
    TransportKind transport {TransportKind::kInMemory};
    MessagePolicy message_policy;

    Status Validate() const
    {
        auto status = ValidateName("topic name", name.view());
        if (!status.ok()) {
            return status;
        }
        return message_policy.Validate();
    }
};

struct EndpointSpec {
    EndpointKind kind {EndpointKind::kReader};
    TopicName topic;

    Status Validate() const
    {
        return ValidateName("topic name", topic.view());
    }
};

struct TriggerSpec {
    TriggerKind kind {TriggerKind::kManual};
    Nanoseconds period {0};
    DependencyPolicy dependency_policy {DependencyPolicy::kAll};
    std::vector<TopicName> data_dependencies;
    std::vector<TaskName> task_dependencies;

    Status Validate() const
    {
        if (kind == TriggerKind::kPeriodic && period <= Nanoseconds::zero()) {
            return Status::InvalidArgument("periodic trigger period must be greater than zero");
        }
        return Status::Ok();
    }
};

}  // namespace puppet_master::core
