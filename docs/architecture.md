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

Transport adapters implement a common reader/writer/node contract. FastDDS,
ZMQ, IPC, and in-memory queues should live behind this boundary. The runtime
selects a transport by configuration or registration, but it should not depend
on concrete middleware headers.

### Runtime

Runtime owns registries and lifecycle orchestration:

- transport registry
- topic registry
- component registry
- task registry
- executor ownership
- startup and shutdown sequencing

The runtime should expose explicit phases such as configure, initialize, start,
stop, and shutdown.

### Component

Component APIs are the main user-facing layer. A component should declare its
inputs, outputs, trigger policy, and lifecycle callbacks without manually
creating DDS readers, writers, or scheduler internals.

### Scheduler

Scheduler converts periodic timers, data availability, task dependencies, and
manual triggers into executor work. Its behavior must define deadline handling,
queue overflow, trigger coalescing, and dependency semantics.

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
