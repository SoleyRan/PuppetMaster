#include <puppet_master/configuration/application.h>

#include <atomic>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

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
             << topic.message_type;
        if (topic.message_version != 0) {
            text << " v" << topic.message_version;
        }
        text << ", " << core::ToString(policy.freshness);
        if (policy.freshness == core::FreshnessPolicy::kQueued) {
            text << " depth " << policy.queue_depth;
        }
        text << ", " << core::ToString(policy.delivery);
        for (const auto& [key, value] : topic.transport_parameters) {
            text << ", " << key << "=" << value;
        }
        text << "]\n";
    }

    if (!project.services.empty()) {
        text << "services (" << project.services.size() << "):\n";
        for (const auto& service : project.services) {
            const auto version = [](std::uint32_t value) {
                return value == 0 ? std::string() : " v" + std::to_string(value);
            };
            text << "  " << service.id << " -> " << service.name << " [" << core::ToString(service.transport) << ", "
                 << service.request_type << version(service.request_version) << " -> " << service.reply_type
                 << version(service.reply_version) << ", queue " << service.queue_depth << ", heartbeat "
                 << Milliseconds(service.heartbeat_interval);
            for (const auto& [key, value] : service.transport_parameters) {
                text << ", " << key << "=" << value;
            }
            text << "]\n";
        }
    }

    text << "components (" << project.components.size() << "):\n";
    for (const auto& component : project.components) {
        text << "  " << component.name;
        if (!component.type.empty()) {
            text << " (" << component.type << ")";
        }
        text << '\n';
        if (!component.readers.empty()) {
            std::vector<std::string> readers;
            for (const auto& id : component.readers) {
                const auto policy = component.reader_policies.find(id);
                if (policy == component.reader_policies.end()) {
                    readers.push_back(id);
                } else if (policy->second.freshness == core::FreshnessPolicy::kQueued) {
                    readers.push_back(id + " (queued depth " + std::to_string(policy->second.queue_depth) + ")");
                } else {
                    readers.push_back(id + " (" + core::ToString(policy->second.freshness) + ")");
                }
            }
            text << "    reads: " << Join(readers) << '\n';
        }
        if (!component.writers.empty()) {
            text << "    writes: " << Join(component.writers) << '\n';
        }
        if (!component.servers.empty()) {
            text << "    serves: " << Join(component.servers) << '\n';
        }
        if (!component.clients.empty()) {
            text << "    calls: " << Join(component.clients) << '\n';
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
                case core::TriggerKind::kService:
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

SystemReport ValidateSystem(const std::vector<NamedProject>& projects)
{
    // Every declaration and use of each wire topic across the system.
    struct Use {
        std::string label;
        const TopicConfig* topic;
    };
    struct TopicUses {
        std::vector<Use> declarations;
        std::set<std::string> readers;  // labels
        std::set<std::string> writers;
    };
    std::map<std::string, TopicUses> uses;

    for (const auto& entry : projects) {
        const auto& project = *entry.project;
        for (const auto& topic : project.topics) {
            uses[topic.name].declarations.push_back(Use {entry.label, &topic});
        }
        for (const auto& component : project.components) {
            for (const auto& id : component.readers) {
                auto topic = project.FindTopic(id);
                if (topic.ok()) {
                    uses[topic.value().name].readers.insert(entry.label);
                }
            }
            for (const auto& id : component.writers) {
                auto topic = project.FindTopic(id);
                if (topic.ok()) {
                    uses[topic.value().name].writers.insert(entry.label);
                }
            }
        }
    }

    const auto describe = [](const TopicConfig& topic) {
        return transport::MessageDescriptor {topic.message_type, topic.encoding, topic.message_version}.ToString()
            + " over " + core::ToString(topic.transport);
    };

    SystemReport report;
    for (const auto& [name, topic] : uses) {
        // Unused declarations, such as topics a process imports but does not
        // touch, take no part in the checks.
        if (topic.readers.empty() && topic.writers.empty()) {
            continue;
        }
        const Use* first = nullptr;
        for (const auto& use : topic.declarations) {
            if (topic.readers.count(use.label) == 0 && topic.writers.count(use.label) == 0) {
                continue;
            }
            if (first == nullptr) {
                first = &use;
                continue;
            }
            const auto& a = *first->topic;
            const auto& b = *use.topic;
            if (a.message_type != b.message_type || a.encoding != b.encoding || a.message_version != b.message_version
                || a.transport != b.transport) {
                report.errors.push_back("topic " + name + " is " + describe(a) + " in " + first->label + " but "
                                        + describe(b) + " in " + use.label);
            }
        }
        if (topic.writers.empty()) {
            report.errors.push_back("topic " + name + " is read by " + Join({topic.readers.begin(), topic.readers.end()})
                                    + " but written by no process");
        } else if (topic.writers.size() > 1) {
            report.errors.push_back("topic " + name + " is written by several processes: "
                                    + Join({topic.writers.begin(), topic.writers.end()}));
        }
        if (topic.readers.empty()) {
            report.warnings.push_back("topic " + name + " is written by "
                                      + Join({topic.writers.begin(), topic.writers.end()}) + " but read by no process");
        }
    }

    // Services, by wire name: one serving process, and the same contract
    // wherever the service is used.
    struct ServiceUse {
        std::string label;
        const ServiceConfig* service;
    };
    struct ServiceUses {
        std::vector<ServiceUse> declarations;
        std::set<std::string> servers;
        std::set<std::string> clients;
    };
    std::map<std::string, ServiceUses> services;
    for (const auto& entry : projects) {
        const auto& project = *entry.project;
        for (const auto& service : project.services) {
            services[service.name].declarations.push_back(ServiceUse {entry.label, &service});
        }
        for (const auto& component : project.components) {
            for (const auto& id : component.servers) {
                auto service = project.FindService(id);
                if (service.ok()) {
                    services[service.value().name].servers.insert(entry.label);
                }
            }
            for (const auto& id : component.clients) {
                auto service = project.FindService(id);
                if (service.ok()) {
                    services[service.value().name].clients.insert(entry.label);
                }
            }
        }
    }
    const auto contract = [](const ServiceConfig& service) {
        return transport::MessageDescriptor {service.request_type, service.encoding, service.request_version}.ToString()
            + " -> "
            + transport::MessageDescriptor {service.reply_type, service.encoding, service.reply_version}.ToString()
            + " over " + core::ToString(service.transport);
    };
    for (const auto& [name, service] : services) {
        if (service.servers.empty() && service.clients.empty()) {
            continue;
        }
        const ServiceUse* first = nullptr;
        for (const auto& use : service.declarations) {
            if (service.servers.count(use.label) == 0 && service.clients.count(use.label) == 0) {
                continue;
            }
            if (first == nullptr) {
                first = &use;
                continue;
            }
            if (contract(*first->service) != contract(*use.service)) {
                report.errors.push_back("service " + name + " is " + contract(*first->service) + " in "
                                        + first->label + " but " + contract(*use.service) + " in " + use.label);
            }
        }
        if (service.servers.empty()) {
            report.errors.push_back("service " + name + " is called by "
                                    + Join({service.clients.begin(), service.clients.end()})
                                    + " but served by no process");
        } else if (service.servers.size() > 1) {
            // Not detected at runtime: every server would answer each request.
            report.errors.push_back("service " + name + " is served by several processes: "
                                    + Join({service.servers.begin(), service.servers.end()}));
        }
        if (service.clients.empty()) {
            report.warnings.push_back("service " + name + " is served by "
                                      + Join({service.servers.begin(), service.servers.end()})
                                      + " but called by no process");
        }
    }
    return report;
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

    auto status = core::Status::Ok();
    if (options.register_transports) {
        status = options.register_transports(*impl->runtime);
    }
    if (status.ok() && options.register_project_transports) {
        status = options.register_project_transports(*impl->runtime, project);
    }
    if (!status.ok()) {
        return ResultType::FromStatus(
            core::Status::Error(status.code(), "registering transports failed: " + status.message()));
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
