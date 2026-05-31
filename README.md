# PuppetMaster

PuppetMaster is a C++ real-time middleware runtime for message-driven
components, transport abstraction, and deterministic task scheduling.

The project is being refined from a production-oriented autonomous driving
middleware into a cleaner, reusable foundation. The first refactor milestone
focuses on a buildable project skeleton, a stable public include layout, and a
CMake package that downstream projects can consume.

## Current Status

This branch intentionally keeps the default build small:

- `include/puppet_master/...` contains the public API surface for new code.
- `include/puppet_master/core` contains transport-neutral core types, message
  policies, and the common error model.
- `include/puppet_master/transport` contains backend-neutral reader, writer,
  message, and registry abstractions.
- `include/puppet_master/transport/inmemory` contains the first concrete local
  pub/sub backend for scheduler and component tests.
- `include/puppet_master/runtime` contains the runtime context, component
  registry, and algorithm module interface used to assemble middleware
  participants.
- `include/puppet_master/scheduler` contains trigger dispatch for manual,
  periodic, and data-driven component execution.
- `include/puppet_master/configuration` contains the code-first project
  configuration model used to declare topics, components, and triggers in one
  place.
- `src/communication/fastdds` is kept as an adapter migration area and is not
  built by default yet.
- `PuppetMaster::PuppetMaster` is the canonical CMake target.
- Minimal demos and smoke tests verify the project skeleton, core API, transport
  abstraction, in-memory pub/sub behavior, runtime assembly, scheduler triggers,
  and configuration assembly.

## Goals

- Decouple component logic from communication backends.
- Support multiple transports such as in-memory queues, FastDDS, ZMQ, and IPC.
- Provide deterministic task scheduling with periodic, data-driven,
  dependency-driven, and manual triggers.
- Make configuration, lifecycle management, metrics, and diagnostics first
  class parts of the runtime.

## Layout

```text
include/puppet_master/   Public headers for downstream projects.
src/                     Library implementation and adapter migration area.
demo/                    Small runnable examples.
test/                    Smoke tests and future unit tests.
cmake/                   Package configuration templates.
docs/                    Architecture notes and design decisions.
```

## Build

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build
```

Useful options:

```bash
cmake -S . -B build -DPUPPETMASTER_BUILD_DEMOS=OFF
cmake -S . -B build -DPUPPETMASTER_BUILD_TESTS=OFF
cmake -S . -B build -DPUPPETMASTER_ENABLE_FASTDDS=ON
```

`PUPPETMASTER_ENABLE_FASTDDS` builds the optional FastDDS adapter target. The
default build remains transport-neutral and does not require FastDDS.

## Downstream Usage

After installation, another CMake project can consume PuppetMaster with:

```cmake
find_package(PuppetMaster CONFIG REQUIRED)
target_link_libraries(my_component PRIVATE PuppetMaster::PuppetMaster)
```

## Roadmap

1. Project skeleton and public CMake package. Done.
2. Core types and error model. Done.
3. Transport abstraction. Done.
4. In-memory transport for tests and local pipelines. Done.
5. FastDDS adapter as an optional backend. In progress.
6. Runtime context and component registry. Done.
7. Component model and algorithm module interface. Done.
8. Scheduler triggers. Done.
9. Code-first configuration system. In progress.
10. Observability and production demos.

See [Architecture](docs/architecture.md) for the intended module boundaries.
See [Core API](docs/core.md) for the current public core model.
See [Transport Abstraction](docs/transport.md) for the backend contract.
See [In-Memory Transport](docs/inmemory-transport.md) for the local pub/sub
backend.
See [Runtime Context](docs/runtime.md) for runtime assembly and registries.
See [Component Model](docs/component-model.md) for the algorithm module
interface.
See [Scheduler](docs/scheduler.md) for trigger dispatch and component execution.
See [FastDDS Adapter](docs/fastdds-adapter.md) for the optional DDS backend.
See [Configuration System](docs/configuration.md) for code-first project
assembly.
