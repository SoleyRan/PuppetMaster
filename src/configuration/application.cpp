#include <puppet_master/configuration/application.h>

#include <atomic>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>

namespace puppet_master::configuration {

namespace {

const char* PolicyName(core::DependencyPolicy policy)
{
    return policy == core::DependencyPolicy::kAll ? "all" : "any";
}

std::string Milliseconds(core::Nanoseconds duration)
{
    std::ostringstream text;
    text << static_cast<double>(duration.count()) / 1e6 << "ms";
    return text.str();
}

std::string Join(const std::vector<std::string>& items)
{
    std::string joined;
    for (const auto& item : items) {
        if (!joined.empty()) {
            joined += ", ";
        }
        joined += item;
    }
    return joined;
}

// The process-wide signal target. Signal handlers may only touch lock-free
// atomics, so the handler sets a flag and a watcher thread forwards it.
std::atomic<bool> g_signal_received {false};
std::atomic<Application*> g_signal_owner {nullptr};

static_assert(std::atomic<bool>::is_always_lock_free, "signal flag must be lock-free");

void HandleStopSignal(int)
{
    g_signal_received.store(true);
}

}  // namespace

ValidationReport ValidateProject(const ProjectConfig& project, const ComponentFactoryRegistry* factories)
{
    ValidationReport report;
    auto status = project.Validate();
    if (!status.ok()) {
        report.errors.push_back(status.message());
    }
    if (factories != nullptr) {
        for (const auto& component : project.components) {
            status = factories->ValidateType(component.type);
            if (!status.ok()) {
                report.errors.push_back("component " + component.name + ": " + status.message());
            }
        }
    }
    return report;
}

std::string DescribeProject(const ProjectConfig& project)
{
    std::ostringstream text;
    text << "scheduler: " << project.scheduler_options.worker_count << " worker(s)";
    if (project.scheduler_options.priority_aging_interval > core::Nanoseconds::zero()) {
        text << ", priority aging every " << Milliseconds(project.scheduler_options.priority_aging_interval);
    }
    text << '\n';

    text << "topics (" << project.topics.size() << "):\n";
    for (const auto& topic : project.topics) {
        const auto& policy = topic.message_policy;
        text << "  " << topic.id << " -> " << topic.name << " [" << core::ToString(topic.transport) << ", "
             << topic.message_type << ", " << core::ToString(policy.freshness);
        if (policy.freshness == core::FreshnessPolicy::kQueued) {
            text << " depth " << policy.queue_depth;
        }
        text << ", " << core::ToString(policy.delivery) << "]\n";
    }

    text << "components (" << project.components.size() << "):\n";
    for (const auto& component : project.components) {
        text << "  " << component.name;
        if (!component.type.empty()) {
            text << " (" << component.type << ")";
        }
        text << '\n';
        if (!component.readers.empty()) {
            text << "    reads: " << Join(component.readers) << '\n';
        }
        if (!component.writers.empty()) {
            text << "    writes: " << Join(component.writers) << '\n';
        }
        if (!component.parameters.empty()) {
            std::vector<std::string> parameters;
            for (const auto& [key, value] : component.parameters) {
                parameters.push_back(key + "=" + value);
            }
            text << "    parameters: " << Join(parameters) << '\n';
        }
        for (const auto& trigger : component.triggers) {
            text << "    trigger: " << core::ToString(trigger.kind);
            switch (trigger.kind) {
                case core::TriggerKind::kPeriodic:
                    text << " every " << Milliseconds(trigger.period);
                    break;
                case core::TriggerKind::kData:
                    text << " on " << PolicyName(trigger.dependency_policy) << " of " << Join(trigger.data_topics);
                    break;
                case core::TriggerKind::kTaskDependency:
                    text << " after " << PolicyName(trigger.dependency_policy) << " of "
                         << Join(trigger.task_dependencies);
                    break;
                case core::TriggerKind::kManual:
                    break;
            }
            if (trigger.priority != 0) {
                text << ", priority " << trigger.priority;
            }
            if (trigger.deadline > core::Nanoseconds::zero()) {
                text << ", deadline " << Milliseconds(trigger.deadline);
            }
            text << '\n';
        }
    }
    return text.str();
}

struct Application::Impl {
    enum class Phase : std::uint8_t {
        kCreated = 0,
        kRunning,
        kStopped,
    };

