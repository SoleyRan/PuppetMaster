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
case. Across compilers, architectures, or hosts, use the
[portable codec](#portable-codec) or a custom codec.

## Portable Codec

`puppet_master/transport/portable_codec.h` encodes a struct field by field in a
fixed little-endian format, independent of padding, compiler, and host byte
order. It also supports variable-size fields. A type lists its fields once, in
wire order:

```cpp
#include <puppet_master/transport/portable_codec.h>

struct Pose {
    double x;
    double y;
    std::string frame;
    std::vector<float> covariance;
};

template <>
struct puppet_master::transport::PortableFields<Pose> {
    template <typename Message, typename Visitor>
    static void Visit(Message& pose, Visitor&& field)
    {
        field(pose.x);
        field(pose.y);
        field(pose.frame);
        field(pose.covariance);
    }
};

template <>
struct puppet_master::transport::MessageTypeName<Pose> {
    static constexpr const char* value = "demo.Pose";
    static constexpr std::uint32_t version = 1;
};

template <>
struct puppet_master::transport::Codec<Pose> : transport::PortableCodec<Pose> {};
```

`Visit` is used for encoding (`const Pose&`) and decoding (`Pose&`), so the
field list cannot differ between the two.

| Field type | Wire format |
|---|---|
| `bool` | 1 byte, 0 or 1; anything else fails to decode |
| integers, `float`, `double` | their width, little-endian; floats as IEEE 754 bits |
| enums | their underlying type |
| `std::string` | `u32` byte count, then the bytes (any content, including NUL) |
| `std::vector<T>` | `u32` element count, then the elements |
| `std::array<T, N>` | the N elements, no count |
| a type with `PortableFields` | its fields in order |

`std::vector<bool>` is rejected at compile time; use
`std::vector<std::uint8_t>`. The encoding is `application/x-puppetmaster-portable`.

Decoding returns `DataLoss`, naming the type, when the message ends inside a
field, a count exceeds the remaining bytes (checked before allocating), a
`bool` byte is not 0 or 1, or bytes are left over. A reader built with a
different field list therefore fails instead of misreading, but the format has
no field tags: adding, removing, or reordering fields changes it, so raise the
type's version with any such change, as for structs.

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

`TypeName`, `Encoding`, and the optional `static std::uint32_t Version()`
become the endpoint's `MessageDescriptor`. Wrapping a schema library such as
Protocol Buffers or FlatBuffers is a codec like this one; PuppetMaster does not
depend on any of them.

## Versions

When processes are built separately, for example by different teams, a struct
can change on one side only. If its size changes, `TrivialCodec` reports
`DataLoss` on every message; if only the field order or meaning changes, it
decodes garbage silently. Give such types a version and raise it on every
change that old readers cannot decode:

```cpp
template <>
struct puppet_master::transport::MessageTypeName<WheelSpeed> {
    static constexpr const char* value = "vehicle.WheelSpeed";
    static constexpr std::uint32_t version = 2;
};
```

A custom codec returns it from `Version()` instead. The version is part of
`MessageDescriptor`, and endpoints on one topic must agree on type name,
encoding, and version, so a mismatch is refused when the endpoint is created,
not discovered in the data:

- `TypedReader`/`TypedWriter::Wrap` (and `MakeTypedReader`/`MakeTypedWriter`)
  reject an endpoint whose configured version differs from the one compiled
  into the binary.
- Transports reject a second version on a live topic. On the shared-memory
  transport this holds across processes: the segment records the creator's
  version, and the error names both, for example `vehicle.WheelSpeed v2
  (...), not vehicle.WheelSpeed v3 (...)`.

In configuration, the topic's `message_version` must match. Version `0` means
unversioned and is the default, so existing types keep working; unversioned
and versioned endpoints do not match each other.

## Behavior

- `Write` encodes through `Writer::WriteWith`. On the shared-memory transport
  the codec serializes directly into shared memory; other backends encode into
  a temporary buffer first. See [Transport Abstraction](transport.md).
- An encoder that returns an error, throws, or reports more than `MaxSize`
  bytes publishes nothing. Exceptions surface as `Internal`.
- A message that fails to decode is consumed and returned as `DataLoss`; the
  next `Read` continues with the following message.
- Topic metrics record the encoded size.
