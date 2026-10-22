# Typed Messages

Transports move bytes. `puppet_master/transport/typed.h` adds typed readers and
writers on top, so components exchange structs instead of hand-packing
buffers. The typed layer is header-only and works with every backend.

## Trivially Copyable Structs

A trivially copyable struct needs only a wire type name:

```cpp
#include <puppet_master/transport/typed.h>

namespace transport = puppet_master::transport;

struct VehicleSpeed {
    double meters_per_second;
    std::int64_t stamp_ns;
};

template <>
struct puppet_master::transport::MessageTypeName<VehicleSpeed> {
    static constexpr const char* value = "demo.VehicleSpeed";
};
```

The bytes are a raw copy of the struct, so writer and reader must agree on
layout: same struct definition, compiler ABI, and endianness. That holds for
processes built together on one host, which is the in-memory and shared-memory
case. Use a custom codec for anything else.

## Endpoints, Readers, And Writers

`EndpointFor<T>` fills in the message descriptor from T's codec:

```cpp
auto endpoint = transport::EndpointFor<VehicleSpeed>(
    core::TopicSpec {topic_name, core::TransportKind::kInMemory, policy});

auto writer = transport::MakeTypedWriter<VehicleSpeed>(context.CreateWriter(endpoint));
auto reader = transport::MakeTypedReader<VehicleSpeed>(context.CreateReader(endpoint));

writer.value().Write(VehicleSpeed {12.5, stamp});

auto message = reader.value().Read();
if (message.ok()) {
    Use(message.value().value, message.value().metadata.sequence);
}
```

`MakeTypedReader` and `MakeTypedWriter` take the `Result` from any
`CreateReader` or `CreateWriter` (transport, runtime, or `ComponentContext`)
and pass creation errors through. They return `InvalidArgument` if the
endpoint's descriptor was declared for a different type, so a reader for one
type cannot silently decode another.

`TypedReader` also forwards `SetDataAvailableCallback` and
`PendingMessageCount`, and `raw()` returns the underlying byte endpoint.

### From A Component Spec

Inside a component, `puppet_master/runtime/typed_endpoints.h` picks the
endpoint by message type from the component's declared readers or writers, so
the code does not depend on their order in the configuration:

```cpp
core::Status Configure(runtime::ComponentContext& context) override
{
    auto wheel = runtime::MakeTypedReader<WheelSpeed>(context, spec_);
    auto state = runtime::MakeTypedWriter<VehicleState>(context, spec_);
    ...
}
```

It returns `NotFound` when the spec declares no reader (or writer) for T, and
`InvalidArgument` when it declares several; create those by topic instead.

## Custom Codecs

Specialize `transport::Codec<T>` for variable-size or portable formats:

```cpp
template <>
struct puppet_master::transport::Codec<Label> {
    static std::string TypeName() { return "demo.Label"; }
    static std::string Encoding() { return "application/x-demo-label"; }

    // Upper bound on Encode's output for this value.
    static std::size_t MaxSize(const Label& label) { return 4 + label.text.size(); }

    // Writes into `buffer` (exactly MaxSize bytes) and returns the bytes used.
    static core::Result<std::size_t> Encode(const Label& label, transport::MutableByteView buffer);

    static core::Result<Label> Decode(transport::ByteView bytes);
};
```

`TypeName` and `Encoding` become the endpoint's `MessageDescriptor`. Wrapping a
schema library such as Protocol Buffers or FlatBuffers is a codec like this one;
PuppetMaster does not depend on any of them.

## Behavior

- `Write` encodes through `Writer::WriteWith`. On the shared-memory transport
  the codec serializes directly into shared memory; other backends encode into
  a temporary buffer first. See [Transport Abstraction](transport.md).
- An encoder that returns an error, throws, or reports more than `MaxSize`
  bytes publishes nothing. Exceptions surface as `Internal`.
- A message that fails to decode is consumed and returned as `DataLoss`; the
  next `Read` continues with the following message.
- Topic metrics record the encoded size.
