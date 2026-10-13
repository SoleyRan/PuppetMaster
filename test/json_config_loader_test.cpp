#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>

#include <puppet_master/configuration/json_loader.h>
#include <puppet_master/runtime/context.h>

namespace config = puppet_master::configuration;
namespace core = puppet_master::core;
namespace runtime = puppet_master::runtime;

namespace {

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

void OmittedSchedulingFieldsUseDefaults()
{
    auto result = config::ParseJsonConfig(
        R"json({"components":[{"name":"monitor","triggers":[{"kind":"manual"}]}]})json");
    assert(result.ok());
    assert(result.value().ToSchedulerOptions().value().worker_count == 1);
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
    OmittedSchedulingFieldsUseDefaults();
    InvalidJsonIsRejected();
    JsonFileIsLoaded();
    return 0;
}
