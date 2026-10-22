#include <puppet_master/configuration/configuration.h>

#include <algorithm>
#include <cctype>
#include <deque>
#include <map>
#include <set>
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

core::Status ValidateTaskDependencies(const std::vector<ComponentConfig>& components)
{
    std::map<std::string, std::size_t> indegree;
    std::map<std::string, std::vector<std::string>> dependents;
    for (const auto& component : components) {
        indegree.emplace(component.name, 0);
    }

    for (const auto& component : components) {
        for (std::size_t index = 0; index < component.triggers.size(); ++index) {
            const auto& trigger = component.triggers[index];
            if (trigger.kind != core::TriggerKind::kTaskDependency) {
                continue;
            }
            std::set<std::string> seen;
            for (const auto& upstream : trigger.task_dependencies) {
                const auto location = "component " + component.name + " trigger " + std::to_string(index);
                if (upstream == component.name) {
                    return core::Status::InvalidArgument(location + " has self task dependency: " + upstream);
                }
                if (!seen.insert(upstream).second) {
                    return core::Status::InvalidArgument(location + " has duplicate task dependency: " + upstream);
                }
                if (indegree.find(upstream) == indegree.end()) {
                    return core::Status::InvalidArgument(location + " references unknown task dependency: " + upstream);
                }
                dependents[upstream].push_back(component.name);
                ++indegree[component.name];
            }
        }
    }

    std::deque<std::string> ready;
    for (const auto& entry : indegree) {
        if (entry.second == 0) {
            ready.push_back(entry.first);
        }
    }
    std::size_t visited = 0;
    while (!ready.empty()) {
        const auto upstream = ready.front();
        ready.pop_front();
        ++visited;
        for (const auto& dependent : dependents[upstream]) {
            if (--indegree[dependent] == 0) {
                ready.push_back(dependent);
            }
        }
    }
    if (visited != components.size()) {
        return core::Status::InvalidArgument("task dependency graph contains a cycle");
    }
    return core::Status::Ok();
}

bool SamePolicy(const core::MessagePolicy& lhs, const core::MessagePolicy& rhs)
{
    return lhs.delivery == rhs.delivery && lhs.retention == rhs.retention
        && lhs.freshness == rhs.freshness && lhs.overflow == rhs.overflow
        && lhs.queue_depth == rhs.queue_depth;
}

bool SameEndpoints(
    const std::vector<transport::EndpointConfig>& lhs,
    const std::vector<transport::EndpointConfig>& rhs)
{
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        if (lhs[i].topic.name != rhs[i].topic.name
            || lhs[i].topic.transport != rhs[i].topic.transport
            || !SamePolicy(lhs[i].topic.message_policy, rhs[i].topic.message_policy)
            || lhs[i].message != rhs[i].message) {
            return false;
        }
    }
    return true;
}

bool SameTriggers(const std::vector<core::TriggerSpec>& lhs, const std::vector<core::TriggerSpec>& rhs)
{
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        if (lhs[i].kind != rhs[i].kind || lhs[i].period != rhs[i].period
            || lhs[i].dependency_policy != rhs[i].dependency_policy
            || lhs[i].data_dependencies != rhs[i].data_dependencies
            || lhs[i].task_dependencies != rhs[i].task_dependencies
            || lhs[i].priority != rhs[i].priority || lhs[i].deadline != rhs[i].deadline) {
            return false;
        }
    }
    return true;
}

