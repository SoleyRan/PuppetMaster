#include <atomic>
#include <cassert>
#include <chrono>
#include <csignal>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <puppet_master/configuration/application.h>
#include <puppet_master/puppet_master.h>

namespace config = puppet_master::configuration;
namespace core = puppet_master::core;
namespace runtime = puppet_master::runtime;
namespace transport = puppet_master::transport;

namespace {

using namespace std::chrono_literals;

// Shared, thread-safe record of lifecycle calls across components.
struct Journal {
    std::mutex mutex;
    std::vector<std::string> events;

    void Add(std::string event)
    {
        std::lock_guard<std::mutex> lock(mutex);
        events.push_back(std::move(event));
    }

    std::vector<std::string> Snapshot()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return events;
    }
};

class RecordingComponent final : public runtime::Component {
public:
    RecordingComponent(runtime::ComponentSpec spec, Journal& journal, std::string fail_at)
        : spec_(std::move(spec)), journal_(journal), fail_at_(std::move(fail_at))
    {
    }

    runtime::ComponentSpec Describe() const override { return spec_; }

    core::Status Configure(runtime::ComponentContext&) override { return Step("configure"); }
    core::Status Initialize(runtime::ComponentContext&) override { return Step("initialize"); }
    core::Status Start(runtime::ComponentContext&) override { return Step("start"); }
    core::Status Stop(runtime::ComponentContext&) override { return Step("stop"); }
    core::Status Shutdown(runtime::ComponentContext&) override { return Step("shutdown"); }

    core::Status Execute(runtime::ComponentContext&) override
    {
        ++executions;
        return core::Status::Ok();
    }

    std::atomic<int> executions {0};

private:
    core::Status Step(const char* step)
    {
        journal_.Add(spec_.name.str() + "." + step);
        if (fail_at_ == step) {
            return core::Status::Internal(std::string("injected ") + step + " failure");
        }
        return core::Status::Ok();
    }

    runtime::ComponentSpec spec_;
    Journal& journal_;
    std::string fail_at_;
};

config::ProjectConfig MakeProject(const std::vector<std::string>& names, const std::string& type = "recorder")
{
    config::ProjectConfig project;
    for (const auto& name : names) {
        config::TriggerConfig trigger;
        trigger.kind = core::TriggerKind::kManual;
        config::ComponentConfig component;
        component.name = name;
        component.type = type;
        component.triggers = {trigger};
        assert(project.AddComponent(std::move(component)).ok());
    }
    return project;
}

// Factories that record lifecycle calls; `fail` maps a component name to the
// lifecycle step at which it should fail.
config::ComponentFactoryRegistry MakeFactories(
    Journal& journal, std::vector<std::shared_ptr<RecordingComponent>>& created,
    std::pair<std::string, std::string> fail = {})
{
    config::ComponentFactoryRegistry factories;
    assert(factories.Register("recorder", [&journal, &created, fail](const runtime::ComponentSpec& spec) {
        auto component = std::make_shared<RecordingComponent>(
            spec, journal, spec.name.str() == fail.first ? fail.second : std::string());
        created.push_back(component);
        return core::Result<runtime::ComponentPtr>(runtime::ComponentPtr(component));
    }).ok());
    return factories;
}

void StartAndStopFollowLifecycleOrder()
{
    Journal journal;
    std::vector<std::shared_ptr<RecordingComponent>> created;
    auto app = config::Application::Create(MakeProject({"a", "b"}), MakeFactories(journal, created));
    assert(app.ok());
    assert(!app.value()->running());

    assert(app.value()->Start().ok());
    assert(app.value()->running());
    assert(app.value()->Start().ok());  // idempotent

    // Components execute through the owned scheduler.
    assert(app.value()->scheduler().Trigger(core::ComponentName::Unsafe("b")).ok());
    assert(app.value()->scheduler().WaitIdle(1s).ok());
    assert(created.size() == 2 && created[1]->executions == 1);

    assert(app.value()->Stop().ok());
    assert(!app.value()->running());
    assert(app.value()->Stop().ok());  // idempotent
    assert(app.value()->Start().code() == core::StatusCode::kFailedPrecondition);

    const std::vector<std::string> expected {
        "a.configure", "a.initialize", "a.start",
        "b.configure", "b.initialize", "b.start",
        "b.stop", "b.shutdown", "a.stop", "a.shutdown",
    };
    assert(journal.Snapshot() == expected);
}

void FailedStartRollsBack()
{
    // b fails in initialize: a (fully started) and b (configured) are both
    // rolled back, c is never touched.
    Journal journal;
    std::vector<std::shared_ptr<RecordingComponent>> created;
    auto app = config::Application::Create(
        MakeProject({"a", "b", "c"}), MakeFactories(journal, created, {"b", "initialize"}));
    assert(app.ok());

    auto status = app.value()->Start();
    assert(status.code() == core::StatusCode::kInternal);
    assert(status.message().find("component b: initialize failed") != std::string::npos);
    assert(!app.value()->running());
    assert(app.value()->Stop().ok());

    const std::vector<std::string> expected {
        "a.configure", "a.initialize", "a.start",
        "b.configure", "b.initialize",
        "b.shutdown", "a.stop", "a.shutdown",
    };
    assert(journal.Snapshot() == expected);
}

void DestructorStopsRunningApplication()
{
    Journal journal;
    std::vector<std::shared_ptr<RecordingComponent>> created;
    {
        auto app = config::Application::Create(MakeProject({"only"}), MakeFactories(journal, created));
        assert(app.ok() && app.value()->Start().ok());
    }
    const auto events = journal.Snapshot();
    assert(events.size() == 5 && events[3] == "only.stop" && events[4] == "only.shutdown");
}

