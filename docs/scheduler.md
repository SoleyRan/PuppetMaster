# Scheduler

The scheduler turns component trigger declarations into runtime execution. It
is the first layer where PuppetMaster starts actively driving algorithm modules
instead of only registering components and transports.

## Scope

This milestone adds:

- `scheduler::Scheduler`
- manual trigger dispatch
- periodic trigger dispatch
- data trigger dispatch through reader callbacks
- task dependency dispatch after successful upstream execution
- service trigger dispatch when a request arrives for a served service
- a configurable worker pool (one worker by default)
- per-component serial execution with cross-component parallelism
- trigger coalescing with a per-component pending-event bound
- priority and relative deadline metadata for trigger events
- non-preemptive priority/deadline queue ordering
- scheduler stats, including a coalesced-event counter, and idle waiting

## Execution Model

Components still declare triggers through `core::TriggerSpec`:

```cpp
core::TriggerSpec {
    core::TriggerKind::kData,
    {},
    core::DependencyPolicy::kAny,
    {topic_name},
    {}
}
```

The scheduler reads component specs from `RuntimeContext`, validates their
triggers, then converts readiness events into `RuntimeContext::ExecuteComponent`
calls.

All trigger events pass through one queue. `TriggerSpec` has optional trailing
`priority` (`std::int32_t`, default 0) and `deadline` (`Nanoseconds`, default
zero) fields; existing five-field aggregate initializers remain valid. Higher
priority is dispatched first. Among events with equal priority, the scheduler
chooses the earliest absolute due time, computed when an event is enqueued as
the enqueue time plus its effective relative deadline. Zero means no explicit
deadline: periodic triggers then use their period as the budget, while manual,
data, and task-dependency triggers remain unbounded. Negative deadlines are
rejected by `TriggerSpec::Validate()`. Unbounded events sort after bounded
events of the same priority, and exact ties retain FIFO enqueue order. With
default priorities and no deadlines, ordinary events remain FIFO. Dispatch is
non-preemptive: metadata affects only the next eligible event, not work already
executing.

By default, one worker executes components in sequence. Pass
`SchedulerOptions {worker_count}` to the constructor to run different components
in parallel. Workers reserve a component before taking its next queued event:
events for the same component never execute concurrently, even across trigger
kinds. Ordering applies among currently eligible components; a queued event for
an executing component waits until that component is free. `worker_count == 0`
is rejected by `Start()` with `InvalidArgument`. `WaitIdle()` waits until both
queued and executing events (including dependency propagation) are finished; a
positive timeout returns `DeadlineExceeded` if work is still in progress.

## Trigger Coalescing

Manual, periodic, data, and task-dependency triggers all enter through the same
queueing function. The scheduler keeps an index of components that already have
a pending event. A component can therefore have at most one event waiting to
start. When that component is already executing, it may still have one queued
successor; additional triggers while that successor is present are collapsed
into it and counted in `SchedulerStats::coalesced_events`.

This bounds the backlog for a slow component even when timers or data callbacks
fire faster than it can finish. The first accepted event supplies all stored
metadata: priority, effective relative deadline, and absolute due time. Later
coalesced triggers do not replace any of these values, even when they come from
a different trigger declaration or have a higher priority or earlier deadline.

Coalescing applies only after a trigger's readiness rule has been evaluated.
Multi-topic and multi-task `kAll` policies still track each dependency
independently. Repeated readiness from one dependency is not allowed to satisfy
the other dependencies; once the complete dependency set is ready, the resulting
execution request participates in the same one-event pending bound.

## Manual Triggers

Manual triggers are useful for tests, demos, command-driven tools, and future
runtime control APIs.

```cpp
scheduler::Scheduler sched(context);
sched.RegisterAllComponents();
sched.Start();
sched.Trigger(component_name);
sched.WaitIdle(std::chrono::milliseconds(500));
```

`Trigger()` only accepts components that declare a manual trigger. If a
component declares multiple manual triggers, `Trigger()` uses the first one in
declaration order for priority and deadline.

## Periodic Triggers

Periodic triggers start lightweight timer threads. Timer threads do not execute
component code directly; they only enqueue readiness events. Worker threads
perform the actual `ExecuteComponent()` call.

