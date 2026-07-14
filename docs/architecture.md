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
interfaces, transport capabilities, and a lightweight registry. Concrete
backends remain separate implementation milestones.

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
manual triggers into executor work. Its behavior must define deadline handling,
queue overflow, trigger coalescing, and dependency semantics.

The current scheduler milestone supports manual, periodic, and data-driven
trigger dispatch through `scheduler::Scheduler`. Events pass through a
serialized dispatcher queue and call `RuntimeContext::ExecuteComponent()`.
Task dependency graphs, executor pools, priorities, deadlines, and trigger
coalescing policies remain future work.

### Configuration

Configuration is the assembly layer above core, transport, runtime, component,
and scheduler contracts. It gives applications one place to declare topics,
message policies, component endpoints, and trigger metadata.

The current configuration milestone is code-first: applications build a
`ProjectConfig` directly in C++ and then ask it to produce `EndpointConfig`,
`ComponentSpec`, and `RuntimeOptions` objects. YAML, JSON, TOML, or command-line
loaders can be added later without changing the runtime contracts.

### Compatibility

Compatibility APIs are migration bridges for existing `itage_engine` users.
They may preserve old names and call shapes, but they must route into the new
runtime and transport contracts instead of reintroducing backend-specific
dependencies into the public API.

The current facade exposes `compat::itage::Node`, `WriterBase`, and
`ReaderBase`. It supports fixed-size struct payloads and explicit byte buffers
through the current byte-oriented transport layer. New code should still prefer
the native component, scheduler, and configuration APIs.

### Tooling

Tooling includes examples, configuration validation, metrics exporters, trace
generation, and migration helpers. Tooling should depend on the runtime, not the
other way around.

## Current Skeleton Boundary

The first refactor branch builds only the stable skeleton:

- public headers under `include/puppet_master`
- a small compiled library target
- generated version metadata
- installable CMake package files
- minimal demo and smoke test

Existing files under `src/communication/fastdds` are treated as migration
material. They will be cleaned up and connected in a dedicated FastDDS adapter
branch after the transport interface is finalized.

The core API added after the skeleton milestone remains public and
transport-neutral under `include/puppet_master/core`.

## Intended Public CMake Contract

Downstream projects should link only against:

```cmake
PuppetMaster::PuppetMaster
```

This keeps the external contract stable even if internal source directories are
reorganized during later refactor steps.
