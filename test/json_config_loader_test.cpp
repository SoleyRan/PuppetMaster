#include <atomic>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <utility>

#include <puppet_master/configuration/json_loader.h>
#include <puppet_master/runtime/context.h>

namespace config = puppet_master::configuration;
namespace core = puppet_master::core;
namespace runtime = puppet_master::runtime;
namespace transport = puppet_master::transport;

namespace {

class SpeedMonitor final : public runtime::Component {
public:
    explicit SpeedMonitor(runtime::ComponentSpec spec) : spec_(std::move(spec)) {}
    runtime::ComponentSpec Describe() const override { return spec_; }
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
        ++executions_;
        return core::Status::Ok();
    }
    int executions() const { return executions_.load(); }
private:
    runtime::ComponentSpec spec_;
    transport::ReaderPtr reader_;
    std::atomic<int> executions_ {0};
};

class CountingComponent final : public runtime::Component {
public:
    explicit CountingComponent(runtime::ComponentSpec spec) : spec_(std::move(spec)) {}
    runtime::ComponentSpec Describe() const override { return spec_; }
    core::Status Execute(runtime::ComponentContext&) override
    {
        ++executions_;
        return core::Status::Ok();
    }
    int executions() const { return executions_.load(); }
private:
    runtime::ComponentSpec spec_;
    std::atomic<int> executions_ {0};
};

const char* kValidConfig = R"json({
    "runtime": {
        "default_transport_name": "runtime",
        "register_inmemory_transport": true,
        "open_registered_transports": false
    },
    "scheduler": {"worker_count": 3},
    "topics": [{
        "id": "speed",
        "name": "/vehicle/speed",
        "transport": "in_memory",
        "message_type": "demo.Speed",
        "encoding": "text/plain",
        "message_policy": {
            "freshness": "queued",
            "queue_depth": 8
        }
    }],
    "components": [{
        "name": "speed_monitor",
        "type": "speed",
        "description": "runs when speed changes",
        "readers": ["speed"],
        "triggers": [{
            "kind": "data",
            "dependency_policy": "any",
            "data_topics": ["speed"],
            "priority": -7,
            "deadline_ns": 1250
        }]
    }]
})json";

void ValidJsonBuildsProject()
{
    auto result = config::ParseJsonConfig(kValidConfig);
    assert(result.ok());
    const auto& project = result.value();
    assert(project.runtime_options.default_transport_name.str() == "runtime");
    assert(!project.runtime_options.open_registered_transports);
    assert(project.ToSchedulerOptions().ok());
    assert(project.ToSchedulerOptions().value().worker_count == 3);
    assert(project.topics.size() == 1);
    assert(project.topics.front().message_policy.freshness == core::FreshnessPolicy::kQueued);
    assert(project.topics.front().message_policy.queue_depth == 8);
    assert(project.components.size() == 1);
    assert(project.components.front().type == "speed");
    assert(project.components.front().triggers.front().kind == core::TriggerKind::kData);
    assert(project.components.front().triggers.front().priority == -7);
    assert(project.components.front().triggers.front().deadline == core::Nanoseconds(1250));
    auto endpoint = project.BuildEndpoint("speed");
    assert(endpoint.ok());
    auto component = project.BuildComponentSpec("speed_monitor");
    assert(component.ok());
    assert(component.value().triggers.front().priority == -7);
    assert(component.value().triggers.front().deadline == core::Nanoseconds(1250));
    assert(component.value().triggers.front().data_dependencies.front().str() == "/vehicle/speed");
}

void JsonConfigCreatesConfiguredScheduler()
{
    auto parsed = config::ParseJsonConfig(kValidConfig);
    assert(parsed.ok());
    auto runtime_options = parsed.value().ToRuntimeOptions();
    assert(runtime_options.ok());
    auto context = runtime::RuntimeContext::Create(runtime_options.value());
    assert(context.ok());

    auto scheduler_instance = config::CreateScheduler(parsed.value(), *context.value());
    assert(scheduler_instance.ok());
    assert(scheduler_instance.value()->stats().registered_components == 1);

    auto name = core::ComponentName::Create("speed_monitor");
    assert(name.ok());
    auto spec = context.value()->FindComponent(name.value());
    assert(spec.ok());
    assert(spec.value().triggers.front().priority == -7);
    assert(spec.value().triggers.front().deadline == core::Nanoseconds(1250));
    assert(!context.value()->FindComponentInstance(name.value()).ok());
    assert(scheduler_instance.value()->Start().ok());
    assert(scheduler_instance.value()->is_running());
    assert(scheduler_instance.value()->Stop().ok());
}

