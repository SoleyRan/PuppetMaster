// Services served through the scheduler: a component with a service trigger
// handles requests in its Execute(), under its priority and serially with its
// other triggers, instead of on a thread of the server's own.

#include <atomic>
#include <cctype>
#include <cassert>
#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <puppet_master/puppet_master.h>
#include <puppet_master/service/service.h>

namespace core = puppet_master::core;
namespace runtime = puppet_master::runtime;
namespace scheduler = puppet_master::scheduler;
namespace service = puppet_master::service;
namespace transport = puppet_master::transport;

using namespace std::chrono_literals;

namespace {

transport::ByteBuffer Buffer(const std::string& text)
{
    return transport::ByteBuffer(text.begin(), text.end());
}

// A view of a literal, which outlives the call.
transport::ByteView Bytes(const char* text)
{
    return transport::ByteView::From(text, std::char_traits<char>::length(text));
}

std::string Text(const transport::ByteBuffer& bytes)
{
    return std::string(bytes.begin(), bytes.end());
}

service::ServiceSpec Spec(const std::string& name)
{
    service::ServiceSpec spec(core::TopicName::Unsafe(name));
    spec.request = transport::MessageDescriptor {"test.Text", "text/plain"};
    spec.reply = transport::MessageDescriptor {"test.Text", "text/plain"};
    spec.heartbeat_interval = 20ms;
    return spec;
}

core::TriggerSpec ServiceTrigger(const service::ServiceSpec& spec, std::int32_t priority = 0)
{
    core::TriggerSpec trigger;
    trigger.kind = core::TriggerKind::kService;
    trigger.service_dependencies = {spec.name};
    trigger.priority = priority;
    return trigger;
}

core::TriggerSpec Manual()
{
    core::TriggerSpec trigger;
    trigger.kind = core::TriggerKind::kManual;
    return trigger;
}

std::shared_ptr<runtime::RuntimeContext> MakeRuntime()
{
    // The runtime comes with the in-memory transport.
    auto runtime = runtime::RuntimeContext::Create();
    assert(runtime.ok());
    return runtime.value();
}

// Serves upper-casing on a kCaller server from Execute(). Records the thread
// each request ran on, and can be made to block.
class UpperServer final : public runtime::Component {
public:
    UpperServer(std::string name, service::ServiceSpec spec, std::vector<core::TriggerSpec> triggers)
        : name_(std::move(name)), spec_(std::move(spec)), triggers_(std::move(triggers))
    {
    }

    runtime::ComponentSpec Describe() const override
    {
        runtime::ComponentSpec spec {core::ComponentName::Unsafe(name_), "serves upper-casing", {}, {}, triggers_};
        spec.servers = {spec_};
        return spec;
    }

    core::Status Initialize(runtime::ComponentContext& context) override
    {
        service::ServerOptions options;
        options.mode = service::ServeMode::kCaller;
        auto server = service::Server::Create(
            context.runtime(), spec_,
            [this](const service::Request& request) {
                handler_threads.push_back(std::this_thread::get_id());
                ++handler_entered;
                while (block.load()) {
                    std::this_thread::sleep_for(1ms);
                }
                std::string text(request.payload.data(), request.payload.data() + request.payload.size());
                for (auto& c : text) {
                    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                }
                return core::Result<transport::ByteBuffer>(Buffer(text));
            },
            options);
        if (!server.ok()) {
            return server.status();
        }
        server_ = std::move(server).value();
        return core::Status::Ok();
    }

    core::Status Execute(runtime::ComponentContext&) override
    {
        execute_threads.push_back(std::this_thread::get_id());
        ++executions;
        auto served = server_->ServePending();
        if (!served.ok()) {
            return served.status();
        }
        served_total += served.value();
        return core::Status::Ok();
    }

    core::Status Shutdown(runtime::ComponentContext&) override
    {
        server_.reset();
        return core::Status::Ok();
    }

    service::Server& server() { return *server_; }

