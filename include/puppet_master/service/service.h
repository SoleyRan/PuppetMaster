#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>

#include <puppet_master/core/result.h>
#include <puppet_master/core/status.h>
#include <puppet_master/core/types.h>
#include <puppet_master/export.h>
#include <puppet_master/transport/message.h>
#include <puppet_master/transport/transport.h>

namespace puppet_master::runtime {
class RuntimeContext;
}

// Request/reply services over pub/sub topics. A service `/a/b` uses the
// topics `/a/b/request`, `/a/b/reply`, and `/a/b/presence`, so it works on
// every transport. See docs/services.md.
namespace puppet_master::service {

struct ServiceSpec {
    explicit ServiceSpec(core::TopicName name_in) : name(std::move(name_in)) {}

    core::TopicName name;
    core::TransportKind transport {core::TransportKind::kInMemory};
    transport::MessageDescriptor request;
    transport::MessageDescriptor reply;
    // Requests the server may hold before the oldest is dropped. Every client
    // keeps the same number of unread replies.
    std::size_t queue_depth {16};
    // How often the server announces itself. Clients treat it as gone after
    // three missed intervals.
    core::Nanoseconds heartbeat_interval {std::chrono::milliseconds(100)};

    PUPPET_MASTER_API core::Status Validate() const;
};

// Endpoints of the three topics, for transports and configuration tools
// that need to know them, such as a deployment file or ValidateSystem.
struct ServiceEndpoints {
    transport::EndpointConfig request;
    transport::EndpointConfig reply;
    transport::EndpointConfig presence;
};

PUPPET_MASTER_API core::Result<ServiceEndpoints> EndpointsFor(const ServiceSpec& spec);

struct Request {
    transport::ByteView payload;
    std::uint64_t client_id {0};
    std::uint64_t request_id {0};
    // When the client stops waiting, on this process's steady clock: the
    // time the request arrived plus the client's timeout. {} if none.
    // Handlers that can give up early should check it.
    core::TimePoint deadline {};
};

// Handles one request. An error status is returned to the client with its
// message; an exception is returned as Internal.
using Handler = std::function<core::Result<transport::ByteBuffer>(const Request&)>;

// Who runs the handler.
enum class ServeMode : std::uint8_t {
    // A thread of the server's own, as soon as a request arrives.
    kOwnThread = 0,
    // Whoever calls ServePending(), typically a component's Execute() run by
    // the scheduler on a service trigger, so requests are handled under the
    // component's priority and deadline, serially with its other work.
    kCaller,
};

struct ServerOptions {
    ServeMode mode {ServeMode::kOwnThread};
};

class PUPPET_MASTER_API Server final {
public:
    // Opens the service's topics on `runtime` and starts announcing. With
    // kOwnThread it also serves on a dedicated thread, in arrival order. The
    // handler is never called after Close() returns.
    static core::Result<std::unique_ptr<Server>> Create(
        runtime::RuntimeContext& runtime, ServiceSpec spec, Handler handler, ServerOptions options = {});

    // kCaller only: takes queued requests in arrival order on the calling
    // thread, at most `limit` (0 means all queued), and returns how many it
    // handled. Requests whose client deadline already passed are taken but
    // dropped unhandled (see expired_count()), so they do not count. Requests
    // that arrive meanwhile are left for the next call, so a burst cannot
    // keep one call running forever. FailedPrecondition in kOwnThread mode
    // or after Close().
    core::Result<std::size_t> ServePending(std::size_t limit = 0);

    // Requests waiting to be handled, where the transport can tell.
    core::Result<std::size_t> PendingCount() const;

    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // Random per server instance; changes when the server restarts.
    std::uint64_t instance_id() const noexcept;
    const ServiceSpec& spec() const noexcept;

    // Requests handled, and requests dropped because their deadline passed.
    std::uint64_t handled_count() const noexcept;
    std::uint64_t expired_count() const noexcept;

    // Stops serving and announcing. Idempotent; the destructor calls it.
    void Close() noexcept;

private:
    struct Impl;
    explicit Server(std::shared_ptr<Impl> impl) noexcept;

    std::shared_ptr<Impl> impl_;
};

using ReplyCallback = std::function<void(core::Result<transport::ByteBuffer>)>;

struct ServerInfo {
    std::uint64_t instance_id {0};
    // Age of the newest heartbeat when it was read.
    core::Nanoseconds age {0};
};

class PUPPET_MASTER_API Client final {
public:
    static core::Result<std::unique_ptr<Client>> Create(runtime::RuntimeContext& runtime, ServiceSpec spec);

    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    // Sends `request` and waits up to `timeout` for its reply. Returns the
    // reply payload, or:
    //   Unavailable       no live server, or its heartbeat stopped while waiting
    //   DeadlineExceeded  the server is alive but did not reply in time
    //   Cancelled         the client was closed during the call
    //   the handler's own error, with its message
    // Safe to call from several threads at once. Blocks the calling thread:
    // from a component's Execute(), prefer CallAsync, so a scheduler worker
    // is not held while the server, perhaps on the same scheduler, works.
    core::Result<transport::ByteBuffer> Call(transport::ByteView request, core::Nanoseconds timeout);

    // Sends `request` without waiting for the reply. `done` is called
    // exactly once with what Call would have returned:
    //   - on the client's dispatcher thread, for a reply, a timeout, or a
    //     server that stopped during the call;
    //   - on the calling thread, before CallAsync returns, if the call fails
    //     before it is sent (no live server, client closed), so do not hold a
    //     lock `done` takes while calling CallAsync;
    //   - on the thread calling Close(), with Cancelled, for calls still
    //     pending then.
    // Like Call, it may first wait up to one liveness window for a server it
    // has not seen yet, unless called from `done` itself. `done` should not
    // block: it delays every other reply of this client. Hand the result on,
    // for example to the component that consumes it (Scheduler::Trigger).
    // Returns InvalidArgument for a bad timeout or empty callback, without
    // calling `done`; otherwise Ok.
    core::Status CallAsync(transport::ByteView request, core::Nanoseconds timeout, ReplyCallback done);

    // Calls sent and not yet completed.
    std::size_t PendingCalls() const;

    // Waits until a live server is seen, up to `timeout`.
    core::Result<ServerInfo> WaitForService(core::Nanoseconds timeout);

    // The live server, or Unavailable.
    core::Result<ServerInfo> Server() const;

    std::uint64_t client_id() const noexcept;
    const ServiceSpec& spec() const noexcept;

    // Wakes pending calls with Cancelled. Idempotent; the destructor calls it.
    void Close() noexcept;

private:
    struct Impl;
    explicit Client(std::shared_ptr<Impl> impl) noexcept;

    std::shared_ptr<Impl> impl_;
};

}  // namespace puppet_master::service
