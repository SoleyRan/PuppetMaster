# Core API

The core API contains small, transport-neutral types shared by every future
layer of PuppetMaster. It is safe for components, runtime code, schedulers, and
transport adapters to include these headers.

## Error Model

`Status` describes recoverable failures with a compact `StatusCode` plus a
human-readable message. It is deliberately independent of FastDDS, ZMQ, IPC, or
operating-system error types.

`Result<T>` carries either a value or a `Status`. Public APIs should prefer
`Result<T>` when failure is part of normal control flow, such as invalid user
configuration, missing topics, unavailable transports, or deadline misses.

Exceptions should be reserved for programmer errors or unrecoverable internal
conditions. This keeps runtime and adapter boundaries predictable.

## Names

`TopicName`, `TaskName`, `ComponentName`, and `TransportName` are strong named
values. They prevent accidental parameter swaps and validate user-facing names
before those names enter registries.

Current validation rejects:

- empty names
- names longer than 255 bytes
- whitespace or control characters

Topic names such as `/vehicle/speed` are valid.

## Message Policy

`MessagePolicy` describes PuppetMaster's delivery intent instead of exposing a
backend-specific QoS model:

- delivery guarantee
- retention policy
- freshness policy
- overflow behavior
- queue depth

Transport adapters are responsible for mapping this policy to native backend
settings. FastDDS can map parts of it to DDS QoS. ZMQ and IPC can emulate parts
of it with local queues, high-water marks, blocking writes, or explicit
`StatusCode::kUnsupported` results when a behavior cannot be provided.

DDS-specific concepts such as transient-local durability should live in a
FastDDS adapter options type, not in the public core model.

Examples:

- `kBestEffort` maps naturally to DDS best effort, ZMQ fire-and-forget, and IPC
  drop/reject policies.
- `kReliable` maps naturally to DDS reliable delivery, but ZMQ and IPC may need
  acknowledgement, blocking, or explicit unsupported handling.
- `kKeepLast` maps to DDS history depth, ZMQ high-water marks plus local cache,
  and IPC ring buffers.
- `kKeepAll` is only safe when the adapter can bound resources or reject writes
  predictably.

## Trigger Specs

`TriggerSpec` is a lightweight description of how a task should become ready:

- periodic trigger
- data trigger
- task dependency trigger
- manual trigger

The scheduler branch will turn this description into executable behavior.
