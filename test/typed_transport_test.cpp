#include <cassert>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include <puppet_master/puppet_master.h>
#include <puppet_master/transport/typed.h>

namespace core = puppet_master::core;
namespace inmemory = puppet_master::transport::inmemory;
namespace runtime = puppet_master::runtime;
namespace transport = puppet_master::transport;

// A trivially copyable message: works with only a type name.
struct VehicleSpeed {
    double meters_per_second;
    std::int64_t stamp_ns;
};

template <>
struct puppet_master::transport::MessageTypeName<VehicleSpeed> {
    static constexpr const char* value = "test.VehicleSpeed";
};

// A variable-size message with a custom codec: length-prefixed UTF-8.
struct Label {
    std::string text;
};

template <>
struct puppet_master::transport::Codec<Label> {
    static std::string TypeName() { return "test.Label"; }
    static std::string Encoding() { return "application/x-test-label"; }
    static std::size_t MaxSize(const Label& label) { return 4 + label.text.size(); }

    static core::Result<std::size_t> Encode(const Label& label, MutableByteView buffer)
    {
        if (label.text == "throw") {
            throw std::runtime_error("encode failed");
        }
        const auto size = static_cast<std::uint32_t>(label.text.size());
        std::memcpy(buffer.data(), &size, 4);
        std::memcpy(buffer.data() + 4, label.text.data(), label.text.size());
        return core::Result<std::size_t>(4 + label.text.size());
    }

    static core::Result<Label> Decode(ByteView bytes)
    {
        std::uint32_t size = 0;
        if (bytes.size() < 4) {
            return core::Result<Label>::FromStatus(core::Status::DataLoss("label header truncated"));
        }
        std::memcpy(&size, bytes.data(), 4);
        if (bytes.size() != 4 + size) {
            return core::Result<Label>::FromStatus(core::Status::DataLoss("label length mismatch"));
        }
        return core::Result<Label>(Label {std::string(reinterpret_cast<const char*>(bytes.data()) + 4, size)});
    }
};

// Two releases of one wire type: same name, different versions.
struct OdometryV2 {
    double distance_m;
};

struct OdometryV3 {
    double distance_m;
    double heading_rad;
};

template <>
struct puppet_master::transport::MessageTypeName<OdometryV2> {
    static constexpr const char* value = "test.Odometry";
    static constexpr std::uint32_t version = 2;
};

template <>
struct puppet_master::transport::MessageTypeName<OdometryV3> {
    static constexpr const char* value = "test.Odometry";
    static constexpr std::uint32_t version = 3;
};

// A custom codec declares its version through Version(), overriding the one
// from MessageTypeName.
struct Tag {
    std::uint8_t id;
};

template <>
struct puppet_master::transport::MessageTypeName<Tag> {
    static constexpr const char* value = "test.Tag";
};

template <>
struct puppet_master::transport::Codec<Tag> : puppet_master::transport::TrivialCodec<Tag> {
    static std::uint32_t Version() { return 7; }
};

