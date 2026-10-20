# FastDDS Adapter

The FastDDS adapter is optional. The default PuppetMaster build remains
transport-neutral and does not require FastDDS headers or libraries.

## Build Option

Enable the adapter with:

```bash
cmake -S . -B build -DPUPPETMASTER_ENABLE_FASTDDS=ON
```

For an explicit cross-process UDP smoke executable (used by the FastDDS CI
job), also enable:

```bash
cmake -S . -B build \
  -DPUPPETMASTER_ENABLE_FASTDDS=ON \
  -DPUPPETMASTER_BUILD_FASTDDS_CROSS_PROCESS_SMOKE=ON
```

The smoke executable starts independent publisher and subscriber processes and
checks binary payload transfer, acknowledgement, and read timeout behavior.
It is intentionally opt-in because discovery depends on the host network.

When enabled, PuppetMaster builds an additional target:

```cmake
PuppetMaster::FastDdsAdapter
```

The main target remains:

```cmake
PuppetMaster::PuppetMaster
```

For an installed adapter, downstream projects must make the installed Fast-CDR
and Fast-DDS packages discoverable alongside PuppetMaster (via
`CMAKE_PREFIX_PATH` or their package-specific `*_DIR` variables). The adapter
links publicly to `fastcdr` and `fastdds` on Fast-DDS 3.x, or to `fastcdr` and
`fastrtps` on Fast-DDS 2.x. Consumers only need to link the exported adapter
target; the package config resolves those transitive dependencies:

```cmake
find_package(PuppetMaster CONFIG REQUIRED)
target_link_libraries(my_component PRIVATE PuppetMaster::FastDdsAdapter)
```

## Scope

The adapter implements the full transport contract over FastDDS 2.x and 3.x:

- FastDDS-specific options live under `puppet_master/transport/fastdds`.
- `core::MessagePolicy` is mapped to a FastDDS-specific QoS profile.
- `FastDdsTransport` owns participant, publisher, and subscriber lifecycle.
- `BytePayloadType`-based DataReader/DataWriter binding is implemented.
- `RegisterTransport()` registers the adapter with a `RuntimeContext`.
- Cross-process UDP delivery is covered by an opt-in smoke test in CI.

## Policy Mapping

Core keeps backend-neutral intent:

- `DeliveryGuarantee`
- `RetentionPolicy`
- `FreshnessPolicy`
- `QueueOverflowPolicy`
- `queue_depth`

The adapter maps delivery to best-effort or reliable DDS QoS, and maps
freshness and retention to DDS history:

- Reader `kLatest`: keep-last history with depth 1, regardless of `queue_depth`.
- Reader `kQueued` + `kKeepLast`: keep-last history with `queue_depth`.
- Reader `kQueued` + `kKeepAll`: keep-all history (not limited by `queue_depth`,
  but still subject to DDS resource limits).
- Writer history follows retention and `queue_depth` independently of reader
  freshness, so a latest-only reader does not force the writer cache to depth 1.
- Only `kDropOldest` is accepted for reader-history overflow. `kDropNewest`,
  `kBlock`, and `kReject` return `Unsupported` at endpoint validation.
- A keep-last depth greater than the DDS signed 32-bit limit returns
  `InvalidArgument` rather than being truncated.

These are DDS history settings, not an in-memory mailbox: DDS writer resource
limits, reliability, and asynchronous publication may produce different
backpressure and drop behavior. The accepted `kDropOldest` describes reader
history replacement, not a guarantee that every writer write succeeds or that
all samples are delivered. Volatile or transient-local durability and UDP/SHM
transport selection stay in adapter options instead of leaking into core.

## Reader And Writer Binding

The transport abstraction moves byte payloads, so the adapter registers a
`BytePayloadType` (CDR encapsulation header, little-endian uint32 payload
length, then opaque bytes) once per `type_name` and binds DataReaders and
DataWriters to it.

- `transport::Reader` supports listener callbacks (invoked outside locks) and
  blocking reads via `ReadOptions{wait, timeout}`; `timeout == 0` waits
  indefinitely, an expired timeout returns `DeadlineExceeded`, and a
  non-waiting read with no data returns `Unavailable`.
- Endpoints whose descriptor conflicts with an existing topic return
  `InvalidArgument`.
- After `Close()`, remaining reader/writer handles return `Unavailable`.
- Received messages carry a `reception_timestamp` but leave
  `source_timestamp` unset, so the runtime records no topic latency for
  FastDDS readers. An explicit `WriteOptions::source_timestamp` returns
  `Unsupported`: a process-local steady-clock time cannot be preserved across
  participants.
- `supports_zero_copy` is `false`. `Options::data_sharing` defaults to false;
  explicitly enabling it returns `Unsupported` because the byte payload type
  is unbounded and cannot use FastDDS data-sharing.
- Generated FastDDS headers stay out of core and runtime APIs.

## Known Limitations

These are intentional for the byte-oriented transport contract and are not
planned for this adapter:

- No zero-copy or data-sharing. Loaned samples need a bounded type, while
  `BytePayloadType` is unbounded. Zero-copy is a goal of the planned
  shared-memory IPC transport instead.
- No cross-participant latency. See the timestamp rule above.
- Only `kDropOldest` overflow, because DDS reader history has no equivalent
  for drop-newest, reject, or block.
- No matched-endpoint or incompatible-QoS reporting. DDS discovery events are
  not part of the transport contract.
- DDS resource limits, discovery lease, and data representation use FastDDS
  defaults. Only the settings in `Options` are configurable.

## Migration From The Legacy DDS Node

The previous middleware's `DDSNode`, `DDSReader<T>`, and `DDSWriter` have been
removed. Their behavior maps to this adapter as follows:

| Legacy | Adapter |
|---|---|
| `TransType_UDP` / `TransType_SHM` / `TransType_BOTH` | `Options::transport_mode` = `kUdp` / `kSharedMemory` / `kHybrid` |
| `UDP_BUFFER_SIZE`, `SHM_SEGMENT_SIZE` | `Options::udp_buffer_size`, `Options::shm_segment_size` |
| `QOS_PROFILE_*` macros | `core::MessagePolicy` on the topic, mapped by `MapMessagePolicy()` |
| `TRANSIENT_LOCAL` durability | `Options::durability` |
| `is_fresh` reader flag | `FreshnessPolicy::kLatest` |
| Generated `TopicDataType` structs | Byte payloads; `compat::itage` handles fixed-size structs |
| Writer loan-based zero-copy | Not supported (see Known Limitations) |
