#include <puppet_master/service/service.h>

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <map>
#include <mutex>
#include <random>
#include <thread>
#include <utility>

#include <puppet_master/runtime/context.h>

namespace puppet_master::service {

namespace {

using transport::ByteBuffer;
using transport::ByteView;

constexpr std::uint32_t kRequestMagic = 0x51524d50;  // "PMRQ", little-endian
constexpr std::uint32_t kReplyMagic = 0x50524d50;    // "PMRP"
constexpr std::uint16_t kHeaderVersion = 1;
constexpr std::size_t kHeaderSize = 32;
constexpr std::size_t kPresenceSize = 16;
// Heartbeat sequences start at 1; 0 announces that the server stopped.
constexpr std::uint64_t kOfflineSequence = 0;
// How often blocking loops wake to notice Close(), at most.
constexpr auto kPollInterval = std::chrono::milliseconds(20);

core::TimePoint Now()
{
    return std::chrono::time_point_cast<core::Nanoseconds>(core::SteadyClock::now());
}

// Fixed-width little-endian fields, so headers mean the same on every host.
void PutU16(core::Byte* out, std::uint16_t value)
{
    for (int i = 0; i < 2; ++i) {
        out[i] = static_cast<core::Byte>(value >> (8 * i));
    }
}

void PutU32(core::Byte* out, std::uint32_t value)
{
    for (int i = 0; i < 4; ++i) {
        out[i] = static_cast<core::Byte>(value >> (8 * i));
    }
}

void PutU64(core::Byte* out, std::uint64_t value)
{
    for (int i = 0; i < 8; ++i) {
        out[i] = static_cast<core::Byte>(value >> (8 * i));
    }
}

std::uint16_t GetU16(const core::Byte* in)
{
    return static_cast<std::uint16_t>(in[0] | (in[1] << 8));
}

std::uint32_t GetU32(const core::Byte* in)
{
    std::uint32_t value = 0;
    for (int i = 3; i >= 0; --i) {
        value = (value << 8) | in[i];
    }
    return value;
}

std::uint64_t GetU64(const core::Byte* in)
{
    std::uint64_t value = 0;
    for (int i = 7; i >= 0; --i) {
        value = (value << 8) | in[i];
    }
    return value;
}

struct Header {
    std::uint32_t magic {0};
    core::StatusCode status {core::StatusCode::kOk};
    std::uint64_t client_id {0};
    std::uint64_t request_id {0};
    // Request only: time the client will wait, from when it sent. A duration,
    // not a time point, because steady clocks of different hosts do not
    // compare. 0 means no limit.
    std::int64_t budget_ns {0};
};

ByteBuffer Encode(const Header& header, ByteView payload)
{
    ByteBuffer buffer(kHeaderSize + payload.size());
    PutU32(buffer.data(), header.magic);
    PutU16(buffer.data() + 4, kHeaderVersion);
    PutU16(buffer.data() + 6, static_cast<std::uint16_t>(header.status));
    PutU64(buffer.data() + 8, header.client_id);
    PutU64(buffer.data() + 16, header.request_id);
    PutU64(buffer.data() + 24, static_cast<std::uint64_t>(header.budget_ns));
    if (!payload.empty()) {
        std::memcpy(buffer.data() + kHeaderSize, payload.data(), payload.size());
    }
    return buffer;
}

// Returns false for a message that is not a service message of this kind
// and version, which is dropped.
bool Decode(const ByteBuffer& message, std::uint32_t magic, Header& header, ByteView& payload)
{
    if (message.size() < kHeaderSize || GetU32(message.data()) != magic
        || GetU16(message.data() + 4) != kHeaderVersion) {
        return false;
    }
    const auto status = GetU16(message.data() + 6);
    if (status > static_cast<std::uint16_t>(core::StatusCode::kInternal)) {
        return false;
    }
    header.magic = magic;
    header.status = static_cast<core::StatusCode>(status);
    header.client_id = GetU64(message.data() + 8);
    header.request_id = GetU64(message.data() + 16);
    header.budget_ns = static_cast<std::int64_t>(GetU64(message.data() + 24));
    payload = ByteView(message.data() + kHeaderSize, message.size() - kHeaderSize);
    return true;
}

// Nonzero random ids; collisions between live instances are what the id
// space makes negligible, not impossible.
std::uint64_t RandomId()
{
    static std::mutex mutex;
    static std::mt19937_64 generator([] {
        std::random_device device;
        std::seed_seq seed {device(), device(), device(), device(),
                            static_cast<unsigned>(Now().time_since_epoch().count())};
        return std::mt19937_64(seed);
    }());
    std::lock_guard<std::mutex> lock(mutex);
    std::uint64_t id = 0;
    while (id == 0) {
        id = generator();
    }
    return id;
}

transport::MessageDescriptor Prefixed(const char* prefix, const transport::MessageDescriptor& payload)
{
    return transport::MessageDescriptor {prefix + payload.type_name, payload.encoding, payload.version};
}

core::Result<core::TopicName> SubTopic(const core::TopicName& service, const char* suffix)
{
    return core::TopicName::Create(service.str() + "/" + suffix);
}

std::string Describe(const ServiceSpec& spec)
{
    return "service " + spec.name.str();
}

}  // namespace

core::Status ServiceSpec::Validate() const
{
    auto status = core::ValidateName("service name", name.view());
    if (!status.ok()) {
        return status;
    }
    status = request.Validate();
    if (!status.ok()) {
        return core::Status::InvalidArgument(Describe(*this) + " request: " + status.message());
    }
    status = reply.Validate();
    if (!status.ok()) {
        return core::Status::InvalidArgument(Describe(*this) + " reply: " + status.message());
    }
    if (queue_depth == 0) {
        return core::Status::InvalidArgument(Describe(*this) + ": queue_depth must be greater than zero");
    }
    if (heartbeat_interval <= core::Nanoseconds::zero()) {
        return core::Status::InvalidArgument(Describe(*this) + ": heartbeat_interval must be greater than zero");
    }
    return core::Status::Ok();
}

core::Result<ServiceEndpoints> EndpointsFor(const ServiceSpec& spec)
{
    using ResultType = core::Result<ServiceEndpoints>;
    auto status = spec.Validate();
    if (!status.ok()) {
        return ResultType::FromStatus(std::move(status));
    }
    auto request = SubTopic(spec.name, "request");
    auto reply = SubTopic(spec.name, "reply");
    auto presence = SubTopic(spec.name, "presence");
    for (const auto* name : {&request, &reply, &presence}) {
        if (!name->ok()) {
            return ResultType::FromStatus(name->status());
        }
    }

    // Requests and replies are sent once, so they ask for reliable delivery;
    // on FastDDS that is the difference between a lost UDP packet costing a
    // retransmission and costing the whole call.
    core::MessagePolicy queued;
    queued.delivery = core::DeliveryGuarantee::kReliable;
    queued.freshness = core::FreshnessPolicy::kQueued;
    queued.overflow = core::QueueOverflowPolicy::kDropOldest;
    queued.queue_depth = spec.queue_depth;
    // Presence: only the newest heartbeat matters, and the next one replaces a
    // lost one, so best effort is enough.
    core::MessagePolicy latest;

    return ResultType(ServiceEndpoints {
        transport::EndpointConfig {core::TopicSpec {request.value(), spec.transport, queued},
                                   Prefixed("service.request:", spec.request)},
        transport::EndpointConfig {core::TopicSpec {reply.value(), spec.transport, queued},
                                   Prefixed("service.reply:", spec.reply)},
        transport::EndpointConfig {core::TopicSpec {presence.value(), spec.transport, latest},
                                   transport::MessageDescriptor {"service.Presence", "application/x-puppetmaster-presence", 1}},
    });
}

// ---------------------------------------------------------------- Server

struct Server::Impl {
    Impl(ServiceSpec spec_in, Handler handler_in) : spec(std::move(spec_in)), handler(std::move(handler_in)) {}

