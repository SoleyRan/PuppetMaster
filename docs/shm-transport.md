# Shared-Memory Transport

The shared-memory transport moves messages between processes on the same host
without a broker or third-party dependency. It implements the standard
`Transport`, `Reader`, and `Writer` contract under `core::TransportKind::kIpc`,
so components switch to it by changing a topic's transport kind. The design and
its rationale are in [Shared-Memory Transport Design](shm-transport-design.md).

Supported on Linux and Windows. `Write()` copies the payload into shared memory;
`WriteWith()` lets the producer write it there directly (see
[Zero-Copy Writes](#zero-copy-writes)). Readers always receive a private copy.

## Build

The adapter is built by default where supported:

```bash
cmake -S . -B build                          # PuppetMaster::ShmAdapter included
cmake -S . -B build -DPUPPETMASTER_ENABLE_SHM=OFF
```

```cmake
find_package(PuppetMaster CONFIG REQUIRED)
target_link_libraries(my_component PRIVATE PuppetMaster::ShmAdapter)
```

## Usage

```cpp
#include <puppet_master/transport/shm/shm_transport.h>

namespace shm = puppet_master::transport::shm;

shm::Options options;
options.domain_id = 3;                                       // isolates deployments
options.topics["/lidar/points"] = shm::TopicOptions {4 << 20, 8};  // 4 MiB x 8 slots

auto status = shm::RegisterTransport(*runtime, name, options);
```

Topics use `core::TransportKind::kIpc`. In configuration files the transport
may be written as `"ipc"` or `"shm"`.

## Configuration

In a configured application, the domain and ring sizes come from each ipc
topic's `transport_parameters`, normally set by a deployment file (see
[Deployment Files](configuration.md#deployment-files)):

| Key | Meaning |
|---|---|
| `slot_payload_size` | max bytes per message on this topic |
| `slot_count` | ring capacity of this topic |
| `domain_id` | `Options::domain_id`; every topic that sets it must agree |

`OptionsFromProject` turns them into `Options`, starting from a base you
pass. Register it through the project hook so it sees the loaded project:

```cpp
options.register_project_transports = [](runtime::RuntimeContext& context, const config::ProjectConfig& project) {
    auto shm_options = shm::OptionsFromProject(project);
    if (!shm_options.ok()) {
        return shm_options.status();  // unknown key or invalid value, naming the topic
    }
    return shm::RegisterTransport(context, core::TransportName::Unsafe("shm"), std::move(shm_options).value());
};
```

Because every process reads the same deployment file, they agree on the
geometry of each topic.

## Zero-Copy Writes

`WriteWith` hands the producer the shared-memory slot itself, so a large
message is serialized once, directly where readers will copy it from:

```cpp
auto status = writer->WriteWith(max_size, [&](transport::MutableByteView buffer) {
    const std::size_t used = SerializePointCloud(cloud, buffer.data(), buffer.size());
    return core::Result<std::size_t>(used);
});
```

`max_size` must not exceed the topic's `slot_payload_size`, and `fill` must
return at most `max_size`. The callback runs while this process holds the
topic's cross-process writer lock, so:

- keep it short: other writers on the topic wait for it;
- do not write to the same topic from inside it;
- return an error, or throw, to cancel: nothing is published and the lock is
  released.

It works the same through `RuntimeContext::CreateWriter`, and topic metrics
record the size `fill` returned.

## Topic Geometry

Each topic is a fixed ring of `slot_count` slots of `slot_payload_size` bytes.
The default is 64 KiB x 16 slots, about 1 MiB per topic. Size large-payload
topics (point clouds, images) explicitly.

Every process on a topic must use the same geometry and message descriptor
(type name, encoding, and version).
The first process creates the segment; a later process with different values
gets `FailedPrecondition` (geometry) or `InvalidArgument` (descriptor) from
`CreateReader` or `CreateWriter`, with the segment name in the message.

## Policies

| Policy | Behavior |
|---|---|
| `FreshnessPolicy::kLatest` | Reader returns only the newest message. |
| `kQueued` + `kKeepLast` | Reader sees up to `queue_depth` unread messages. `queue_depth` above `slot_count` is `InvalidArgument`. |
| `RetentionPolicy::kKeepAll` | `Unsupported`. |
| `QueueOverflowPolicy::kDropOldest` | Supported. A slow reader loses its oldest unread messages. |
| `kDropNewest`, `kBlock`, `kReject` | `Unsupported`. |
| `kBestEffort`, `kReliable` | Both accepted; messages are lost only by overflow. |

Writers are never blocked by readers. A reader that stalls or crashes cannot
affect writers or other readers.

## Semantics

- Readers are volatile: a new reader sees only messages written after it was
  created.
- `metadata.sequence` is the topic-wide sequence, shared by all processes, so a
  gap in sequences means messages were dropped for this reader.
- `source_timestamp` is preserved across processes, including explicit
  `WriteOptions::source_timestamp` values, so topic latency metrics work.
- A payload larger than `slot_payload_size` returns `OutOfRange`.
- Callbacks run on a per-topic dispatcher thread, once per new message up to
  the reader's depth, outside all locks. An exception from a callback is
  caught and does not stop delivery.
- After `Close()`, reads, writes, and callback registration return
  `Unavailable`, and blocked reads wake with `Unavailable`.
- Up to 64 processes can use one topic. Entries of crashed processes are
  reclaimed when another process joins.

## Segment Lifetime

On Linux, segments live in `/dev/shm` as `pm.<domain>.<hash>` and persist after
all processes exit, so a restarted system reuses them. To change a topic's
geometry, stop its processes and delete the old segment:

```cpp
shm::RemoveSegment(domain_id, "/lidar/points");
```

or remove `/dev/shm/pm.<domain>.<hash>` by hand. On Windows, a segment is freed
when the last process closes it.

### Listing And Cleaning Up

`puppet_master_config shm` inspects this host's segments without attaching to
them, so it changes nothing for running processes:

```bash
puppet_master_config shm list [--domain N]
puppet_master_config shm clean [--domain N] [--dry-run]
```

`list` prints each segment's object name, state, topic, message descriptor,
geometry, messages written, and the pids of running processes attached to it.
The states are:

| State | Meaning | Removed by `clean` |
|---|---|---|
| `in_use` | a running process is attached | never |
| `unused` | valid, but every attached process has exited | yes |
| `uninitialized` | its creator crashed before publishing the header | yes |
| `incompatible` | truncated, or another layout version | yes |

A process that exits without closing its transport, for example after a
crash, leaves its participant entry behind; `list` counts it as exited, and
the segment is `unused` once no running process remains. Only names of the
form `pm.<domain>.<16 hex digits>` are considered, so other programs' files in
`/dev/shm` are never touched.

Run `clean` while the system is stopped: a process that attaches between the
check and the removal keeps working on the removed segment, but no longer
shares it with processes started later. The same operations are available
in C++ as `shm::ListSegments` and `shm::RemoveUnusedSegments`. On Windows they
return `Unsupported`, since there is nothing left over to clean.

## Limits

- Same host only. Use the FastDDS adapter across hosts.
- Linux: all processes on a topic must share one PID namespace.
- Windows: processes must share a logon session (`Local\` namespace).
- Segments are owner-only (`0600` on Linux). Any process of the same user can
  read and write a topic.
- Not available on macOS or other platforms.