    ProjectConfig project;
    std::shared_ptr<runtime::RuntimeContext> runtime;
    std::unique_ptr<scheduler::Scheduler> scheduler;
    std::vector<core::ComponentName> components;  // declaration order

    mutable std::mutex mutex;
    std::condition_variable stop_requested_cv;
    bool stop_requested {false};
    Phase phase {Phase::kCreated};

    std::thread signal_watcher;
    std::atomic<bool> watcher_stop {false};

    // Stops, then shuts down, every component that got past creation, in
    // reverse order. Keeps going after errors and returns the first one.
    core::Status TearDownComponents(std::size_t count)
    {
        core::Status first = core::Status::Ok();
        const auto record = [&first](const core::ComponentName& name, const char* step, core::Status status) {
            if (first.ok() && !status.ok()) {
                first = core::Status::Error(
                    status.code(), "component " + name.str() + ": " + step + " failed: " + status.message());
            }
        };
        for (std::size_t index = count; index > 0; --index) {
            const auto& name = components[index - 1];
            auto state = runtime->GetComponentState(name);
            if (!state.ok()) {
                record(name, "stop", state.status());
                continue;
            }
            if (state.value() == runtime::ComponentState::kStarted) {
                record(name, "stop", runtime->StopComponent(name));
                state = runtime->GetComponentState(name);
            }
            if (state.ok() && state.value() != runtime::ComponentState::kShutdown) {
                record(name, "shutdown", runtime->ShutdownComponent(name));
            }
        }
        return first;
    }

    void StopSignalWatcher() noexcept
    {
        if (!signal_watcher.joinable()) {
            return;
        }
        watcher_stop.store(true);
        signal_watcher.join();
        std::signal(SIGINT, SIG_DFL);
        std::signal(SIGTERM, SIG_DFL);
        g_signal_owner.store(nullptr);
    }
};

core::Result<std::unique_ptr<Application>> Application::Create(
    ProjectConfig project, const ComponentFactoryRegistry& factories, ApplicationOptions options)
{
    using ResultType = core::Result<std::unique_ptr<Application>>;
    const auto report = ValidateProject(project, &factories);
    if (!report.ok()) {
        std::string message = "invalid project configuration";
        for (const auto& error : report.errors) {
            message += "; " + error;
        }
        return ResultType::FromStatus(core::Status::InvalidArgument(std::move(message)));
    }

    auto impl = std::make_unique<Impl>();
    for (const auto& component : project.components) {
        auto name = core::ComponentName::Create(component.name);
        if (!name.ok()) {
            return ResultType::FromStatus(name.status());
        }
        impl->components.push_back(std::move(name).value());
    }

    auto runtime_options = project.ToRuntimeOptions();
    if (!runtime_options.ok()) {
        return ResultType::FromStatus(runtime_options.status());
    }
    auto runtime = runtime::RuntimeContext::Create(std::move(runtime_options).value());
    if (!runtime.ok()) {
        return ResultType::FromStatus(runtime.status());
    }
    impl->runtime = std::move(runtime).value();

    if (options.register_transports) {
        auto status = options.register_transports(*impl->runtime);
        if (!status.ok()) {
            return ResultType::FromStatus(
                core::Status::Error(status.code(), "registering transports failed: " + status.message()));
        }
    }

    auto created = CreateScheduler(project, *impl->runtime, factories);
    if (!created.ok()) {
        return ResultType::FromStatus(created.status());
    }
    impl->scheduler = std::move(created).value();
    impl->project = std::move(project);
    return ResultType(std::unique_ptr<Application>(new Application(std::move(impl))));
}

Application::Application(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

Application::~Application()
{
    impl_->StopSignalWatcher();
    Stop();
}

core::Status Application::Start()
{
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->phase == Impl::Phase::kRunning) {
            return core::Status::Ok();
        }
        if (impl_->phase == Impl::Phase::kStopped) {
            return core::Status::FailedPrecondition("application already stopped; create a new one to run again");
        }
    }

