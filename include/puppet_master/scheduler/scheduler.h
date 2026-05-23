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
};

// Scheduler turns TriggerSpec declarations into ExecuteComponent() calls. This
// first implementation intentionally keeps execution local and deterministic:
// one dispatcher thread serializes trigger events, while periodic trigger
// threads only enqueue readiness events.
class Scheduler final {
public:
    explicit Scheduler(runtime::RuntimeContext& runtime);
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