    ServiceSpec spec;
    Handler handler;
    std::uint64_t instance_id {RandomId()};
    transport::ReaderPtr requests;
    transport::WriterPtr replies;
    transport::WriterPtr presence;

    std::mutex mutex;
    std::condition_variable wake;  // requests arrived, or close
    std::condition_variable beat;  // close, for the heartbeat thread
    bool pending {false};  // set by the data callback
    bool closed {false};
    std::thread worker;
    std::thread heartbeat;
    std::atomic<std::uint64_t> handled {0};
    std::atomic<std::uint64_t> expired {0};
    std::uint64_t heartbeat_sequence {0};

    // A heartbeat, or with `offline` the announcement that this server stops.
    void Announce(bool offline = false)
    {
        core::Byte message[kPresenceSize];
        PutU64(message, instance_id);
        PutU64(message + 8, offline ? kOfflineSequence : ++heartbeat_sequence);
        presence->Write(ByteView(message, sizeof(message)));
    }

    void Reply(const Header& request, core::StatusCode code, ByteView payload)
    {
        Header header;
        header.magic = kReplyMagic;
        header.status = code;
        header.client_id = request.client_id;
        header.request_id = request.request_id;
        const auto message = Encode(header, payload);
        replies->Write(ByteView(message));
    }

    void Serve(const transport::Message& message)
    {
        Header header;
        ByteView payload;
        if (!Decode(message.payload, kRequestMagic, header, payload)) {
            return;
        }
        // The deadline is measured on this host's clock from when the request
        // arrived. Transit time is not counted, so the server errs toward
        // handling a request its client may just have given up on.
        const auto received = message.metadata.reception_timestamp == core::TimePoint {}
            ? Now() : message.metadata.reception_timestamp;
        const auto deadline = header.budget_ns == 0 ? core::TimePoint {}
                                                    : received + core::Nanoseconds(header.budget_ns);
        Request request {payload, header.client_id, header.request_id, deadline};
        if (header.budget_ns != 0 && Now() > deadline) {
            ++expired;  // the client has stopped waiting
            return;
        }
        core::Result<ByteBuffer> result = core::Result<ByteBuffer>::FromStatus(core::Status::Internal(""));
        try {
            result = handler(request);
        } catch (const std::exception& error) {
            result = core::Result<ByteBuffer>::FromStatus(
                core::Status::Internal(std::string("service handler threw: ") + error.what()));
        } catch (...) {
            result = core::Result<ByteBuffer>::FromStatus(core::Status::Internal("service handler threw"));
        }
        ++handled;
        if (result.ok()) {
            Reply(header, core::StatusCode::kOk, ByteView(result.value()));
        } else {
            const auto& text = result.status().message();
            Reply(header, result.status().code(),
                  ByteView(reinterpret_cast<const core::Byte*>(text.data()), text.size()));
        }
    }

