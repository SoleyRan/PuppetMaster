#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <puppet_master/core/message_policy.h>
#include <puppet_master/core/result.h>
#include <puppet_master/core/status.h>
#include <puppet_master/core/types.h>
#include <puppet_master/runtime/component.h>
#include <puppet_master/runtime/context.h>
#include <puppet_master/transport/transport.h>

namespace puppet_master::configuration {

struct ProjectConfig;

struct TopicConfig {
    std::string id;
    std::string name;
    core::TransportKind transport {core::TransportKind::kInMemory};
    std::string message_type;
    std::string encoding {"application/octet-stream"};
    core::MessagePolicy message_policy;

    core::Status Validate() const;
    core::Result<transport::EndpointConfig> ToEndpointConfig() const;
};

struct TriggerConfig {
    core::TriggerKind kind {core::TriggerKind::kManual};
    core::Nanoseconds period {0};
    core::DependencyPolicy dependency_policy {core::DependencyPolicy::kAll};
    std::vector<std::string> data_topics;
    std::vector<std::string> task_dependencies;

    core::Status Validate(const ProjectConfig& project) const;
    core::Result<core::TriggerSpec> ToTriggerSpec(const ProjectConfig& project) const;
};

struct ComponentConfig {
    std::string name;
    std::string description;
    std::vector<std::string> readers;
    std::vector<std::string> writers;
    std::vector<TriggerConfig> triggers;

    core::Status Validate(const ProjectConfig& project) const;
    core::Result<runtime::ComponentSpec> ToComponentSpec(const ProjectConfig& project) const;
};

// ProjectConfig is a code-first configuration model. It keeps the runtime
// usable before choosing a file format such as YAML, JSON, or TOML.
struct ProjectConfig {
    runtime::RuntimeOptions runtime_options;
    std::vector<TopicConfig> topics;
    std::vector<ComponentConfig> components;

    core::Status AddTopic(TopicConfig topic);
    core::Status AddComponent(ComponentConfig component);

    core::Status Validate() const;

    core::Result<runtime::RuntimeOptions> ToRuntimeOptions() const;
    core::Result<TopicConfig> FindTopic(std::string_view id) const;
    core::Result<ComponentConfig> FindComponent(std::string_view name) const;
    core::Result<transport::EndpointConfig> BuildEndpoint(std::string_view topic_id) const;
    core::Result<runtime::ComponentSpec> BuildComponentSpec(std::string_view component_name) const;
};

core::Status ApplyComponentSpecs(const ProjectConfig& project, runtime::RuntimeContext& runtime);

core::Result<core::TransportKind> ParseTransportKind(std::string_view value);
core::Result<core::TriggerKind> ParseTriggerKind(std::string_view value);
core::Result<core::DeliveryGuarantee> ParseDeliveryGuarantee(std::string_view value);
core::Result<core::RetentionPolicy> ParseRetentionPolicy(std::string_view value);
core::Result<core::FreshnessPolicy> ParseFreshnessPolicy(std::string_view value);
core::Result<core::QueueOverflowPolicy> ParseQueueOverflowPolicy(std::string_view value);
core::Result<core::DependencyPolicy> ParseDependencyPolicy(std::string_view value);

}  // namespace puppet_master::configuration
