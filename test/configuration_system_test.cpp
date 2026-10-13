#include <cassert>
#include <string>

#include <puppet_master/puppet_master.h>

namespace config = puppet_master::configuration;
namespace core = puppet_master::core;
namespace runtime = puppet_master::runtime;

namespace {

config::ProjectConfig MakeSpeedProject()
{
    core::MessagePolicy queued_policy;
    queued_policy.freshness = core::FreshnessPolicy::kQueued;
    queued_policy.retention = core::RetentionPolicy::kKeepLast;
    queued_policy.queue_depth = 8;

    config::ProjectConfig project;
    assert(project.AddTopic(config::TopicConfig {
        "speed",
        "/vehicle/speed",
        core::TransportKind::kInMemory,
        "demo.Speed",
        "text/plain",
        queued_policy
    }).ok());

    config::TriggerConfig trigger;
    trigger.kind = core::TriggerKind::kData;
    trigger.dependency_policy = core::DependencyPolicy::kAny;
    trigger.data_topics = {"speed"};

    assert(project.AddComponent(config::ComponentConfig {
        "speed_monitor",
        "runs when a speed sample arrives",
        {"speed"},
        {},
        {trigger}
    }).ok());

    return project;
}

void EnumParsersAcceptHumanReadableValues()
{
    auto transport = config::ParseTransportKind(" FastDDS ");
    assert(transport.ok());
    assert(transport.value() == core::TransportKind::kFastDds);

    auto trigger = config::ParseTriggerKind("task_dependency");
    assert(trigger.ok());
    assert(trigger.value() == core::TriggerKind::kTaskDependency);

    auto freshness = config::ParseFreshnessPolicy("queued");
    assert(freshness.ok());
    assert(freshness.value() == core::FreshnessPolicy::kQueued);

    auto dependency = config::ParseDependencyPolicy("any");
    assert(dependency.ok());
    assert(dependency.value() == core::DependencyPolicy::kAny);

    auto invalid = config::ParseRetentionPolicy("forever");
    assert(!invalid.ok());
    assert(invalid.status().code() == core::StatusCode::kInvalidArgument);
}

void SchedulerOptionsAreValidatedAndExposed()
{
    config::ProjectConfig project;
    project.scheduler_options.worker_count = 3;
    auto options = project.ToSchedulerOptions();
    assert(options.ok());
    assert(options.value().worker_count == 3);
    assert(project.Validate().ok());

    project.scheduler_options.worker_count = 0;
    assert(!project.ToSchedulerOptions().ok());
    assert(project.Validate().code() == core::StatusCode::kInvalidArgument);
}

void ProjectConfigBuildsEndpointAndComponentSpec()
{
    const auto project = MakeSpeedProject();
    assert(project.Validate().ok());

    auto endpoint = project.BuildEndpoint("speed");
    assert(endpoint.ok());
    assert(endpoint.value().topic.name.str() == "/vehicle/speed");
    assert(endpoint.value().topic.transport == core::TransportKind::kInMemory);
    assert(endpoint.value().message.type_name == "demo.Speed");
    assert(endpoint.value().topic.message_policy.freshness == core::FreshnessPolicy::kQueued);

    auto spec = project.BuildComponentSpec("speed_monitor");
    assert(spec.ok());
    assert(spec.value().name.str() == "speed_monitor");
    assert(spec.value().readers.size() == 1);
    assert(spec.value().writers.empty());
    assert(spec.value().triggers.size() == 1);
    assert(spec.value().triggers.front().kind == core::TriggerKind::kData);
    assert(spec.value().triggers.front().data_dependencies.front().str() == "/vehicle/speed");
}

void ProjectValidationRejectsMissingDataReader()
{
    auto project = MakeSpeedProject();
    project.components.front().readers.clear();

    auto status = project.Validate();
    assert(!status.ok());
    assert(status.code() == core::StatusCode::kInvalidArgument);
}

void ProjectValidationRejectsUnknownTopicReference()
{
    config::ProjectConfig project;

    config::TriggerConfig trigger;
    trigger.kind = core::TriggerKind::kData;
    trigger.data_topics = {"missing"};

    assert(project.AddComponent(config::ComponentConfig {
        "broken_monitor",
        "references a topic that does not exist",
        {"missing"},
        {},
        {trigger}
    }).ok());

    auto status = project.Validate();
    assert(!status.ok());
    assert(status.code() == core::StatusCode::kNotFound);
}

void ProjectValidationRejectsTriggerFieldMismatch()
{
    auto project = MakeSpeedProject();
    project.components.front().triggers.front().kind = core::TriggerKind::kManual;

    auto status = project.Validate();
    assert(!status.ok());
    assert(status.code() == core::StatusCode::kInvalidArgument);
}

void TriggerSchedulingFieldsReachComponentSpec()
{
    auto project = MakeSpeedProject();
    auto& trigger = project.components.front().triggers.front();
    assert(trigger.priority == 0);
    assert(trigger.deadline == core::Nanoseconds::zero());
    trigger.priority = -7;
    trigger.deadline = core::Nanoseconds(1250);
    assert(project.Validate().ok());
    auto spec = project.BuildComponentSpec("speed_monitor");
    assert(spec.ok());
    assert(spec.value().triggers.front().priority == -7);
    assert(spec.value().triggers.front().deadline == core::Nanoseconds(1250));
    trigger.deadline = core::Nanoseconds(-1);
    assert(project.Validate().code() == core::StatusCode::kInvalidArgument);
}

void ProjectConfigAppliesComponentSpecsToRuntime()
{
    const auto project = MakeSpeedProject();
    auto context = runtime::RuntimeContext::Create(project.ToRuntimeOptions().value());
    assert(context.ok());

    auto status = config::ApplyComponentSpecs(project, *context.value());
    assert(status.ok());

    auto name = core::ComponentName::Create("speed_monitor");
    assert(name.ok());

    auto spec = context.value()->FindComponent(name.value());
    assert(spec.ok());
    assert(spec.value().readers.size() == 1);
}

}  // namespace

int main()
{
    EnumParsersAcceptHumanReadableValues();
    SchedulerOptionsAreValidatedAndExposed();
    ProjectConfigBuildsEndpointAndComponentSpec();
    ProjectValidationRejectsMissingDataReader();
    ProjectValidationRejectsUnknownTopicReference();
    ProjectValidationRejectsTriggerFieldMismatch();
    TriggerSchedulingFieldsReachComponentSpec();
    ProjectConfigAppliesComponentSpecsToRuntime();
    return 0;
}
