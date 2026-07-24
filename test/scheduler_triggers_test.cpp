#include <cassert>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <utility>

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
    explicit ManualCounter(core::ComponentName name)
        : spec_ {
            std::move(name),
            "counts manual scheduler triggers",
            {},
            {},
            {core::TriggerSpec {core::TriggerKind::kManual, {}, {}, {}, {}}}
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

    std::this_thread::sleep_for(std::chrono::milliseconds(8));
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

void UnsupportedTaskDependencyIsRejected()
{
    auto context = runtime::RuntimeContext::Create();
    assert(context.ok());

    const auto name = MakeComponentName("task_dependency");
    runtime::ComponentSpec spec {
        name,
        "unsupported task dependency trigger",
        {},
        {},
        {core::TriggerSpec {core::TriggerKind::kTaskDependency, {}, {}, {}, {core::TaskName::Unsafe("upstream")}}}
    };
    assert(context.value()->RegisterComponent(spec).ok());

    scheduler::Scheduler sched(*context.value());
    auto status = sched.RegisterComponent(name);
    assert(!status.ok());
    assert(status.code() == core::StatusCode::kUnsupported);
}

}  // namespace

int main()
{
    ManualTriggerExecutesComponent();
    PeriodicTriggerExecutesComponent();
    PeriodicDeadlineMissIsObservable();
    DataTriggerExecutesComponent();
    UnsupportedTaskDependencyIsRejected();
    return 0;
}
