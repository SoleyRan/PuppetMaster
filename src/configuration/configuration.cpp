#include <puppet_master/configuration/configuration.h>

#include <algorithm>
#include <cctype>
#include <map>
#include <string>
#include <utility>

namespace puppet_master::configuration {

namespace {

std::string Normalize(std::string_view value)
{
    auto begin = value.begin();
    auto end = value.end();

    while (begin != end && std::isspace(static_cast<unsigned char>(*begin)) != 0) {
        ++begin;
    }

    while (begin != end && std::isspace(static_cast<unsigned char>(*(end - 1))) != 0) {
        --end;
    }

    std::string normalized(begin, end);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return normalized;
}

core::Status ValidateConfigId(std::string_view kind, std::string_view value)
{
    return core::ValidateName(kind, value);
}

template <typename T>
core::Result<T> ParseEnum(
    std::string_view value,
    const std::map<std::string, T>& mapping,
    const char* kind)
{
    const auto normalized = Normalize(value);
    const auto found = mapping.find(normalized);
    if (found == mapping.end()) {
        return core::Result<T>::FromStatus(
            core::Status::InvalidArgument(std::string("unknown ") + kind + ": " + normalized));
    }

    return found->second;
}

core::Status ValidateUniqueIds(const std::vector<TopicConfig>& topics)
{
    std::map<std::string, bool> seen;
    for (const auto& topic : topics) {
        if (seen.find(topic.id) != seen.end()) {
            return core::Status::AlreadyExists("topic id is duplicated: " + topic.id);
        }
        seen.emplace(topic.id, true);
    }
    return core::Status::Ok();
}

core::Status ValidateUniqueComponents(const std::vector<ComponentConfig>& components)
{
    std::map<std::string, bool> seen;
    for (const auto& component : components) {
        if (seen.find(component.name) != seen.end()) {
            return core::Status::AlreadyExists("component name is duplicated: " + component.name);
        }
        seen.emplace(component.name, true);
    }
    return core::Status::Ok();
}

}  // namespace

core::Status TopicConfig::Validate() const
{
    auto status = ValidateConfigId("topic id", id);
    if (!status.ok()) {
        return status;
    }

    auto topic_name = core::TopicName::Create(name);
    if (!topic_name.ok()) {
        return topic_name.status();
    }

    if (message_type.empty()) {
        return core::Status::InvalidArgument("topic message_type must not be empty: " + id);
    }

    if (encoding.empty()) {
        return core::Status::InvalidArgument("topic encoding must not be empty: " + id);
    }

    return message_policy.Validate();
}

core::Result<transport::EndpointConfig> TopicConfig::ToEndpointConfig() const
{
    auto status = Validate();
    if (!status.ok()) {
        return core::Result<transport::EndpointConfig>::FromStatus(std::move(status));
    }

    auto topic_name = core::TopicName::Create(name);
    if (!topic_name.ok()) {
        return core::Result<transport::EndpointConfig>::FromStatus(topic_name.status());
    }

    transport::EndpointConfig endpoint {
        core::TopicSpec {topic_name.value(), transport, message_policy},
        transport::MessageDescriptor {message_type, encoding}
    };

    status = endpoint.Validate();
    if (!status.ok()) {
        return core::Result<transport::EndpointConfig>::FromStatus(std::move(status));
    }

    return endpoint;
}

core::Status TriggerConfig::Validate(const ProjectConfig& project) const
{
    auto trigger = ToTriggerSpec(project);
    if (!trigger.ok()) {
        return trigger.status();
    }
    return trigger.value().Validate();
}

core::Result<core::TriggerSpec> TriggerConfig::ToTriggerSpec(const ProjectConfig& project) const
{
    if (kind != core::TriggerKind::kPeriodic && period != core::Nanoseconds::zero()) {
        return core::Result<core::TriggerSpec>::FromStatus(
            core::Status::InvalidArgument("trigger period can only be used with periodic triggers"));
    }

    if (kind != core::TriggerKind::kData && !data_topics.empty()) {
        return core::Result<core::TriggerSpec>::FromStatus(
            core::Status::InvalidArgument("trigger data_topics can only be used with data triggers"));
    }

    if (kind != core::TriggerKind::kTaskDependency && !task_dependencies.empty()) {
        return core::Result<core::TriggerSpec>::FromStatus(
            core::Status::InvalidArgument(
                "trigger task_dependencies can only be used with task dependency triggers"));
    }

    if (kind == core::TriggerKind::kTaskDependency && task_dependencies.empty()) {
        return core::Result<core::TriggerSpec>::FromStatus(
            core::Status::InvalidArgument(
                "task dependency trigger requires at least one task dependency"));
    }

    core::TriggerSpec trigger;
    trigger.kind = kind;
    trigger.period = period;
    trigger.dependency_policy = dependency_policy;

    if (kind == core::TriggerKind::kData && data_topics.empty()) {
        return core::Result<core::TriggerSpec>::FromStatus(
            core::Status::InvalidArgument("data trigger requires at least one topic id"));
    }

    for (const auto& topic_id : data_topics) {
        auto topic = project.FindTopic(topic_id);
        if (!topic.ok()) {
            return core::Result<core::TriggerSpec>::FromStatus(topic.status());
        }

        auto topic_name = core::TopicName::Create(topic.value().name);
        if (!topic_name.ok()) {
            return core::Result<core::TriggerSpec>::FromStatus(topic_name.status());
        }
        trigger.data_dependencies.push_back(topic_name.value());
    }

    for (const auto& task_name_value : task_dependencies) {
        auto task_name = core::TaskName::Create(task_name_value);
        if (!task_name.ok()) {
            return core::Result<core::TriggerSpec>::FromStatus(task_name.status());
        }
        trigger.task_dependencies.push_back(task_name.value());
    }

    auto status = trigger.Validate();
    if (!status.ok()) {
        return core::Result<core::TriggerSpec>::FromStatus(std::move(status));
    }

    return trigger;
}

core::Status ComponentConfig::Validate(const ProjectConfig& project) const
{
    auto spec = ToComponentSpec(project);
    if (!spec.ok()) {
        return spec.status();
    }

    auto status = spec.value().Validate();
    if (!status.ok()) {
        return status;
    }

    for (const auto& trigger : triggers) {
        if (trigger.kind != core::TriggerKind::kData) {
            continue;
        }

        for (const auto& topic_id : trigger.data_topics) {
            const auto found = std::find(readers.begin(), readers.end(), topic_id);
            if (found == readers.end()) {
                return core::Status::InvalidArgument(
                    "component data trigger topic must also be declared as a reader: " + topic_id);
            }
        }
    }

    return core::Status::Ok();
}

core::Result<runtime::ComponentSpec> ComponentConfig::ToComponentSpec(const ProjectConfig& project) const
{
    auto component_name = core::ComponentName::Create(name);
    if (!component_name.ok()) {
        return core::Result<runtime::ComponentSpec>::FromStatus(component_name.status());
    }

    runtime::ComponentSpec spec {component_name.value(), description, {}, {}, {}};

    for (const auto& topic_id : readers) {
        auto endpoint = project.BuildEndpoint(topic_id);
        if (!endpoint.ok()) {
            return core::Result<runtime::ComponentSpec>::FromStatus(endpoint.status());
        }
        spec.readers.push_back(endpoint.value());
    }

    for (const auto& topic_id : writers) {
        auto endpoint = project.BuildEndpoint(topic_id);
        if (!endpoint.ok()) {
            return core::Result<runtime::ComponentSpec>::FromStatus(endpoint.status());
        }
        spec.writers.push_back(endpoint.value());
    }

    for (const auto& trigger_config : triggers) {
        auto trigger = trigger_config.ToTriggerSpec(project);
        if (!trigger.ok()) {
            return core::Result<runtime::ComponentSpec>::FromStatus(trigger.status());
        }
        spec.triggers.push_back(trigger.value());
    }

    auto status = spec.Validate();
    if (!status.ok()) {
        return core::Result<runtime::ComponentSpec>::FromStatus(std::move(status));
    }

    return spec;
}

core::Status ProjectConfig::AddTopic(TopicConfig topic)
{
    auto status = topic.Validate();
    if (!status.ok()) {
        return status;
    }

    if (FindTopic(topic.id).ok()) {
        return core::Status::AlreadyExists("topic id is duplicated: " + topic.id);
    }

    topics.push_back(std::move(topic));
    return core::Status::Ok();
}

core::Status ProjectConfig::AddComponent(ComponentConfig component)
{
    auto component_name = core::ComponentName::Create(component.name);
    if (!component_name.ok()) {
        return component_name.status();
    }

    if (FindComponent(component.name).ok()) {
        return core::Status::AlreadyExists("component name is duplicated: " + component.name);
    }

    components.push_back(std::move(component));
    return core::Status::Ok();
}

core::Status ProjectConfig::Validate() const
{
    auto status = runtime_options.Validate();
    if (!status.ok()) {
        return status;
    }

    status = ValidateUniqueIds(topics);
    if (!status.ok()) {
        return status;
    }

    status = ValidateUniqueComponents(components);
    if (!status.ok()) {
        return status;
    }

    for (const auto& topic : topics) {
        status = topic.Validate();
        if (!status.ok()) {
            return status;
        }
    }

    for (const auto& component : components) {
        status = component.Validate(*this);
        if (!status.ok()) {
            return status;
        }
    }

    return core::Status::Ok();
}

core::Result<runtime::RuntimeOptions> ProjectConfig::ToRuntimeOptions() const
{
    auto status = runtime_options.Validate();
    if (!status.ok()) {
        return core::Result<runtime::RuntimeOptions>::FromStatus(std::move(status));
    }
    return runtime_options;
}

core::Result<TopicConfig> ProjectConfig::FindTopic(std::string_view id) const
{
    const std::string key(id.data(), id.size());
    const auto found = std::find_if(topics.begin(), topics.end(), [&key](const auto& topic) {
        return topic.id == key;
    });

    if (found == topics.end()) {
        return core::Result<TopicConfig>::FromStatus(
            core::Status::NotFound("topic id is not configured: " + key));
    }

    return *found;
}

core::Result<ComponentConfig> ProjectConfig::FindComponent(std::string_view name) const
{
    const std::string key(name.data(), name.size());
    const auto found = std::find_if(components.begin(), components.end(), [&key](const auto& component) {
        return component.name == key;
    });

    if (found == components.end()) {
        return core::Result<ComponentConfig>::FromStatus(
            core::Status::NotFound("component is not configured: " + key));
    }

    return *found;
}

core::Result<transport::EndpointConfig> ProjectConfig::BuildEndpoint(std::string_view topic_id) const
{
    auto topic = FindTopic(topic_id);
    if (!topic.ok()) {
        return core::Result<transport::EndpointConfig>::FromStatus(topic.status());
    }

    return topic.value().ToEndpointConfig();
}

core::Result<runtime::ComponentSpec> ProjectConfig::BuildComponentSpec(
    std::string_view component_name) const
{
    auto component = FindComponent(component_name);
    if (!component.ok()) {
        return core::Result<runtime::ComponentSpec>::FromStatus(component.status());
    }

    return component.value().ToComponentSpec(*this);
}

core::Status ApplyComponentSpecs(const ProjectConfig& project, runtime::RuntimeContext& runtime)
{
    auto status = project.Validate();
    if (!status.ok()) {
        return status;
    }

    for (const auto& component : project.components) {
        auto spec = component.ToComponentSpec(project);
        if (!spec.ok()) {
            return spec.status();
        }

        status = runtime.RegisterComponent(spec.value());
        if (!status.ok()) {
            return status;
        }
    }

    return core::Status::Ok();
}

core::Result<core::TransportKind> ParseTransportKind(std::string_view value)
{
    return ParseEnum<core::TransportKind>(
        value,
        {
            {"inmemory", core::TransportKind::kInMemory},
            {"in_memory", core::TransportKind::kInMemory},
            {"fastdds", core::TransportKind::kFastDds},
            {"fast_dds", core::TransportKind::kFastDds},
            {"zmq", core::TransportKind::kZmq},
            {"ipc", core::TransportKind::kIpc},
        },
        "transport kind");
}

core::Result<core::TriggerKind> ParseTriggerKind(std::string_view value)
{
    return ParseEnum<core::TriggerKind>(
        value,
        {
            {"manual", core::TriggerKind::kManual},
            {"periodic", core::TriggerKind::kPeriodic},
            {"data", core::TriggerKind::kData},
            {"task", core::TriggerKind::kTaskDependency},
            {"task_dependency", core::TriggerKind::kTaskDependency},
        },
        "trigger kind");
}

core::Result<core::DeliveryGuarantee> ParseDeliveryGuarantee(std::string_view value)
{
    return ParseEnum<core::DeliveryGuarantee>(
        value,
        {
            {"best_effort", core::DeliveryGuarantee::kBestEffort},
            {"besteffort", core::DeliveryGuarantee::kBestEffort},
            {"reliable", core::DeliveryGuarantee::kReliable},
        },
        "delivery guarantee");
}

core::Result<core::RetentionPolicy> ParseRetentionPolicy(std::string_view value)
{
    return ParseEnum<core::RetentionPolicy>(
        value,
        {
            {"keep_last", core::RetentionPolicy::kKeepLast},
            {"keeplast", core::RetentionPolicy::kKeepLast},
            {"keep_all", core::RetentionPolicy::kKeepAll},
            {"keepall", core::RetentionPolicy::kKeepAll},
        },
        "retention policy");
}

core::Result<core::FreshnessPolicy> ParseFreshnessPolicy(std::string_view value)
{
    return ParseEnum<core::FreshnessPolicy>(
        value,
        {
            {"latest", core::FreshnessPolicy::kLatest},
            {"queued", core::FreshnessPolicy::kQueued},
        },
        "freshness policy");
}

core::Result<core::QueueOverflowPolicy> ParseQueueOverflowPolicy(std::string_view value)
{
    return ParseEnum<core::QueueOverflowPolicy>(
        value,
        {
            {"drop_oldest", core::QueueOverflowPolicy::kDropOldest},
            {"dropoldest", core::QueueOverflowPolicy::kDropOldest},
            {"drop_newest", core::QueueOverflowPolicy::kDropNewest},
            {"dropnewest", core::QueueOverflowPolicy::kDropNewest},
            {"block", core::QueueOverflowPolicy::kBlock},
            {"reject", core::QueueOverflowPolicy::kReject},
        },
        "queue overflow policy");
}

core::Result<core::DependencyPolicy> ParseDependencyPolicy(std::string_view value)
{
    return ParseEnum<core::DependencyPolicy>(
        value,
        {
            {"all", core::DependencyPolicy::kAll},
            {"any", core::DependencyPolicy::kAny},
        },
        "dependency policy");
}

}  // namespace puppet_master::configuration