namespace {

core::TopicSpec Topic(const std::string& name, core::MessagePolicy policy = {})
{
    auto topic = core::TopicName::Create(name);
    assert(topic.ok());
    return core::TopicSpec {topic.value(), core::TransportKind::kInMemory, policy};
}

core::MessagePolicy Queued(std::size_t depth)
{
    core::MessagePolicy policy;
    policy.freshness = core::FreshnessPolicy::kQueued;
    policy.queue_depth = depth;
    return policy;
}

std::shared_ptr<inmemory::InMemoryTransport> MakeTransport()
{
    auto name = core::TransportName::Create("inmemory");
    assert(name.ok());
    auto created = std::make_shared<inmemory::InMemoryTransport>(name.value());
    assert(created->Open().ok());
    return created;
}

void DescriptorsComeFromCodecs()
{
    const auto speed = transport::DescriptorFor<VehicleSpeed>();
    assert(speed.type_name == "test.VehicleSpeed");
    assert(speed.encoding == "application/x-puppetmaster-trivial");
    const auto label = transport::DescriptorFor<Label>();
    assert(label.type_name == "test.Label" && label.encoding == "application/x-test-label");

    const auto endpoint = transport::EndpointFor<VehicleSpeed>(Topic("/vehicle/speed"));
    assert(endpoint.message.type_name == "test.VehicleSpeed");
    assert(endpoint.topic.name.str() == "/vehicle/speed");
}

void TrivialStructRoundTrip()
{
    auto shared = MakeTransport();
    const auto endpoint = transport::EndpointFor<VehicleSpeed>(Topic("/vehicle/speed", Queued(4)));
    auto reader = transport::MakeTypedReader<VehicleSpeed>(shared->CreateReader(endpoint));
    auto writer = transport::MakeTypedWriter<VehicleSpeed>(shared->CreateWriter(endpoint));
    assert(reader.ok() && writer.ok());

    assert(writer.value().Write(VehicleSpeed {12.5, 100}).ok());
    assert(writer.value().Write(VehicleSpeed {13.0, 200}).ok());
    assert(reader.value().PendingMessageCount().value() == 2);

    auto first = reader.value().Read();
    assert(first.ok());
    assert(first.value().value.meters_per_second == 12.5 && first.value().value.stamp_ns == 100);
    assert(first.value().metadata.sequence == 1);
    auto second = reader.value().Read();
    assert(second.ok() && second.value().value.stamp_ns == 200);
    assert(reader.value().Read().status().code() == core::StatusCode::kUnavailable);
}

void CustomCodecRoundTripAndErrors()
{
    auto shared = MakeTransport();
    const auto endpoint = transport::EndpointFor<Label>(Topic("/labels", Queued(4)));
    auto reader = transport::MakeTypedReader<Label>(shared->CreateReader(endpoint));
    auto writer = transport::MakeTypedWriter<Label>(shared->CreateWriter(endpoint));
    assert(reader.ok() && writer.ok());

    assert(writer.value().Write(Label {"pedestrian"}).ok());
    assert(writer.value().Write(Label {""}).ok());
    // A throwing encoder publishes nothing.
    assert(writer.value().Write(Label {"throw"}).code() == core::StatusCode::kInternal);

    auto pedestrian = reader.value().Read();
    assert(pedestrian.ok() && pedestrian.value().value.text == "pedestrian");
    auto empty = reader.value().Read();
    assert(empty.ok() && empty.value().value.text.empty());
    assert(reader.value().Read().status().code() == core::StatusCode::kUnavailable);

    // Bytes written outside the codec fail to decode as DataLoss, and the
    // reader moves on to the next message.
    const std::string garbage = "xy";
    assert(writer.value().raw()->Write(transport::ByteView::From(garbage.data(), garbage.size())).ok());
    assert(writer.value().Write(Label {"cyclist"}).ok());
    assert(reader.value().Read().status().code() == core::StatusCode::kDataLoss);
    auto cyclist = reader.value().Read();
    assert(cyclist.ok() && cyclist.value().value.text == "cyclist");
}

void TrivialDecodeRejectsWrongSize()
{
    const unsigned char short_bytes[3] = {1, 2, 3};
    auto decoded = transport::Codec<VehicleSpeed>::Decode(transport::ByteView(short_bytes, sizeof(short_bytes)));
    assert(decoded.status().code() == core::StatusCode::kDataLoss);

    unsigned char small[4];
    auto encoded = transport::Codec<VehicleSpeed>::Encode(
        VehicleSpeed {}, transport::MutableByteView(small, sizeof(small)));
    assert(encoded.status().code() == core::StatusCode::kOutOfRange);
}

void WrapRejectsMismatchedEndpoints()
{
    auto shared = MakeTransport();
    const auto label_endpoint = transport::EndpointFor<Label>(Topic("/mismatch"));
    auto as_speed_reader = transport::MakeTypedReader<VehicleSpeed>(shared->CreateReader(label_endpoint));
    assert(as_speed_reader.status().code() == core::StatusCode::kInvalidArgument);
    auto as_speed_writer = transport::MakeTypedWriter<VehicleSpeed>(shared->CreateWriter(label_endpoint));
    assert(as_speed_writer.status().code() == core::StatusCode::kInvalidArgument);

    assert(transport::TypedReader<Label>::Wrap(nullptr).status().code() == core::StatusCode::kInvalidArgument);
    assert(transport::TypedWriter<Label>::Wrap(nullptr).status().code() == core::StatusCode::kInvalidArgument);

    // Creation errors pass through unchanged.
    auto failed = transport::MakeTypedReader<Label>(
        core::Result<transport::ReaderPtr>::FromStatus(core::Status::NotFound("no transport")));
    assert(failed.status().code() == core::StatusCode::kNotFound);
}

void VersionsSeparateReleases()
{
    assert(transport::DescriptorFor<VehicleSpeed>().version == 0);
    assert(transport::DescriptorFor<OdometryV2>().version == 2);
    assert(transport::DescriptorFor<OdometryV3>().version == 3);
    assert(transport::DescriptorFor<Tag>().version == 7);
    assert(transport::DescriptorFor<Label>().version == 0);  // codec without Version()
    assert(transport::DescriptorFor<OdometryV2>().ToString()
           == "test.Odometry v2 (application/x-puppetmaster-trivial)");
    assert(transport::DescriptorFor<VehicleSpeed>().ToString()
           == "test.VehicleSpeed (application/x-puppetmaster-trivial)");

    // A binary built for v3 refuses an endpoint configured for v2.
    auto shared = MakeTransport();
    const auto v2_endpoint = transport::EndpointFor<OdometryV2>(Topic("/odometry"));
    auto v2_writer = transport::MakeTypedWriter<OdometryV2>(shared->CreateWriter(v2_endpoint));
    assert(v2_writer.ok());
    auto v3_reader = transport::MakeTypedReader<OdometryV3>(shared->CreateReader(v2_endpoint));
    assert(v3_reader.status().code() == core::StatusCode::kInvalidArgument);
    assert(v3_reader.status().message().find("test.Odometry v2") != std::string::npos);
    assert(v3_reader.status().message().find("test.Odometry v3") != std::string::npos);

    // The transport also refuses a second version on a live topic.
    const auto v3_endpoint = transport::EndpointFor<OdometryV3>(Topic("/odometry"));
    auto conflicting = shared->CreateReader(v3_endpoint);
    assert(conflicting.status().code() == core::StatusCode::kInvalidArgument);
    assert(conflicting.status().message().find("test.Odometry v2") != std::string::npos);
}

void CallbacksAndRuntimeIntegration()
{
    auto context = runtime::RuntimeContext::Create();
    assert(context.ok());
    assert(context.value()->Open().ok());
    const auto endpoint = transport::EndpointFor<VehicleSpeed>(Topic("/runtime/speed", Queued(2)));
    auto reader = transport::MakeTypedReader<VehicleSpeed>(context.value()->CreateReader(endpoint));
    auto writer = transport::MakeTypedWriter<VehicleSpeed>(context.value()->CreateWriter(endpoint));
    assert(reader.ok() && writer.ok());

    int callbacks = 0;
    assert(reader.value().SetDataAvailableCallback([&callbacks] { ++callbacks; }).ok());
    assert(writer.value().Write(VehicleSpeed {3.0, 7}).ok());
    assert(callbacks == 1);
    auto message = reader.value().Read();
    assert(message.ok() && message.value().value.stamp_ns == 7);

    // Metrics see the encoded size through the runtime's writer decorator.
    const auto snapshot = context.value()->observer()->Snapshot();
    bool found = false;
    for (const auto& topic : snapshot.topics) {
        if (topic.topic_name == "/runtime/speed") {
            found = true;
            assert(topic.messages_published == 1 && topic.bytes_published == sizeof(VehicleSpeed));
        }
    }
    assert(found);
    assert(context.value()->Close().ok());
}

}  // namespace

int main()
{
    DescriptorsComeFromCodecs();
    TrivialStructRoundTrip();
    CustomCodecRoundTripAndErrors();
    TrivialDecodeRejectsWrongSize();
    WrapRejectsMismatchedEndpoints();
    VersionsSeparateReleases();
    CallbacksAndRuntimeIntegration();
    return 0;
}
