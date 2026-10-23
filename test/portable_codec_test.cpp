// PortableCodec: a layout-independent wire format. The golden bytes below
// pin the format, so any change to it fails here first.

#include <array>
#include <cassert>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include <puppet_master/puppet_master.h>
#include <puppet_master/transport/portable_codec.h>
#include <puppet_master/transport/typed.h>

namespace core = puppet_master::core;
namespace inmemory = puppet_master::transport::inmemory;
namespace transport = puppet_master::transport;

enum class Gear : std::uint8_t { kPark = 0, kDrive = 3 };

struct Point {
    float x;
    float y;
};

// Fields of mixed width in an order that leaves padding in the C++ struct;
// the wire format has none.
struct Sample {
    bool valid;
    std::int16_t temperature;
    std::uint64_t stamp;
    double value;
    Gear gear;
    std::string frame;
    std::vector<std::int32_t> readings;
    std::array<std::uint8_t, 3> flags;
    std::vector<Point> path;
    Point origin;
};

template <>
struct puppet_master::transport::PortableFields<Point> {
    template <typename Message, typename Visitor>
    static void Visit(Message& point, Visitor&& field)
    {
        field(point.x);
        field(point.y);
    }
};

template <>
struct puppet_master::transport::PortableFields<Sample> {
    template <typename Message, typename Visitor>
    static void Visit(Message& sample, Visitor&& field)
    {
        field(sample.valid);
        field(sample.temperature);
        field(sample.stamp);
        field(sample.value);
        field(sample.gear);
        field(sample.frame);
        field(sample.readings);
        field(sample.flags);
        field(sample.path);
        field(sample.origin);
    }
};

template <>
struct puppet_master::transport::MessageTypeName<Sample> {
    static constexpr const char* value = "test.Sample";
    static constexpr std::uint32_t version = 4;
};

template <>
struct puppet_master::transport::Codec<Sample> : puppet_master::transport::PortableCodec<Sample> {};

// The same wire type name with one field fewer: a reader built from an older
// field list.
struct ShortSample {
    bool valid;
};

template <>
struct puppet_master::transport::PortableFields<ShortSample> {
    template <typename Message, typename Visitor>
    static void Visit(Message& sample, Visitor&& field)
    {
        field(sample.valid);
    }
};

template <>
struct puppet_master::transport::MessageTypeName<ShortSample> {
    static constexpr const char* value = "test.ShortSample";
};

