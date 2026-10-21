#include <atomic>
#include <cassert>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <puppet_master/puppet_master.h>
#include <puppet_master/transport/inmemory/inmemory_transport.h>

namespace core = puppet_master::core;
namespace observability = puppet_master::observability;
namespace runtime = puppet_master::runtime;
namespace scheduler = puppet_master::scheduler;
namespace transport = puppet_master::transport;

namespace {

core::ComponentName MakeComponentName(const std::string& value)
{
    auto name = core::ComponentName::Create(value);
    assert(name.ok());
    return name.value();
}

core::TopicName MakeTopicName(const std::string& value)
{
    auto name = core::TopicName::Create(value);
    assert(name.ok());
    return name.value();
}

transport::EndpointConfig MakeEndpoint(const std::string& topic)
{
    core::MessagePolicy policy;
    policy.freshness = core::FreshnessPolicy::kQueued;
    policy.retention = core::RetentionPolicy::kKeepLast;
    policy.queue_depth = 8;

    return transport::EndpointConfig {
        core::TopicSpec {MakeTopicName(topic), core::TransportKind::kInMemory, policy},
        transport::MessageDescriptor {"test.SchedulerPayload", "text/plain"}
    };
}

core::Status BringUp(runtime::RuntimeContext& context, const core::ComponentName& name)
{
    auto status = context.ConfigureComponent(name);
    if (!status.ok()) {
        return status;
    }

    status = context.InitializeComponent(name);
    if (!status.ok()) {
        return status;
    }

    return context.StartComponent(name);
}

std::string PayloadToString(const transport::ByteBuffer& payload)
{
    if (payload.empty()) {
        return {};
    }

    return std::string(reinterpret_cast<const char*>(payload.data()), payload.size());
}

class ManualCounter final : public runtime::Component {
public:
    explicit ManualCounter(core::ComponentName name, bool fail = false)
        : spec_ {
            std::move(name),
            "counts manual scheduler triggers",
            {},
            {},
            {core::TriggerSpec {core::TriggerKind::kManual, {}, {}, {}, {}}}
        },
          fail_(fail)
    {
    }

    runtime::ComponentSpec Describe() const override
    {
        return spec_;
    }

    core::Status Execute(runtime::ComponentContext&) override
    {
        ++execute_count_;
        if (fail_) {
            return core::Status::FailedPrecondition("intentional task failure");
        }
        return core::Status::Ok();
    }

    int execute_count() const noexcept
    {
        return execute_count_;
    }

private:
    runtime::ComponentSpec spec_;
    bool fail_;
    int execute_count_ {0};
};

class TaskCounter final : public runtime::Component {
public:
    TaskCounter(core::ComponentName name, std::vector<core::TaskName> dependencies,
                core::DependencyPolicy policy = core::DependencyPolicy::kAll)
        : spec_ {
            std::move(name), "counts task dependency triggers", {}, {},
            {core::TriggerSpec {core::TriggerKind::kTaskDependency, {}, policy, {},
                                std::move(dependencies)}}
        }
    {
    }

    runtime::ComponentSpec Describe() const override
    {
        return spec_;
    }

    core::Status Execute(runtime::ComponentContext&) override
    {
        ++execute_count_;
        return core::Status::Ok();
    }

    int execute_count() const noexcept
    {
        return execute_count_;
    }

private:
    runtime::ComponentSpec spec_;
    int execute_count_ {0};
};

class PeriodicCounter final : public runtime::Component {
public:
    PeriodicCounter(
        core::ComponentName name,
        core::Nanoseconds period,
        core::Nanoseconds execution_delay = core::Nanoseconds::zero())
        : spec_ {
            std::move(name),
            "counts periodic scheduler triggers",
            {},
            {},
            {core::TriggerSpec {core::TriggerKind::kPeriodic, period, {}, {}, {}}}
        },
          execution_delay_(execution_delay)
    {
    }

    runtime::ComponentSpec Describe() const override
    {
        return spec_;
    }

    core::Status Execute(runtime::ComponentContext&) override
    {
        if (execution_delay_ > core::Nanoseconds::zero()) {
            std::this_thread::sleep_for(execution_delay_);
        }
        ++execute_count_;
        return core::Status::Ok();
    }

    int execute_count() const noexcept
    {
        return execute_count_;
    }

private:
    runtime::ComponentSpec spec_;
    core::Nanoseconds execution_delay_;
    int execute_count_ {0};
};

class DataConsumer final : public runtime::Component {
public:
    DataConsumer(core::ComponentName name, transport::EndpointConfig endpoint)
        : spec_ {std::move(name), "runs when input data arrives", {std::move(endpoint)}, {}, {}}
    {
        spec_.triggers = {core::TriggerSpec {
            core::TriggerKind::kData,
            {},
            core::DependencyPolicy::kAny,
            {spec_.readers.front().topic.name},
            {}
        }};
    }

    runtime::ComponentSpec Describe() const override
    {
        return spec_;
    }

    core::Status Configure(runtime::ComponentContext& context) override
    {
        auto reader = context.CreateReader(spec_.readers.front());
        if (!reader.ok()) {
            return reader.status();
        }

        reader_ = reader.value();
        return core::Status::Ok();
    }

    core::Status Execute(runtime::ComponentContext&) override
    {
        auto message = reader_->Read();
        if (!message.ok()) {
            return message.status();
        }

        last_payload_ = PayloadToString(message.value().payload);
        ++execute_count_;
        return core::Status::Ok();
    }

    int execute_count() const noexcept
    {
        return execute_count_;
    }

    const std::string& last_payload() const noexcept
    {
        return last_payload_;
    }

private:
    runtime::ComponentSpec spec_;
    transport::ReaderPtr reader_;
    std::string last_payload_;
    int execute_count_ {0};
};

class BlockingDataConsumer final : public runtime::Component {
public:
    BlockingDataConsumer(
        core::ComponentName name,
        transport::EndpointConfig endpoint,
        std::atomic<int>& barrier)
        : spec_ {std::move(name), "blocks data-triggered execution", {std::move(endpoint)}, {}, {}},
          barrier_(barrier)
    {
        spec_.triggers = {core::TriggerSpec {
            core::TriggerKind::kData,
            {},
            core::DependencyPolicy::kAny,
            {spec_.readers.front().topic.name},
            {}
        }};
    }

    runtime::ComponentSpec Describe() const override
    {
        return spec_;
    }

    core::Status Configure(runtime::ComponentContext& context) override
    {
        auto reader = context.CreateReader(spec_.readers.front());
        if (!reader.ok()) {
            return reader.status();
        }

        reader_ = reader.value();
        return core::Status::Ok();
    }

    core::Status Execute(runtime::ComponentContext&) override
    {
        ++entered_;
        while (barrier_.load() < 2) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        auto message = reader_->Read();
        if (!message.ok()) {
            return message.status();
        }

        ++execute_count_;
        return core::Status::Ok();
    }

    int entered() const noexcept
    {
        return entered_;
    }

    int execute_count() const noexcept
    {
        return execute_count_;
    }

private:
    runtime::ComponentSpec spec_;
    transport::ReaderPtr reader_;
    std::atomic<int>& barrier_;
    std::atomic<int> entered_ {0};
    int execute_count_ {0};
};

class MultiTopicCounter final : public runtime::Component {
public:
    MultiTopicCounter(core::ComponentName name, transport::EndpointConfig first, transport::EndpointConfig second)
        : spec_ {
            std::move(name), "counts complete data dependency sets",
            {std::move(first), std::move(second)}, {}, {}
        }
    {
        spec_.triggers = {core::TriggerSpec {
            core::TriggerKind::kData, {}, core::DependencyPolicy::kAll,
            {spec_.readers[0].topic.name, spec_.readers[1].topic.name}, {}
        }};
    }

