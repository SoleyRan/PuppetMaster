#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <utility>

#include <puppet_master/core/result.h>
#include <puppet_master/core/status.h>
#include <puppet_master/core/types.h>
#include <puppet_master/transport/message.h>

namespace puppet_master::transport {

struct EndpointConfig {
    core::TopicSpec topic;
    MessageDescriptor message;

    core::Status Validate() const
    {
        auto status = topic.Validate();
        if (!status.ok()) {
            return status;
        }
        return message.Validate();
    }
};

struct ReadOptions {
    bool wait {false};
    core::Nanoseconds timeout {0};
};

struct WriteOptions {
    core::TimePoint source_timestamp {};
};

using DataAvailableCallback = std::function<void()>;

// Fills a message in place. Receives a writable buffer and returns how many
// bytes of it the message uses; returning an error publishes nothing.
using FillFunction = std::function<core::Result<std::size_t>(MutableByteView buffer)>;

namespace detail {

// Runs `fill` and validates the size it reports. Exceptions become Internal
// errors so callers can always release resources they hold around the call.
inline core::Result<std::size_t> InvokeFill(const FillFunction& fill, MutableByteView buffer)
{
    using ResultType = core::Result<std::size_t>;
    try {
        auto used = fill(buffer);
        if (used.ok() && used.value() > buffer.size()) {
            return ResultType::FromStatus(core::Status::OutOfRange(
                "fill function reported " + std::to_string(used.value()) + " bytes for a "
                + std::to_string(buffer.size()) + "-byte buffer"));
        }
        return used;
    } catch (...) {
        return ResultType::FromStatus(core::Status::Internal("fill function threw an exception"));
    }
}

}  // namespace detail

class Reader {
public:
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;
    Reader(Reader&&) = delete;
    Reader& operator=(Reader&&) = delete;
    virtual ~Reader() = default;

    virtual const core::TopicName& topic_name() const noexcept = 0;
    virtual const MessageDescriptor& message_descriptor() const noexcept = 0;
    virtual core::Result<Message> Read(ReadOptions options = {}) = 0;
    virtual core::Status SetDataAvailableCallback(DataAvailableCallback callback) = 0;
    virtual core::Result<std::size_t> PendingMessageCount() const
    {
        return core::Result<std::size_t>::FromStatus(
            core::Status::Unsupported("reader does not expose pending message count"));
    }

protected:
    Reader() = default;
};

class Writer {
public:
    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;
    Writer(Writer&&) = delete;
    Writer& operator=(Writer&&) = delete;
    virtual ~Writer() = default;

    virtual const core::TopicName& topic_name() const noexcept = 0;
    virtual const MessageDescriptor& message_descriptor() const noexcept = 0;
    virtual core::Status Write(ByteView payload, WriteOptions options = {}) = 0;

    // Publishes a message that `fill` writes in place, so producers can
    // serialize without building their own buffer first. `fill` receives
    // exactly `capacity` writable bytes and returns how many it used. It must
    // not write to the same writer.
    //
    // Backends that report TransportCapabilities::supports_zero_copy hand
    // `fill` transport memory directly. The default fills a temporary buffer
    // and passes it to Write().
    virtual core::Status WriteWith(std::size_t capacity, const FillFunction& fill, WriteOptions options = {})
    {
        if (!fill) {
            return core::Status::InvalidArgument("fill function must not be empty");
        }
        ByteBuffer buffer(capacity);
        auto used = detail::InvokeFill(fill, MutableByteView(buffer.data(), buffer.size()));
        if (!used.ok()) {
            return used.status();
        }
        return Write(ByteView(buffer.data(), used.value()), options);
    }

protected:
    Writer() = default;
};

using ReaderPtr = std::shared_ptr<Reader>;
using WriterPtr = std::shared_ptr<Writer>;

struct TransportCapabilities {
    core::TransportKind kind {core::TransportKind::kInMemory};
    bool supports_callbacks {false};
    bool supports_blocking_read {false};
    bool supports_reliable_delivery {false};
    bool supports_keep_all {false};
    bool supports_zero_copy {false};
};

class Transport {
public:
    Transport(const Transport&) = delete;
    Transport& operator=(const Transport&) = delete;
    Transport(Transport&&) = delete;
    Transport& operator=(Transport&&) = delete;
    virtual ~Transport() = default;

    virtual const core::TransportName& name() const noexcept = 0;
    virtual core::TransportKind kind() const noexcept = 0;
    virtual TransportCapabilities capabilities() const noexcept = 0;

    virtual core::Status Open() = 0;
    virtual core::Status Close() noexcept = 0;
    virtual bool is_open() const noexcept = 0;

    virtual core::Status ValidateEndpoint(const EndpointConfig& endpoint) const
    {
        return endpoint.Validate();
    }

    virtual core::Result<ReaderPtr> CreateReader(const EndpointConfig& endpoint) = 0;
    virtual core::Result<WriterPtr> CreateWriter(const EndpointConfig& endpoint) = 0;

protected:
    Transport() = default;
};

using TransportPtr = std::shared_ptr<Transport>;

}  // namespace puppet_master::transport
