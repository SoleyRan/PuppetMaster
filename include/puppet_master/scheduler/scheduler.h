#pragma once

#include <cstddef>
#include <memory>

#include <puppet_master/core/result.h>
#include <puppet_master/core/status.h>
#include <puppet_master/core/types.h>
#include <puppet_master/runtime/context.h>

namespace puppet_master::scheduler {

struct SchedulerStats {
    std::size_t registered_components {0};
    std::size_t pending_events {0};
    std::size_t active_events {0};
    std::size_t dispatched_events {0};
    std::size_t coalesced_events {0};
    // Dispatches that aging moved ahead of an event with a higher declared
    // priority.
    std::size_t aged_dispatches {0};
};

struct SchedulerOptions {
    std::size_t worker_count {1};

    // Starvation protection. When positive, a queued event's effective
    // priority rises by one level for every full interval it has waited, so
    // sustained higher-priority work cannot postpone it forever. Zero keeps
    // strict priority order.
    core::Nanoseconds priority_aging_interval {0};

    core::Status Validate() const
    {
        if (worker_count == 0) {
            return core::Status::InvalidArgument("scheduler worker_count must be greater than zero");
        }
        if (priority_aging_interval < core::Nanoseconds::zero()) {
            return core::Status::InvalidArgument("scheduler priority_aging_interval must not be negative");
        }
        return core::Status::Ok();
    }
};

// Scheduler turns TriggerSpec declarations into ExecuteComponent() calls.
// Workers may execute different components concurrently; each component is
// executed serially.
class Scheduler final {
public:
    explicit Scheduler(runtime::RuntimeContext& runtime, SchedulerOptions options = {});
    ~Scheduler();

    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;
    Scheduler(Scheduler&&) = delete;
    Scheduler& operator=(Scheduler&&) = delete;

    core::Status RegisterComponent(const core::ComponentName& name);
    core::Status RegisterAllComponents();

    core::Status Start();
    core::Status Stop();
    bool is_running() const noexcept;

    core::Status Trigger(const core::ComponentName& name);
    core::Status WaitIdle(core::Nanoseconds timeout);

    SchedulerStats stats() const;
    core::Status last_error() const;

private:
    struct Impl;

    std::shared_ptr<Impl> impl_;
};

}  // namespace puppet_master::scheduler
