#include <cassert>
#include <memory>
#include <string>
#include <utility>

#include <puppet_master/puppet_master.h>

namespace config = puppet_master::configuration;
namespace core = puppet_master::core;
namespace runtime = puppet_master::runtime;

namespace {

class SpecComponent final : public runtime::Component {
public:
    explicit SpecComponent(runtime::ComponentSpec spec) : spec_(std::move(spec)) {}
    runtime::ComponentSpec Describe() const override { return spec_; }
private:
    runtime::ComponentSpec spec_;
};

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

void ProjectConfigCreatesConfiguredScheduler()
{
    auto project = MakeSpeedProject();
    project.scheduler_options.worker_count = 2;
    auto context = runtime::RuntimeContext::Create(project.ToRuntimeOptions().value());
    assert(context.ok());

    auto scheduler_instance = config::CreateScheduler(project, *context.value());
    assert(scheduler_instance.ok());
    assert(scheduler_instance.value()->stats().registered_components == 1);
    auto name = core::ComponentName::Create("speed_monitor");
    assert(name.ok());
    assert(context.value()->FindComponent(name.value()).ok());
    assert(!context.value()->FindComponentInstance(name.value()).ok());
    assert(scheduler_instance.value()->RegisterComponent(name.value()).code() == core::StatusCode::kAlreadyExists);
    assert(scheduler_instance.value()->RegisterAllComponents().ok());
    assert(scheduler_instance.value()->Start().ok());
    assert(scheduler_instance.value()->is_running());
    assert(scheduler_instance.value()->Stop().ok());
}

void InvalidProjectDoesNotRegisterComponents()
{
    auto project = MakeSpeedProject();
    project.scheduler_options.worker_count = 0;
    auto context = runtime::RuntimeContext::Create();
    assert(context.ok());

    auto scheduler_instance = config::CreateScheduler(project, *context.value());
    assert(!scheduler_instance.ok());
    assert(scheduler_instance.status().code() == core::StatusCode::kInvalidArgument);
    assert(context.value()->ListComponentNames().empty());
}

void DuplicateRuntimeComponentIsRejected()
{
    auto project = MakeSpeedProject();
    auto context = runtime::RuntimeContext::Create();
    assert(context.ok());
    assert(config::ApplyComponentSpecs(project, *context.value()).ok());

    auto scheduler_instance = config::CreateScheduler(project, *context.value());
    assert(!scheduler_instance.ok());
    assert(scheduler_instance.status().code() == core::StatusCode::kAlreadyExists);
    assert(context.value()->ListComponentNames().size() == 1);
}

void FactoryCreatesComponentInstances()
{
    auto project = MakeSpeedProject();
    auto context = runtime::RuntimeContext::Create();
    assert(context.ok());
    auto factory = [](const runtime::ComponentSpec& spec) -> core::Result<runtime::ComponentPtr> {
        runtime::ComponentPtr component = std::make_shared<SpecComponent>(spec);
        return component;
    };
    auto sched = config::CreateScheduler(project, *context.value(), factory);
    assert(sched.ok());
    assert(sched.value()->stats().registered_components == 1);
    auto name = core::ComponentName::Create("speed_monitor").value();
    assert(context.value()->FindComponentInstance(name).ok());
    assert(context.value()->GetComponentState(name).value() == runtime::ComponentState::kCreated);
    assert(context.value()->ConfigureComponent(name).ok());
    assert(context.value()->InitializeComponent(name).ok());
    assert(context.value()->StartComponent(name).ok());
    assert(context.value()->StopComponent(name).ok());
    assert(context.value()->ShutdownComponent(name).ok());
    auto duplicate = config::CreateScheduler(project, *context.value(), factory);
    assert(!duplicate.ok());
    assert(duplicate.status().code() == core::StatusCode::kAlreadyExists);
}

void FactoryFailuresLeaveRuntimeUnchanged()
{
    auto project = MakeSpeedProject();
    assert(project.AddComponent(config::ComponentConfig {"second_monitor", "second", {}, {}, {}}).ok());
    auto context = runtime::RuntimeContext::Create();
    assert(context.ok());
    auto failing = config::CreateScheduler(project, *context.value(),
        [](const runtime::ComponentSpec& spec) -> core::Result<runtime::ComponentPtr> {
            if (spec.name.str() == "second_monitor") {
                return core::Result<runtime::ComponentPtr>::FromStatus(
                    core::Status::NotFound("factory not registered"));
            }
            runtime::ComponentPtr component = std::make_shared<SpecComponent>(spec);
            return component;
        });
    assert(!failing.ok());
    assert(failing.status().code() == core::StatusCode::kNotFound);
    assert(context.value()->ListComponentNames().empty());

    auto mismatched = config::CreateScheduler(project, *context.value(),
        [](const runtime::ComponentSpec& spec) -> core::Result<runtime::ComponentPtr> {
            auto changed = spec;
            changed.triggers.clear();
            runtime::ComponentPtr component = std::make_shared<SpecComponent>(changed);
            return component;
        });
    assert(!mismatched.ok());
    assert(mismatched.status().code() == core::StatusCode::kInvalidArgument);
    assert(context.value()->ListComponentNames().empty());

    auto null_instance = config::CreateScheduler(project, *context.value(),
        [](const runtime::ComponentSpec&) -> core::Result<runtime::ComponentPtr> {
            return runtime::ComponentPtr {};
        });
    assert(!null_instance.ok());
    assert(null_instance.status().code() == core::StatusCode::kInvalidArgument);
    assert(context.value()->ListComponentNames().empty());
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
    ProjectConfigCreatesConfiguredScheduler();
    InvalidProjectDoesNotRegisterComponents();
    DuplicateRuntimeComponentIsRejected();
    FactoryCreatesComponentInstances();
    FactoryFailuresLeaveRuntimeUnchanged();
    return 0;
}
