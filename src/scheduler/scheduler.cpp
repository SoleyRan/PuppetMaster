#include <puppet_master/scheduler/scheduler.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace puppet_master::scheduler {

namespace {

bool HasTrigger(const runtime::ComponentSpec& spec, core::TriggerKind kind)
{
    return std::any_of(spec.triggers.begin(), spec.triggers.end(), [kind](const auto& trigger) {
        return trigger.kind == kind;
    });
}

core::Result<transport::EndpointConfig> FindReaderEndpoint(
    const runtime::ComponentSpec& spec,
    const core::TopicName& topic)
{
    const auto found = std::find_if(
        spec.readers.begin(),
        spec.readers.end(),
        [&topic](const auto& endpoint) {
            return endpoint.topic.name == topic;
        });

    if (found == spec.readers.end()) {
        return core::Result<transport::EndpointConfig>::FromStatus(
            core::Status::InvalidArgument(
                "data trigger topic has no matching reader endpoint: " + topic.str()));
    }

    return *found;
}

transport::EndpointConfig MakeTriggerEndpoint(transport::EndpointConfig endpoint)
{
    endpoint.topic.message_policy.freshness = core::FreshnessPolicy::kLatest;
    endpoint.topic.message_policy.retention = core::RetentionPolicy::kKeepLast;
    endpoint.topic.message_policy.overflow = core::QueueOverflowPolicy::kDropOldest;
    endpoint.topic.message_policy.queue_depth = 1;
    return endpoint;
}

core::Status ValidateTrigger(const runtime::ComponentSpec& spec, const core::TriggerSpec& trigger)
{
    auto status = trigger.Validate();
    if (!status.ok()) {
        return status;
    }

    if (trigger.kind == core::TriggerKind::kData) {
        if (trigger.data_dependencies.empty()) {
            return core::Status::InvalidArgument("data trigger requires at least one topic dependency");
        }

        for (const auto& topic : trigger.data_dependencies) {
            auto endpoint = FindReaderEndpoint(spec, topic);
            if (!endpoint.ok()) {
                return endpoint.status();
            }
        }
    }

    if (trigger.kind == core::TriggerKind::kTaskDependency && trigger.task_dependencies.empty()) {
        return core::Status::InvalidArgument("task dependency trigger requires at least one task dependency");
    }

    return core::Status::Ok();
}

core::Status ValidateComponentSpec(const runtime::ComponentSpec& spec)
{
    auto status = spec.Validate();
    if (!status.ok()) {
        return status;
    }

    for (const auto& trigger : spec.triggers) {
        status = ValidateTrigger(spec, trigger);
        if (!status.ok()) {
            return status;
        }
    }

    return core::Status::Ok();
}

core::Status RunningRequired()
{
    return core::Status::FailedPrecondition("scheduler must be running before accepting trigger events");
}

}  // namespace

struct Scheduler::Impl {
    struct ScheduledEvent {
        core::ComponentName component;
        core::Nanoseconds deadline {0};
    };

    struct DataTriggerState {
        core::DependencyPolicy policy;
        std::vector<bool> ready;
    };

    struct TaskTriggerState {
        core::DependencyPolicy policy;
        std::vector<bool> ready;
    };

    struct TaskDependent {
        std::string component;
        std::size_t trigger_index;
        std::size_t dependency_index;
    };

    struct ScheduledComponent {
        explicit ScheduledComponent(runtime::ComponentSpec component_spec)
            : spec(std::move(component_spec))
        {
        }

        runtime::ComponentSpec spec;
        std::vector<transport::ReaderPtr> data_trigger_readers;
        std::vector<DataTriggerState> data_triggers;
        std::vector<TaskTriggerState> task_triggers;
    };

    explicit Impl(runtime::RuntimeContext& runtime_ref, SchedulerOptions scheduler_options)
        : runtime(runtime_ref), options(scheduler_options)
    {
    }

    ~Impl()
    {
        Stop();
    }

    core::Status RegisterComponent(const core::ComponentName& name)
    {
        auto spec = runtime.FindComponent(name);
        if (!spec.ok()) {
            return spec.status();
        }

        auto status = ValidateComponentSpec(spec.value());
        if (!status.ok()) {
            return status;
        }

        std::lock_guard<std::mutex> lock(mutex);
        if (running || stopping) {
            return core::Status::FailedPrecondition(
                "components must be registered before scheduler starts");
        }

        const auto key = name.str();
        if (components.find(key) != components.end()) {
            return core::Status::AlreadyExists("component already registered in scheduler: " + key);
        }

        components.emplace(key, ScheduledComponent {spec.value()});
        return core::Status::Ok();
    }