    // Serves requests in arrival order. Polls as well as waiting for the data
    // callback, for transports without callbacks.
    void Run()
    {
        while (true) {
            {
                std::unique_lock<std::mutex> lock(mutex);
                wake.wait_for(lock, kPollInterval, [this] { return pending || closed; });
                if (closed) {
                    return;
                }
                pending = false;
            }
            // Drain everything queued; the flag only says something arrived.
            for (auto message = requests->Read(); message.ok(); message = requests->Read()) {
                Serve(message.value());
                std::lock_guard<std::mutex> lock(mutex);
                if (closed) {
                    return;
                }
            }
        }
    }

    // Announces on its own thread, so a long handler does not look like a
    // dead server: presence means the process is alive, not idle.
    void Beat()
    {
        std::unique_lock<std::mutex> lock(mutex);
        while (!beat.wait_for(lock, spec.heartbeat_interval, [this] { return closed; })) {
            lock.unlock();
            Announce();
            lock.lock();
        }
    }
};

core::Result<std::unique_ptr<Server>> Server::Create(
    runtime::RuntimeContext& runtime, ServiceSpec spec, Handler handler)
{
    using ResultType = core::Result<std::unique_ptr<Server>>;
    if (!handler) {
        return ResultType::FromStatus(core::Status::InvalidArgument("service handler must not be empty"));
    }
    auto endpoints = EndpointsFor(spec);
    if (!endpoints.ok()) {
        return ResultType::FromStatus(endpoints.status());
    }
    auto impl = std::make_shared<Impl>(std::move(spec), std::move(handler));

    auto requests = runtime.CreateReader(endpoints.value().request);
    auto replies = runtime.CreateWriter(endpoints.value().reply);
    auto presence = runtime.CreateWriter(endpoints.value().presence);
    for (const core::Status* status : {&requests.status(), &replies.status(), &presence.status()}) {
        if (!status->ok()) {
            return ResultType::FromStatus(core::Status::Error(
                status->code(), Describe(impl->spec) + ": " + status->message()));
        }
    }
    impl->requests = std::move(requests).value();
    impl->replies = std::move(replies).value();
    impl->presence = std::move(presence).value();

    std::weak_ptr<Impl> weak = impl;
    auto status = impl->requests->SetDataAvailableCallback([weak] {
        if (auto self = weak.lock()) {
            {
                std::lock_guard<std::mutex> lock(self->mutex);
                self->pending = true;
            }
            self->wake.notify_one();
        }
    });
    if (!status.ok() && status.code() != core::StatusCode::kUnsupported) {
        return ResultType::FromStatus(status);
    }
    // The first heartbeat goes out before Create returns, so clients that
    // already exist see the server at once.
    impl->Announce();
    impl->heartbeat = std::thread([impl = impl.get()] { impl->Beat(); });
    impl->worker = std::thread([impl = impl.get()] { impl->Run(); });
    return ResultType(std::unique_ptr<Server>(new Server(std::move(impl))));
}

Server::Server(std::shared_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

Server::~Server()
{
    Close();
}

std::uint64_t Server::instance_id() const noexcept
{
    return impl_->instance_id;
}

const ServiceSpec& Server::spec() const noexcept
{
    return impl_->spec;
}

std::uint64_t Server::handled_count() const noexcept
{
    return impl_->handled.load();
}

std::uint64_t Server::expired_count() const noexcept
{
    return impl_->expired.load();
}

void Server::Close() noexcept
{
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->closed) {
            return;
        }
        impl_->closed = true;
    }
    // Heartbeats stop first, so clients learn the server is gone even while
    // the worker finishes a long handler.
    impl_->beat.notify_all();
    if (impl_->heartbeat.joinable()) {
        impl_->heartbeat.join();
    }
    // An orderly stop says so, so clients need not wait out the liveness
    // window; only a crashed server is noticed by silence.
    impl_->Announce(true);
    impl_->wake.notify_all();
    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }
    impl_->requests->SetDataAvailableCallback({});
}

