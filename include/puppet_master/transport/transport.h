#pragma once

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
