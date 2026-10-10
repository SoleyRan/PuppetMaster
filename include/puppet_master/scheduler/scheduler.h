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

struct SchedulerOptions {
    std::size_t worker_count {1};
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
