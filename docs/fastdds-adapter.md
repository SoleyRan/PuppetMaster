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

The adapter maps that intent to FastDDS concepts:

- best-effort or reliable delivery
- keep-last or keep-all history
- history depth
- volatile or transient-local durability

FastDDS-only details such as transient-local durability and UDP/SHM transport
selection stay in adapter options instead of leaking into core.

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
- `source_timestamp` is the local receive time.
- `supports_zero_copy` is `false`; data-sharing is not used for byte payloads.
- Generated FastDDS headers stay out of core and runtime APIs.
