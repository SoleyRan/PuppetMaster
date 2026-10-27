# Changelog

PuppetMaster follows [Semantic Versioning](https://semver.org/). Before 1.0,
each minor release may break the API and the ABI: the shared libraries carry
the minor version in their soname (`libpuppet_master.so.0.2`), and
`find_package(PuppetMaster 0.2)` accepts only 0.2.x.

## Unreleased

### Added

- **Scheduled servers.** `TriggerKind::kService` runs a component when a
  request arrives for a service it serves, under its priority, deadline,
  and serial execution like any other trigger. The component's server uses
  `ServeMode::kCaller` and is drained with `Server::ServePending()` in
  `Execute()`. In configuration: `{"kind": "service", "services": [...]}`;
  `CreateServer` picks the mode from the component's triggers. See
  [Scheduled Servers](docs/services.md#scheduled-servers).
- **Asynchronous calls.** `Client::CallAsync` and `TypedClient::CallAsync`
  send without blocking and report the result to a callback, so a component
  can call a service from `Execute()` without holding a scheduler worker.
  See [Asynchronous Calls](docs/services.md#asynchronous-calls).

### Changed

- Breaking API: `core::TriggerSpec` has a new last field,
  `service_dependencies`, and `TriggerKind` a new value, `kService`; code
  that switches over `TriggerKind` needs a case for it.
- `Server::Create` and `MakeTypedServer` take an optional `ServerOptions`.

## 0.2.0

### Added

- **Services.** Request/reply calls between components, in one process or
  across processes, on every transport (`puppet_master/service/service.h`).
  Calls return the reply, or `Unavailable` (no live server, or it stopped
  mid-call), `DeadlineExceeded` (alive but slow), or `Cancelled`. Servers
  publish a heartbeat from their own thread and say goodbye when they stop,
  so clients tell a dead server from a slow one and notice restarts.
  Typed variants use `Codec<T>` and message versions. See
  [Services](docs/services.md).
- **Services in configuration.** `services` in JSON and interface files,
  `servers` and `clients` on components, per-service deployment settings,
  `--describe` output, and helpers that create servers and clients from a
  component's spec. `validate --system` checks services like topics: one
  contract everywhere, exactly one serving process.
- **Message versions.** `MessageTypeName<T>::version` or `Codec::Version()`;
  `message_version` on topics. Endpoints of different versions refuse to
  connect, across processes too.
- **Multi-file configuration.** `imports` of interface files through
  `PUPPETMASTER_CONFIG_PATH` and `--config-path`; per-reader queue policy;
  deployment files (`--deployment`) that set transports and shm sizes by
  wire name but cannot change what a topic carries; `validate --system` for
  checks across processes.
- **System manifests.** One file listing a system's nodes, binaries,
  configs, search paths, and deployment, read by
  `puppet_master_config system validate|describe|commands`. See
  [System Manifests](docs/configuration.md#system-manifests).
- **Portable codec.** `PortableCodec<T>`: a fixed little-endian field-by-field
  format, independent of padding, compiler, and byte order, for processes
  built with different compilers or on different architectures. Strict
  decoding: truncation or leftover bytes are `DataLoss`. See
  [Portable Codec](docs/typed-messages.md#portable-codec).
- **Shared-memory segment tools.** `shm::ListSegments`,
  `shm::RemoveUnusedSegments`, and `puppet_master_config shm list|clean`
  show leftover segments in `/dev/shm` and remove those no running process
  uses.
- **Component parameters** from configuration, with typed accessors.
- `Writer::MatchedReaderCount()`, implemented by FastDDS.
- Transport benchmarks (`PUPPETMASTER_BUILD_BENCHMARKS`). See
  [Benchmarks](docs/benchmarks.md).
- The vehicle pipeline example: interface and node packages built
  separately against an install, run as three processes over shared memory.

### Fixed

- FastDDS: a deadlock between a reader's DDS listener and a concurrent
  `Read()` on another thread. It could hang any reader whose data arrived
  while it was being read, with the runtime's queue-depth callback.

### Changed

- Breaking ABI: `transport::Writer` has a new virtual function, and
  `runtime::ComponentSpec` has new `servers` and `clients` fields. Rebuild
  everything against 0.2; the new soname keeps 0.1 binaries from loading it.
  Source stays compatible: positional brace-initializers of `ComponentSpec`
  still compile, leaving the new fields empty, though GCC and Clang warn
  with `-Wextra` (`-Wmissing-field-initializers`).
- The shared libraries' soname now includes the minor version, and the
  CMake package accepts only the same minor version.
- Shell scripts are kept at LF line endings (`.gitattributes`).

### CI

- Windows builds the JSON configuration adapter and tools with `/W4 /WX`.
- FastDDS 2.14 and 3.2 run the service tests.
- A benchmark job runs a quick pass and keeps full results as an artifact.

## 0.1.0

First release: core types and error model, transport abstraction with
in-memory, shared-memory, and FastDDS backends, typed messages, runtime
context and component model, scheduler with manual, periodic, data, and
task-dependency triggers, code-first and JSON configuration with an
`Application` launcher and `puppet_master_config`, observability with
Prometheus, JSON log, and Chrome trace exporters, and the `itage_engine`
compatibility facade.
