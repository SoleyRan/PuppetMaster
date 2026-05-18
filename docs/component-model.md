# Component Model

The component model defines how user algorithm modules plug into PuppetMaster.
It builds on the previous runtime context work: components declare their
endpoints and triggers, then the runtime drives a small lifecycle around that
declaration.

## Design Goal

The interface is intentionally narrow:

- `ComponentSpec` describes what a module needs
- `Component` implements the algorithm module
- `ComponentContext` gives the module access to runtime services
- `RuntimeContext` owns registration and lifecycle state

Transport details stay behind `Reader`, `Writer`, and `Transport`. Scheduler
details stay out of the component interface for now.

## ComponentSpec

`ComponentSpec` remains the declarative part of a module:

- component name
- description
- reader endpoints
- writer endpoints
- trigger specs

This lets the runtime inspect a component before it starts executing. The
scheduler can later use the same spec to wire periodic, data-driven, dependency,
or manual triggers.

## Component

`Component` is the runtime-facing base class for algorithm modules:

```cpp
class MyAlgorithm final : public puppet_master::runtime::Component {
public:
    puppet_master::runtime::ComponentSpec Describe() const override;

    puppet_master::core::Status Configure(
        puppet_master::runtime::ComponentContext& context) override;

    puppet_master::core::Status Execute(
        puppet_master::runtime::ComponentContext& context) override;
};
```

The lifecycle is explicit:

```text
created -> configured -> initialized -> started -> stopped -> shutdown
```

`Execute()` is only valid after the component has started. A failed lifecycle
callback moves the component to `error`.

## ComponentContext

`ComponentContext` is passed into lifecycle callbacks. It lets a module create
readers and writers from its declared endpoints:

```cpp
auto writer = context.CreateWriter(spec_.writers.front());
auto reader = context.CreateReader(spec_.readers.front());
```

This keeps algorithm code independent from concrete transports. The same module
can use in-memory transport in tests and a DDS-backed transport later, as long
as the endpoint spec selects the desired backend.

## Runtime Ownership

`RuntimeContext::RegisterComponent(ComponentPtr)` stores both:

- the component declaration returned by `Describe()`
- the component instance and its current state

The older `RegisterComponent(ComponentSpec)` path still exists for pure
declarative registration, but lifecycle operations require a real component
instance.

## What This Milestone Does Not Do

This milestone does not implement the scheduler. It does not automatically
invoke `Execute()` from timers or data callbacks. That remains the next natural
step: the scheduler can read registered component specs, observe their triggers,
and call `ExecuteComponent()` when a component becomes ready.