void JsonConfigRunsFactoryComponent()
{
    auto parsed = config::ParseJsonConfig(kValidConfig);
    assert(parsed.ok());
    auto context = runtime::RuntimeContext::Create(parsed.value().ToRuntimeOptions().value());
    assert(context.ok());
    std::shared_ptr<SpeedMonitor> monitor;
    config::ComponentFactoryRegistry factories;
    assert(factories.Register("speed",
        [&monitor](const runtime::ComponentSpec& spec) -> core::Result<runtime::ComponentPtr> {
            monitor = std::make_shared<SpeedMonitor>(spec);
            runtime::ComponentPtr component = monitor;
            return component;
        }).ok());
    auto sched = config::CreateScheduler(parsed.value(), *context.value(), factories);
    assert(sched.ok());
    auto name = core::ComponentName::Create("speed_monitor").value();
    assert(context.value()->FindComponentInstance(name).ok());
    assert(context.value()->ConfigureComponent(name).ok());
    assert(context.value()->InitializeComponent(name).ok());
    assert(context.value()->StartComponent(name).ok());
    assert(sched.value()->Start().ok());
    auto writer = context.value()->CreateWriter(parsed.value().BuildEndpoint("speed").value());
    assert(writer.ok());
    const std::string payload = "speed=12.5";
    assert(writer.value()->Write(transport::ByteView::From(payload.data(), payload.size())).ok());
    assert(sched.value()->WaitIdle(std::chrono::milliseconds(500)).ok());
    assert(monitor->executions() == 1);
    assert(sched.value()->Stop().ok());
    assert(context.value()->StopComponent(name).ok());
    assert(context.value()->ShutdownComponent(name).ok());
}

void JsonConfigRunsForwardDeclaredTaskDependency()
{
    auto parsed = config::ParseJsonConfig(R"json({
        "components": [
            {"name":"downstream","type":"counter","triggers":[
                {"kind":"task","task_dependencies":["upstream"]}]},
            {"name":"upstream","type":"counter","triggers":[{"kind":"manual"}]}
        ]
    })json");
    assert(parsed.ok());
    auto context = runtime::RuntimeContext::Create(parsed.value().ToRuntimeOptions().value());
    assert(context.ok());
    std::shared_ptr<CountingComponent> upstream;
    std::shared_ptr<CountingComponent> downstream;
    config::ComponentFactoryRegistry factories;
    assert(factories.Register("counter",
        [&upstream, &downstream](const runtime::ComponentSpec& spec) -> core::Result<runtime::ComponentPtr> {
            auto instance = std::make_shared<CountingComponent>(spec);
            if (spec.name.str() == "upstream") {
                upstream = instance;
            } else {
                downstream = instance;
            }
            runtime::ComponentPtr component = instance;
            return component;
        }).ok());
    auto sched = config::CreateScheduler(parsed.value(), *context.value(), factories);
    assert(sched.ok());
    assert(upstream && downstream);
    auto upstream_name = core::ComponentName::Create("upstream").value();
    auto downstream_name = core::ComponentName::Create("downstream").value();
    for (const auto& name : {downstream_name, upstream_name}) {
        assert(context.value()->ConfigureComponent(name).ok());
        assert(context.value()->InitializeComponent(name).ok());
        assert(context.value()->StartComponent(name).ok());
    }
    assert(sched.value()->Start().ok());
    assert(sched.value()->Trigger(upstream_name).ok());
    assert(sched.value()->WaitIdle(std::chrono::milliseconds(500)).ok());
    assert(upstream->executions() == 1);
    assert(downstream->executions() == 1);
    assert(sched.value()->Stop().ok());
    for (const auto& name : {downstream_name, upstream_name}) {
        assert(context.value()->StopComponent(name).ok());
        assert(context.value()->ShutdownComponent(name).ok());
    }
}

void OmittedSchedulingFieldsUseDefaults()
{
    auto result = config::ParseJsonConfig(
        R"json({"components":[{"name":"monitor","triggers":[{"kind":"manual"}]}]})json");
    assert(result.ok());
    assert(result.value().ToSchedulerOptions().value().worker_count == 1);
    assert(result.value().components.front().type.empty());
    auto context = runtime::RuntimeContext::Create();
    assert(context.ok());
    assert(config::CreateScheduler(result.value(), *context.value()).ok());
    auto spec = result.value().BuildComponentSpec("monitor");
    assert(spec.ok());
    assert(spec.value().triggers.front().priority == 0);
    assert(spec.value().triggers.front().deadline == core::Nanoseconds::zero());
}