    runtime::ComponentSpec Describe() const override
    {
        return spec_;
    }

    core::Status Execute(runtime::ComponentContext&) override
    {
        ++execute_count_;
        return core::Status::Ok();
    }

    int execute_count() const noexcept
    {
        return execute_count_;
    }

private:
    runtime::ComponentSpec spec_;
    int execute_count_ {0};
};

class SelfWaitingComponent final : public runtime::Component {
public:
    explicit SelfWaitingComponent(core::ComponentName name)
        : spec_ {std::move(name), "checks worker idle wait", {}, {},
                 {core::TriggerSpec {core::TriggerKind::kManual, {}, {}, {}, {}}}}
    {
    }

    runtime::ComponentSpec Describe() const override { return spec_; }

    core::Status Execute(runtime::ComponentContext&) override
    {
        wait_status = sched->WaitIdle(core::Nanoseconds::zero());
        ++executions;
        return core::Status::Ok();
    }

    scheduler::Scheduler* sched {nullptr};
    core::Status wait_status;
    std::atomic<int> executions {0};

private:
    runtime::ComponentSpec spec_;
};

class StopTimeSelfWaitingComponent final : public runtime::Component {
public:
    explicit StopTimeSelfWaitingComponent(core::ComponentName name)
        : spec_ {std::move(name), "checks worker wait during stop", {}, {},
                 {core::TriggerSpec {core::TriggerKind::kManual, {}, {}, {}, {}}}}
    {
    }

    runtime::ComponentSpec Describe() const override { return spec_; }

    core::Status Execute(runtime::ComponentContext&) override
    {
        entered.store(true);
        while (!release.load()) {
            std::this_thread::yield();
        }
        wait_status = sched->WaitIdle(std::chrono::milliseconds(20));
        stop_status = sched->Stop();
        return core::Status::Ok();
    }

    scheduler::Scheduler* sched {nullptr};
    std::atomic<bool> entered {false};
    std::atomic<bool> release {false};
    core::Status wait_status;
    core::Status stop_status;

private:
    runtime::ComponentSpec spec_;
};

void WorkerCannotWaitOrStopDuringShutdown()
{
    auto context = runtime::RuntimeContext::Create();
    assert(context.ok());
    const auto name = MakeComponentName("stop_time_self_waiting_component");
    auto component = std::make_shared<StopTimeSelfWaitingComponent>(name);
    assert(context.value()->RegisterComponent(component).ok());
    assert(BringUp(*context.value(), name).ok());

    scheduler::Scheduler sched(*context.value());
    component->sched = &sched;
    assert(sched.RegisterComponent(name).ok());
    assert(sched.Start().ok());
    assert(sched.Trigger(name).ok());
    const auto entered_deadline = core::SteadyClock::now() + std::chrono::seconds(1);
    while (!component->entered.load() && core::SteadyClock::now() < entered_deadline) {
        std::this_thread::yield();
    }
    assert(component->entered.load());

    core::Status shutdown_status;
    std::thread shutdown([&]() { shutdown_status = sched.Stop(); });
    const auto stopping_deadline = core::SteadyClock::now() + std::chrono::seconds(1);
    while (sched.is_running() && core::SteadyClock::now() < stopping_deadline) {
        std::this_thread::yield();
    }
    const bool stopping = !sched.is_running();
    component->release.store(true);
    shutdown.join();
    assert(stopping);
    assert(shutdown_status.ok());
    assert(component->wait_status.code() == core::StatusCode::kFailedPrecondition);
    assert(component->stop_status.code() == core::StatusCode::kFailedPrecondition);
    assert(component->stop_status.message() == "scheduler worker cannot stop itself");

    component->entered.store(false);
    component->release.store(false);
    component->wait_status = core::Status::Ok();
    component->stop_status = core::Status::Ok();
    assert(sched.Start().ok());
    assert(sched.Trigger(name).ok());
    const auto restart_deadline = core::SteadyClock::now() + std::chrono::seconds(1);
    while (!component->entered.load() && core::SteadyClock::now() < restart_deadline) {
        std::this_thread::yield();
    }
    const bool restarted = component->entered.load();
    component->release.store(true);
    assert(restarted);
    assert(sched.WaitIdle(std::chrono::seconds(1)).ok());
    assert(component->wait_status.code() == core::StatusCode::kFailedPrecondition);
    assert(component->stop_status.code() == core::StatusCode::kFailedPrecondition);
    assert(sched.Stop().ok());
}

void WorkerCannotWaitForItselfToBecomeIdle()
{
    auto context = runtime::RuntimeContext::Create();
    assert(context.ok());
    const auto name = MakeComponentName("self_waiting_component");
    auto component = std::make_shared<SelfWaitingComponent>(name);
    assert(context.value()->RegisterComponent(component).ok());
    assert(BringUp(*context.value(), name).ok());

    scheduler::Scheduler sched(*context.value());
    component->sched = &sched;
    assert(sched.RegisterComponent(name).ok());
    assert(sched.Start().ok());
    assert(sched.Trigger(name).ok());
    assert(sched.WaitIdle(std::chrono::milliseconds(500)).ok());
    assert(component->executions == 1);
    assert(component->wait_status.code() == core::StatusCode::kFailedPrecondition);
    assert(sched.Stop().ok());
}

void ManualTriggerExecutesComponent()
{
    auto context = runtime::RuntimeContext::Create();
    assert(context.ok());

    const auto name = MakeComponentName("manual_counter");
    auto component = std::make_shared<ManualCounter>(name);
    assert(context.value()->RegisterComponent(component).ok());
    assert(BringUp(*context.value(), name).ok());

    scheduler::Scheduler sched(*context.value());
    assert(sched.RegisterAllComponents().ok());
    assert(sched.Start().ok());

    assert(sched.Trigger(name).ok());
    assert(sched.WaitIdle(std::chrono::milliseconds(500)).ok());
    assert(component->execute_count() == 1);
    assert(sched.stats().dispatched_events == 1);

    assert(sched.Stop().ok());

    const auto snapshot = context.value()->observer()->Snapshot();
    assert(snapshot.tasks.size() == 1);
    assert(snapshot.tasks.front().task_name == "manual_counter");
    assert(snapshot.tasks.front().executions == 1);
}

void PeriodicTriggerExecutesComponent()
{
    auto context = runtime::RuntimeContext::Create();
    assert(context.ok());

    const auto name = MakeComponentName("periodic_counter");
    auto component = std::make_shared<PeriodicCounter>(name, std::chrono::milliseconds(5));
    assert(context.value()->RegisterComponent(component).ok());
    assert(BringUp(*context.value(), name).ok());

    scheduler::Scheduler sched(*context.value());
    assert(sched.RegisterComponent(name).ok());
    assert(sched.Start().ok());

    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    assert(sched.Stop().ok());
    assert(component->execute_count() > 0);
}

void StopDuringStartLeavesSchedulerRestartable()
{
    auto context = runtime::RuntimeContext::Create();
    assert(context.ok());
    scheduler::SchedulerOptions options;
    options.worker_count = 8;
    scheduler::Scheduler sched(*context.value(), options);
    for (int index = 0; index < 24; ++index) {
        const auto name = MakeComponentName("startup_timer_" + std::to_string(index));
        auto component = std::make_shared<PeriodicCounter>(name, std::chrono::seconds(10));
        assert(context.value()->RegisterComponent(component).ok());
        assert(BringUp(*context.value(), name).ok());
        assert(sched.RegisterComponent(name).ok());
    }

    for (int iteration = 0; iteration < 12; ++iteration) {
        core::Status start_status;
        std::thread starter([&]() { start_status = sched.Start(); });
        while (!sched.is_running()) {
            std::this_thread::yield();
        }
        assert(sched.Stop().ok());
        starter.join();
        assert(start_status.ok());
        assert(!sched.is_running());
    }
    assert(sched.Start().ok());
    assert(sched.Stop().ok());
}

class StartupCallbackTransport final : public transport::Transport {
public:
    const core::TransportName& name() const noexcept override { return inner_.name(); }
    core::TransportKind kind() const noexcept override { return inner_.kind(); }
    transport::TransportCapabilities capabilities() const noexcept override { return inner_.capabilities(); }
    core::Status Open() override { return inner_.Open(); }
    core::Status Close() noexcept override { return inner_.Close(); }
    bool is_open() const noexcept override { return inner_.is_open(); }
    core::Result<transport::ReaderPtr> CreateReader(const transport::EndpointConfig& endpoint) override
    {
        if (on_create_reader) {
            on_create_reader();
        }
        return inner_.CreateReader(endpoint);
    }
    core::Result<transport::WriterPtr> CreateWriter(const transport::EndpointConfig& endpoint) override
    {
        return inner_.CreateWriter(endpoint);
    }

