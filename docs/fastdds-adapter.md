# FastDDS Adapter

The FastDDS adapter is optional. The default PuppetMaster build remains
transport-neutral and does not require FastDDS headers or libraries.

## Build Option

Enable the adapter with:

```bash
cmake -S . -B build -DPUPPETMASTER_ENABLE_FASTDDS=ON
```

When enabled, PuppetMaster builds an additional target:

```cmake
PuppetMaster::FastDdsAdapter
```

The main target remains:

```cmake
PuppetMaster::PuppetMaster
```

## Current Scope

This milestone establishes the clean FastDDS adapter boundary:

- FastDDS-specific options live under `puppet_master/transport/fastdds`.
- `core::MessagePolicy` is mapped to a FastDDS-specific QoS profile.
- `FastDdsTransport` owns participant, publisher, and subscriber lifecycle.
- `BytePayloadType`-based DataReader/DataWriter binding is implemented.

The old files under `src/communication/fastdds` remain migration references.
They are not compiled into the new adapter target.

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
`BytePayloadType` (sequence number + opaque bytes) once per `type_name` and
binds DataReaders and DataWriters to it.

- `transport::Reader` supports listener callbacks (invoked outside locks) and
  blocking reads via `ReadOptions{wait, timeout}`; `timeout == 0` waits
  indefinitely, an expired timeout returns `DeadlineExceeded`, and a
  non-waiting read with no data returns `Unavailable`.
- Endpoints whose descriptor conflicts with an existing topic return
  `InvalidArgument`.
- After `Close()`, remaining reader/writer handles return `Unavailable`.
- With default write options, `source_timestamp` is the local receive time.
  An explicit `WriteOptions::source_timestamp` returns `Unsupported`: a
  process-local steady-clock time cannot be preserved across participants.
- `supports_zero_copy` is `false`. `Options::data_sharing` defaults to false;
  explicitly enabling it returns `Unsupported` because the byte payload type
  is unbounded and cannot use FastDDS data-sharing.
- Generated FastDDS headers stay out of core and runtime APIs.