bool SameSpec(const runtime::ComponentSpec& lhs, const runtime::ComponentSpec& rhs)
{
    return lhs.name == rhs.name && lhs.description == rhs.description
        && SameEndpoints(lhs.readers, rhs.readers) && SameEndpoints(lhs.writers, rhs.writers)
        && SameTriggers(lhs.triggers, rhs.triggers);
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
        transport::MessageDescriptor {message_type, encoding, message_version}
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

    if (kind != core::TriggerKind::kData && kind != core::TriggerKind::kTaskDependency
        && dependency_policy != core::DependencyPolicy::kAll) {
        return core::Result<core::TriggerSpec>::FromStatus(core::Status::InvalidArgument(
            "trigger dependency_policy can only be used with data or task dependency triggers"));
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
    trigger.priority = priority;
    trigger.deadline = deadline;

    if (kind == core::TriggerKind::kData && data_topics.empty()) {
        return core::Result<core::TriggerSpec>::FromStatus(
            core::Status::InvalidArgument("data trigger requires at least one topic id"));
    }

    std::map<core::TopicName, std::string> seen_data_topics;
    for (const auto& topic_id : data_topics) {
        auto topic = project.FindTopic(topic_id);
        if (!topic.ok()) {
            return core::Result<core::TriggerSpec>::FromStatus(topic.status());
        }

        auto topic_name = core::TopicName::Create(topic.value().name);
        if (!topic_name.ok()) {
            return core::Result<core::TriggerSpec>::FromStatus(topic_name.status());
        }
        const auto inserted = seen_data_topics.emplace(topic_name.value(), topic_id);
        if (!inserted.second) {
            return core::Result<core::TriggerSpec>::FromStatus(core::Status::InvalidArgument(
                "data trigger has duplicate topic dependency ids " + inserted.first->second + " and " + topic_id
                + " for topic " + topic_name.value().str()));
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

    runtime::ComponentSpec spec {component_name.value(), description, {}, {}, {}, parameters};

    for (const auto& topic_id : readers) {
        auto endpoint = project.BuildEndpoint(topic_id);
        if (!endpoint.ok()) {
            return core::Result<runtime::ComponentSpec>::FromStatus(endpoint.status());
        }
        const auto policy = reader_policies.find(topic_id);
        if (policy != reader_policies.end()) {
            endpoint.value().topic.message_policy = policy->second;
        }
        spec.readers.push_back(endpoint.value());
    }

    for (const auto& [topic_id, policy] : reader_policies) {
        if (std::find(readers.begin(), readers.end(), topic_id) == readers.end()) {
            return core::Result<runtime::ComponentSpec>::FromStatus(core::Status::InvalidArgument(
                "component " + name + " sets a reader policy for " + topic_id + ", which it does not read"));
        }
        auto status = policy.Validate();
        if (!status.ok()) {
            return core::Result<runtime::ComponentSpec>::FromStatus(core::Status::Error(
                status.code(), "component " + name + " reader policy for " + topic_id + ": " + status.message()));
        }
    }

    for (const auto& topic_id : writers) {
        auto endpoint = project.BuildEndpoint(topic_id);
        if (!endpoint.ok()) {
            return core::Result<runtime::ComponentSpec>::FromStatus(endpoint.status());
        }
        spec.writers.push_back(endpoint.value());
    }

    std::map<core::TopicName, std::size_t> data_trigger_topics;
    for (std::size_t index = 0; index < triggers.size(); ++index) {
        auto trigger = triggers[index].ToTriggerSpec(project);
        if (!trigger.ok()) {
            return core::Result<runtime::ComponentSpec>::FromStatus(core::Status::Error(
                trigger.status().code(), "component " + name + " trigger " + std::to_string(index)
                    + ": " + trigger.status().message()));
        }
        spec.triggers.push_back(trigger.value());
        if (trigger.value().kind == core::TriggerKind::kData) {
            for (const auto& topic : trigger.value().data_dependencies) {
                const auto matches = std::count_if(spec.readers.begin(), spec.readers.end(),
                    [&topic](const auto& reader) { return reader.topic.name == topic; });
                if (matches > 1) {
                    return core::Result<runtime::ComponentSpec>::FromStatus(core::Status::InvalidArgument(
                        "component " + name + " trigger " + std::to_string(index)
                            + " has multiple reader endpoints for topic " + topic.str()));
                }
                const auto inserted = data_trigger_topics.emplace(topic, index);
                if (!inserted.second) {
                    return core::Result<runtime::ComponentSpec>::FromStatus(core::Status::InvalidArgument(
                        "component " + name + " triggers " + std::to_string(inserted.first->second)
                            + " and " + std::to_string(index) + " share data topic " + topic.str()));
                }
            }
        }
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

    status = scheduler_options.Validate();
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

    std::map<std::pair<core::TransportKind, std::string>, const TopicConfig*> declared_topics;
    for (const auto& topic : topics) {
        const auto key = std::make_pair(topic.transport, topic.name);
        const auto inserted = declared_topics.emplace(key, &topic);
        if (!inserted.second) {
            const auto& first = *inserted.first->second;
            if (first.message_type != topic.message_type || first.encoding != topic.encoding
                || first.message_version != topic.message_version) {
                return core::Status::InvalidArgument(
                    "topic ids " + first.id + " and " + topic.id + " declare conflicting message descriptors for "
                    + std::string(core::ToString(topic.transport)) + " topic " + topic.name);
            }
        }
    }

    for (const auto& component : components) {
        status = component.Validate(*this);
        if (!status.ok()) {
            return status;
        }
    }

    return ValidateTaskDependencies(components);
}

core::Result<runtime::RuntimeOptions> ProjectConfig::ToRuntimeOptions() const
{
    auto status = runtime_options.Validate();
    if (!status.ok()) {
        return core::Result<runtime::RuntimeOptions>::FromStatus(std::move(status));
    }
    return runtime_options;
}

core::Result<scheduler::SchedulerOptions> ProjectConfig::ToSchedulerOptions() const
{
    auto status = scheduler_options.Validate();
    if (!status.ok()) {
        return core::Result<scheduler::SchedulerOptions>::FromStatus(std::move(status));
    }
    return scheduler_options;
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

core::Result<std::unique_ptr<scheduler::Scheduler>> CreateScheduler(
    const ProjectConfig& project,
    runtime::RuntimeContext& runtime)
{
    auto status = project.Validate();
    if (!status.ok()) {
        return core::Result<std::unique_ptr<scheduler::Scheduler>>::FromStatus(std::move(status));
    }

    status = ApplyComponentSpecs(project, runtime);
    if (!status.ok()) {
        return core::Result<std::unique_ptr<scheduler::Scheduler>>::FromStatus(std::move(status));
    }

    auto options = project.ToSchedulerOptions();
    if (!options.ok()) {
        return core::Result<std::unique_ptr<scheduler::Scheduler>>::FromStatus(options.status());
    }

    auto scheduler_instance = std::make_unique<scheduler::Scheduler>(runtime, options.value());
    status = scheduler_instance->RegisterAllComponents();
    if (!status.ok()) {
        return core::Result<std::unique_ptr<scheduler::Scheduler>>::FromStatus(std::move(status));
    }

    return scheduler_instance;
}

core::Result<std::unique_ptr<scheduler::Scheduler>> CreateScheduler(
    const ProjectConfig& project,
    runtime::RuntimeContext& runtime,
    const ComponentFactory& factory)
{
    using SchedulerResult = core::Result<std::unique_ptr<scheduler::Scheduler>>;
    auto status = project.Validate();
    if (!status.ok()) {
        return SchedulerResult::FromStatus(std::move(status));
    }
    if (!factory) {
        return SchedulerResult::FromStatus(core::Status::InvalidArgument("component factory must not be empty"));
    }

    std::vector<runtime::ComponentPtr> instances;
    instances.reserve(project.components.size());
    for (const auto& component : project.components) {
        auto spec = component.ToComponentSpec(project);
        if (!spec.ok()) {
            return SchedulerResult::FromStatus(spec.status());
        }
        if (runtime.FindComponent(spec.value().name).ok()) {
            return SchedulerResult::FromStatus(
                core::Status::AlreadyExists("component is already registered: " + component.name));
        }
        auto instance = factory(spec.value());
        if (!instance.ok()) {
            return SchedulerResult::FromStatus(instance.status());
        }
        if (!instance.value()) {
            return SchedulerResult::FromStatus(
                core::Status::InvalidArgument("component factory returned null: " + component.name));
        }
        if (!SameSpec(spec.value(), instance.value()->Describe())) {
            return SchedulerResult::FromStatus(
                core::Status::InvalidArgument("component factory returned mismatched spec: " + component.name));
        }
        instances.push_back(std::move(instance.value()));
    }

    for (auto& instance : instances) {
        status = runtime.RegisterComponent(std::move(instance));
        if (!status.ok()) {
            return SchedulerResult::FromStatus(std::move(status));
        }
    }

    auto scheduler_instance = std::make_unique<scheduler::Scheduler>(runtime, project.scheduler_options);
    status = scheduler_instance->RegisterAllComponents();
    if (!status.ok()) {
        return SchedulerResult::FromStatus(std::move(status));
    }
    return scheduler_instance;
}

core::Status ComponentFactoryRegistry::Register(std::string type, ComponentFactory factory)
{
    auto status = ValidateConfigId("component type", type);
    if (!status.ok()) {
        return status;
    }
    if (!factory) {
        return core::Status::InvalidArgument("component factory must not be empty");
    }
    if (factories_.find(type) != factories_.end()) {
        return core::Status::AlreadyExists("component type is already registered: " + type);
    }
    factories_.emplace(std::move(type), std::move(factory));
    return core::Status::Ok();
}

core::Status ComponentFactoryRegistry::ValidateType(std::string_view type) const
{
    auto status = ValidateConfigId("component type", type);
    if (!status.ok()) {
        return status;
    }
    if (factories_.find(std::string(type)) == factories_.end()) {
        return core::Status::NotFound("component type is not registered: " + std::string(type));
    }
    return core::Status::Ok();
}

core::Result<runtime::ComponentPtr> ComponentFactoryRegistry::Create(
    std::string_view type, const runtime::ComponentSpec& spec) const
{
    auto status = ValidateType(type);
    if (!status.ok()) {
        return core::Result<runtime::ComponentPtr>::FromStatus(std::move(status));
    }
    return factories_.find(std::string(type))->second(spec);
}

core::Result<std::unique_ptr<scheduler::Scheduler>> CreateScheduler(
    const ProjectConfig& project,
    runtime::RuntimeContext& runtime,
    const ComponentFactoryRegistry& factories)
{
    using SchedulerResult = core::Result<std::unique_ptr<scheduler::Scheduler>>;
    auto status = project.Validate();
    if (!status.ok()) {
        return SchedulerResult::FromStatus(std::move(status));
    }
    for (const auto& component : project.components) {
        status = factories.ValidateType(component.type);
        if (!status.ok()) {
            return SchedulerResult::FromStatus(
                core::Status::Error(status.code(), "component " + component.name + ": " + status.message()));
        }
    }
    return CreateScheduler(project, runtime,
        [&project, &factories](const runtime::ComponentSpec& spec) -> core::Result<runtime::ComponentPtr> {
            auto component = project.FindComponent(spec.name.view());
            if (!component.ok()) {
                return core::Result<runtime::ComponentPtr>::FromStatus(component.status());
            }
            return factories.Create(component.value().type, spec);
        });
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
        if (runtime.FindComponent(spec.value().name).ok()) {
            return core::Status::AlreadyExists("component is already registered: " + component.name);
        }
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
            {"shm", core::TransportKind::kIpc},
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