    core::Status RegisterAllComponents()
    {
        for (const auto& name : runtime.ListComponentNames()) {
            auto status = RegisterComponent(name);
            if (!status.ok() && status.code() != core::StatusCode::kAlreadyExists) {
                return status;
            }
        }

        return core::Status::Ok();
    }

    core::Status Start(std::weak_ptr<Impl> weak_self)
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (running) {
                return core::Status::Ok();
            }
            if (stopping) {
                return core::Status::FailedPrecondition("scheduler is stopping");
            }
            if (options.worker_count == 0) {
                return core::Status::InvalidArgument("scheduler worker_count must be greater than zero");
            }

            auto status = InstallTaskTriggers();
            if (!status.ok()) {
                return status;
            }

            stopping = false;
            running = true;
            last_status = core::Status::Ok();
        }

        for (std::size_t index = 0; index < options.worker_count; ++index) {
            workers.emplace_back([weak_self]() {
                if (auto self = weak_self.lock()) {
                    self->DispatchLoop();
                }
            });
        }

        auto status = InstallDataTriggers(weak_self);
        if (!status.ok()) {
            Stop();
            return status;
        }

        status = StartPeriodicTriggers(weak_self);
        if (!status.ok()) {
            Stop();
            return status;
        }

        if (const auto observer = runtime.observer()) {
            observer->Log(observability::LogRecord {
                observability::LogLevel::kInfo,
                "scheduler",
                "scheduler_started",
                "scheduler is accepting trigger events",
                {
                    {"components", std::to_string(Stats().registered_components)},
                },
            });
        }

        return core::Status::Ok();
    }

    core::Status Stop()
    {
        std::vector<std::thread> periodic_threads_to_join;
        std::vector<std::thread> workers_to_join;

        {
            std::lock_guard<std::mutex> lock(mutex);
            if (stopping) {
                return core::Status::FailedPrecondition("scheduler is already stopping");
            }
            if (!running) {
                return core::Status::Ok();
            }
            const auto caller = std::this_thread::get_id();
            for (const auto& worker : workers) {
                if (worker.get_id() == caller) {
                    return core::Status::FailedPrecondition("scheduler worker cannot stop itself");
                }
            }
            for (const auto& timer : periodic_threads) {
                if (timer.get_id() == caller) {
                    return core::Status::FailedPrecondition("scheduler timer cannot stop itself");
                }
            }

            stopping = true;
            running = false;
            periodic_threads_to_join.swap(periodic_threads);
            workers_to_join.swap(workers);
        }

        event_available.notify_all();
        periodic_stop.notify_all();

        for (auto& worker : periodic_threads_to_join) {
            if (worker.joinable()) {
                worker.join();
            }
        }

        for (auto& worker : workers_to_join) {
            if (worker.joinable()) {
                worker.join();
            }
        }

        {
            std::lock_guard<std::mutex> lock(mutex);
            pending_events.clear();
            active_events = 0;
            for (auto& entry : components) {
                entry.second.data_trigger_readers.clear();
                entry.second.data_triggers.clear();
                entry.second.task_triggers.clear();
            }
            task_dependents.clear();
            stopping = false;
        }

        idle.notify_all();

        if (const auto observer = runtime.observer()) {
            observer->Log(observability::LogRecord {
                observability::LogLevel::kInfo,
                "scheduler",
                "scheduler_stopped",
                "scheduler stopped after draining pending events",
                {
                    {"dispatched_events", std::to_string(Stats().dispatched_events)},
                },
            });
        }

        return core::Status::Ok();
    }

    bool IsRunning() const noexcept
    {
        std::lock_guard<std::mutex> lock(mutex);
        return running;
    }

    core::Status Trigger(const core::ComponentName& name)
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!running || stopping) {
                return RunningRequired();
            }

            const auto found = components.find(name.str());
            if (found == components.end()) {
                return core::Status::NotFound("component is not registered in scheduler: " + name.str());
            }

            if (!HasTrigger(found->second.spec, core::TriggerKind::kManual)) {
                return core::Status::FailedPrecondition(
                    "component has no manual trigger: " + name.str());
            }
        }

        return Enqueue(name, core::Nanoseconds::zero());
    }

    core::Status WaitIdle(core::Nanoseconds timeout)
    {
        std::unique_lock<std::mutex> lock(mutex);
        const auto ready = [this]() {
            return pending_events.empty() && active_events == 0;
        };

        if (timeout <= core::Nanoseconds::zero()) {
            idle.wait(lock, ready);
            return core::Status::Ok();
        }

        if (!idle.wait_for(lock, timeout, ready)) {
            return core::Status::DeadlineExceeded("timed out waiting for scheduler to become idle");
        }

        return core::Status::Ok();
    }

    SchedulerStats Stats() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return SchedulerStats {
            components.size(),
            pending_events.size(),
            active_events,
            dispatched_events
        };
    }

    core::Status LastError() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return last_status;
    }