// ---------------------------------------------------------------- Client

struct Client::Impl {
    explicit Impl(ServiceSpec spec_in) : spec(std::move(spec_in)) {}

    ServiceSpec spec;
    std::uint64_t client_id {RandomId()};
    transport::WriterPtr requests;
    transport::ReaderPtr replies;
    transport::ReaderPtr presence;

    struct Pending {
        bool done {false};
        core::StatusCode code {core::StatusCode::kOk};
        ByteBuffer payload;
    };

    mutable std::mutex mutex;
    std::condition_variable replied;  // a pending call completed, or close
    std::condition_variable arrived;  // the dispatcher has data to read
    bool data {false};
    bool closed {false};
    std::uint64_t next_request {0};
    std::map<std::uint64_t, Pending*> pending;
    std::thread dispatcher;

    // Newest heartbeat seen. Guarded by mutex. heartbeat_at is when this
    // process received a new one, on its own clock: source timestamps are
    // not carried by every transport and do not compare across hosts.
    std::uint64_t server_instance {0};
    // Set by an orderly goodbye; cleared by the next heartbeat. A call then
    // fails at once instead of waiting for a server to appear.
    bool server_withdrew {false};
    std::uint64_t heartbeat_sequence {0};
    core::TimePoint heartbeat_at {};

    core::Nanoseconds Liveness() const { return 3 * spec.heartbeat_interval; }

    // Caller holds mutex.
    void PollPresenceLocked()
    {
        for (auto message = presence->Read(); message.ok(); message = presence->Read()) {
            if (message.value().payload.size() != kPresenceSize) {
                continue;
            }
            const auto instance = GetU64(message.value().payload.data());
            const auto sequence = GetU64(message.value().payload.data() + 8);
            // A repeated (instance, sequence) is the same heartbeat read again,
            // not a sign of life.
            if (instance == server_instance && sequence == heartbeat_sequence) {
                continue;
            }
            if (sequence == kOfflineSequence) {
                // Only the current server can withdraw itself; a stale
                // goodbye from an older instance is ignored.
                if (instance == server_instance) {
                    server_instance = 0;
                    heartbeat_sequence = 0;
                    heartbeat_at = {};
                    server_withdrew = true;
                    replied.notify_all();  // wake calls waiting on this server
                }
                continue;
            }
            server_instance = instance;
            heartbeat_sequence = sequence;
            heartbeat_at = Now();
            server_withdrew = false;
        }
    }

