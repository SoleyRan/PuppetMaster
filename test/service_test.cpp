#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <puppet_master/runtime/context.h>
#include <puppet_master/service/service.h>
#include <puppet_master/service/typed_service.h>

namespace core = puppet_master::core;
namespace runtime = puppet_master::runtime;
namespace service = puppet_master::service;
namespace transport = puppet_master::transport;

using namespace std::chrono_literals;

struct AddRequest {
    std::int64_t a;
    std::int64_t b;
};

struct AddReply {
    std::int64_t sum;
};

template <>
struct puppet_master::transport::MessageTypeName<AddRequest> {
    static constexpr const char* value = "test.AddRequest";
    static constexpr std::uint32_t version = 1;
};

template <>
struct puppet_master::transport::MessageTypeName<AddReply> {
    static constexpr const char* value = "test.AddReply";
};

struct AddRequestV2 {
    std::int64_t a;
    std::int64_t b;
    std::int64_t c;
};

template <>
struct puppet_master::transport::MessageTypeName<AddRequestV2> {
    static constexpr const char* value = "test.AddRequest";
    static constexpr std::uint32_t version = 2;
};

namespace {

std::shared_ptr<runtime::RuntimeContext> MakeRuntime()
{
    auto context = runtime::RuntimeContext::Create();
    assert(context.ok());
    return context.value();
}

service::ServiceSpec Spec(const std::string& name)
{
    service::ServiceSpec spec(core::TopicName::Unsafe(name));
    spec.request = transport::MessageDescriptor {"test.Text", "text/plain"};
    spec.reply = transport::MessageDescriptor {"test.Text", "text/plain"};
    spec.heartbeat_interval = 20ms;
    return spec;
}

transport::ByteView Bytes(const std::string& text)
{
    return transport::ByteView::From(text.data(), text.size());
}

std::string Text(const transport::ByteBuffer& buffer)
{
    return std::string(buffer.begin(), buffer.end());
}

// Echoes the request reversed; "fail" and "throw" exercise errors, "slow:N"
// sleeps N ms first.
core::Result<transport::ByteBuffer> Reverse(const service::Request& request)
{
    std::string text(request.payload.begin(), request.payload.end());
    if (text == "fail") {
        return core::Result<transport::ByteBuffer>::FromStatus(core::Status::NotFound("no such entry"));
    }
    if (text == "throw") {
        throw std::runtime_error("handler exploded");
    }
    if (text.rfind("slow:", 0) == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(std::stoi(text.substr(5))));
    }
    return core::Result<transport::ByteBuffer>(transport::ByteBuffer(text.rbegin(), text.rend()));
}

void SpecValidation()
{
    auto spec = Spec("/svc/valid");
    assert(spec.Validate().ok());
    auto endpoints = service::EndpointsFor(spec);
    assert(endpoints.ok());
    assert(endpoints.value().request.topic.name.str() == "/svc/valid/request");
    assert(endpoints.value().reply.topic.name.str() == "/svc/valid/reply");
    assert(endpoints.value().presence.topic.name.str() == "/svc/valid/presence");
    assert(endpoints.value().request.message.type_name == "service.request:test.Text");
    assert(endpoints.value().reply.message.type_name == "service.reply:test.Text");
    assert(endpoints.value().request.topic.message_policy.queue_depth == 16);

    auto bad = spec;
    bad.queue_depth = 0;
    assert(bad.Validate().code() == core::StatusCode::kInvalidArgument);
    bad = spec;
    bad.heartbeat_interval = 0ms;
    assert(bad.Validate().code() == core::StatusCode::kInvalidArgument);
    bad = spec;
    bad.request.type_name.clear();
    assert(bad.Validate().code() == core::StatusCode::kInvalidArgument);

    auto runtime = MakeRuntime();
    assert(service::Server::Create(*runtime, spec, {}).status().code() == core::StatusCode::kInvalidArgument);
}