namespace {

using Codec = transport::Codec<Sample>;

Sample MakeSample()
{
    Sample sample;
    sample.valid = true;
    sample.temperature = -2;
    sample.stamp = 0x0102030405060708ULL;
    sample.value = 1.5;
    sample.gear = Gear::kDrive;
    sample.frame = "map";
    sample.readings = {1, -1};
    sample.flags = {7, 8, 9};
    sample.path = {Point {1.0f, -2.0f}};
    sample.origin = Point {0.0f, 0.5f};
    return sample;
}

transport::ByteBuffer Encode(const Sample& sample)
{
    transport::ByteBuffer buffer(Codec::MaxSize(sample));
    auto used = Codec::Encode(sample, transport::MutableByteView(buffer.data(), buffer.size()));
    assert(used.ok());
    buffer.resize(used.value());
    return buffer;
}

void GoldenBytes()
{
    // clang-format off
    const transport::ByteBuffer expected {
        0x01,                                            // valid
        0xfe, 0xff,                                      // temperature -2
        0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,  // stamp
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf8, 0x3f,  // value 1.5
        0x03,                                            // gear kDrive
        0x03, 0x00, 0x00, 0x00, 'm', 'a', 'p',           // frame
        0x02, 0x00, 0x00, 0x00,                          // readings: 2 elements
        0x01, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,  //   1, -1
        0x07, 0x08, 0x09,                                // flags (no count)
        0x01, 0x00, 0x00, 0x00,                          // path: 1 element
        0x00, 0x00, 0x80, 0x3f, 0x00, 0x00, 0x00, 0xc0,  //   {1.0, -2.0}
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3f,  // origin {0.0, 0.5}
    };
    // clang-format on
    const auto sample = MakeSample();
    assert(Codec::MaxSize(sample) == expected.size());
    assert(Encode(sample) == expected);

    auto decoded = Codec::Decode(transport::ByteView(expected));
    assert(decoded.ok());
    const auto& value = decoded.value();
    assert(value.valid && value.temperature == -2 && value.stamp == sample.stamp && value.value == 1.5);
    assert(value.gear == Gear::kDrive && value.frame == "map" && value.readings == sample.readings);
    assert(value.flags == sample.flags && value.path.size() == 1 && value.path[0].y == -2.0f);
    assert(value.origin.y == 0.5f);
}

void Limits()
{
    Sample sample {};
    sample.temperature = std::numeric_limits<std::int16_t>::min();
    sample.stamp = std::numeric_limits<std::uint64_t>::max();
    sample.value = -std::numeric_limits<double>::infinity();
    sample.frame = std::string("a\0b", 3);  // embedded NUL survives
    auto decoded = Codec::Decode(transport::ByteView(Encode(sample)));
    assert(decoded.ok());
    assert(decoded.value().temperature == std::numeric_limits<std::int16_t>::min());
    assert(decoded.value().stamp == std::numeric_limits<std::uint64_t>::max());
    assert(decoded.value().value == -std::numeric_limits<double>::infinity());
    assert(decoded.value().frame == std::string("a\0b", 3) && decoded.value().readings.empty());

    // Encode reports a buffer that is too small rather than overrunning it.
    transport::ByteBuffer small(5);
    auto overflow = Codec::Encode(MakeSample(), transport::MutableByteView(small.data(), small.size()));
    assert(overflow.status().code() == core::StatusCode::kOutOfRange);
}

void CorruptInputIsDataLoss()
{
    const auto bytes = Encode(MakeSample());
    const auto expect_data_loss = [](const transport::ByteBuffer& input, const char* part) {
        auto result = Codec::Decode(transport::ByteView(input));
        assert(result.status().code() == core::StatusCode::kDataLoss);
        assert(result.status().message().find("test.Sample: ") == 0);
        assert(result.status().message().find(part) != std::string::npos);
    };

    // Every truncation is caught, never read past the end.
    for (std::size_t size = 0; size < bytes.size(); ++size) {
        auto result = Codec::Decode(transport::ByteView(bytes.data(), size));
        assert(result.status().code() == core::StatusCode::kDataLoss);
    }

    auto extra = bytes;
    extra.push_back(0);
    expect_data_loss(extra, "1 bytes left over");

    auto bad_bool = bytes;
    bad_bool[0] = 2;
    expect_data_loss(bad_bool, "bool byte is 2");

    // A huge element count is rejected before allocating for it.
    auto huge = bytes;
    const std::size_t readings_count = 1 + 2 + 8 + 8 + 1 + 4 + 3;
    huge[readings_count] = 0xff;
    huge[readings_count + 1] = 0xff;
    huge[readings_count + 2] = 0xff;
    huge[readings_count + 3] = 0x7f;
    expect_data_loss(huge, "exceeds the message");
}

void DescriptorAndTransport()
{
    const auto descriptor = transport::DescriptorFor<Sample>();
    assert(descriptor.type_name == "test.Sample");
    assert(descriptor.encoding == "application/x-puppetmaster-portable" && descriptor.version == 4);

    auto name = core::TransportName::Create("inmemory");
    assert(name.ok());
    auto memory = std::make_shared<inmemory::InMemoryTransport>(name.value());
    assert(memory->Open().ok());
    auto topic = core::TopicSpec {core::TopicName::Unsafe("/test/portable"), core::TransportKind::kInMemory, {}};
    auto writer = transport::MakeTypedWriter<Sample>(memory->CreateWriter(transport::EndpointFor<Sample>(topic)));
    auto reader = transport::MakeTypedReader<Sample>(memory->CreateReader(transport::EndpointFor<Sample>(topic)));
    assert(writer.ok() && reader.ok());
    assert(writer.value().Write(MakeSample()).ok());
    auto received = reader.value().Read();
    assert(received.ok() && received.value().value.frame == "map" && received.value().value.path.size() == 1);

    // A reader whose field list is shorter sees leftover bytes, not garbage.
    auto bytes = Encode(MakeSample());
    auto short_read = transport::PortableCodec<ShortSample>::Decode(transport::ByteView(bytes));
    assert(short_read.status().code() == core::StatusCode::kDataLoss);
    assert(short_read.status().message().find("left over") != std::string::npos);
}

}  // namespace

int main()
{
    GoldenBytes();
    Limits();
    CorruptInputIsDataLoss();
    DescriptorAndTransport();
    return 0;
}