void RunReturnsAfterRequestStop()
{
    Journal journal;
    std::vector<std::shared_ptr<RecordingComponent>> created;
    auto app = config::Application::Create(MakeProject({"loop"}), MakeFactories(journal, created));
    assert(app.ok());

    std::thread stopper([&] {
        while (!app.value()->running()) {
            std::this_thread::sleep_for(1ms);
        }
        app.value()->RequestStop();
    });
    assert(app.value()->Run().ok());
    stopper.join();
    assert(!app.value()->running());
    assert(journal.Snapshot().back() == "loop.shutdown");
}

void SignalStopsRun()
{
    Journal journal;
    std::vector<std::shared_ptr<RecordingComponent>> created;
    auto app = config::Application::Create(MakeProject({"signalled"}), MakeFactories(journal, created));
    assert(app.ok());
    assert(app.value()->InstallSignalHandlers().ok());
    assert(app.value()->InstallSignalHandlers().ok());  // idempotent

    // A second application cannot take over the handlers.
    std::vector<std::shared_ptr<RecordingComponent>> other_created;
    auto other = config::Application::Create(MakeProject({"other"}), MakeFactories(journal, other_created));
    assert(other.ok());
    assert(other.value()->InstallSignalHandlers().code() == core::StatusCode::kFailedPrecondition);

    std::thread raiser([&] {
        while (!app.value()->running()) {
            std::this_thread::sleep_for(1ms);
        }
        std::raise(SIGTERM);
    });
    assert(app.value()->Run().ok());
    raiser.join();
    assert(journal.Snapshot().back() == "signalled.shutdown");
}

void ValidationReportsEveryProblem()
{
    Journal journal;
    std::vector<std::shared_ptr<RecordingComponent>> created;
    auto factories = MakeFactories(journal, created);

    auto project = MakeProject({"good"});
    config::ComponentConfig missing_one;
    missing_one.name = "first_unknown";
    missing_one.type = "missing";
    assert(project.AddComponent(missing_one).ok());
    config::ComponentConfig missing_two;
    missing_two.name = "second_unknown";
    missing_two.type = "also_missing";
    assert(project.AddComponent(missing_two).ok());

    assert(config::ValidateProject(project).ok());  // types are not checked without factories
    const auto report = config::ValidateProject(project, &factories);
    assert(report.errors.size() == 2);
    assert(report.errors[0].find("first_unknown") != std::string::npos);
    assert(report.errors[1].find("second_unknown") != std::string::npos);

    auto app = config::Application::Create(project, factories);
    assert(app.status().code() == core::StatusCode::kInvalidArgument);
    assert(app.status().message().find("first_unknown") != std::string::npos);
    assert(app.status().message().find("second_unknown") != std::string::npos);
    assert(created.empty());
}

void RegisterTransportsHookRunsAndFailsCleanly()
{
    Journal journal;
    std::vector<std::shared_ptr<RecordingComponent>> created;
    int calls = 0;
    config::ApplicationOptions options;
    options.register_transports = [&calls](runtime::RuntimeContext& context) {
        ++calls;
        assert(context.is_open());
        return core::Status::Ok();
    };
    auto app = config::Application::Create(MakeProject({"hooked"}), MakeFactories(journal, created), options);
    assert(app.ok() && calls == 1);

    options.register_transports = [](runtime::RuntimeContext&) {
        return core::Status::Unavailable("no network");
    };
    auto failed = config::Application::Create(MakeProject({"hooked"}), MakeFactories(journal, created), options);
    assert(failed.status().code() == core::StatusCode::kUnavailable);
    assert(failed.status().message().find("registering transports failed") != std::string::npos);
}

void DescribeListsTopicsComponentsAndTriggers()
{
    config::ProjectConfig project;
    core::MessagePolicy queued;
    queued.freshness = core::FreshnessPolicy::kQueued;
    queued.queue_depth = 8;
    assert(project.AddTopic(config::TopicConfig {
        "speed", "/vehicle/speed", core::TransportKind::kIpc, "demo.Speed", "text/plain", queued}).ok());
    project.scheduler_options.worker_count = 2;
    project.scheduler_options.priority_aging_interval = 10ms;

    config::TriggerConfig on_speed;
    on_speed.kind = core::TriggerKind::kData;
    on_speed.dependency_policy = core::DependencyPolicy::kAny;
    on_speed.data_topics = {"speed"};
    on_speed.priority = 3;
    config::ComponentConfig monitor {"monitor", "", {"speed"}, {}, {on_speed}, "speed_monitor"};
    assert(project.AddComponent(monitor).ok());

    const auto text = config::DescribeProject(project);
    for (const char* expected : {"2 worker(s)", "priority aging every 10ms", "speed -> /vehicle/speed",
                                 "ipc", "queued depth 8", "monitor (speed_monitor)", "reads: speed",
                                 "trigger: data on any of speed", "priority 3"}) {
        assert(text.find(expected) != std::string::npos);
    }
}

}  // namespace

int main()
{
    StartAndStopFollowLifecycleOrder();
    FailedStartRollsBack();
    DestructorStopsRunningApplication();
    RunReturnsAfterRequestStop();
    SignalStopsRun();
    ValidationReportsEveryProblem();
    RegisterTransportsHookRunsAndFailsCleanly();
    DescribeListsTopicsComponentsAndTriggers();
    return 0;
}