    // Caller holds mutex.
    core::Result<ServerInfo> LiveServerLocked()
    {
        PollPresenceLocked();
        const auto age = Now() - heartbeat_at;
        if (server_instance == 0 || age > Liveness()) {
            std::string reason;
            if (server_withdrew) {
                reason = " (the server stopped)";
            } else if (server_instance != 0) {
                reason = " (last heartbeat " + std::to_string(age.count() / 1000000) + " ms ago)";
            }
            return core::Result<ServerInfo>::FromStatus(
                core::Status::Unavailable(Describe(spec) + " has no live server" + reason));
        }
        return core::Result<ServerInfo>(ServerInfo {server_instance, age});
    }

    // Routes replies to their calls. Replies for other clients or for calls
    // that already gave up are dropped.
    void Dispatch()
    {
        std::unique_lock<std::mutex> lock(mutex);
        while (!closed) {
            arrived.wait_for(lock, kPollInterval, [this] { return data || closed; });
            data = false;
            // Keep heartbeat receive times current between calls.
            PollPresenceLocked();
            lock.unlock();
            for (auto message = replies->Read(); message.ok(); message = replies->Read()) {
                Header header;
                ByteView payload;
                if (!Decode(message.value().payload, kReplyMagic, header, payload) || header.client_id != client_id) {
                    continue;
                }
                std::lock_guard<std::mutex> guard(mutex);
                const auto found = pending.find(header.request_id);
                if (found == pending.end() || found->second->done) {
                    continue;
                }
                found->second->done = true;
                found->second->code = header.status;
                found->second->payload.assign(payload.begin(), payload.end());
                replied.notify_all();
            }
            lock.lock();
        }
    }
};

core::Result<std::unique_ptr<Client>> Client::Create(runtime::RuntimeContext& runtime, ServiceSpec spec)
{
    using ResultType = core::Result<std::unique_ptr<Client>>;
    auto endpoints = EndpointsFor(spec);
    if (!endpoints.ok()) {
        return ResultType::FromStatus(endpoints.status());
    }
    auto impl = std::make_shared<Impl>(std::move(spec));

    // Reply and presence readers first, so nothing sent after a request is
    // written can be missed by a volatile reader.
    auto replies = runtime.CreateReader(endpoints.value().reply);
    auto presence = runtime.CreateReader(endpoints.value().presence);
    auto requests = runtime.CreateWriter(endpoints.value().request);
    for (const core::Status* status : {&replies.status(), &presence.status(), &requests.status()}) {
        if (!status->ok()) {
            return ResultType::FromStatus(core::Status::Error(
                status->code(), Describe(impl->spec) + ": " + status->message()));
        }
    }
    impl->replies = std::move(replies).value();
    impl->presence = std::move(presence).value();
    impl->requests = std::move(requests).value();

    std::weak_ptr<Impl> weak = impl;
    auto status = impl->replies->SetDataAvailableCallback([weak] {
        if (auto self = weak.lock()) {
            {
                std::lock_guard<std::mutex> lock(self->mutex);
                self->data = true;
            }
            self->arrived.notify_one();
        }
    });
    if (!status.ok() && status.code() != core::StatusCode::kUnsupported) {
        return ResultType::FromStatus(status);
    }
    impl->dispatcher = std::thread([impl = impl.get()] { impl->Dispatch(); });
    return ResultType(std::unique_ptr<Client>(new Client(std::move(impl))));
}

