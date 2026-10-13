#include <puppet_master/configuration/json_loader.h>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include <nlohmann/json.hpp>

namespace puppet_master::configuration {
namespace {

using Json = nlohmann::json;

void CheckObject(const Json& value, const std::string& path,
                 std::initializer_list<std::string_view> fields)
{
    if (!value.is_object()) {
        throw std::invalid_argument(path + " must be an object");
    }
    for (auto it = value.begin(); it != value.end(); ++it) {
        if (std::find(fields.begin(), fields.end(), it.key()) == fields.end()) {
            throw std::invalid_argument(path + "." + it.key() + " is unknown");
        }
    }
}

template <typename T>
void Read(const Json& object, const char* key, T& output, const std::string& path)
{
    auto found = object.find(key);
    if (found == object.end()) {
        return;
    }
    const auto field = path + "." + key;
    if constexpr (std::is_same_v<T, std::string>) {
        if (!found->is_string()) {
            throw std::invalid_argument(field + " must be a string");
        }
    } else if constexpr (std::is_same_v<T, bool>) {
        if (!found->is_boolean()) {
            throw std::invalid_argument(field + " must be a boolean");
        }
    } else if constexpr (std::is_integral_v<T>) {
        if (!found->is_number_integer()) {
            throw std::invalid_argument(field + " must be an integer");
        }
        if (found->is_number_unsigned()) {
            if constexpr (std::is_signed_v<T>) {
                if (found->get<std::uint64_t>() > static_cast<std::uint64_t>(std::numeric_limits<T>::max())) {
                    throw std::invalid_argument(field + " is out of range");
                }
            } else if (found->get<std::uint64_t>() > std::numeric_limits<T>::max()) {
                throw std::invalid_argument(field + " is out of range");
            }
        } else {
            const auto number = found->get<std::int64_t>();
            if constexpr (std::is_unsigned_v<T>) {
                if (number < 0 || static_cast<std::uint64_t>(number) > std::numeric_limits<T>::max()) {
                    throw std::invalid_argument(field + " is out of range");
                }
            } else if (number < std::numeric_limits<T>::min() || number > std::numeric_limits<T>::max()) {
                throw std::invalid_argument(field + " is out of range");
            }
        }
    } else {
        if (!found->is_array() || !std::all_of(found->begin(), found->end(),
                [](const Json& item) { return item.is_string(); })) {
            throw std::invalid_argument(field + " must be an array of strings");
        }
    }
    output = found->get<T>();
}

template <typename T>
void ReadEnum(const Json& object, const char* key, T& output,
              core::Result<T> (*parse)(std::string_view), const std::string& path)
{
    if (!object.contains(key)) {
        return;
    }
    std::string text;
    Read(object, key, text, path);
    auto result = parse(text);
    if (!result.ok()) {
        throw std::invalid_argument(path + "." + key + ": " + result.status().message());
    }
    output = result.value();
}

const Json& Array(const Json& object, const char* key, const std::string& path)
{
    static const Json empty = Json::array();
    const auto found = object.find(key);
    if (found == object.end()) {
        return empty;
    }
    if (!found->is_array()) {
        throw std::invalid_argument(path + "." + key + " must be an array");
    }
    return *found;
}

ProjectConfig Decode(const Json& root)
{
    CheckObject(root, "$", {"runtime", "scheduler", "topics", "components"});
    ProjectConfig project;
    if (root.contains("runtime")) {
        const auto& options = root.at("runtime");
        CheckObject(options, "$.runtime", {"default_transport_name", "register_inmemory_transport",
                                         "open_registered_transports"});
        std::string name = project.runtime_options.default_transport_name.str();
        Read(options, "default_transport_name", name, "$.runtime");
        auto parsed_name = core::TransportName::Create(std::move(name));
        if (!parsed_name.ok()) {
            throw std::invalid_argument("$.runtime.default_transport_name: " + parsed_name.status().message());
        }
        project.runtime_options.default_transport_name = std::move(parsed_name).value();
        Read(options, "register_inmemory_transport", project.runtime_options.register_inmemory_transport, "$.runtime");
        Read(options, "open_registered_transports", project.runtime_options.open_registered_transports, "$.runtime");
    }
    if (root.contains("scheduler")) {
        const auto& options = root.at("scheduler");
        CheckObject(options, "$.scheduler", {"worker_count"});
        Read(options, "worker_count", project.scheduler_options.worker_count, "$.scheduler");
    }
    std::size_t index = 0;
    for (const auto& value : Array(root, "topics", "$")) {
        const auto path = "$.topics[" + std::to_string(index++) + "]";
        CheckObject(value, path, {"id", "name", "transport", "message_type", "encoding", "message_policy"});
        TopicConfig topic;
        Read(value, "id", topic.id, path);
        Read(value, "name", topic.name, path);
        Read(value, "message_type", topic.message_type, path);
        Read(value, "encoding", topic.encoding, path);
        ReadEnum(value, "transport", topic.transport, ParseTransportKind, path);
        if (value.contains("message_policy")) {
            const auto& policy = value.at("message_policy");
            const auto policy_path = path + ".message_policy";
            CheckObject(policy, policy_path, {"delivery", "retention", "freshness", "overflow", "queue_depth"});
            ReadEnum(policy, "delivery", topic.message_policy.delivery, ParseDeliveryGuarantee, policy_path);
            ReadEnum(policy, "retention", topic.message_policy.retention, ParseRetentionPolicy, policy_path);
            ReadEnum(policy, "freshness", topic.message_policy.freshness, ParseFreshnessPolicy, policy_path);
            ReadEnum(policy, "overflow", topic.message_policy.overflow, ParseQueueOverflowPolicy, policy_path);
            Read(policy, "queue_depth", topic.message_policy.queue_depth, policy_path);
        }
        project.topics.push_back(std::move(topic));
    }
    index = 0;
    for (const auto& value : Array(root, "components", "$")) {
        const auto path = "$.components[" + std::to_string(index++) + "]";
        CheckObject(value, path, {"name", "description", "readers", "writers", "triggers"});
        ComponentConfig component;
        Read(value, "name", component.name, path);
        Read(value, "description", component.description, path);
        Read(value, "readers", component.readers, path);
        Read(value, "writers", component.writers, path);
        std::size_t trigger_index = 0;
        for (const auto& entry : Array(value, "triggers", path)) {
            const auto trigger_path = path + ".triggers[" + std::to_string(trigger_index++) + "]";
            CheckObject(entry, trigger_path, {"kind", "period_ns", "dependency_policy", "data_topics", "task_dependencies", "priority", "deadline_ns"});
            TriggerConfig trigger;
            ReadEnum(entry, "kind", trigger.kind, ParseTriggerKind, trigger_path);
            auto period = trigger.period.count();
            Read(entry, "period_ns", period, trigger_path);
            trigger.period = core::Nanoseconds(period);
            Read(entry, "priority", trigger.priority, trigger_path);
            auto deadline = trigger.deadline.count();
            Read(entry, "deadline_ns", deadline, trigger_path);
            trigger.deadline = core::Nanoseconds(deadline);
            ReadEnum(entry, "dependency_policy", trigger.dependency_policy, ParseDependencyPolicy, trigger_path);
            Read(entry, "data_topics", trigger.data_topics, trigger_path);
            Read(entry, "task_dependencies", trigger.task_dependencies, trigger_path);
            component.triggers.push_back(std::move(trigger));
        }
        project.components.push_back(std::move(component));
    }
    return project;
}

}  // namespace

core::Result<ProjectConfig> ParseJsonConfig(std::string_view json)
{
    try {
        auto project = Decode(Json::parse(json.begin(), json.end()));
        auto status = project.Validate();
        if (!status.ok()) {
            return core::Result<ProjectConfig>::FromStatus(std::move(status));
        }
        return project;
    } catch (const Json::exception& error) {
        return core::Result<ProjectConfig>::FromStatus(core::Status::InvalidArgument(error.what()));
    } catch (const std::invalid_argument& error) {
        return core::Result<ProjectConfig>::FromStatus(core::Status::InvalidArgument(error.what()));
    }
}

core::Result<ProjectConfig> LoadJsonConfig(const std::string& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        return core::Result<ProjectConfig>::FromStatus(core::Status::Unavailable("cannot open configuration file: " + path));
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    if (input.bad()) {
        return core::Result<ProjectConfig>::FromStatus(core::Status::DataLoss("cannot read configuration file: " + path));
    }
    return ParseJsonConfig(buffer.str());
}

}  // namespace puppet_master::configuration