private:
    core::Status InstallTaskTriggers()
    {
        std::map<std::string, std::vector<TaskDependent>> dependents;
        std::map<std::string, std::vector<TaskTriggerState>> states;
        std::map<std::string, std::size_t> indegree;
        for (const auto& entry : components) {
            indegree.emplace(entry.first, 0);
        }

        for (const auto& entry : components) {
            const auto& name = entry.first;
            for (const auto& trigger : entry.second.spec.triggers) {
                if (trigger.kind != core::TriggerKind::kTaskDependency) {
                    continue;
                }
                if (trigger.task_dependencies.empty()) {
                    return core::Status::InvalidArgument("task dependency trigger requires at least one task dependency");
                }
                const auto trigger_index = states[name].size();
                states[name].push_back(TaskTriggerState {
                    trigger.dependency_policy,
                    std::vector<bool>(trigger.task_dependencies.size(), false)
                });
                std::set<std::string> seen;
                for (std::size_t index = 0; index < trigger.task_dependencies.size(); ++index) {
                    const auto& upstream = trigger.task_dependencies[index].str();
                    if (upstream == name || !seen.insert(upstream).second) {
                        return core::Status::InvalidArgument("self or duplicate task dependency: " + upstream);
                    }
                    if (components.find(upstream) == components.end()) {
                        return core::Status::InvalidArgument("task dependency is not registered in scheduler: " + upstream);
                    }
                    dependents[upstream].push_back(TaskDependent {name, trigger_index, index});
                    ++indegree[name];
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
                if (--indegree[dependent.component] == 0) {
                    ready.push_back(dependent.component);
                }
            }
        }
        if (visited != components.size()) {
            return core::Status::InvalidArgument("task dependency graph contains a cycle");
        }

        for (auto& entry : components) {
            entry.second.task_triggers = std::move(states[entry.first]);
        }
        task_dependents = std::move(dependents);
        return core::Status::Ok();
    }

    core::Status Enqueue(
        const core::ComponentName& name,
        core::Nanoseconds deadline)
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!running || stopping) {
                return RunningRequired();
            }
            pending_events.push_back(ScheduledEvent {name, deadline});
        }

        event_available.notify_one();
        return core::Status::Ok();
    }

    void OnDataAvailable(const core::ComponentName& name, std::size_t trigger_index, std::size_t topic_index)
    {
        bool enqueue = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!running || stopping) {
                return;
            }
            auto& trigger = components.at(name.str()).data_triggers.at(trigger_index);
            if (trigger.policy == core::DependencyPolicy::kAny) {
                enqueue = true;
            } else {
                trigger.ready[topic_index] = true;
                enqueue = std::all_of(trigger.ready.begin(), trigger.ready.end(), [](bool ready) {
                    return ready;
                });
                if (enqueue) {
                    std::fill(trigger.ready.begin(), trigger.ready.end(), false);
                }
            }
            if (enqueue) {
                pending_events.push_back(ScheduledEvent {name, core::Nanoseconds::zero()});
            }
        }
        if (enqueue) {
            event_available.notify_one();
        }
    }

    core::Status InstallDataTriggers(std::weak_ptr<Impl> weak_self)
    {
        std::vector<core::ComponentName> names;
        {
            std::lock_guard<std::mutex> lock(mutex);
            names.reserve(components.size());
            for (const auto& entry : components) {
                names.push_back(core::ComponentName::Unsafe(entry.first));
            }
        }

        for (const auto& name : names) {
            auto spec = runtime.FindComponent(name);
            if (!spec.ok()) {
                return spec.status();
            }

            for (const auto& trigger : spec.value().triggers) {
                if (trigger.kind != core::TriggerKind::kData) {
                    continue;
                }

                std::size_t trigger_index;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    auto& states = components.at(name.str()).data_triggers;
                    trigger_index = states.size();
                    states.push_back(DataTriggerState {
                        trigger.dependency_policy,
                        std::vector<bool>(trigger.data_dependencies.size(), false)
                    });
                }

                for (std::size_t topic_index = 0; topic_index < trigger.data_dependencies.size(); ++topic_index) {
                    const auto& topic = trigger.data_dependencies[topic_index];
                    auto endpoint = FindReaderEndpoint(spec.value(), topic);
                    if (!endpoint.ok()) {
                        return endpoint.status();
                    }

                    auto reader = runtime.CreateReader(MakeTriggerEndpoint(endpoint.value()));
                    if (!reader.ok()) {
                        return reader.status();
                    }

                    auto status = reader.value()->SetDataAvailableCallback(
                        [weak_self, name, trigger_index, topic_index]() {
                            if (auto self = weak_self.lock()) {
                                self->OnDataAvailable(name, trigger_index, topic_index);
                            }
                        });
                    if (!status.ok()) {
                        return status;
                    }

                    std::lock_guard<std::mutex> lock(mutex);
                    const auto found = components.find(name.str());
                    if (found == components.end()) {
                        return core::Status::NotFound(
                            "component is not registered in scheduler: " + name.str());
                    }
                    found->second.data_trigger_readers.push_back(reader.value());
                }
            }
        }

        return core::Status::Ok();
    }

    core::Status StartPeriodicTriggers(std::weak_ptr<Impl> weak_self)
    {
        std::vector<std::pair<core::ComponentName, core::Nanoseconds>> periodic_work;

        {
            std::lock_guard<std::mutex> lock(mutex);
            for (const auto& entry : components) {
                for (const auto& trigger : entry.second.spec.triggers) {
                    if (trigger.kind == core::TriggerKind::kPeriodic) {
                        periodic_work.emplace_back(
                            core::ComponentName::Unsafe(entry.first),
                            trigger.period);
                    }
                }
            }
        }

        for (const auto& work : periodic_work) {
            std::lock_guard<std::mutex> lock(mutex);
            periodic_threads.emplace_back([weak_self, work]() {
                while (true) {
                    auto self = weak_self.lock();
                    if (!self) {
                        return;
                    }

                    {
                        std::unique_lock<std::mutex> lock(self->mutex);
                        const bool should_stop = self->periodic_stop.wait_for(lock, work.second, [self]() {
                            return !self->running || self->stopping;
                        });
                        if (should_stop) {
                            return;
                        }
                    }

                    self->Enqueue(work.first, work.second);
                }
            });
        }

        return core::Status::Ok();
    }

    void DispatchLoop()
    {
        while (true) {
            ScheduledEvent event {
                core::ComponentName::Unsafe("__invalid__"),
                core::Nanoseconds::zero(),
            };

            {
                std::unique_lock<std::mutex> lock(mutex);
                event_available.wait(lock, [this]() {
                    return stopping || !pending_events.empty();
                });

                if (stopping && pending_events.empty()) {
                    break;
                }

                auto selected = pending_events.end();
                for (auto candidate = pending_events.begin(); candidate != pending_events.end(); ++candidate) {
                    if (executing_components.find(candidate->component.str()) == executing_components.end()) {
                        selected = candidate;
                        break;
                    }
                }
                if (selected == pending_events.end()) {
                    event_available.wait(lock);
                    continue;
                }
                event = *selected;
                pending_events.erase(selected);
                executing_components.insert(event.component.str());
                ++active_events;
            }

            auto status = ExecuteNow(event);

            {
                std::lock_guard<std::mutex> lock(mutex);
                if (!status.ok()) {
                    last_status = status;
                }
                ++dispatched_events;
                --active_events;
                executing_components.erase(event.component.str());
                event_available.notify_all();
                if (status.ok() && !stopping) {
                    const auto found = task_dependents.find(event.component.str());
                    if (found != task_dependents.end()) {
                        for (const auto& dependent : found->second) {
                            auto& trigger = components.at(dependent.component).task_triggers.at(dependent.trigger_index);
                            bool enqueue = false;
                            if (trigger.policy == core::DependencyPolicy::kAny) {
                                enqueue = true;
                            } else {
                                trigger.ready[dependent.dependency_index] = true;
                                enqueue = std::all_of(trigger.ready.begin(), trigger.ready.end(), [](bool ready) {
                                    return ready;
                                });
                                if (enqueue) {
                                    std::fill(trigger.ready.begin(), trigger.ready.end(), false);
                                }
                            }
                            if (enqueue) {
                                pending_events.push_back(ScheduledEvent {
                                    core::ComponentName::Unsafe(dependent.component),
                                    core::Nanoseconds::zero(),
                                });
                                event_available.notify_one();
                            }
                        }
                    }
                }
                if (pending_events.empty() && active_events == 0) {
                    idle.notify_all();
                }
            }
        }

        std::lock_guard<std::mutex> lock(mutex);
        if (pending_events.empty() && active_events == 0) {
            idle.notify_all();
        }
    }

    core::Status ExecuteNow(const ScheduledEvent& event)
    {
        const auto started_at = core::SteadyClock::now();
        auto status = runtime.ExecuteComponent(event.component);
        const auto execution_time = std::chrono::duration_cast<core::Nanoseconds>(
            core::SteadyClock::now() - started_at);

        const auto observer = runtime.observer();
        if (observer) {
            observer->RecordTaskExecution(
                event.component,
                execution_time,
                event.deadline,
                status.ok());

            if (!status.ok()) {
                observer->Log(observability::LogRecord {
                    observability::LogLevel::kError,
                    event.component.str(),
                    "task_execution_failed",
                    status.message(),
                    {
                        {"status", core::StatusCodeName(status.code())},
                        {"execution_us", std::to_string(
                            std::chrono::duration_cast<std::chrono::microseconds>(
                                execution_time).count())},
                    },
                });
            }

            if (event.deadline > core::Nanoseconds::zero()
                && execution_time > event.deadline) {
                observer->Log(observability::LogRecord {
                    observability::LogLevel::kWarning,
                    event.component.str(),
                    "task_deadline_missed",
                    "component execution exceeded its periodic deadline",
                    {
                        {"execution_us", std::to_string(
                            std::chrono::duration_cast<std::chrono::microseconds>(
                                execution_time).count())},
                        {"deadline_us", std::to_string(
                            std::chrono::duration_cast<std::chrono::microseconds>(
                                event.deadline).count())},
                    },
                });
            }
        }

        return status;
    }

    runtime::RuntimeContext& runtime;
    SchedulerOptions options;
    mutable std::mutex mutex;
    std::condition_variable event_available;
    std::condition_variable periodic_stop;
    std::condition_variable idle;
    std::map<std::string, ScheduledComponent> components;
    std::map<std::string, std::vector<TaskDependent>> task_dependents;
    std::deque<ScheduledEvent> pending_events;
    std::unordered_set<std::string> executing_components;
    std::vector<std::thread> periodic_threads;
    std::vector<std::thread> workers;
    std::size_t active_events {0};
    std::size_t dispatched_events {0};
    core::Status last_status;
    bool running {false};
    bool stopping {false};
};

Scheduler::Scheduler(runtime::RuntimeContext& runtime, SchedulerOptions options)
    : impl_(std::make_shared<Impl>(runtime, options))
{
}

Scheduler::~Scheduler()
{
    Stop();
}

core::Status Scheduler::RegisterComponent(const core::ComponentName& name)
{
    return impl_->RegisterComponent(name);
}

core::Status Scheduler::RegisterAllComponents()
{
    return impl_->RegisterAllComponents();
}

core::Status Scheduler::Start()
{
    return impl_->Start(impl_);
}

core::Status Scheduler::Stop()
{
    return impl_->Stop();
}

bool Scheduler::is_running() const noexcept
{
    return impl_->IsRunning();
}

core::Status Scheduler::Trigger(const core::ComponentName& name)
{
    return impl_->Trigger(name);
}

core::Status Scheduler::WaitIdle(core::Nanoseconds timeout)
{
    return impl_->WaitIdle(timeout);
}

SchedulerStats Scheduler::stats() const
{
    return impl_->Stats();
}

core::Status Scheduler::last_error() const
{
    return impl_->LastError();
}

}  // namespace puppet_master::scheduler