    auto& runtime = *impl_->runtime;
    for (std::size_t index = 0; index < impl_->components.size(); ++index) {
        const auto& name = impl_->components[index];
        struct Step {
            const char* label;
            core::Status (runtime::RuntimeContext::*call)(const core::ComponentName&);
        };
        for (const Step step : {Step {"configure", &runtime::RuntimeContext::ConfigureComponent},
                                Step {"initialize", &runtime::RuntimeContext::InitializeComponent},
                                Step {"start", &runtime::RuntimeContext::StartComponent}}) {
            auto status = (runtime.*step.call)(name);
            if (!status.ok()) {
                // Include the failing component: it may be configured or
                // initialized and still needs its shutdown callback.
                impl_->TearDownComponents(index + 1);
                std::lock_guard<std::mutex> lock(impl_->mutex);
                impl_->phase = Impl::Phase::kStopped;
                return core::Status::Error(status.code(), "component " + name.str() + ": " + step.label
                                                             + " failed: " + status.message());
            }
        }
    }

    auto status = impl_->scheduler->Start();
    if (!status.ok()) {
        impl_->TearDownComponents(impl_->components.size());
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->phase = Impl::Phase::kStopped;
        return core::Status::Error(status.code(), "scheduler start failed: " + status.message());
    }

    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->phase = Impl::Phase::kRunning;
    return core::Status::Ok();
}

core::Status Application::Stop()
{
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->phase != Impl::Phase::kRunning) {
            impl_->phase = Impl::Phase::kStopped;
            return core::Status::Ok();
        }
        impl_->phase = Impl::Phase::kStopped;
    }

    // The scheduler drains queued work first, so no component executes
    // while it is being stopped.
    core::Status first = impl_->scheduler->Stop();
    if (!first.ok()) {
        first = core::Status::Error(first.code(), "scheduler stop failed: " + first.message());
    }
    auto status = impl_->TearDownComponents(impl_->components.size());
    if (first.ok()) {
        first = status;
    }
    status = impl_->runtime->Close();
    if (first.ok() && !status.ok()) {
        first = core::Status::Error(status.code(), "closing transports failed: " + status.message());
    }
    return first;
}

core::Status Application::Run()
{
    auto status = Start();
    if (!status.ok()) {
        return status;
    }
    {
        std::unique_lock<std::mutex> lock(impl_->mutex);
        impl_->stop_requested_cv.wait(lock, [this] { return impl_->stop_requested; });
    }
    return Stop();
}

void Application::RequestStop() noexcept
{
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->stop_requested = true;
    }
    impl_->stop_requested_cv.notify_all();
}

core::Status Application::InstallSignalHandlers()
{
    if (impl_->signal_watcher.joinable()) {
        return core::Status::Ok();
    }
    Application* expected = nullptr;
    if (!g_signal_owner.compare_exchange_strong(expected, this)) {
        return core::Status::FailedPrecondition("another application already owns the signal handlers");
    }
    g_signal_received.store(false);
    impl_->watcher_stop.store(false);
    if (std::signal(SIGINT, HandleStopSignal) == SIG_ERR || std::signal(SIGTERM, HandleStopSignal) == SIG_ERR) {
        std::signal(SIGINT, SIG_DFL);
        std::signal(SIGTERM, SIG_DFL);
        g_signal_owner.store(nullptr);
        return core::Status::Internal("installing SIGINT/SIGTERM handlers failed");
    }
    // A handler may only set a lock-free flag; this thread turns it into a
    // RequestStop() within one polling interval.
    impl_->signal_watcher = std::thread([this] {
        while (!impl_->watcher_stop.load()) {
            if (g_signal_received.exchange(false)) {
                RequestStop();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    });
    return core::Status::Ok();
}

bool Application::running() const noexcept
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->phase == Impl::Phase::kRunning;
}

const ProjectConfig& Application::project() const noexcept
{
    return impl_->project;
}

runtime::RuntimeContext& Application::runtime() noexcept
{
    return *impl_->runtime;
}

scheduler::Scheduler& Application::scheduler() noexcept
{
    return *impl_->scheduler;
}

}  // namespace puppet_master::configuration