void CallsAndErrors()
{
    auto runtime = MakeRuntime();
    auto server = service::Server::Create(*runtime, Spec("/svc/reverse"), Reverse);
    auto client = service::Client::Create(*runtime, Spec("/svc/reverse"));
    assert(server.ok() && client.ok());
    auto& c = *client.value();

    auto info = c.WaitForService(1s);
    assert(info.ok() && info.value().instance_id == server.value()->instance_id());

    auto reply = c.Call(Bytes("hello"), 1s);
    assert(reply.ok() && Text(reply.value()) == "olleh");
    auto empty = c.Call(Bytes(""), 1s);
    assert(empty.ok() && empty.value().empty());

    // Handler errors arrive with their code and message.
    auto failed = c.Call(Bytes("fail"), 1s);
    assert(failed.status().code() == core::StatusCode::kNotFound);
    assert(failed.status().message() == "no such entry");
    auto thrown = c.Call(Bytes("throw"), 1s);
    assert(thrown.status().code() == core::StatusCode::kInternal);
    assert(thrown.status().message().find("handler exploded") != std::string::npos);

    // A slow handler is DeadlineExceeded, and its late reply does not leak
    // into the next call.
    auto slow = c.Call(Bytes("slow:150"), 40ms);
    assert(slow.status().code() == core::StatusCode::kDeadlineExceeded);
    auto next = c.Call(Bytes("abc"), 1s);
    assert(next.ok() && Text(next.value()) == "cba");

    assert(c.Call(Bytes("x"), 0ms).status().code() == core::StatusCode::kInvalidArgument);
    assert(server.value()->handled_count() == 6);
}

void CallRightAfterCreate()
{
    auto runtime = MakeRuntime();
    auto server = service::Server::Create(*runtime, Spec("/svc/eager"), Reverse);
    assert(server.ok());
    // No WaitForService: a fresh client waits for its first heartbeat.
    for (int index = 0; index < 20; ++index) {
        auto client = service::Client::Create(*runtime, Spec("/svc/eager"));
        assert(client.ok());
        auto reply = client.value()->Call(Bytes("now"), 1s);
        assert(reply.ok() && Text(reply.value()) == "won");
    }

    // A server created after the client is found within the first call too.
    auto early = service::Client::Create(*runtime, Spec("/svc/late-server"));
    assert(early.ok());
    std::thread starter([&] {
        std::this_thread::sleep_for(10ms);
        auto late = service::Server::Create(*runtime, Spec("/svc/late-server"), Reverse);
        assert(late.ok());
        std::this_thread::sleep_for(200ms);
    });
    auto reply = early.value()->Call(Bytes("hi"), 1s);
    starter.join();
    assert(reply.ok() && Text(reply.value()) == "ih");
}

