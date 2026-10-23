// Services over FastDDS. Discovery there is asynchronous, unlike the
// in-memory and shared-memory transports, so these cases check that a call
// made the moment a server appears is not lost, and that offline detection
// works without source timestamps, which FastDDS does not carry.

#include <cassert>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <puppet_master/runtime/context.h>
#include <puppet_master/service/service.h>
#include <puppet_master/service/typed_service.h>
#include <puppet_master/transport/fastdds/fastdds_transport.h>

namespace core = puppet_master::core;
namespace fastdds = puppet_master::transport::fastdds;
namespace runtime = puppet_master::runtime;
namespace service = puppet_master::service;
namespace transport = puppet_master::transport;

using namespace std::chrono_literals;

struct Sum {
    std::int64_t a;
    std::int64_t b;
};

struct SumResult {
    std::int64_t value;
};

template <>
struct puppet_master::transport::MessageTypeName<Sum> {
    static constexpr const char* value = "test.Sum";
    static constexpr std::uint32_t version = 1;
};

template <>
struct puppet_master::transport::MessageTypeName<SumResult> {
    static constexpr const char* value = "test.SumResult";
};

namespace {

// Each runtime is its own DDS participant, like a separate process.
std::shared_ptr<runtime::RuntimeContext> MakeParticipant(const std::string& name)
{
    auto context = runtime::RuntimeContext::Create();
    assert(context.ok());
    fastdds::Options options;
    options.domain_id = 77;
    options.participant_name = name;
    assert(fastdds::RegisterTransport(*context.value(), core::TransportName::Unsafe("fastdds"), options).ok());
    return context.value();
}

std::string UniqueName(const char* tag)
{
    return std::string("/pm/svc-test/") + tag + "/"
        + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
}

service::ServiceSpec Spec(const std::string& name)
{
    auto spec = service::TypedServiceSpec<Sum, SumResult>(core::TopicName::Unsafe(name), core::TransportKind::kFastDds);
    spec.heartbeat_interval = 50ms;
    return spec;
}

core::Result<SumResult> Add(const Sum& request)
{
    return core::Result<SumResult>(SumResult {request.a + request.b});
}

// The first call goes out as soon as the client sees the server, while
// discovery between the other endpoints may still be running.
void FirstCallIsNotLost()
{
    for (int round = 0; round < 5; ++round) {
        const auto name = UniqueName("first");
        auto server_side = MakeParticipant("svc-server");
        auto client_side = MakeParticipant("svc-client");
        auto server = service::MakeTypedServer<Sum, SumResult>(*server_side, Spec(name), Add);
        auto client = service::MakeTypedClient<Sum, SumResult>(*client_side, Spec(name));
        assert(server.ok() && client.ok());
        assert(client.value().raw().WaitForService(5s).ok());
        auto reply = client.value().Call(Sum {round, 40}, 2s);
        if (!reply.ok()) {
            std::cerr << "round " << round << ": " << reply.status().ToString() << '\n';
        }
        assert(reply.ok() && reply.value().value == round + 40);
    }
}

void ManyCallsFromTwoParticipants()
{
    const auto name = UniqueName("many");
    auto server_side = MakeParticipant("svc-server");
    auto server = service::MakeTypedServer<Sum, SumResult>(*server_side, Spec(name), Add);
    assert(server.ok());

    std::vector<std::thread> callers;
    std::vector<int> failures(2, 0);
    for (int index = 0; index < 2; ++index) {
        callers.emplace_back([&, index] {
            auto side = MakeParticipant("svc-client-" + std::to_string(index));
            auto client = service::MakeTypedClient<Sum, SumResult>(*side, Spec(name));
            assert(client.ok() && client.value().raw().WaitForService(5s).ok());
            for (std::int64_t call = 0; call < 50; ++call) {
                auto reply = client.value().Call(Sum {index * 1000, call}, 2s);
                if (!reply.ok() || reply.value().value != index * 1000 + call) {
                    ++failures[static_cast<std::size_t>(index)];
                }
            }
        });
    }
    for (auto& caller : callers) {
        caller.join();
    }
    assert(failures[0] == 0 && failures[1] == 0);
    assert(server.value()->handled_count() == 100);
}

// Offline detection runs on receive times, since FastDDS messages carry no
// source timestamp.
void ServerStopAndRestart()
{
    const auto name = UniqueName("restart");
    auto client_side = MakeParticipant("svc-client");
    auto client = service::MakeTypedClient<Sum, SumResult>(*client_side, Spec(name));
    assert(client.ok());

    std::uint64_t first_id = 0;
    {
        auto server_side = MakeParticipant("svc-server");
        auto server = service::MakeTypedServer<Sum, SumResult>(*server_side, Spec(name), Add);
        assert(server.ok());
        first_id = server.value()->instance_id();
        auto seen = client.value().raw().WaitForService(5s);
        assert(seen.ok() && seen.value().instance_id == first_id);
        assert(client.value().Call(Sum {1, 2}, 2s).ok());
        server.value()->Close();
    }
    const auto stopped = std::chrono::steady_clock::now();
    while (client.value().raw().Server().ok()) {
        assert(std::chrono::steady_clock::now() - stopped < 2s);
        std::this_thread::sleep_for(10ms);
    }
    assert(client.value().Call(Sum {1, 2}, 2s).status().code() == core::StatusCode::kUnavailable);

    auto server_side = MakeParticipant("svc-server-2");
    auto restarted = service::MakeTypedServer<Sum, SumResult>(*server_side, Spec(name), Add);
    assert(restarted.ok());
    auto again = client.value().raw().WaitForService(5s);
    assert(again.ok() && again.value().instance_id != first_id);
    auto reply = client.value().Call(Sum {20, 22}, 2s);
    assert(reply.ok() && reply.value().value == 42);
}

}  // namespace

int main()
{
    FirstCallIsNotLost();
    ManyCallsFromTwoParticipants();
    ServerStopAndRestart();
    std::cout << "fastdds service tests passed\n";
    return 0;
}
