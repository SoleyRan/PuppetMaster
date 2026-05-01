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
- `src/communication/fastdds` is kept as an adapter migration area and is not
  built by default yet.
- `PuppetMaster::PuppetMaster` is the canonical CMake target.
- A minimal demo and smoke test verify the project skeleton.

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

`PUPPETMASTER_ENABLE_FASTDDS` is reserved for the upcoming FastDDS adapter
branch. It does not pull the staging adapter into the default build yet.

## Downstream Usage

After installation, another CMake project can consume PuppetMaster with:

```cmake
find_package(PuppetMaster CONFIG REQUIRED)
target_link_libraries(my_component PRIVATE PuppetMaster::PuppetMaster)
```

## Roadmap

1. Project skeleton and public CMake package.
2. Core types, status model, and transport abstraction.
3. In-memory transport for tests and local pipelines.
4. FastDDS adapter as an optional backend.
5. Runtime registry, component lifecycle, and scheduler.
6. Configuration, observability, and production demos.

See [Architecture](docs/architecture.md) for the intended module boundaries.
