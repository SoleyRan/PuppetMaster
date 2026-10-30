# PuppetMaster

PuppetMaster is a C++ real-time middleware runtime for message-driven
components, transport abstraction, and deterministic task scheduling.

The project is being refined from a production-oriented autonomous driving
middleware into a cleaner, reusable foundation. The core runtime, scheduler,
configuration, observability, and in-memory transport are in place; optional
adapters add FastDDS, JSON configuration, and GoodLog integration.

## Current Status

The default build depends only on the C++ standard library:

- `include/puppet_master/...` contains the public API surface for new code.
- `include/puppet_master/core` contains transport-neutral core types, message
  policies, and the common error model.
- `include/puppet_master/transport` contains backend-neutral reader, writer,
  message, and registry abstractions, plus typed readers and writers.
- `include/puppet_master/transport/inmemory` contains the local pub/sub backend
  for single-process pipelines, scheduler tests, and component tests.
- `include/puppet_master/transport/fastdds` contains the optional FastDDS
  backend for cross-process and cross-host communication.
- `include/puppet_master/transport/shm` contains the shared-memory backend for
  same-host, cross-process communication on Linux and Windows.
- `include/puppet_master/runtime` contains the runtime context, component
  registry, and algorithm module interface used to assemble middleware
  participants.
- `include/puppet_master/scheduler` contains manual, periodic, data-driven, and
  task-dependency dispatch with priority and deadline ordering.
- `include/puppet_master/configuration` contains the code-first project
  configuration model and the optional JSON loader.
- `include/puppet_master/observability` contains structured events, runtime
  metrics, trace hooks, Prometheus, JSON log, and Chrome trace exporters, and
  the optional GoodLog adapter.
- `include/puppet_master/logging` contains the dependency-free default console
  logger and the optional `LOG_Info()` style compatibility macros.
- `include/puppet_master/compat` contains a migration facade for projects that
  still use the old `itage_engine` node/reader/writer style APIs.
- `PuppetMaster::PuppetMaster` is the canonical CMake target.
- Demos and tests cover every public module. CI builds on Linux and Windows and
  exercises the optional FastDDS and JSON configuration adapters.

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
src/                     Library and optional adapter implementations.
demo/                    Small runnable examples and an example configuration.
examples/                Standalone downstream projects built against an install.
tools/                   Command-line tools: puppet_master_config, _launch, _topic.
test/                    Unit, integration, and smoke tests.
bench/                   Transport benchmarks (PUPPETMASTER_BUILD_BENCHMARKS).
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
cmake -S . -B build -DPUPPETMASTER_ENABLE_GOODLOG=ON
cmake -S . -B build -DPUPPETMASTER_ENABLE_JSON_CONFIG=ON
cmake -S . -B build -DPUPPETMASTER_BUILD_BENCHMARKS=ON
```

`PUPPETMASTER_ENABLE_FASTDDS` builds the optional FastDDS adapter target. The
default build remains transport-neutral and does not require FastDDS.

`PUPPETMASTER_ENABLE_JSON_CONFIG` builds `PuppetMaster::JsonConfigAdapter`,
which requires an installed `nlohmann_json` package.

`PUPPETMASTER_ENABLE_SHM` (default `ON`) builds `PuppetMaster::ShmAdapter` on
Linux and Windows. It needs no third-party packages.

`PUPPETMASTER_BUILD_BENCHMARKS` (default `OFF`) builds the transport
benchmark; see [Benchmarks](docs/benchmarks.md).

`PUPPETMASTER_ENABLE_GOODLOG` builds `PuppetMaster::GoodLogAdapter`. CMake uses
an installed GoodLog package when available or fetches a pinned revision from
<https://github.com/SoleyRan/GoodLog>.

## Install

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DPUPPETMASTER_ENABLE_JSON_CONFIG=ON
cmake --build build --parallel
ctest --test-dir build
cmake --install build --prefix /opt/puppetmaster
```

This installs headers under `include/`, the libraries, the CMake package under
`lib/cmake/PuppetMaster`, and, with the JSON adapter, the tools in `bin/`:
`puppet_master_config` checks configurations, `puppet_master_launch` runs and
supervises a system, and `puppet_master_topic` inspects, records, and replays
shared-memory topics (with the shm adapter too). See
[Launcher And Configuration Tools](docs/launcher.md).
Add `-DBUILD_SHARED_LIBS=ON` for shared libraries.

