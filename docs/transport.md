# Transport Abstraction

The transport layer defines the public contract that concrete communication
backends must implement. It is intentionally independent of FastDDS, ZMQ, IPC,
and any generated message type system.

## Scope

This milestone adds only abstractions:

- byte-oriented message views and buffers
- message descriptors
- reader and writer interfaces
- transport capabilities
- endpoint configuration
- a lightweight transport registry

Concrete transports live behind this interface. The in-memory backend is the
reference local implementation, while FastDDS and other adapters can provide
external middleware integration without changing component code.

## Message Model

The public transport interface moves bytes:

- `ByteView` references data to write without taking ownership.
- `ByteBuffer` owns received payload bytes.
- `MessageDescriptor` names the logical message type and encoding.
- `MessageMetadata` carries sequence and timestamp fields.

Typed serialization belongs above or beside the transport adapter. For example,
a future FastDDS adapter can associate a `MessageDescriptor` with native
FastDDS type support internally without exposing those headers to the public
transport interface.

## Endpoint Configuration

`EndpointConfig` combines:

- `core::TopicSpec`
- `MessageDescriptor`

The topic spec selects the transport kind and `MessagePolicy`. The descriptor
identifies the payload contract used by a reader or writer.

## Reader And Writer

`Reader` and `Writer` are pure interfaces:

- `Reader::Read()` returns `core::Result<Message>`.
- `Reader::SetDataAvailableCallback()` lets data-triggered schedulers subscribe
  to availability notifications when a backend supports callbacks.
- `Writer::Write()` accepts `ByteView` plus optional timestamps.

Backends should report unsupported behavior through `core::Status::Unsupported`
instead of silently pretending to provide stronger delivery guarantees.

The in-memory backend is intentionally strict about this contract: it supports
callbacks, blocking reads, reliable local delivery, bounded queues, and
unbounded reader mailboxes. See [In-Memory Transport](inmemory-transport.md) for
the concrete semantics.

## Registry

`TransportRegistry` is a small registration utility for tests and future
runtime assembly. It is intentionally not a full runtime service yet; lifecycle,
thread safety, and configuration ownership belong to the runtime milestone.
