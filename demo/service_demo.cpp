// Request/reply services: a parameter store answers get/set calls from two
// clients, then goes away and comes back.
//
//     puppet_master_service_demo
//
// Shows typed calls, handler errors, timeouts against a slow handler, and how
// a client tells "server gone" (Unavailable) from "server slow"
// (DeadlineExceeded). Runs on the in-memory transport; the same code runs
// across processes with a shared-memory or FastDDS transport.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include <puppet_master/runtime/context.h>
#include <puppet_master/service/typed_service.h>

namespace core = puppet_master::core;
namespace runtime = puppet_master::runtime;
namespace service = puppet_master::service;
namespace transport = puppet_master::transport;

using namespace std::chrono_literals;

// Fixed-size messages, so TrivialCodec carries them.
struct ParameterRequest {
    enum Operation : std::uint8_t { kGet = 0, kSet = 1, kSlowGet = 2 };
    Operation operation;
    char key[32];
    double value;  // kSet only
};

struct ParameterReply {
    double value;
    std::uint64_t revision;
};

template <>
struct puppet_master::transport::MessageTypeName<ParameterRequest> {
    static constexpr const char* value = "demo.ParameterRequest";
    static constexpr std::uint32_t version = 1;
};

template <>
struct puppet_master::transport::MessageTypeName<ParameterReply> {
    static constexpr const char* value = "demo.ParameterReply";
    static constexpr std::uint32_t version = 1;
};

namespace {

ParameterRequest Make(ParameterRequest::Operation operation, const std::string& key, double value = 0)
{
    ParameterRequest request {};
    request.operation = operation;
    // Truncates to fit and stays NUL-terminated (the struct is zeroed above).
    std::memcpy(request.key, key.data(), std::min(key.size(), sizeof(request.key) - 1));
    request.value = value;
    return request;
}

// Key-value store with a revision counter, guarded for the handler thread.
class ParameterStore {
public:
    core::Result<ParameterReply> Handle(const ParameterRequest& request)
    {
        const std::string key(request.key, strnlen(request.key, sizeof(request.key)));
        if (key.empty()) {
            return core::Result<ParameterReply>::FromStatus(core::Status::InvalidArgument("key must not be empty"));
        }
        if (request.operation == ParameterRequest::kSlowGet) {
            std::this_thread::sleep_for(300ms);  // e.g. a cold cache
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (request.operation == ParameterRequest::kSet) {
            values_[key] = request.value;
            return core::Result<ParameterReply>(ParameterReply {request.value, ++revision_});
        }
        const auto found = values_.find(key);
        if (found == values_.end()) {
            return core::Result<ParameterReply>::FromStatus(core::Status::NotFound("no parameter " + key));
        }
        return core::Result<ParameterReply>(ParameterReply {found->second, revision_});
    }

private:
    std::mutex mutex_;
    std::map<std::string, double> values_;
    std::uint64_t revision_ {0};
};

using Client = service::TypedClient<ParameterRequest, ParameterReply>;

void Show(const char* label, const core::Result<ParameterReply>& reply)
{
    std::cout << "  " << label << ": ";
    if (reply.ok()) {
        std::cout << reply.value().value << " (revision " << reply.value().revision << ")\n";
    } else {
        std::cout << core::StatusCodeName(reply.status().code()) << " - " << reply.status().message() << '\n';
    }
}

core::Result<std::unique_ptr<service::Server>> StartStore(runtime::RuntimeContext& context, ParameterStore& store,
                                                          const service::ServiceSpec& spec)
{
    return service::MakeTypedServer<ParameterRequest, ParameterReply>(
        context, spec, [&store](const ParameterRequest& request) { return store.Handle(request); });
}

}  // namespace

int main()
{
    auto context = runtime::RuntimeContext::Create();
    if (!context.ok()) {
        std::cerr << context.status().ToString() << '\n';
        return 1;
    }
    auto& runtime = *context.value();

    auto spec = service::TypedServiceSpec<ParameterRequest, ParameterReply>(
        core::TopicName::Unsafe("/demo/parameters"), core::TransportKind::kInMemory);
    spec.heartbeat_interval = 50ms;  // clients notice a dead server within 150 ms

    ParameterStore store;
    auto server = StartStore(runtime, store, spec);
    auto tuner = service::MakeTypedClient<ParameterRequest, ParameterReply>(runtime, spec);
    auto viewer = service::MakeTypedClient<ParameterRequest, ParameterReply>(runtime, spec);
    if (!server.ok() || !tuner.ok() || !viewer.ok()) {
        std::cerr << "setup failed\n";
        return 1;
    }

    std::cout << "1. calls from two clients\n";
    Show("tuner sets gain", tuner.value().Call(Make(ParameterRequest::kSet, "gain", 0.8), 500ms));
    Show("viewer gets gain", viewer.value().Call(Make(ParameterRequest::kGet, "gain"), 500ms));
    Show("viewer gets limit", viewer.value().Call(Make(ParameterRequest::kGet, "limit"), 500ms));
    Show("tuner sends no key", tuner.value().Call(Make(ParameterRequest::kSet, "", 1), 500ms));

    std::cout << "2. slow handler: alive but late\n";
    const auto slow = viewer.value().Call(Make(ParameterRequest::kSlowGet, "gain"), 100ms);
    Show("viewer, 100 ms timeout", slow);
    Show("viewer, 1 s timeout", viewer.value().Call(Make(ParameterRequest::kSlowGet, "gain"), 1s));

    std::cout << "3. server gone\n";
    const auto old_instance = server.value()->instance_id();
    server.value()->Close();
    const auto started = std::chrono::steady_clock::now();
    const auto gone = tuner.value().Call(Make(ParameterRequest::kGet, "gain"), 2s);
    const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
    Show("tuner, 2 s timeout", gone);
    std::cout << "  returned after " << waited.count() << " ms, not 2 s\n";

    std::cout << "4. server restarted\n";
    auto restarted = StartStore(runtime, store, spec);
    if (!restarted.ok()) {
        std::cerr << restarted.status().ToString() << '\n';
        return 1;
    }
    auto info = tuner.value().raw().WaitForService(1s);
    std::cout << "  new instance: " << (info.ok() && info.value().instance_id != old_instance ? "yes" : "no") << '\n';
    Show("tuner gets gain", tuner.value().Call(Make(ParameterRequest::kGet, "gain"), 500ms));

    const bool ok = slow.status().code() == core::StatusCode::kDeadlineExceeded
        && gone.status().code() == core::StatusCode::kUnavailable && waited < 100ms && info.ok();
    std::cout << (ok ? "service demo finished\n" : "service demo FAILED\n");
    return ok ? 0 : 1;
}
