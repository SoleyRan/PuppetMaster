# PuppetMaster Architecture

This document records the intended boundaries for the refactor. The goal is to
keep PuppetMaster small at the core while allowing transport backends,
schedulers, diagnostics, and deployment tooling to grow independently.

## Design Principles

- Keep public APIs transport-agnostic.
- Make lifecycle ownership explicit.
- Prefer typed interfaces over raw pointers in user-facing APIs.
- Keep adapter dependencies optional and isolated.
- Make every layer testable without a vehicle, DDS daemon, or network setup.

## Layers

### Core

Core owns stable concepts such as status codes, topic names, task names,
transport kinds, trigger kinds, queue policies, and time-related metadata. It
must not include FastDDS, ZMQ, IPC, or platform-specific headers.

The current public core is header-only and includes:

- `Status` and `Result<T>` for recoverable failures.
- Strong named values for topics, tasks, components, and transports.
- Middleware-neutral message delivery and retention policies.
- Trigger metadata used later by the scheduler.

### Transport

Transport adapters implement a common reader/writer/transport contract. FastDDS,
ZMQ, IPC, and in-memory queues should live behind this boundary. The runtime
selects a transport by configuration or registration, but it should not depend
on concrete middleware headers.

The public transport abstraction now lives under `include/puppet_master/transport`.
It exposes byte-oriented messages, endpoint configuration, reader/writer
interfaces, transport capabilities, and a lightweight registry. The in-memory
backend is part of the core library; other backends are separate targets.

The optional FastDDS adapter is built as `PuppetMaster::FastDdsAdapter` when
`PUPPETMASTER_ENABLE_FASTDDS=ON`. DDS-specific QoS and transport settings stay
inside that adapter boundary.

### Runtime

Runtime owns registries and lifecycle orchestration:

- transport registry
- component registry
- component instance and state registry
- default local transport assembly
- reader and writer creation through registered transports
- startup and shutdown sequencing for transports and components

The current runtime and component milestones add `RuntimeContext`,
`ComponentSpec`, `Component`, `ComponentContext`, and `ComponentRegistry`.
Automatic trigger dispatch lives in the scheduler layer.

The runtime should expose explicit phases such as configure, initialize, start,
stop, and shutdown.

### Component

Component APIs are the main user-facing layer. A component should declare its
inputs, outputs, trigger policy, and lifecycle callbacks without manually
creating DDS readers, writers, or scheduler internals.

The current component model exposes `Describe()`, lifecycle callbacks, and an
`Execute()` hook. Components use `ComponentContext` to create readers and
writers from declared endpoints, which keeps algorithm modules independent from
the selected transport backend.

### Scheduler

Scheduler converts periodic timers, data availability, task dependencies, and
manual triggers into executor work. Its behavior defines deadline observation,
trigger coalescing, and dependency semantics.

The current scheduler milestone supports manual, periodic, data-driven, and
task-dependency trigger dispatch through `scheduler::Scheduler`. Events pass
through a shared queue and a configurable worker pool calls
`RuntimeContext::ExecuteComponent()`. Each component executes serially while
different components may run in parallel, and each component has at most one
pending successor event. Eligible queued events are selected by descending
priority, then ascending absolute due time (enqueue time plus the effective
relative deadline); exact ties retain FIFO order. The policy is non-preemptive.
The concrete trigger declaration supplies both metadata fields, and coalescing
retains the first event's complete metadata. Multiple manual declarations use
the first declaration. A zero deadline uses the period for periodic triggers
and means unbounded for other kinds; negative deadlines are invalid. Ordinary
default-priority events without deadlines remain FIFO.

This is not a real-time scheduler and does not guarantee deadline satisfaction
or bounded waiting. Sustained higher-priority work can starve lower-priority
events. Deadline-miss observation compares the end-to-end response time (queue
wait plus execution time) with the effective relative budget.

### Configuration

Configuration is the assembly layer above core, transport, runtime, component,
and scheduler contracts. It gives applications one place to declare topics,
message policies, component endpoints, and trigger metadata.

Configuration is code-first: applications build a `ProjectConfig` directly in
C++ and then ask it to produce `EndpointConfig`, `ComponentSpec`, and
`RuntimeOptions` objects. The optional `PuppetMaster::JsonConfigAdapter` loads
the same `ProjectConfig` from JSON. Other file formats or command-line loaders
can be added the same way without changing the runtime contracts.

### Compatibility

Compatibility APIs are migration bridges for existing `itage_engine` users.
They may preserve old names and call shapes, but they must route into the new
runtime and transport contracts instead of reintroducing backend-specific
dependencies into the public API.

The current facade exposes `compat::itage::Node`, `WriterBase`, and
`ReaderBase`. It supports fixed-size struct payloads and explicit byte buffers
through the current byte-oriented transport layer. New code should still prefer
the native component, scheduler, and configuration APIs.

### Observability

Observability is a transport-neutral event and metrics layer shared by the
runtime, transports, and scheduler. It records topic throughput, message
latency, queue depth, task execution time, failures, and deadline misses.

The core `Observer` exposes event, metrics, and structured log callbacks without
depending on a monitoring product. A standard-library console sink is available
by default. GoodLog is integrated through the optional
`PuppetMaster::GoodLogAdapter` target and replaces the same process-wide sink.
Prometheus, JSON log, and Chrome trace exporters are sibling adapter targets
rather than part of the core library.

### Tooling

Tooling includes examples, configuration validation, metrics exporters, trace
generation, and migration helpers. Tooling should depend on the runtime, not the
other way around.

## Intended Public CMake Contract

Most downstream projects should link against:

```cmake
PuppetMaster::PuppetMaster
```

Dependency-free exporters are separate targets that are always built:

```cmake
PuppetMaster::PrometheusAdapter
PuppetMaster::JsonLogAdapter
PuppetMaster::ChromeTraceAdapter
```

The shared-memory transport has no third-party dependencies and is built by
default on Linux and Windows (`PUPPETMASTER_ENABLE_SHM`):

```cmake
PuppetMaster::ShmAdapter
```

Integrations with third-party dependencies are opt-in targets:

```cmake
PuppetMaster::FastDdsAdapter      # PUPPETMASTER_ENABLE_FASTDDS
PuppetMaster::JsonConfigAdapter   # PUPPETMASTER_ENABLE_JSON_CONFIG
PuppetMaster::GoodLogAdapter      # PUPPETMASTER_ENABLE_GOODLOG
```

This keeps the core contract stable while allowing applications to opt into
backend-specific dependencies.