    // Only touched on the component's serial execution; other threads read
    // them only after Scheduler::WaitIdle, which synchronizes with it.
    std::vector<std::thread::id> handler_threads;
    std::vector<std::thread::id> execute_threads;
    std::atomic<int> handler_entered {0};
    std::atomic<int> executions {0};
    std::atomic<std::size_t> served_total {0};
    std::atomic<bool> block {false};

private:
    std::string name_;
    service::ServiceSpec spec_;
    std::vector<core::TriggerSpec> triggers_;
    std::unique_ptr<service::Server> server_;
};

void BringUp(runtime::RuntimeContext& runtime, const core::ComponentName& name)
{
    assert(runtime.ConfigureComponent(name).ok());
    assert(runtime.InitializeComponent(name).ok());
    assert(runtime.StartComponent(name).ok());
}

// A request triggers an execution, and the handler runs inside it, on a
// scheduler worker, not on a thread of the server's own.
void RequestsRunInsideExecute()
{
    auto runtime = MakeRuntime();
    const auto spec = Spec("/sched/upper");
    auto component = std::make_shared<UpperServer>("upper", spec, std::vector<core::TriggerSpec> {ServiceTrigger(spec)});
    assert(runtime->RegisterComponent(component).ok());
    BringUp(*runtime, core::ComponentName::Unsafe("upper"));

    scheduler::Scheduler sched(*runtime);
    assert(sched.RegisterAllComponents().ok());
    assert(sched.Start().ok());

    auto client = service::Client::Create(*runtime, spec);
    assert(client.ok());
    for (const char* word : {"one", "two", "three"}) {
        auto reply = client.value()->Call(Bytes(word), 1s);
        assert(reply.ok());
        std::string expected(word);
        for (auto& c : expected) {
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
        assert(Text(reply.value()) == expected);
    }
    assert(sched.WaitIdle(1s).ok());
    assert(component->served_total.load() == 3);
    assert(component->server().handled_count() == 3);
    // Every handler call happened on a thread that was running Execute().
    for (const auto& thread : component->handler_threads) {
        bool inside = false;
        for (const auto& execute : component->execute_threads) {
            inside = inside || execute == thread;
        }
        assert(inside);
    }
    assert(sched.Stop().ok());
}

// While the component is busy, requests queue; the next execution drains
// them all at once, so several requests coalesce into fewer executions.
void BurstCoalescesIntoOneExecution()
{
    auto runtime = MakeRuntime();
    const auto spec = Spec("/sched/burst");
    auto component = std::make_shared<UpperServer>("burst", spec, std::vector<core::TriggerSpec> {ServiceTrigger(spec)});
    assert(runtime->RegisterComponent(component).ok());
    BringUp(*runtime, core::ComponentName::Unsafe("burst"));

    scheduler::Scheduler sched(*runtime);
    assert(sched.RegisterAllComponents().ok());
    assert(sched.Start().ok());

    auto client = service::Client::Create(*runtime, spec);
    assert(client.ok());
    // Hold the first request in the handler, queue five more behind it.
    component->block.store(true);
    std::thread first([&] { assert(client.value()->Call(Bytes("a"), 2s).ok()); });
    while (component->handler_entered.load() == 0) {
        std::this_thread::sleep_for(1ms);
    }
    std::vector<std::thread> callers;
    std::atomic<int> ok {0};
    for (int index = 0; index < 5; ++index) {
        callers.emplace_back([&] {
            if (client.value()->Call(Bytes("b"), 2s).ok()) {
                ++ok;
            }
        });
    }
    const auto queued_by = std::chrono::steady_clock::now() + 1s;
    while (component->server().PendingCount().value() < 5 && std::chrono::steady_clock::now() < queued_by) {
        std::this_thread::sleep_for(1ms);
    }
    assert(component->server().PendingCount().value() == 5);
    component->block.store(false);
    first.join();
    for (auto& caller : callers) {
        caller.join();
    }
    assert(ok.load() == 5);
    assert(sched.WaitIdle(1s).ok());
    assert(component->served_total.load() == 6);
    // One execution for the first request, one (maybe two, if the trigger
    // raced the drain) for the five queued behind it: never six.
    assert(component->executions.load() <= 3);
    assert(sched.stats().coalesced_events >= 1);
    assert(sched.Stop().ok());
}

// The service trigger shares the component's serial execution with its other
// triggers: requests never run concurrently with a manual execution.
void ServiceAndManualTriggersSerialize()
{
    auto runtime = MakeRuntime();
    const auto spec = Spec("/sched/serial");
    auto component = std::make_shared<UpperServer>(
        "serial", spec, std::vector<core::TriggerSpec> {ServiceTrigger(spec), Manual()});
    assert(runtime->RegisterComponent(component).ok());
    BringUp(*runtime, core::ComponentName::Unsafe("serial"));

    scheduler::SchedulerOptions options;
    options.worker_count = 4;
    scheduler::Scheduler sched(*runtime, options);
    assert(sched.RegisterAllComponents().ok());
    assert(sched.Start().ok());

    auto client = service::Client::Create(*runtime, spec);
    assert(client.ok());
    std::atomic<bool> done {false};
    std::thread manual([&] {
        while (!done.load()) {
            sched.Trigger(core::ComponentName::Unsafe("serial"));
            std::this_thread::sleep_for(100us);
        }
    });
    for (int index = 0; index < 50; ++index) {
        assert(client.value()->Call(Bytes("x"), 1s).ok());
    }
    done.store(true);
    manual.join();
    assert(sched.WaitIdle(1s).ok());
    assert(component->served_total.load() == 50);
    assert(sched.Stop().ok());
}

// A higher-priority service trigger is dispatched ahead of queued lower
// priority work on a single worker.
class Recorder final : public runtime::Component {
public:
    Recorder(std::string name, std::vector<std::string>& order, std::atomic<bool>& gate)
        : name_(std::move(name)), order_(order), gate_(gate)
    {
    }

    runtime::ComponentSpec Describe() const override
    {
        return runtime::ComponentSpec {core::ComponentName::Unsafe(name_), "records order", {}, {}, {Manual()}};
    }

    core::Status Execute(runtime::ComponentContext&) override
    {
        while (!gate_.load()) {
            std::this_thread::sleep_for(1ms);
        }
        order_.push_back(name_);
        return core::Status::Ok();
    }

private:
    std::string name_;
    std::vector<std::string>& order_;
    std::atomic<bool>& gate_;
};

void ServicePriorityOrdersDispatch()
{
    auto runtime = MakeRuntime();
    std::vector<std::string> order;
    std::atomic<bool> gate {false};
    auto blocker = std::make_shared<Recorder>("blocker", order, gate);
    auto low = std::make_shared<Recorder>("low", order, gate);
    const auto spec = Spec("/sched/priority");
    auto urgent = std::make_shared<UpperServer>(
        "urgent", spec, std::vector<core::TriggerSpec> {ServiceTrigger(spec, 10)});
    for (const auto& component : std::vector<runtime::ComponentPtr> {blocker, low, urgent}) {
        assert(runtime->RegisterComponent(component).ok());
        BringUp(*runtime, component->Describe().name);
    }

    scheduler::Scheduler sched(*runtime);  // one worker
    assert(sched.RegisterAllComponents().ok());
    assert(sched.Start().ok());

    auto client = service::Client::Create(*runtime, spec);
    assert(client.ok());
    assert(client.value()->WaitForService(1s).ok());
    // The blocker occupies the only worker; then low work and a request queue.
    assert(sched.Trigger(core::ComponentName::Unsafe("blocker")).ok());
    while (sched.stats().active_events == 0) {
        std::this_thread::sleep_for(1ms);
    }
    assert(sched.Trigger(core::ComponentName::Unsafe("low")).ok());
    std::thread caller([&] { assert(client.value()->Call(Bytes("hi"), 2s).ok()); });
    while (sched.stats().pending_events < 2) {
        std::this_thread::sleep_for(1ms);
    }
    gate.store(true);
    caller.join();
    assert(sched.WaitIdle(1s).ok());
    // blocker, then the request's execution (priority 10), then low.
    assert(order.size() == 2 && order[0] == "blocker" && order[1] == "low");
    assert(urgent->served_total.load() == 1);
    assert(sched.Stop().ok());
}

// Calls the upper-casing service from its own Execute(). With `async`, it
// sends with CallAsync, and the reply callback triggers it again to consume
// the result; without, it blocks the worker in Call.
class Caller final : public runtime::Component {
public:
    Caller(std::string name, service::ServiceSpec spec, bool async)
        : name_(std::move(name)), spec_(std::move(spec)), async_(async)
    {
    }

    runtime::ComponentSpec Describe() const override
    {
        runtime::ComponentSpec spec {core::ComponentName::Unsafe(name_), "calls upper-casing", {}, {}, {Manual()}};
        spec.clients = {spec_};
        return spec;
    }

    core::Status Initialize(runtime::ComponentContext& context) override
    {
        auto client = service::Client::Create(context.runtime(), spec_);
        if (!client.ok()) {
            return client.status();
        }
        client_ = std::move(client).value();
        return core::Status::Ok();
    }

    core::Status Execute(runtime::ComponentContext&) override
    {
        if (!async_) {
            blocking_result = client_->Call(Bytes("blocked"), 200ms).status();
            done.store(true);
            return core::Status::Ok();
        }
        if (reply_ready.load()) {
            consumed.store(true);  // second execution: the reply is here
            return core::Status::Ok();
        }
        return client_->CallAsync(Bytes("async"), 2s, [this](core::Result<transport::ByteBuffer> reply) {
            reply_text = reply.ok() ? Text(reply.value()) : reply.status().ToString();
            reply_ready.store(true);
            // Hand the result back to the scheduler instead of working here.
            scheduler->Trigger(core::ComponentName::Unsafe(name_));
        });
    }

    core::Status Shutdown(runtime::ComponentContext&) override
    {
        client_.reset();
        return core::Status::Ok();
    }

    scheduler::Scheduler* scheduler {nullptr};
    std::string reply_text;  // written before reply_ready, read after it
    std::atomic<bool> reply_ready {false};
    std::atomic<bool> consumed {false};
    core::Status blocking_result {core::Status::Ok()};
    std::atomic<bool> done {false};

private:
    std::string name_;
    service::ServiceSpec spec_;
    bool async_;
    std::unique_ptr<service::Client> client_;
};

// One worker, a caller and a scheduled server: a blocking Call from Execute
// holds the only worker, so the server never runs; CallAsync frees it.
void AsyncCallBetweenScheduledComponents()
{
    for (const bool async : {false, true}) {
        auto runtime = MakeRuntime();
        const auto spec = Spec(async ? "/sched/async-call" : "/sched/blocking-call");
        auto server = std::make_shared<UpperServer>("upper", spec, std::vector<core::TriggerSpec> {ServiceTrigger(spec)});
        auto caller = std::make_shared<Caller>("caller", spec, async);
        assert(runtime->RegisterComponent(server).ok());
        assert(runtime->RegisterComponent(caller).ok());
        BringUp(*runtime, core::ComponentName::Unsafe("upper"));
        BringUp(*runtime, core::ComponentName::Unsafe("caller"));

        scheduler::Scheduler sched(*runtime);  // one worker
        caller->scheduler = &sched;
        assert(sched.RegisterAllComponents().ok());
        assert(sched.Start().ok());
        assert(sched.Trigger(core::ComponentName::Unsafe("caller")).ok());

        const auto deadline = std::chrono::steady_clock::now() + 3s;
        if (async) {
            while (!caller->consumed.load() && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(1ms);
            }
            assert(caller->consumed.load());
            assert(caller->reply_text == "ASYNC");
            assert(server->served_total.load() == 1);
        } else {
            while (!caller->done.load() && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(1ms);
            }
            assert(caller->done.load());
            // The server is alive (heartbeats) but could not run in time.
            assert(caller->blocking_result.code() == core::StatusCode::kDeadlineExceeded);
        }
        assert(sched.WaitIdle(1s).ok());
        assert(sched.Stop().ok());
    }
}

void TriggerValidation()
{
    auto runtime = MakeRuntime();
    const auto spec = Spec("/sched/valid");

    // A service trigger for a service the component does not serve.
    class NotServing final : public runtime::Component {
    public:
        runtime::ComponentSpec Describe() const override
        {
            core::TriggerSpec trigger;
            trigger.kind = core::TriggerKind::kService;
            trigger.service_dependencies = {core::TopicName::Unsafe("/sched/other")};
            return runtime::ComponentSpec {core::ComponentName::Unsafe("not_serving"), "", {}, {}, {trigger}};
        }
    };
    assert(runtime->RegisterComponent(std::make_shared<NotServing>()).ok());
    scheduler::Scheduler sched(*runtime);
    const auto status = sched.RegisterComponent(core::ComponentName::Unsafe("not_serving"));
    assert(status.code() == core::StatusCode::kInvalidArgument);
    assert(status.message().find("does not serve") != std::string::npos);

    // TriggerSpec itself: kService needs services, others must not have any.
    core::TriggerSpec empty;
    empty.kind = core::TriggerKind::kService;
    assert(empty.Validate().code() == core::StatusCode::kInvalidArgument);
    core::TriggerSpec stray;
    stray.kind = core::TriggerKind::kManual;
    stray.service_dependencies = {spec.name};
    assert(stray.Validate().code() == core::StatusCode::kInvalidArgument);
    assert(std::string(core::ToString(core::TriggerKind::kService)) == "service");
}

// ServePending's contract outside the scheduler.
void ServePendingContract()
{
    auto runtime = MakeRuntime();
    auto own = service::Server::Create(*runtime, Spec("/sched/own"), [](const service::Request&) {
        return core::Result<transport::ByteBuffer>(transport::ByteBuffer {});
    });
    assert(own.ok());
    assert(own.value()->ServePending().status().code() == core::StatusCode::kFailedPrecondition);

    service::ServerOptions caller_mode;
    caller_mode.mode = service::ServeMode::kCaller;
    std::unique_ptr<service::Server>* self = nullptr;
    core::Status nested = core::Status::Ok();
    auto server = service::Server::Create(
        *runtime, Spec("/sched/caller"),
        [&](const service::Request&) {
            // Re-entering from the handler is refused, not a deadlock.
            nested = (*self)->ServePending().status();
            return core::Result<transport::ByteBuffer>(Buffer("ok"));
        },
        caller_mode);
    assert(server.ok());
    self = &server.value();
    // Nothing queued: serves nothing.
    assert(server.value()->ServePending().value() == 0);

    auto client = service::Client::Create(*runtime, Spec("/sched/caller"));
    assert(client.ok());
    // Nobody drives the server, so calls time out while requests queue up.
    std::vector<std::thread> calls;
    for (int index = 0; index < 3; ++index) {
        calls.emplace_back([&] { client.value()->Call(Bytes("q"), 2s); });
    }
    while (server.value()->PendingCount().value() < 3) {
        std::this_thread::sleep_for(1ms);
    }
    // `limit` bounds a pass.
    assert(server.value()->ServePending(2).value() == 2);
    assert(server.value()->ServePending().value() == 1);
    for (auto& call : calls) {
        call.join();
    }
    assert(nested.code() == core::StatusCode::kFailedPrecondition);
    assert(nested.message().find("its own handler") != std::string::npos);

    // A request whose client gave up is taken but not handled: it counts
    // toward expired_count(), not toward what ServePending returns.
    const auto handled_before = server.value()->handled_count();
    assert(client.value()->Call(Bytes("late"), 30ms).status().code() == core::StatusCode::kDeadlineExceeded);
    assert(server.value()->PendingCount().value() == 1);
    assert(server.value()->ServePending().value() == 0);
    assert(server.value()->expired_count() == 1);
    assert(server.value()->handled_count() == handled_before);
    assert(server.value()->PendingCount().value() == 0);

    server.value()->Close();
    assert(server.value()->ServePending().status().code() == core::StatusCode::kFailedPrecondition);
}

}  // namespace

int main()
{
    RequestsRunInsideExecute();
    BurstCoalescesIntoOneExecution();
    ServiceAndManualTriggersSerialize();
    ServicePriorityOrdersDispatch();
    AsyncCallBetweenScheduledComponents();
    TriggerValidation();
    ServePendingContract();
    std::cout << "scheduled service tests passed\n";
    return 0;
}
