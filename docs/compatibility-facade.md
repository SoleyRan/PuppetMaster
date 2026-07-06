# itage_engine Compatibility Facade

This facade gives projects migrating from `itage_engine` a smaller first step.
It keeps the familiar `Node`, `WriterBase`, and `ReaderBase` shape while routing
the work through PuppetMaster's current `RuntimeContext` and transport
abstraction.

The facade lives under:

```cpp
#include <puppet_master/compat/itage_facade.h>
```

and uses the namespace:

```cpp
namespace compat = puppet_master::compat::itage;
```

## What It Preserves

The old code usually looked like this:

```cpp
auto writer = node.CreateWriter("/vehicle/speed", nullptr, attribute);
writer->Write(&sample);

auto reader = node.CreateReader("/vehicle/speed", nullptr, attribute);
reader->SetCallBack([]() {});
reader->Read(&sample, time_diff);
```

The compatibility facade keeps that shape:

```cpp
compat::TopicOptions topic;
topic.message_type = "demo.SpeedSample";
topic.encoding = "application/x-struct";
topic.fixed_payload_size = sizeof(SpeedSample);
topic.message_policy.freshness = puppet_master::core::FreshnessPolicy::kQueued;
topic.message_policy.queue_depth = 4;

compat::Node node("legacy_speed_node");

auto reader = node.CreateReader("/vehicle/speed", topic);
auto writer = node.CreateWriter("/vehicle/speed", topic);

writer->Write(&sample);
reader->Read(&received, time_diff_ms);
```

`TopicOptions::fixed_payload_size` is important when using the legacy
`Write(&object)` and `Read(&object, time_diff)` calls. The new transport layer is
byte-oriented, so the facade needs an explicit size for fixed-size payloads.

For variable-size payloads, use explicit lengths:

```cpp
writer->Write(buffer.data(), buffer.size());
reader->Read(buffer.data(), buffer.size(), time_diff_ms);
```

## Mapping

| itage_engine idea | Compatibility facade | New core underneath |
| --- | --- | --- |
| `NodeBase` | `compat::itage::NodeBase` | `runtime::RuntimeContext` |
| `CreateWriter(topic, data, attribute)` | same call shape | `RuntimeContext::CreateWriter()` |
| `CreateReader(topic, data, attribute)` | same call shape | `RuntimeContext::CreateReader()` |
| `WriterBase::Write(void*, len)` | same call shape | `transport::Writer::Write(ByteView)` |
| `ReaderBase::Read(void*, time_diff)` | same call shape for fixed-size payloads | `transport::Reader::Read()` |
| DDS-specific attribute | `TopicOptions` | `EndpointConfig` and `MessagePolicy` |

## Using the Old Attribute Slot

The old API carried backend-specific data through `void* attribute`. This facade
keeps the slot, but the expected object is now `compat::TopicOptions`:

```cpp
compat::TopicOptions topic;
topic.fixed_payload_size = sizeof(SpeedSample);

auto reader = node.CreateReader("/vehicle/speed", nullptr, &topic);
auto writer = node.CreateWriter("/vehicle/speed", nullptr, &topic);
```

Do not pass the old DDS attribute structure directly. Convert it to
`TopicOptions` first so the migration remains transport-neutral.

## Recommended Migration Steps

1. Replace old node creation with `compat::itage::Node`.
2. Convert each old DDS/ZMQ/IPC attribute into `TopicOptions`.
3. Add `fixed_payload_size = sizeof(T)` for fixed-size struct messages.
4. Keep `Write(&msg)` and `Read(&msg, time_diff)` temporarily.
5. Gradually move modules to `runtime::Component`, `ComponentSpec`, and
   configuration-driven endpoints.

## Limitations

This facade is intentionally transitional:

- it does not expose FastDDS headers or DDS type support
- it treats messages as bytes or fixed-size structs
- it does not provide zero-copy loaned samples
- it does not replace the new component model or scheduler APIs
- unsafe `Read(void*, time_diff)` requires `fixed_payload_size`

New modules should prefer PuppetMaster's native runtime, component,
configuration, and scheduler APIs. The facade is mainly for reducing migration
friction in existing projects.
