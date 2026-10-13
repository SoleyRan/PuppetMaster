#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>

#include <puppet_master/configuration/json_loader.h>

namespace config = puppet_master::configuration;
namespace core = puppet_master::core;

namespace {

const char* kValidConfig = R"json({
    "runtime": {
        "default_transport_name": "runtime",
        "register_inmemory_transport": true,
        "open_registered_transports": false
    },
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
            "data_topics": ["speed"]
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
    assert(project.topics.size() == 1);
    assert(project.topics.front().message_policy.freshness == core::FreshnessPolicy::kQueued);
    assert(project.topics.front().message_policy.queue_depth == 8);
    assert(project.components.size() == 1);
    assert(project.components.front().triggers.front().kind == core::TriggerKind::kData);
    auto endpoint = project.BuildEndpoint("speed");
    assert(endpoint.ok());
    auto component = project.BuildComponentSpec("speed_monitor");
    assert(component.ok());
    assert(component.value().triggers.front().data_dependencies.front().str() == "/vehicle/speed");
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
    InvalidJsonIsRejected();
    JsonFileIsLoaded();
    return 0;
}
