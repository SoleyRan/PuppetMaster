#include <cassert>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <puppet_master/puppet_master.h>

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

}  // namespace

int main()
{
    ManualTriggerExecutesComponent();
    PeriodicTriggerExecutesComponent();
    PeriodicDeadlineMissIsObservable();
    DataTriggerExecutesComponent();
    AllDataTriggerWaitsForEveryTopic();
    TaskDependencyChainAndPolicies();
    FailedTaskDoesNotPropagate();
    InvalidTaskDependencyGraphsAreRejectedAtStart();
    EmptyTaskDependenciesAndLateRegistrationAreRejected();
    TaskReadinessResetsAfterRestart();
    return 0;
}