```cpp
core::TriggerSpec {
    core::TriggerKind::kPeriodic,
    std::chrono::milliseconds(10),
    {},
    {},
    {}
}
```

`Stop()` wakes periodic waits, joins timer threads, drains pending events, and
then returns.

## Data Triggers

Data triggers are connected through transport reader callbacks. For each data
dependency, the scheduler creates a small trigger reader on the same topic and
sets `SetDataAvailableCallback()`.

The trigger reader uses latest-data mailbox semantics internally so it does not
grow an unconsumed queue. The component still owns its normal reader and can
consume the actual message during `Execute()`.

This design keeps scheduler readiness separate from component data consumption.
For a multi-topic `kAll` trigger, each dependency must report new data since
its previous dispatch. Repeated notifications from one topic count only once;
when all dependencies are ready, the scheduler enqueues one execution and
resets that trigger's readiness. `kAny` requests an execution on each
notification; those requests are coalesced if the component already has a
pending event.

## Task Dependency Triggers

A task dependency refers to a registered component by its task name (the
component name). For example, after registering and bringing up both components:

```cpp
core::TriggerSpec {
    core::TriggerKind::kTaskDependency,
    {},
    core::DependencyPolicy::kAll,
    {},
    {core::TaskName::Unsafe("upstream")}
}
```

A successful upstream execution marks that task ready for each downstream
trigger. Failed executions do not propagate readiness. `kAny` requests an
execution for each successful completion of any named upstream, subject to the
pending-event coalescing bound. `kAll` dispatches after
every named upstream has completed successfully since the previous dispatch;
repeated completions from one upstream count only once until the others finish.
A downstream execution may in turn trigger another downstream task, allowing
chains. `Stop()` and the next `Start()` discard partial readiness.

`Start()` rejects unresolved dependencies, duplicate names within a dependency
list, self-dependencies, and cycles in the registered task graph. Register all
upstream components before starting the scheduler.

## Service Triggers

A service trigger runs a component when a request arrives for a service it
serves, so requests are handled under the component's priority and deadline,
serially with its other triggers, instead of on a thread of the server's own:

```cpp
core::TriggerSpec trigger;
trigger.kind = core::TriggerKind::kService;
trigger.service_dependencies = {core::TopicName::Unsafe("/map/query")};
trigger.priority = 10;
```

Each named service must be in the component's `ComponentSpec::servers`; the
scheduler rejects the component otherwise. As for data triggers, the scheduler
watches the service's request topic with its own latest-only reader, and the
component reads the requests itself: it creates its `service::Server` with
`ServeMode::kCaller` and calls `ServePending()` in `Execute()`. Requests that
arrive while the component is queued or running coalesce into the next
execution, which drains them; see
[Scheduled Servers](services.md#scheduled-servers).

## Deadline Observation and Limitations

`RecordTaskExecution()` receives both the queue wait (enqueue to dispatch) and
the `ExecuteComponent()` duration. The deadline-miss metric compares the
end-to-end response time, queue wait plus execution time, with the event's
effective relative budget. A miss also emits a `task_deadline_missed` warning
log with `execution_us`, `queue_wait_us`, `response_us`, and `deadline_us`
fields. Events without an effective deadline never count as misses.

This policy is not a real-time guarantee. Dispatch cannot preempt a running
component, deadlines do not force timely execution, and by default sustained
higher-priority work can starve lower-priority events. The scheduler does not
promise bounded queue waiting or deadline satisfaction.

## Starvation Protection

`SchedulerOptions::priority_aging_interval` enables priority aging:

```cpp
scheduler::SchedulerOptions options;
options.priority_aging_interval = std::chrono::milliseconds(10);
```

While an event waits in the queue, its effective priority rises by one level
for every full interval, so an event of priority 0 that has waited 50 ms
competes as priority 5. Aging only changes which queued event is dispatched
next; it never preempts a running component, and the declared priority still
appears in trace and log output. Ties on effective priority fall back to the
due time and then FIFO order, as without aging.

Zero, the default, keeps strict priority order. Choose an interval that is
long compared with normal queue waits, so aging only matters for events that
are actually starved; a very short interval makes the queue close to FIFO.
`SchedulerStats::aged_dispatches` counts dispatches where aging picked an
event over one with a higher declared priority. In JSON configuration the
option is `scheduler.priority_aging_interval_ns`.
