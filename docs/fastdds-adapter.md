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
- Concrete typed reader/writer binding is intentionally left for the next
  adapter step.

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

FastDDS normally publishes generated or registered native sample types. The
transport abstraction currently moves byte payloads, so the next adapter step
needs an explicit native type binding strategy before creating actual
DataReaders and DataWriters.

The planned direction is:

- keep the generic `transport::Reader` and `transport::Writer` contract
- add FastDDS endpoint binding metadata for native type support
- implement typed sample read/write behind the adapter
- keep generated FastDDS headers out of core and runtime APIs
