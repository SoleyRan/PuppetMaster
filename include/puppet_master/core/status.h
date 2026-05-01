#pragma once

#include <cstdint>
#include <string>
#include <utility>

namespace puppet_master::core {

// StatusCode is intentionally transport-agnostic. Adapters can map native
// errors into this small vocabulary without exposing DDS, ZMQ, or IPC types to
// the runtime and component layers.
enum class StatusCode : std::uint8_t {
    kOk = 0,
    kInvalidArgument,
    kNotFound,
    kAlreadyExists,
    kFailedPrecondition,
    kUnavailable,
    kDeadlineExceeded,
    kInternal,
};

class Status {
public:
    Status() = default;

    static Status Ok()
    {
        return Status();
    }

    static Status Error(StatusCode code, std::string message)
    {
        return Status(code, std::move(message));
    }

    static Status InvalidArgument(std::string message)
    {
        return Error(StatusCode::kInvalidArgument, std::move(message));
    }

    bool ok() const noexcept
    {
        return code_ == StatusCode::kOk;
    }

    StatusCode code() const noexcept
    {
        return code_;
    }

    const std::string& message() const noexcept
    {
        return message_;
    }

    explicit operator bool() const noexcept
    {
        return ok();
    }

private:
    Status(StatusCode code, std::string message)
        : code_(code), message_(std::move(message))
    {
    }

    StatusCode code_ {StatusCode::kOk};
    std::string message_;
};

}  // namespace puppet_master::core