void ManyClientsAndThreads()
{
    auto runtime = MakeRuntime();
    auto spec = Spec("/svc/concurrent");
    spec.queue_depth = 64;
    auto server = service::Server::Create(*runtime, spec, Reverse);
    assert(server.ok());
    std::vector<std::unique_ptr<service::Client>> clients;
    for (int index = 0; index < 3; ++index) {
        auto client = service::Client::Create(*runtime, spec);
        assert(client.ok() && client.value()->WaitForService(1s).ok());
        clients.push_back(std::move(client).value());
    }

    // Each reply must reach the call that sent its request, across clients
    // that all see every reply and threads that share one client.
    std::atomic<int> correct {0};
    std::vector<std::thread> threads;
    for (int index = 0; index < 6; ++index) {
        threads.emplace_back([&, index] {
            auto& client = *clients[static_cast<std::size_t>(index % 3)];
            for (int call = 0; call < 50; ++call) {
                const auto text = std::to_string(index) + ":" + std::to_string(call);
                auto reply = client.Call(Bytes(text), 2s);
                if (reply.ok() && Text(reply.value()) == std::string(text.rbegin(), text.rend())) {
                    ++correct;
                }
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    assert(correct == 300);
}

void ServerAbsenceAndRestart()
{
    auto runtime = MakeRuntime();
    auto client = service::Client::Create(*runtime, Spec("/svc/absent"));
    assert(client.ok());
    auto& c = *client.value();

    // No server: calls fail fast with Unavailable, not after the timeout.
    const auto started = std::chrono::steady_clock::now();
    auto offline = c.Call(Bytes("x"), 2s);
    assert(offline.status().code() == core::StatusCode::kUnavailable);
    assert(std::chrono::steady_clock::now() - started < 500ms);
    assert(c.WaitForService(60ms).status().code() == core::StatusCode::kUnavailable);
    assert(c.Server().status().code() == core::StatusCode::kUnavailable);

    auto first = service::Server::Create(*runtime, Spec("/svc/absent"), Reverse);
    assert(first.ok());
    auto seen = c.WaitForService(1s);
    assert(seen.ok() && seen.value().instance_id == first.value()->instance_id());
    const auto first_id = first.value()->instance_id();

    // The server stops mid-call: the client notices the missing heartbeat
    // well before the call's own timeout.
    std::thread stopper([&] {
        std::this_thread::sleep_for(30ms);
        first.value()->Close();
    });
    const auto call_started = std::chrono::steady_clock::now();
    auto interrupted = c.Call(Bytes("slow:500"), 5s);
    stopper.join();
    assert(interrupted.status().code() == core::StatusCode::kUnavailable);
    assert(std::chrono::steady_clock::now() - call_started < 2s);
    first.value().reset();

    // A restarted server has a new instance id.
    auto second = service::Server::Create(*runtime, Spec("/svc/absent"), Reverse);
    assert(second.ok());
    auto restarted = c.WaitForService(1s);
    assert(restarted.ok() && restarted.value().instance_id != first_id);
    auto reply = c.Call(Bytes("back"), 1s);
    assert(reply.ok() && Text(reply.value()) == "kcab");
}

void ExpiredRequestsAreSkipped()
{
    auto runtime = MakeRuntime();
    auto server = service::Server::Create(*runtime, Spec("/svc/expire"), Reverse);
    auto client = service::Client::Create(*runtime, Spec("/svc/expire"));
    assert(server.ok() && client.ok() && client.value()->WaitForService(1s).ok());

    // The first call keeps the handler busy; the second times out while it
    // waits in the queue, so the server drops it without running it.
    std::thread busy([&] { client.value()->Call(Bytes("slow:200"), 1s); });
    std::this_thread::sleep_for(30ms);
    assert(client.value()->Call(Bytes("late"), 50ms).status().code() == core::StatusCode::kDeadlineExceeded);
    busy.join();
    assert(client.value()->Call(Bytes("sync"), 1s).ok());
    assert(server.value()->expired_count() == 1);
    assert(server.value()->handled_count() == 2);
}

void CloseCancelsPendingCalls()
{
    auto runtime = MakeRuntime();
    auto server = service::Server::Create(*runtime, Spec("/svc/cancel"), Reverse);
    auto client = service::Client::Create(*runtime, Spec("/svc/cancel"));
    assert(server.ok() && client.ok() && client.value()->WaitForService(1s).ok());
    std::thread closer([&] {
        std::this_thread::sleep_for(30ms);
        client.value()->Close();
    });
    auto cancelled = client.value()->Call(Bytes("slow:300"), 5s);
    closer.join();
    assert(cancelled.status().code() == core::StatusCode::kCancelled);
    assert(client.value()->Call(Bytes("x"), 1s).status().code() == core::StatusCode::kCancelled);
    client.value()->Close();  // idempotent
    server.value()->Close();
    server.value()->Close();
}

void TypedServicesAndVersions()
{
    auto runtime = MakeRuntime();
    auto name = core::TopicName::Unsafe("/svc/add");
    auto server = service::MakeTypedServer<AddRequest, AddReply>(
        *runtime, name, core::TransportKind::kInMemory, [](const AddRequest& request) {
            if (request.a < 0) {
                return core::Result<AddReply>::FromStatus(core::Status::OutOfRange("negative"));
            }
            return core::Result<AddReply>(AddReply {request.a + request.b});
        });
    auto client = service::MakeTypedClient<AddRequest, AddReply>(*runtime, name, core::TransportKind::kInMemory);
    assert(server.ok() && client.ok());
    assert(client.value().raw().WaitForService(1s).ok());
    auto sum = client.value().Call(AddRequest {2, 40}, 1s);
    assert(sum.ok() && sum.value().sum == 42);
    assert(client.value().Call(AddRequest {-1, 0}, 1s).status().code() == core::StatusCode::kOutOfRange);

    // A client built against another request version is refused at creation.
    auto stale = service::MakeTypedClient<AddRequestV2, AddReply>(*runtime, name, core::TransportKind::kInMemory);
    assert(stale.status().code() == core::StatusCode::kInvalidArgument);
    assert(stale.status().message().find("service.request:test.AddRequest v1") != std::string::npos);
    assert(stale.status().message().find("/svc/add") != std::string::npos);
}

}  // namespace

int main()
{
    SpecValidation();
    CallsAndErrors();
    CallRightAfterCreate();
    ManyClientsAndThreads();
    ServerAbsenceAndRestart();
    ExpiredRequestsAreSkipped();
    CloseCancelsPendingCalls();
    TypedServicesAndVersions();
    std::cout << "service tests passed\n";
    return 0;
}