    std::function<void()> on_create_reader;

private:
    transport::inmemory::InMemoryTransport inner_ {core::TransportName::Unsafe("inmemory")};
};

void StartupCallbackCannotStopItsOwnScheduler()
{
    runtime::RuntimeOptions options;
    options.register_inmemory_transport = false;
    auto context = runtime::RuntimeContext::Create(options);
    assert(context.ok());
    auto backend = std::make_shared<StartupCallbackTransport>();
    assert(context.value()->RegisterTransport(backend).ok());
    assert(backend->Open().ok());
    const auto name = MakeComponentName("startup_reentrant_consumer");
    auto component = std::make_shared<DataConsumer>(name, MakeEndpoint("/scheduler/startup_reentrant"));
    assert(context.value()->RegisterComponent(component).ok());
    assert(BringUp(*context.value(), name).ok());
    scheduler::Scheduler sched(*context.value());
    assert(sched.RegisterComponent(name).ok());
    int callbacks = 0;
    backend->on_create_reader = [&]() {
        ++callbacks;
        assert(sched.Stop().code() == core::StatusCode::kFailedPrecondition);
        assert(sched.Start().code() == core::StatusCode::kFailedPrecondition);
    };
    assert(sched.Start().ok());
    assert(callbacks == 1);
    assert(sched.is_running());
    assert(sched.Stop().ok());
    assert(sched.Start().ok());
    assert(callbacks == 2);
    assert(sched.Stop().ok());
    backend->on_create_reader = {};
}

void NonDependencyTriggersRejectAnyPolicyAtRegistration()
{
    auto context = runtime::RuntimeContext::Create();
    assert(context.ok());
    for (const auto kind : {core::TriggerKind::kPeriodic, core::TriggerKind::kManual}) {
        const auto name = MakeComponentName(kind == core::TriggerKind::kPeriodic
            ? "invalid_periodic_policy" : "invalid_manual_policy");
        core::TriggerSpec trigger;
        trigger.kind = kind;
        trigger.period = kind == core::TriggerKind::kPeriodic
            ? std::chrono::milliseconds(5) : core::Nanoseconds::zero();
        trigger.dependency_policy = core::DependencyPolicy::kAny;
        runtime::ComponentSpec spec {name, "invalid dependency policy", {}, {}, {trigger}};
        assert(trigger.Validate().code() == core::StatusCode::kInvalidArgument);
        auto status = context.value()->RegisterComponent(spec);
        assert(status.code() == core::StatusCode::kInvalidArgument);
        assert(status.message().find("dependency_policy") != std::string::npos);
    }

    const auto name = MakeComponentName("valid_periodic_policy");
    core::TriggerSpec periodic;
    periodic.kind = core::TriggerKind::kPeriodic;
    periodic.period = std::chrono::milliseconds(5);
    assert(context.value()->RegisterComponent(runtime::ComponentSpec {
        name, "default dependency policy", {}, {}, {periodic}
    }).ok());
    scheduler::Scheduler sched(*context.value());
    assert(sched.RegisterComponent(name).ok());
    assert(sched.stats().registered_components == 1);
}

void PeriodicDeadlineMissIsObservable()
{
    std::size_t deadline_logs = 0;

    runtime::RuntimeOptions options;
    options.observability_options.log_callback =
        [&deadline_logs](const observability::LogRecord& record) {
            if (record.event == "task_deadline_missed") {
                ++deadline_logs;
            }
        };

    auto context = runtime::RuntimeContext::Create(std::move(options));
    assert(context.ok());

    const auto name = MakeComponentName("slow_periodic_counter");
    auto component = std::make_shared<PeriodicCounter>(
        name,
        std::chrono::milliseconds(1),
        std::chrono::milliseconds(3));
    assert(context.value()->RegisterComponent(component).ok());
    assert(BringUp(*context.value(), name).ok());

    scheduler::Scheduler sched(*context.value());
    assert(sched.RegisterComponent(name).ok());
    assert(sched.Start().ok());

    const auto timeout = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (std::chrono::steady_clock::now() < timeout) {
        const auto current = context.value()->observer()->Snapshot();
        if (!current.tasks.empty() && current.tasks.front().deadline_misses > 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(sched.Stop().ok());

    const auto snapshot = context.value()->observer()->Snapshot();
    assert(snapshot.tasks.size() == 1);
    assert(snapshot.tasks.front().executions > 0);
    assert(snapshot.tasks.front().deadline_misses > 0);
    assert(snapshot.tasks.front().max_execution_time > std::chrono::milliseconds(1));
    assert(deadline_logs == snapshot.tasks.front().deadline_misses);
}

void DuplicateDataDependenciesAreRejectedAtSchedulerRegistration()
{
    auto context = runtime::RuntimeContext::Create();
    assert(context.ok());
    const auto name = MakeComponentName("duplicate_data_consumer");
    const auto endpoint = MakeEndpoint("/scheduler/duplicate");
    runtime::ComponentSpec spec {
        name, "duplicate data dependency", {endpoint}, {},
        {core::TriggerSpec {core::TriggerKind::kData, {}, core::DependencyPolicy::kAll,
                           {endpoint.topic.name, endpoint.topic.name}, {}}}
    };
    assert(context.value()->RegisterComponent(spec).ok());
    scheduler::Scheduler sched(*context.value());
    auto status = sched.RegisterComponent(name);
    assert(status.code() == core::StatusCode::kInvalidArgument);
    assert(status.message().find(endpoint.topic.name.str()) != std::string::npos);
    assert(sched.stats().registered_components == 0);
}

void AmbiguousDataReadersAreRejectedAtSchedulerRegistration()
{
    for (const auto transport : {core::TransportKind::kInMemory, core::TransportKind::kFastDds}) {
        auto context = runtime::RuntimeContext::Create();
        assert(context.ok());
        const auto name = MakeComponentName("ambiguous_data_consumer");
        const auto first = MakeEndpoint("/scheduler/ambiguous");
        auto second = first;
        second.topic.transport = transport;
        runtime::ComponentSpec spec {
            name, "ambiguous data readers", {second, first}, {},
            {core::TriggerSpec {core::TriggerKind::kData, {}, core::DependencyPolicy::kAny,
                               {first.topic.name}, {}}}
        };
        assert(context.value()->RegisterComponent(spec).ok());
        scheduler::Scheduler sched(*context.value());
        auto status = sched.RegisterComponent(name);
        assert(status.code() == core::StatusCode::kInvalidArgument);
        assert(status.message().find(first.topic.name.str()) != std::string::npos);
        assert(sched.stats().registered_components == 0);
    }

    auto context = runtime::RuntimeContext::Create();
    assert(context.ok());
    const auto name = MakeComponentName("unambiguous_data_consumer");
    const auto endpoint = MakeEndpoint("/scheduler/unambiguous");
    auto unrelated = MakeEndpoint("/scheduler/unrelated");
    runtime::ComponentSpec spec {
        name, "one matching data reader", {unrelated, endpoint}, {},
        {core::TriggerSpec {core::TriggerKind::kData, {}, core::DependencyPolicy::kAny,
                           {endpoint.topic.name}, {}}}
    };
    assert(context.value()->RegisterComponent(spec).ok());
    scheduler::Scheduler sched(*context.value());
    assert(sched.RegisterComponent(name).ok());
    assert(sched.stats().registered_components == 1);
}

void DataTriggersSharingTopicAreRejectedAtSchedulerRegistration()
{
    auto context = runtime::RuntimeContext::Create();
    assert(context.ok());
    const auto name = MakeComponentName("shared_data_consumer");
    const auto first = MakeEndpoint("/scheduler/shared");
    const auto second = MakeEndpoint("/scheduler/other");
    const core::TriggerSpec data {core::TriggerKind::kData, {}, core::DependencyPolicy::kAny,
                                  {first.topic.name}, {}};
    runtime::ComponentSpec spec {
        name, "separate data triggers", {first, second}, {},
        {core::TriggerSpec {core::TriggerKind::kManual, {}, {}, {}, {}}, data, data}
    };
    assert(context.value()->RegisterComponent(spec).ok());
    scheduler::Scheduler sched(*context.value());
    auto status = sched.RegisterComponent(name);
    assert(status.code() == core::StatusCode::kInvalidArgument);
    assert(status.message().find(name.str()) != std::string::npos);
    assert(status.message().find("1 and 2") != std::string::npos);
    assert(status.message().find(first.topic.name.str()) != std::string::npos);
    assert(sched.stats().registered_components == 0);

    auto valid_context = runtime::RuntimeContext::Create();
    assert(valid_context.ok());
    spec.triggers.back().data_dependencies = {second.topic.name};
    assert(valid_context.value()->RegisterComponent(spec).ok());
    scheduler::Scheduler valid_sched(*valid_context.value());
    assert(valid_sched.RegisterComponent(name).ok());
    assert(valid_sched.stats().registered_components == 1);
}

void DataTriggerExecutesComponent()
{
    auto context = runtime::RuntimeContext::Create();
    assert(context.ok());

    const auto endpoint = MakeEndpoint("/scheduler/data");
    const auto name = MakeComponentName("data_consumer");
    auto component = std::make_shared<DataConsumer>(name, endpoint);
    assert(context.value()->RegisterComponent(component).ok());
    assert(BringUp(*context.value(), name).ok());

    scheduler::Scheduler sched(*context.value());
    assert(sched.RegisterAllComponents().ok());
    assert(sched.Start().ok());

    auto writer = context.value()->CreateWriter(endpoint);
    assert(writer.ok());

    const std::string payload = "sample=42";
    assert(writer.value()->Write(transport::ByteView::From(payload.data(), payload.size())).ok());
    assert(sched.WaitIdle(std::chrono::milliseconds(500)).ok());

    assert(component->execute_count() == 1);
    assert(component->last_payload() == payload);

    assert(sched.Stop().ok());
}

void DataTriggerCoalescesRepeatedArrivals()
{
    auto context = runtime::RuntimeContext::Create();
    assert(context.ok());

    const auto endpoint = MakeEndpoint("/scheduler/coalesce");
    const auto name = MakeComponentName("blocking_data_consumer");
    std::atomic<int> barrier {0};
    auto component = std::make_shared<BlockingDataConsumer>(name, endpoint, barrier);
    assert(context.value()->RegisterComponent(component).ok());
    assert(BringUp(*context.value(), name).ok());

    scheduler::Scheduler sched(*context.value());
    assert(sched.RegisterComponent(name).ok());
    assert(sched.Start().ok());

    auto writer = context.value()->CreateWriter(endpoint);
    assert(writer.ok());

    auto write_payload = [&writer](const std::string& payload) {
        assert(writer.value()->Write(
            transport::ByteView::From(payload.data(), payload.size())).ok());
    };

    write_payload("first");
    const auto entered_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (component->entered() == 0 && std::chrono::steady_clock::now() < entered_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(component->entered() == 1);

    write_payload("second");
    write_payload("third");
    write_payload("fourth");

    const auto coalesced_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (std::chrono::steady_clock::now() < coalesced_deadline) {
        const auto stats = sched.stats();
        if (stats.pending_events == 1
            && stats.active_events == 1
            && stats.coalesced_events >= 2) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto blocked_stats = sched.stats();
    assert(blocked_stats.pending_events == 1);
    assert(blocked_stats.active_events == 1);
    assert(blocked_stats.coalesced_events >= 2);
    assert(sched.WaitIdle(std::chrono::milliseconds(20)).code() == core::StatusCode::kDeadlineExceeded);

    barrier.store(2);
    assert(sched.WaitIdle(std::chrono::milliseconds(500)).ok());
    assert(component->execute_count() == 2);
    assert(sched.Stop().ok());
}

void AllDataTriggerWaitsForEveryTopic()
{
    auto context = runtime::RuntimeContext::Create();
    assert(context.ok());

    const auto first = MakeEndpoint("/scheduler/all-first");
    const auto second = MakeEndpoint("/scheduler/all-second");
    const auto name = MakeComponentName("all_data_consumer");
    auto component = std::make_shared<MultiTopicCounter>(name, first, second);
    assert(context.value()->RegisterComponent(component).ok());
    assert(BringUp(*context.value(), name).ok());

    scheduler::Scheduler sched(*context.value());
    assert(sched.RegisterComponent(name).ok());
    assert(sched.Start().ok());

    auto first_writer = context.value()->CreateWriter(first);
    auto second_writer = context.value()->CreateWriter(second);
    assert(first_writer.ok());
    assert(second_writer.ok());

    const std::string first_payload = "first";
    const std::string second_payload = "second";
    assert(first_writer.value()->Write(transport::ByteView::From(
        first_payload.data(), first_payload.size())).ok());
    assert(sched.WaitIdle(std::chrono::milliseconds(500)).ok());
    assert(component->execute_count() == 0);
    assert(first_writer.value()->Write(transport::ByteView::From(
        first_payload.data(), first_payload.size())).ok());
    assert(sched.WaitIdle(std::chrono::milliseconds(500)).ok());
    assert(component->execute_count() == 0);

    assert(second_writer.value()->Write(transport::ByteView::From(
        second_payload.data(), second_payload.size())).ok());
    assert(sched.WaitIdle(std::chrono::milliseconds(500)).ok());
    assert(component->execute_count() == 1);

    assert(first_writer.value()->Write(transport::ByteView::From(
        first_payload.data(), first_payload.size())).ok());
    assert(sched.WaitIdle(std::chrono::milliseconds(50)).ok());
    assert(component->execute_count() == 1);

    assert(second_writer.value()->Write(transport::ByteView::From(
        second_payload.data(), second_payload.size())).ok());
    assert(sched.WaitIdle(std::chrono::milliseconds(500)).ok());
    assert(component->execute_count() == 2);

    assert(sched.Stop().ok());
}

void TaskDependencyChainAndPolicies()
{
    auto context = runtime::RuntimeContext::Create();
    assert(context.ok());

    const auto first = MakeComponentName("task_first");
    const auto second = MakeComponentName("task_second");
    const auto any = MakeComponentName("task_any");
    const auto all = MakeComponentName("task_all");
    const auto chained = MakeComponentName("task_chained");
    auto first_component = std::make_shared<ManualCounter>(first);
    auto second_component = std::make_shared<ManualCounter>(second);
    auto any_component = std::make_shared<TaskCounter>(
        any, std::vector<core::TaskName> {core::TaskName::Unsafe(first.str()),
                                          core::TaskName::Unsafe(second.str())},
        core::DependencyPolicy::kAny);
    auto all_component = std::make_shared<TaskCounter>(
        all, std::vector<core::TaskName> {core::TaskName::Unsafe(first.str()),
                                          core::TaskName::Unsafe(second.str())});
    auto chained_component = std::make_shared<TaskCounter>(
        chained, std::vector<core::TaskName> {core::TaskName::Unsafe(all.str())});
    for (const auto& component : {std::static_pointer_cast<runtime::Component>(first_component),
                                  std::static_pointer_cast<runtime::Component>(second_component),
                                  std::static_pointer_cast<runtime::Component>(any_component),
                                  std::static_pointer_cast<runtime::Component>(all_component),
                                  std::static_pointer_cast<runtime::Component>(chained_component)}) {
        assert(context.value()->RegisterComponent(component).ok());
        assert(BringUp(*context.value(), component->Describe().name).ok());
    }

    scheduler::Scheduler sched(*context.value());
    assert(sched.RegisterAllComponents().ok());
    assert(sched.Start().ok());

    assert(sched.Trigger(first).ok());
    assert(sched.WaitIdle(std::chrono::milliseconds(500)).ok());
    assert(first_component->execute_count() == 1);
    assert(any_component->execute_count() == 1);
    assert(all_component->execute_count() == 0);
    assert(chained_component->execute_count() == 0);

    assert(sched.Trigger(first).ok());
    assert(sched.WaitIdle(std::chrono::milliseconds(500)).ok());
    assert(any_component->execute_count() == 2);
    assert(all_component->execute_count() == 0);

    assert(sched.Trigger(second).ok());
    assert(sched.WaitIdle(std::chrono::milliseconds(500)).ok());
    assert(any_component->execute_count() == 3);
    assert(all_component->execute_count() == 1);
    assert(chained_component->execute_count() == 1);

    assert(sched.Trigger(second).ok());
    assert(sched.WaitIdle(std::chrono::milliseconds(500)).ok());
    assert(all_component->execute_count() == 1);
    assert(sched.Trigger(first).ok());
    assert(sched.WaitIdle(std::chrono::milliseconds(500)).ok());
    assert(any_component->execute_count() == 5);
    assert(all_component->execute_count() == 2);
    assert(chained_component->execute_count() == 2);
    assert(sched.Stop().ok());
}

void FailedTaskDoesNotPropagate()
{
    auto context = runtime::RuntimeContext::Create();
    assert(context.ok());
    const auto upstream = MakeComponentName("failing_upstream");
    const auto downstream = MakeComponentName("failure_downstream");
    auto failing = std::make_shared<ManualCounter>(upstream, true);
    auto dependent = std::make_shared<TaskCounter>(
        downstream, std::vector<core::TaskName> {core::TaskName::Unsafe(upstream.str())});
    assert(context.value()->RegisterComponent(failing).ok());
    assert(context.value()->RegisterComponent(dependent).ok());
    assert(BringUp(*context.value(), upstream).ok());
    assert(BringUp(*context.value(), downstream).ok());

    scheduler::Scheduler sched(*context.value());
    assert(sched.RegisterAllComponents().ok());
    assert(sched.Start().ok());
    assert(sched.Trigger(upstream).ok());
    assert(sched.WaitIdle(std::chrono::milliseconds(500)).ok());
    assert(failing->execute_count() == 1);
    assert(dependent->execute_count() == 0);
    assert(!sched.last_error().ok());
    assert(sched.Stop().ok());
}

void InvalidTaskDependencyGraphsAreRejectedAtStart()
{
    const auto check = [](const std::vector<std::pair<std::string, std::vector<std::string>>>& graph) {
        auto context = runtime::RuntimeContext::Create();
        assert(context.ok());
        scheduler::Scheduler sched(*context.value());
        for (const auto& entry : graph) {
            const auto name = MakeComponentName(entry.first);
            std::vector<core::TaskName> dependencies;
            for (const auto& dependency : entry.second) {
                dependencies.push_back(core::TaskName::Unsafe(dependency));
            }
            std::shared_ptr<runtime::Component> component;
            if (dependencies.empty()) {
                component = std::make_shared<ManualCounter>(name);
            } else {
                component = std::make_shared<TaskCounter>(name, std::move(dependencies));
            }
            assert(context.value()->RegisterComponent(component).ok());
            assert(BringUp(*context.value(), name).ok());
            assert(sched.RegisterComponent(name).ok());
        }
        const auto status = sched.Start();
        assert(!status.ok());
        assert(status.code() == core::StatusCode::kInvalidArgument);
        assert(!sched.is_running());
    };
    check({{"missing_downstream", {"missing_upstream"}}});
    check({{"duplicate_downstream", {"upstream", "upstream"}}, {"upstream", {}}});
    check({{"self_reference", {"self_reference"}}});
    check({{"cycle_a", {"cycle_b"}}, {"cycle_b", {"cycle_a"}}});
}

void EmptyTaskDependenciesAndLateRegistrationAreRejected()
{
    auto context = runtime::RuntimeContext::Create();
    assert(context.ok());
    const auto empty = MakeComponentName("empty_task_dependencies");
    auto empty_component = std::make_shared<TaskCounter>(empty, std::vector<core::TaskName> {});
    assert(context.value()->RegisterComponent(empty_component).ok());

    scheduler::Scheduler sched(*context.value());
    const auto empty_status = sched.RegisterComponent(empty);
    assert(empty_status.code() == core::StatusCode::kInvalidArgument);

    const auto upstream = MakeComponentName("registered_before_start");
    auto component = std::make_shared<ManualCounter>(upstream);
    assert(context.value()->RegisterComponent(component).ok());
    assert(BringUp(*context.value(), upstream).ok());
    assert(sched.RegisterComponent(upstream).ok());
    assert(sched.Start().ok());
    const auto late_status = sched.RegisterComponent(upstream);
    assert(late_status.code() == core::StatusCode::kFailedPrecondition);
    assert(sched.Stop().ok());
}

void TaskReadinessResetsAfterRestart()
{
    auto context = runtime::RuntimeContext::Create();
    assert(context.ok());
    const auto first = MakeComponentName("restart_first");
    const auto second = MakeComponentName("restart_second");
    const auto all = MakeComponentName("restart_all");
    auto first_component = std::make_shared<ManualCounter>(first);
    auto second_component = std::make_shared<ManualCounter>(second);
    auto dependent = std::make_shared<TaskCounter>(
        all, std::vector<core::TaskName> {core::TaskName::Unsafe(first.str()),
                                          core::TaskName::Unsafe(second.str())});
    assert(context.value()->RegisterComponent(first_component).ok());
    assert(context.value()->RegisterComponent(second_component).ok());
    assert(context.value()->RegisterComponent(dependent).ok());
    assert(BringUp(*context.value(), first).ok());
    assert(BringUp(*context.value(), second).ok());
    assert(BringUp(*context.value(), all).ok());

    scheduler::Scheduler sched(*context.value());
    assert(sched.RegisterAllComponents().ok());
    assert(sched.Start().ok());
    assert(sched.Trigger(first).ok());
    assert(sched.WaitIdle(std::chrono::milliseconds(500)).ok());
    assert(dependent->execute_count() == 0);
    assert(sched.Stop().ok());
    assert(sched.Start().ok());
    assert(sched.Trigger(second).ok());
    assert(sched.WaitIdle(std::chrono::milliseconds(500)).ok());
    assert(dependent->execute_count() == 0);
    assert(sched.Trigger(first).ok());
    assert(sched.WaitIdle(std::chrono::milliseconds(500)).ok());
    assert(dependent->execute_count() == 1);
    assert(sched.Stop().ok());
}

class BlockingCounter final : public runtime::Component {
public:
    explicit BlockingCounter(core::ComponentName name, std::atomic<int>& barrier)
        : spec_ {
            std::move(name),
            "blocks until barrier reaches target",
            {},
            {},
            {core::TriggerSpec {core::TriggerKind::kManual, {}, {}, {}, {}}}
        },
          barrier_(barrier)
    {
    }

    runtime::ComponentSpec Describe() const override
    {
        return spec_;
    }

    core::Status Execute(runtime::ComponentContext&) override
    {
        ++entered_;
        while (barrier_.load() < 2) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ++execute_count_;
        return core::Status::Ok();
    }

    int entered() const noexcept
    {
        return entered_;
    }

    int execute_count() const noexcept
    {
        return execute_count_;
    }

private:
    runtime::ComponentSpec spec_;
    std::atomic<int>& barrier_;
    std::atomic<int> entered_ {0};
    int execute_count_ {0};
};

void ParallelWorkersExecuteDifferentComponents()
{
    auto context = runtime::RuntimeContext::Create();
    assert(context.ok());

    const auto first = MakeComponentName("parallel_first");
    const auto second = MakeComponentName("parallel_second");
    std::atomic<int> barrier {0};
    auto first_component = std::make_shared<BlockingCounter>(first, barrier);
    auto second_component = std::make_shared<BlockingCounter>(second, barrier);
    assert(context.value()->RegisterComponent(first_component).ok());
    assert(context.value()->RegisterComponent(second_component).ok());
    assert(BringUp(*context.value(), first).ok());
    assert(BringUp(*context.value(), second).ok());

    scheduler::SchedulerOptions options;
    options.worker_count = 2;
    scheduler::Scheduler sched(*context.value(), options);
    assert(sched.RegisterAllComponents().ok());
    assert(sched.Start().ok());

    assert(sched.Trigger(first).ok());
    assert(sched.Trigger(second).ok());
    const auto queued_stats = sched.stats();
    assert(queued_stats.pending_events + queued_stats.active_events == 2);
    assert(queued_stats.coalesced_events == 0);

    while (first_component->entered() + second_component->entered() < 2) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(first_component->entered() == 1);
    assert(second_component->entered() == 1);

    barrier.store(2);
    assert(sched.WaitIdle(std::chrono::milliseconds(500)).ok());
    assert(first_component->execute_count() == 1);
    assert(second_component->execute_count() == 1);
    assert(sched.Stop().ok());
}

void SameComponentExecutesSeriallyAndCoalescesTriggers()
{
    auto context = runtime::RuntimeContext::Create();
    assert(context.ok());

    const auto name = MakeComponentName("serial_component");
    std::atomic<int> barrier {0};
    auto component = std::make_shared<BlockingCounter>(name, barrier);
    assert(context.value()->RegisterComponent(component).ok());
    assert(BringUp(*context.value(), name).ok());

    scheduler::SchedulerOptions options;
    options.worker_count = 2;
    scheduler::Scheduler sched(*context.value(), options);
    assert(sched.RegisterComponent(name).ok());
    assert(sched.Start().ok());

    assert(sched.Trigger(name).ok());
    const auto entered_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (component->entered() == 0 && std::chrono::steady_clock::now() < entered_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(component->entered() == 1);

    assert(sched.Trigger(name).ok());
    assert(sched.Trigger(name).ok());
    const auto stats = sched.stats();
    assert(stats.pending_events == 1);
    assert(stats.coalesced_events == 1);
    assert(sched.WaitIdle(std::chrono::milliseconds(20)).code() == core::StatusCode::kDeadlineExceeded);

    barrier.store(2);
    assert(sched.WaitIdle(std::chrono::milliseconds(500)).ok());
    assert(component->execute_count() == 2);
    assert(sched.Stop().ok());
}

class OrderedComponent final : public runtime::Component {
public:
    OrderedComponent(runtime::ComponentSpec spec, std::vector<std::string>& order,
                     std::atomic<bool>* release = nullptr)
        : spec_(std::move(spec)), order_(order), release_(release)
    {
    }

    runtime::ComponentSpec Describe() const override
    {
        return spec_;
    }

    core::Status Execute(runtime::ComponentContext&) override
    {
        entered.store(true);
        if (release_) {
            const auto timeout = core::SteadyClock::now() + std::chrono::seconds(5);
            while (!release_->load() && core::SteadyClock::now() < timeout) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (!release_->load()) {
                return core::Status::DeadlineExceeded("ordering test blocker timed out");
            }
        } else {
            order_.push_back(spec_.name.str());
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return core::Status::Ok();
    }

    std::atomic<bool> entered {false};

private:
    runtime::ComponentSpec spec_;
    std::vector<std::string>& order_;
    std::atomic<bool>* release_;
};

core::TriggerSpec ManualPolicy(std::int32_t priority = 0, core::Nanoseconds deadline = {})
{
    return core::TriggerSpec {core::TriggerKind::kManual, {}, {}, {}, {}, priority, deadline};
}

runtime::ComponentSpec OrderedSpec(const std::string& name, core::TriggerSpec trigger)
{
    return runtime::ComponentSpec {MakeComponentName(name), "records scheduler order", {}, {}, {trigger}};
}

void CheckQueuedOrder(
    std::vector<runtime::ComponentSpec> specs,
    const std::function<void(runtime::RuntimeContext&, scheduler::Scheduler&)>& enqueue,
    const std::vector<std::string>& expected,
    const std::function<void(const observability::MetricsSnapshot&)>& inspect = {})
{
    auto context = runtime::RuntimeContext::Create();
    assert(context.ok());
    std::vector<std::string> order;
    std::atomic<bool> release {false};
    const auto blocker_name = MakeComponentName("ordering_blocker");
    auto blocker = std::make_shared<OrderedComponent>(
        OrderedSpec(blocker_name.str(), ManualPolicy()), order, &release);
    assert(context.value()->RegisterComponent(blocker).ok());
    assert(BringUp(*context.value(), blocker_name).ok());
    for (auto& spec : specs) {
        const auto name = spec.name;
        assert(context.value()->RegisterComponent(std::make_shared<OrderedComponent>(spec, order)).ok());
        assert(BringUp(*context.value(), name).ok());
    }
    scheduler::Scheduler sched(*context.value());
    assert(sched.RegisterAllComponents().ok());
    assert(sched.Start().ok());
    assert(sched.Trigger(blocker_name).ok());
    const auto timeout = core::SteadyClock::now() + std::chrono::milliseconds(500);
    while (!blocker->entered.load() && core::SteadyClock::now() < timeout) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(blocker->entered.load());
    enqueue(*context.value(), sched);
    release.store(true);
    assert(sched.WaitIdle(std::chrono::seconds(2)).ok());
    assert(sched.last_error().ok());
    assert(order == expected);
    assert(sched.Stop().ok());
    if (inspect) {
        inspect(context.value()->observer()->Snapshot());
    }
}

void PriorityAndFifoOrdering()
{
    auto first_manual = OrderedSpec("first_manual", ManualPolicy(3));
    first_manual.triggers.push_back(ManualPolicy(100));
    CheckQueuedOrder(
        {OrderedSpec("low", ManualPolicy(-1, std::chrono::nanoseconds(1))),
         OrderedSpec("tie_first", ManualPolicy()), OrderedSpec("high", ManualPolicy(9)),
         OrderedSpec("tie_second", ManualPolicy()), first_manual},
        [](runtime::RuntimeContext&, scheduler::Scheduler& sched) {
            for (const auto* name : {"low", "tie_first", "high", "tie_second", "first_manual"}) {
                assert(sched.Trigger(MakeComponentName(name)).ok());
            }
        },
        {"high", "first_manual", "tie_first", "tie_second", "low"});
}

void AbsoluteDeadlineOrderingAndCoalescing()
{
    CheckQueuedOrder(
        {OrderedSpec("early", ManualPolicy(0, std::chrono::milliseconds(200))),
         OrderedSpec("shorter", ManualPolicy(0, std::chrono::milliseconds(100))),
         OrderedSpec("unbounded", ManualPolicy())},
        [](runtime::RuntimeContext&, scheduler::Scheduler& sched) {
            assert(sched.Trigger(MakeComponentName("unbounded")).ok());
            assert(sched.Trigger(MakeComponentName("early")).ok());
            // 后入队的短预算仍有更晚的绝对截止时间；重复触发不得刷新首事件。
            const auto later = core::SteadyClock::now() + std::chrono::milliseconds(250);
            std::this_thread::sleep_until(later);
            assert(sched.Trigger(MakeComponentName("shorter")).ok());
            assert(sched.Trigger(MakeComponentName("early")).ok());
            assert(sched.stats().coalesced_events == 1);
        },
        {"early", "shorter", "unbounded"},
        [](const observability::MetricsSnapshot& snapshot) {
            for (const auto& task : snapshot.tasks) {
                if (task.task_name == "early") {
                    assert(task.deadline_misses == 1);
                    assert(task.last_queue_wait > std::chrono::milliseconds(200));
                    assert(task.last_response_time == task.last_queue_wait + task.last_execution_time);
                    return;
                }
            }
            assert(false);
        });
}

void OversizedDeadlineDoesNotWrapQueueOrder()
{
    CheckQueuedOrder(
        {OrderedSpec("oversized", ManualPolicy(0, core::Nanoseconds::max())),
         OrderedSpec("finite", ManualPolicy(0, std::chrono::seconds(1)))},
        [](runtime::RuntimeContext&, scheduler::Scheduler& sched) {
            assert(sched.Trigger(MakeComponentName("oversized")).ok());
            assert(sched.Trigger(MakeComponentName("finite")).ok());
        },
        {"finite", "oversized"});
}

void QueuedDeadlineMissMatchesEventsAndLog()
{
    std::vector<observability::Event> events;
    std::vector<observability::LogRecord> logs;
    runtime::RuntimeOptions options;
    options.observability_options.event_callback = [&events](const observability::Event& event) {
        if (event.resource == "queued_deadline") {
            events.push_back(event);
        }
    };
    options.observability_options.log_callback = [&logs](const observability::LogRecord& record) {
        if (record.event == "task_deadline_missed" && record.component == "queued_deadline") {
            logs.push_back(record);
        }
    };
    auto context = runtime::RuntimeContext::Create(std::move(options));
    assert(context.ok());
    std::vector<std::string> order;
    std::atomic<bool> release {false};
    const auto blocker_name = MakeComponentName("queued_deadline_blocker");
    const auto target_name = MakeComponentName("queued_deadline");
    auto blocker = std::make_shared<OrderedComponent>(
        OrderedSpec(blocker_name.str(), ManualPolicy()), order, &release);
    auto target = std::make_shared<OrderedComponent>(
        OrderedSpec(target_name.str(), ManualPolicy(0, std::chrono::milliseconds(30))), order);
    assert(context.value()->RegisterComponent(blocker).ok());
    assert(context.value()->RegisterComponent(target).ok());
    assert(BringUp(*context.value(), blocker_name).ok());
    assert(BringUp(*context.value(), target_name).ok());
    scheduler::Scheduler sched(*context.value());
    assert(sched.RegisterAllComponents().ok());
    assert(sched.Start().ok());
    assert(sched.Trigger(blocker_name).ok());
    const auto entered_deadline = core::SteadyClock::now() + std::chrono::milliseconds(500);
    while (!blocker->entered.load() && core::SteadyClock::now() < entered_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(blocker->entered.load());
    assert(sched.Trigger(target_name).ok());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    assert(sched.stats().pending_events == 1);
    assert(sched.Trigger(target_name).ok());
    assert(sched.stats().coalesced_events == 1);
    release.store(true);
    assert(sched.WaitIdle(std::chrono::seconds(2)).ok());
    assert(sched.Stop().ok());

    assert(order == std::vector<std::string> {target_name.str()});
    assert(events.size() == 2);
    assert(events[0].kind == observability::EventKind::kTaskExecution);
    assert(events[1].kind == observability::EventKind::kDeadlineMiss);
    assert(events[0].queue_wait > std::chrono::milliseconds(30));
    assert(events[0].queue_wait == events[1].queue_wait);
    assert(events[0].timestamp == events[1].timestamp);
    assert(events[0].duration == events[1].duration);
    assert(events[0].deadline == events[1].deadline);
    assert(events[0].success == events[1].success);
    const auto snapshot = context.value()->observer()->Snapshot();
    assert(snapshot.tasks.size() == 2);
    bool found_target = false;
    for (const auto& task : snapshot.tasks) {
        if (task.task_name == target_name.str()) {
            found_target = true;
            assert(task.executions == 1);
            assert(task.deadline_misses == 1);
            assert(task.last_queue_wait == events[0].queue_wait);
            assert(task.last_execution_time == events[0].duration);
            assert(task.last_response_time == events[0].queue_wait + events[0].duration);
        }
    }
    assert(found_target);
    assert(logs.size() == 1);
    const auto field = [&logs](const std::string& key) {
        for (const auto& entry : logs.front().fields) {
            if (entry.key == key) {
                return std::stoll(entry.value);
            }
        }
        assert(false);
        return 0LL;
    };
    assert(field("queue_wait_us") == std::chrono::duration_cast<std::chrono::microseconds>(events[0].queue_wait).count());
    assert(field("execution_us") == std::chrono::duration_cast<std::chrono::microseconds>(events[0].duration).count());
    assert(field("response_us") == std::chrono::duration_cast<std::chrono::microseconds>(events[0].queue_wait + events[0].duration).count());
    assert(field("deadline_us") == 30000);
}

void NonperiodicDeadlineAndCoalescedMetadata()
{
    const auto endpoint = MakeEndpoint("/scheduler/ordering_metadata");
    auto mixed = OrderedSpec("mixed", ManualPolicy(-1, std::chrono::nanoseconds(1)));
    mixed.readers.push_back(endpoint);
    mixed.triggers.push_back(core::TriggerSpec {
        core::TriggerKind::kData, {}, core::DependencyPolicy::kAny,
        {endpoint.topic.name}, {}, 10, std::chrono::seconds(1)});
    CheckQueuedOrder(
        {mixed, OrderedSpec("ordinary", ManualPolicy())},
        [endpoint](runtime::RuntimeContext& context, scheduler::Scheduler& sched) {
            assert(sched.Trigger(MakeComponentName("mixed")).ok());
            auto writer = context.CreateWriter(endpoint);
            assert(writer.ok());
            const std::string payload = "metadata";
            assert(writer.value()->Write(transport::ByteView::From(payload.data(), payload.size())).ok());
            const auto timeout = core::SteadyClock::now() + std::chrono::milliseconds(500);
            while (sched.stats().coalesced_events == 0 && core::SteadyClock::now() < timeout) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            assert(sched.stats().coalesced_events == 1);
            assert(sched.Trigger(MakeComponentName("ordinary")).ok());
        },
        {"ordinary", "mixed"},
        [](const observability::MetricsSnapshot& snapshot) {
            for (const auto& task : snapshot.tasks) {
                if (task.task_name == "mixed") {
                    assert(task.executions == 1);
                    assert(task.deadline_misses == 1);
                    return;
                }
            }
            assert(false);
        });
}

void DataAndTaskPoliciesReachQueue()
{
    const auto endpoint = MakeEndpoint("/scheduler/ordering_data");
    auto data = OrderedSpec("data", core::TriggerSpec {
        core::TriggerKind::kData, {}, core::DependencyPolicy::kAny,
        {endpoint.topic.name}, {}, 0, std::chrono::seconds(1)});
    data.readers.push_back(endpoint);
    auto task = OrderedSpec("dependent", core::TriggerSpec {
        core::TriggerKind::kTaskDependency, {}, core::DependencyPolicy::kAll, {},
        {core::TaskName::Unsafe("ordering_blocker")}, 5, core::Nanoseconds(1)});
    CheckQueuedOrder(
        {data, task, OrderedSpec("ordinary", ManualPolicy())},
        [endpoint](runtime::RuntimeContext& context, scheduler::Scheduler& sched) {
            assert(sched.Trigger(MakeComponentName("ordinary")).ok());
            auto writer = context.CreateWriter(endpoint);
            assert(writer.ok());
            const std::string payload = "data";
            assert(writer.value()->Write(transport::ByteView::From(payload.data(), payload.size())).ok());
            const auto timeout = core::SteadyClock::now() + std::chrono::milliseconds(500);
            while (sched.stats().pending_events < 2 && core::SteadyClock::now() < timeout) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            assert(sched.stats().pending_events == 2);
        },
        {"dependent", "data", "ordinary"},
        [](const observability::MetricsSnapshot& snapshot) {
            for (const auto& task : snapshot.tasks) {
                if (task.task_name == "dependent") {
                    assert(task.deadline_misses == 1);
                    return;
                }
            }
            assert(false);
        });
}

void NegativeDeadlineIsRejected()
{
    for (const auto kind : {core::TriggerKind::kManual, core::TriggerKind::kPeriodic,
                           core::TriggerKind::kData, core::TriggerKind::kTaskDependency}) {
        core::TriggerSpec trigger {kind, std::chrono::milliseconds(1), {}, {}, {}, 0,
                                   core::Nanoseconds(-1)};
        assert(trigger.Validate().code() == core::StatusCode::kInvalidArgument);
        trigger.deadline = core::Nanoseconds::zero();
        assert(trigger.Validate().ok());
    }
}

// Runs `old_low` (priority 0, queued first) against `new_high` (priority 5,
// queued `gap` later) while a blocker holds the only worker, then returns the
// dispatch order and the aged-dispatch count.
std::pair<std::vector<std::string>, std::size_t> AgingOrder(core::Nanoseconds aging, std::chrono::milliseconds gap)
{
    auto context = runtime::RuntimeContext::Create();
    assert(context.ok());
    std::vector<std::string> order;
    std::atomic<bool> release {false};
    const auto blocker_name = MakeComponentName("aging_blocker");
    auto blocker = std::make_shared<OrderedComponent>(OrderedSpec(blocker_name.str(), ManualPolicy()), order, &release);
    assert(context.value()->RegisterComponent(blocker).ok());
    assert(BringUp(*context.value(), blocker_name).ok());
    for (const auto& [name, priority] : {std::pair<const char*, int> {"old_low", 0}, {"new_high", 5}}) {
        auto component = std::make_shared<OrderedComponent>(OrderedSpec(name, ManualPolicy(priority)), order);
        assert(context.value()->RegisterComponent(component).ok());
        assert(BringUp(*context.value(), MakeComponentName(name)).ok());
    }

    scheduler::SchedulerOptions options;
    options.priority_aging_interval = aging;
    scheduler::Scheduler sched(*context.value(), options);
    assert(sched.RegisterAllComponents().ok());
    assert(sched.Start().ok());
    assert(sched.Trigger(blocker_name).ok());
    const auto timeout = core::SteadyClock::now() + std::chrono::milliseconds(500);
    while (!blocker->entered.load() && core::SteadyClock::now() < timeout) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(blocker->entered.load());

    assert(sched.Trigger(MakeComponentName("old_low")).ok());
    std::this_thread::sleep_for(gap);
    assert(sched.Trigger(MakeComponentName("new_high")).ok());
    release.store(true);
    assert(sched.WaitIdle(std::chrono::seconds(2)).ok());
    const auto aged = sched.stats().aged_dispatches;
    assert(sched.Stop().ok());
    return {order, aged};
}

void PriorityAgingPreventsStarvation()
{
    // Without aging, the later high-priority event always wins.
    auto strict = AgingOrder(core::Nanoseconds::zero(), std::chrono::milliseconds(30));
    assert((strict.first == std::vector<std::string> {"new_high", "old_low"}));
    assert(strict.second == 0);

    // With a 1 ms interval, 30 ms of waiting lifts old_low well past +5.
    auto aged = AgingOrder(std::chrono::milliseconds(1), std::chrono::milliseconds(30));
    assert((aged.first == std::vector<std::string> {"old_low", "new_high"}));
    assert(aged.second == 1);

    // A long interval leaves strict order in place for short waits.
    auto slow = AgingOrder(std::chrono::seconds(10), std::chrono::milliseconds(30));
    assert((slow.first == std::vector<std::string> {"new_high", "old_low"}));
    assert(slow.second == 0);

    scheduler::SchedulerOptions negative;
    negative.priority_aging_interval = core::Nanoseconds(-1);
    assert(negative.Validate().code() == core::StatusCode::kInvalidArgument);
}

void ZeroWorkerCountIsRejected()
{
    auto context = runtime::RuntimeContext::Create();
    assert(context.ok());

    scheduler::SchedulerOptions options;
    options.worker_count = 0;
    scheduler::Scheduler sched(*context.value(), options);
    const auto status = sched.Start();
    assert(status.code() == core::StatusCode::kInvalidArgument);
    assert(!sched.is_running());
}

}  // namespace

int main()
{
    WorkerCannotWaitOrStopDuringShutdown();
    WorkerCannotWaitForItselfToBecomeIdle();
    ManualTriggerExecutesComponent();
    PeriodicTriggerExecutesComponent();
    StopDuringStartLeavesSchedulerRestartable();
    StartupCallbackCannotStopItsOwnScheduler();
    NonDependencyTriggersRejectAnyPolicyAtRegistration();
    PeriodicDeadlineMissIsObservable();
    DuplicateDataDependenciesAreRejectedAtSchedulerRegistration();
    AmbiguousDataReadersAreRejectedAtSchedulerRegistration();
    DataTriggersSharingTopicAreRejectedAtSchedulerRegistration();
    DataTriggerExecutesComponent();
    DataTriggerCoalescesRepeatedArrivals();
    AllDataTriggerWaitsForEveryTopic();
    TaskDependencyChainAndPolicies();
    FailedTaskDoesNotPropagate();
    InvalidTaskDependencyGraphsAreRejectedAtStart();
    EmptyTaskDependenciesAndLateRegistrationAreRejected();
    TaskReadinessResetsAfterRestart();
    ParallelWorkersExecuteDifferentComponents();
    SameComponentExecutesSeriallyAndCoalescesTriggers();
    PriorityAndFifoOrdering();
    AbsoluteDeadlineOrderingAndCoalescing();
    OversizedDeadlineDoesNotWrapQueueOrder();
    QueuedDeadlineMissMatchesEventsAndLog();
    NonperiodicDeadlineAndCoalescedMetadata();
    DataAndTaskPoliciesReachQueue();
    NegativeDeadlineIsRejected();
    PriorityAgingPreventsStarvation();
    ZeroWorkerCountIsRejected();
    return 0;
}