Before 1.0 each minor release may break the API and the ABI, so the shared
libraries' soname includes the minor version (`libpuppet_master.so.0.3`) and
the package accepts only the same minor version. Rebuild dependents when
upgrading; see [CHANGELOG](CHANGELOG.md).

## Downstream Usage

After installation, another CMake project can consume PuppetMaster with:

```cmake
find_package(PuppetMaster 0.3 CONFIG REQUIRED)
target_link_libraries(my_component PRIVATE PuppetMaster::PuppetMaster)
```

Add `/opt/puppetmaster` to `CMAKE_PREFIX_PATH`. Adapters are separate
targets, available when they were built: `PuppetMaster::ShmAdapter`,
`PuppetMaster::JsonConfigAdapter`, `PuppetMaster::FastDdsAdapter`,
`PuppetMaster::GoodLogAdapter`, and the exporter adapters. The package sets
`PUPPETMASTER_WITH_SHM`, `PUPPETMASTER_WITH_JSON_CONFIG`,
`PUPPETMASTER_WITH_FASTDDS`, and `PUPPETMASTER_WITH_GOODLOG` to tell which.

[`examples/vehicle_pipeline`](examples/vehicle_pipeline/README.md) is a
complete downstream system, packaged the way separate teams would release it:
interface packages with versioned message types and shared topic files, node
packages that import them, and an integration with a deployment file and a
system-wide check. CI builds each package separately against the install,
runs the three processes over shared memory, and checks that a mismatched
message version is refused. See
[Multi-File Configuration](docs/configuration.md#multi-file-configuration).

## Roadmap

The foundation milestones are complete: project skeleton and CMake package,
core types and error model, transport abstraction, in-memory transport, runtime
context and component registry, component model, scheduler triggers,
configuration system, `itage_engine` compatibility facade, and observability.

Follow-up phases, all complete:

1. Documentation alignment. Keep README and module docs consistent with the
   implemented behavior. Done.
2. FastDDS adapter completion. Document intentional limitations and remove the
   legacy DDS node. Done.
3. Legacy source cleanup. Remove the uncompiled `src/base`, `src/common`,
   `src/trigger`, and `src/utils` migration areas. Done.
4. Shared-memory IPC transport. A same-host, cross-process backend behind the
   existing transport contract, with crash recovery and zero-copy writes
   through `Writer::WriteWith`. Done. See
   [Shared-Memory Transport](docs/shm-transport.md).
5. User-facing capabilities. Done:
   - Typed message readers and writers over the byte transport.
   - Scheduler starvation protection through priority aging.
   - Configuration validation, a `puppet_master_config` CLI, and an
     `Application` launcher that runs a JSON configuration end to end.
6. Downstream validation. Done: component parameters, message versions,
   multi-file configuration with imports and deployments, system checks, and
   the vehicle pipeline example.
7. Services and release 0.2. Done: request/reply services on every
   transport, system manifests, a portable codec, shared-memory segment
   tools, benchmarks, and Windows JSON CI. See [CHANGELOG](CHANGELOG.md).
8. Scheduled services. Done: service triggers, asynchronous calls, and a
   demo.
9. Operations tools. Done: a system launcher with restart policies, topic
   list/echo/hz, and record/replay. See [Recordings](docs/recording.md).

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
See [Shared-Memory Transport](docs/shm-transport.md) for same-host IPC.
See [Typed Messages](docs/typed-messages.md) for struct and codec-based
endpoints.
See [Services](docs/services.md) for request/reply calls with timeouts and
server liveness.
See [Configuration System](docs/configuration.md) for code-first project
assembly.
See [Launcher And Configuration Tools](docs/launcher.md) for running
JSON-configured applications, checking configurations in CI, launching a
system, and inspecting its topics.
See [Recordings](docs/recording.md) for recording and replaying topics.
See [itage_engine Compatibility Facade](docs/compatibility-facade.md) for the
transitional node/reader/writer API.
See [Observability](docs/observability.md) for runtime metrics, trace hooks,
structured logging, and GoodLog integration.
