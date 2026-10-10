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
- a serialized dispatcher queue
- basic scheduler stats and idle waiting

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

All trigger events pass through one dispatcher queue. This keeps the first
implementation deterministic and prevents the same component from being
executed concurrently by periodic and data triggers.

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

`Trigger()` only accepts components that declare a manual trigger.

## Periodic Triggers

Periodic triggers start lightweight timer threads. Timer threads do not execute
component code directly; they only enqueue readiness events. The dispatcher
thread performs the actual `ExecuteComponent()` call.

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
resets that trigger's readiness. `kAny` continues to enqueue on each notification.

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
trigger. Failed executions do not propagate readiness. `kAny` dispatches once
for each successful completion of any named upstream. `kAll` dispatches after
every named upstream has completed successfully since the previous dispatch;
repeated completions from one upstream count only once until the others finish.
A downstream execution may in turn trigger another downstream task, allowing
chains. `Stop()` and the next `Start()` discard partial readiness.

`Start()` rejects unresolved dependencies, duplicate names within a dependency
list, self-dependencies, and cycles in the registered task graph. Register all
upstream components before starting the scheduler.

## Current Limitations

The scheduler is intentionally small in this branch:

- no executor pool yet
- no priority or deadline policy yet
- no trigger coalescing policy yet

Those should be built on top of the current queue and trigger model instead of
being mixed into the first implementation.