Client::Client(std::shared_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

Client::~Client()
{
    Close();
}

core::Result<transport::ByteBuffer> Client::Call(transport::ByteView request, core::Nanoseconds timeout)
{
    using ResultType = core::Result<ByteBuffer>;
    auto& impl = *impl_;
    if (timeout <= core::Nanoseconds::zero()) {
        return ResultType::FromStatus(core::Status::InvalidArgument("service call timeout must be positive"));
    }
    const auto deadline = Now() + timeout;

    Impl::Pending call;
    std::uint64_t request_id = 0;
    {
        // A client that has not seen a heartbeat yet, such as one created a
        // moment ago, waits up to one liveness window for it before deciding
        // there is no server. A server that said goodbye fails the call at
        // once.
        std::unique_lock<std::mutex> lock(impl.mutex);
        const auto give_up = std::min(deadline, Now() + impl.Liveness());
        while (true) {
            if (impl.closed) {
                return ResultType::FromStatus(core::Status::Cancelled(Describe(impl.spec) + ": client is closed"));
            }
            auto server = impl.LiveServerLocked();  // polls presence first
            if (server.ok()) {
                break;
            }
            const auto now = Now();
            if (impl.server_withdrew || now >= give_up) {
                return ResultType::FromStatus(server.status());
            }
            impl.replied.wait_for(lock, std::min<core::Nanoseconds>(give_up - now, kPollInterval));
        }
        // Seeing the heartbeat proves the server's presence writer reached
        // us, not that our request writer has reached the server's reader.
        // On backends that discover peers asynchronously, wait for that too,
        // or the request is dropped and the call looks like a slow server.
        while (true) {
            const auto matched = impl.requests->MatchedReaderCount();
            if (!matched.ok() || matched.value() > 0) {
                break;  // Unsupported: delivery does not depend on matching
            }
            if (impl.closed) {
                return ResultType::FromStatus(core::Status::Cancelled(Describe(impl.spec) + ": client is closed"));
            }
            const auto now = Now();
            if (now >= deadline) {
                return ResultType::FromStatus(core::Status::Unavailable(
                    Describe(impl.spec) + ": the server is alive but its request reader was not matched in time"));
            }
            impl.replied.wait_for(lock, std::min<core::Nanoseconds>(deadline - now, std::chrono::milliseconds(5)));
        }
        request_id = ++impl.next_request;
        impl.pending.emplace(request_id, &call);
    }
    const auto forget = [&impl, request_id] {
        std::lock_guard<std::mutex> lock(impl.mutex);
        impl.pending.erase(request_id);
    };

    Header header;
    header.magic = kRequestMagic;
    header.client_id = impl.client_id;
    header.request_id = request_id;
    header.budget_ns = timeout.count();
    const auto message = Encode(header, request);
    auto status = impl.requests->Write(ByteView(message));
    if (!status.ok()) {
        forget();
        return ResultType::FromStatus(core::Status::Error(
            status.code(), Describe(impl.spec) + ": sending request failed: " + status.message()));
    }

    std::unique_lock<std::mutex> lock(impl.mutex);
    while (true) {
        const auto now = Now();
        if (call.done || impl.closed || now >= deadline) {
            break;
        }
        // Wake at least once per heartbeat window to notice a dead server.
        const auto step = std::min<core::Nanoseconds>(deadline - now, impl.spec.heartbeat_interval);
        impl.replied.wait_for(lock, step, [&] { return call.done || impl.closed; });
        if (!call.done && !impl.closed && !impl.LiveServerLocked().ok()) {
            impl.pending.erase(request_id);
            return ResultType::FromStatus(core::Status::Unavailable(
                Describe(impl.spec) + ": server stopped responding during the call"));
        }
    }
    impl.pending.erase(request_id);
    if (call.done) {
        if (call.code == core::StatusCode::kOk) {
            return ResultType(std::move(call.payload));
        }
        return ResultType::FromStatus(core::Status::Error(
            call.code, std::string(call.payload.begin(), call.payload.end())));
    }
    if (impl.closed) {
        return ResultType::FromStatus(core::Status::Cancelled(Describe(impl.spec) + ": client closed during the call"));
    }
    return ResultType::FromStatus(core::Status::DeadlineExceeded(
        Describe(impl.spec) + ": no reply within " + std::to_string(timeout.count() / 1000000) + " ms"));
}

core::Result<ServerInfo> Client::WaitForService(core::Nanoseconds timeout)
{
    auto& impl = *impl_;
    const auto deadline = Now() + timeout;
    std::unique_lock<std::mutex> lock(impl.mutex);
    while (true) {
        if (impl.closed) {
            return core::Result<ServerInfo>::FromStatus(
                core::Status::Cancelled(Describe(impl.spec) + ": client is closed"));
        }
        auto server = impl.LiveServerLocked();
        if (server.ok() || Now() >= deadline) {
            return server;
        }
        impl.replied.wait_for(lock, kPollInterval);
    }
}

core::Result<ServerInfo> Client::Server() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->LiveServerLocked();
}

std::uint64_t Client::client_id() const noexcept
{
    return impl_->client_id;
}

const ServiceSpec& Client::spec() const noexcept
{
    return impl_->spec;
}

void Client::Close() noexcept
{
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->closed) {
            return;
        }
        impl_->closed = true;
    }
    impl_->replied.notify_all();
    impl_->arrived.notify_all();
    if (impl_->dispatcher.joinable()) {
        impl_->dispatcher.join();
    }
    impl_->replies->SetDataAvailableCallback({});
}

}  // namespace puppet_master::service
