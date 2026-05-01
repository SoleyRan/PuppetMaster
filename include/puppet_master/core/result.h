#pragma once

#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include <puppet_master/core/status.h>

namespace puppet_master::core {

// Result<T> carries either a value or a Status. It keeps public APIs explicit
// about recoverable failures without forcing transport adapters or runtime code
// to throw exceptions across module boundaries.
template <typename T>
class Result {
    static_assert(!std::is_void<T>::value,
                  "Use the Result<void> specialization for void results.");

public:
    Result(const T& value)
        : value_(value)
    {
    }

    Result(T&& value)
        : value_(std::move(value))
    {
    }

    explicit Result(Status status)
        : status_(NormalizeError(std::move(status)))
    {
    }

    static Result FromStatus(Status status)
    {
        return Result(std::move(status));
    }

    bool ok() const noexcept
    {
        return value_.has_value();
    }

    const Status& status() const noexcept
    {
        return status_;
    }

    T& value() &
    {
        EnsureValue();
        return *value_;
    }

    const T& value() const&
    {
        EnsureValue();
        return *value_;
    }

    T&& value() &&
    {
        EnsureValue();
        return std::move(*value_);
    }

    explicit operator bool() const noexcept
    {
        return ok();
    }

private:
    static Status NormalizeError(Status status)
    {
        if (status.ok()) {
            return Status::Internal("Result<T> cannot store an OK status without a value");
        }
        return status;
    }

    void EnsureValue() const
    {
        if (!value_) {
            throw std::logic_error(status_.ToString());
        }
    }

    std::optional<T> value_;
    Status status_;
};

template <>
class Result<void> {
public:
    Result() = default;

    explicit Result(Status status)
        : status_(NormalizeStatus(std::move(status)))
    {
    }

    static Result Ok()
    {
        return Result();
    }

    static Result FromStatus(Status status)
    {
        return Result(std::move(status));
    }

    bool ok() const noexcept
    {
        return status_.ok();
    }

    const Status& status() const noexcept
    {
        return status_;
    }

    explicit operator bool() const noexcept
    {
        return ok();
    }

private:
    static Status NormalizeStatus(Status status)
    {
        return status;
    }

    Status status_;
};

}  // namespace puppet_master::core