void InvalidJsonIsRejected()
{
    auto unknown = config::ParseJsonConfig(R"json({"unexpected": true})json");
    assert(!unknown.ok());
    assert(unknown.status().code() == core::StatusCode::kInvalidArgument);

    auto wrong_type = config::ParseJsonConfig(R"json({"topics": "not-an-array"})json");
    assert(!wrong_type.ok());

    auto wrong_component_type = config::ParseJsonConfig(
        R"json({"components":[{"name":"monitor","type":12}]})json");
    assert(!wrong_component_type.ok());
    assert(wrong_component_type.status().code() == core::StatusCode::kInvalidArgument);

    auto out_of_range = config::ParseJsonConfig(
        R"json({"topics":[{"id":"speed","name":"/speed","message_type":"demo.Speed","message_policy":{"queue_depth":0}}]})json");
    assert(!out_of_range.ok());

    auto invalid_reference = config::ParseJsonConfig(
        R"json({"components":[{"name":"monitor","readers":["missing"]}]})json");
    assert(!invalid_reference.ok());
    assert(invalid_reference.status().code() == core::StatusCode::kNotFound);

    auto negative_deadline = config::ParseJsonConfig(
        R"json({"components":[{"name":"monitor","triggers":[{"kind":"manual","deadline_ns":-1}]}]})json");
    assert(!negative_deadline.ok());
    assert(negative_deadline.status().code() == core::StatusCode::kInvalidArgument);

    auto wrong_priority_type = config::ParseJsonConfig(
        R"json({"components":[{"name":"monitor","triggers":[{"kind":"manual","priority":"high"}]}]})json");
    assert(!wrong_priority_type.ok());

    auto priority_out_of_range = config::ParseJsonConfig(
        R"json({"components":[{"name":"monitor","triggers":[{"kind":"manual","priority":2147483648}]}]})json");
    assert(!priority_out_of_range.ok());

    auto deadline_out_of_range = config::ParseJsonConfig(
        R"json({"components":[{"name":"monitor","triggers":[{"kind":"manual","deadline_ns":9223372036854775808}]}]})json");
    assert(!deadline_out_of_range.ok());

    auto invalid_worker_count = config::ParseJsonConfig(
        R"json({"scheduler":{"worker_count":0}})json");
    assert(!invalid_worker_count.ok());
    assert(invalid_worker_count.status().code() == core::StatusCode::kInvalidArgument);

    auto wrong_worker_count_type = config::ParseJsonConfig(
        R"json({"scheduler":{"worker_count":"two"}})json");
    assert(!wrong_worker_count_type.ok());

    auto negative_worker_count = config::ParseJsonConfig(
        R"json({"scheduler":{"worker_count":-1}})json");
    assert(!negative_worker_count.ok());

    auto unknown_scheduler_field = config::ParseJsonConfig(
        R"json({"scheduler":{"unexpected":true}})json");
    assert(!unknown_scheduler_field.ok());

    auto unknown_trigger_field = config::ParseJsonConfig(
        R"json({"components":[{"name":"monitor","triggers":[{"kind":"manual","unknown":true}]}]})json");
    assert(!unknown_trigger_field.ok());

    for (const auto* json : {
             R"json({"components":[{"name":"a","triggers":[{"kind":"task","task_dependencies":["missing"]}]}]})json",
             R"json({"components":[{"name":"a","triggers":[{"kind":"task","task_dependencies":["a"]}]}]})json",
             R"json({"components":[{"name":"a","triggers":[{"kind":"task","task_dependencies":["b","b"]}]},{"name":"b","triggers":[{"kind":"manual"}]}]})json",
             R"json({"components":[{"name":"a","triggers":[{"kind":"task","task_dependencies":["b"]}]},{"name":"b","triggers":[{"kind":"task","task_dependencies":["a"]}]}]})json",
             R"json({"components":[{"name":"a","triggers":[{"kind":"task","task_dependencies":["b"]}]},{"name":"b","triggers":[{"kind":"task","task_dependencies":["c"]}]},{"name":"c","triggers":[{"kind":"task","task_dependencies":["a"]}]}]})json",
         }) {
        auto invalid_dependency = config::ParseJsonConfig(json);
        assert(!invalid_dependency.ok());
        assert(invalid_dependency.status().code() == core::StatusCode::kInvalidArgument);
    }

    auto distinct_triggers = config::ParseJsonConfig(
        R"json({"components":[{"name":"a","triggers":[{"kind":"task","task_dependencies":["upstream"]},{"kind":"task","task_dependencies":["upstream"]}]},{"name":"upstream","triggers":[{"kind":"manual"}]}]})json");
    assert(distinct_triggers.ok());
}

void JsonFileIsLoaded()
{
    const auto path = std::filesystem::temp_directory_path() / "puppet_master_json_config_test.json";
    {
        std::ofstream output(path);
        assert(output.is_open());
        output << kValidConfig;
    }

    auto result = config::LoadJsonConfig(path.string());
    std::filesystem::remove(path);
    assert(result.ok());
    assert(result.value().components.front().name == "speed_monitor");
}

}  // namespace

int main()
{
    ValidJsonBuildsProject();
    JsonConfigCreatesConfiguredScheduler();
    JsonConfigRunsFactoryComponent();
    JsonConfigRunsForwardDeclaredTaskDependency();
    OmittedSchedulingFieldsUseDefaults();
    InvalidJsonIsRejected();
    JsonFileIsLoaded();
    return 0;
}
