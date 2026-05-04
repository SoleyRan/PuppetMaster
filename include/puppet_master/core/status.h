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
    kCancelled,
    kUnknown,
    kInvalidArgument,
    kNotFound,
    kAlreadyExists,
    kFailedPrecondition,
    kOutOfRange,
    kPermissionDenied,
    kResourceExhausted,
    kUnsupported,
    kUnavailable,
    kDeadlineExceeded,
    kDataLoss,
    kInternal,
};

inline const char* StatusCodeName(StatusCode code) noexcept
{
    switch (code) {
        case StatusCode::kOk:
            return "ok";
        case StatusCode::kCancelled:
            return "cancelled";
        case StatusCode::kUnknown:
            return "unknown";
        case StatusCode::kInvalidArgument:
            return "invalid_argument";
        case StatusCode::kNotFound:
            return "not_found";
        case StatusCode::kAlreadyExists:
            return "already_exists";
        case StatusCode::kFailedPrecondition:
            return "failed_precondition";
        case StatusCode::kOutOfRange:
            return "out_of_range";
        case StatusCode::kPermissionDenied:
            return "permission_denied";
        case StatusCode::kResourceExhausted:
            return "resource_exhausted";
        case StatusCode::kUnsupported:
            return "unsupported";
        case StatusCode::kUnavailable:
            return "unavailable";
        case StatusCode::kDeadlineExceeded:
            return "deadline_exceeded";
        case StatusCode::kDataLoss:
            return "data_loss";
        case StatusCode::kInternal:
            return "internal";
    }

    return "unrecognized";
}

class Status {
public:
    Status() = default;

    static Status Ok()
    {
        return Status();
    }

    static Status Error(StatusCode code, std::string message)
    {
        if (code == StatusCode::kOk) {
            return Status();
        }
        return Status(code, std::move(message));
    }

    static Status Cancelled(std::string message)
    {
        return Error(StatusCode::kCancelled, std::move(message));
    }

    static Status Unknown(std::string message)
    {
        return Error(StatusCode::kUnknown, std::move(message));
    }

    static Status InvalidArgument(std::string message)
    {
        return Error(StatusCode::kInvalidArgument, std::move(message));
    }

    static Status NotFound(std::string message)
    {
        return Error(StatusCode::kNotFound, std::move(message));
    }

    static Status AlreadyExists(std::string message)
    {
        return Error(StatusCode::kAlreadyExists, std::move(message));
    }

    static Status FailedPrecondition(std::string message)
    {
        return Error(StatusCode::kFailedPrecondition, std::move(message));
    }

    static Status OutOfRange(std::string message)
    {
        return Error(StatusCode::kOutOfRange, std::move(message));
    }

    static Status PermissionDenied(std::string message)
    {
        return Error(StatusCode::kPermissionDenied, std::move(message));
    }

    static Status ResourceExhausted(std::string message)
    {
        return Error(StatusCode::kResourceExhausted, std::move(message));
    }

    static Status Unsupported(std::string message)
    {
        return Error(StatusCode::kUnsupported, std::move(message));
    }

    static Status Unavailable(std::string message)
    {
        return Error(StatusCode::kUnavailable, std::move(message));
    }

    static Status DeadlineExceeded(std::string message)
    {
        return Error(StatusCode::kDeadlineExceeded, std::move(message));
    }

    static Status DataLoss(std::string message)
    {
        return Error(StatusCode::kDataLoss, std::move(message));
    }

    static Status Internal(std::string message)
    {
        return Error(StatusCode::kInternal, std::move(message));
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

    std::string ToString() const
    {
        if (message_.empty()) {
            return StatusCodeName(code_);
        }

        return std::string(StatusCodeName(code_)) + ": " + message_;
    }

    explicit operator bool() const noexcept
    {
        return ok();
    }

    friend bool operator==(const Status& lhs, const Status& rhs)
    {
        return lhs.code_ == rhs.code_ && lhs.message_ == rhs.message_;
    }

    friend bool operator!=(const Status& lhs, const Status& rhs)
    {
        return !(lhs == rhs);
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
