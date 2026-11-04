// The tools' field-by-field encoder and decoder against PortableCodec: the
// same values must give the same bytes, and the codec's bytes must decode to
// the same values.

#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include <puppet_master/transport/portable_codec.h>

#include "portable_fields.h"

namespace core = puppet_master::core;
namespace tools = puppet_master::tools;
namespace transport = puppet_master::transport;

namespace {

struct Inner {
    std::uint16_t id {0};
    std::string label;
};

struct Sample {
    bool flag {false};
    std::int8_t i8 {0};
    std::int16_t i16 {0};
    std::int32_t i32 {0};
    std::int64_t i64 {0};
    std::uint8_t u8 {0};
    std::uint16_t u16 {0};
    std::uint32_t u32 {0};
    std::uint64_t u64 {0};
    float f32 {0};
    double f64 {0};
    std::string text;
    std::vector<std::uint32_t> counts;
    std::array<double, 3> position {};
    std::vector<std::string> names;
    Inner inner;
};

}  // namespace

template <>
struct puppet_master::transport::PortableFields<Inner> {
    template <typename Message, typename Visitor>
    static void Visit(Message& inner, Visitor&& field)
    {
        field(inner.id);
        field(inner.label);
    }
};

template <>
struct puppet_master::transport::PortableFields<Sample> {
    template <typename Message, typename Visitor>
    static void Visit(Message& sample, Visitor&& field)
    {
        field(sample.flag);
        field(sample.i8);
        field(sample.i16);
        field(sample.i32);
        field(sample.i64);
        field(sample.u8);
        field(sample.u16);
        field(sample.u32);
        field(sample.u64);
        field(sample.f32);
        field(sample.f64);
        field(sample.text);
        field(sample.counts);
        field(sample.position);
        field(sample.names);
        field(sample.inner);
    }
};

template <>
struct puppet_master::transport::MessageTypeName<Sample> {
    static constexpr const char* value = "test.Sample";
};

template <>
struct puppet_master::transport::Codec<Sample> : transport::PortableCodec<Sample> {};

namespace {

tools::FieldType Type(const char* text)
{
    auto type = tools::ParseFieldType(text);
    assert(type.ok());
    return type.value();
}

transport::ByteBuffer Encode(const Sample& sample)
{
    using Codec = transport::Codec<Sample>;
    transport::ByteBuffer bytes(Codec::MaxSize(sample));
    auto used = Codec::Encode(sample, transport::MutableByteView(bytes.data(), bytes.size()));
    assert(used.ok());
    bytes.resize(used.value());
    return bytes;
}

struct Field {
    const char* type;
    const char* value;    // as given on the command line
    const char* decoded;  // as printed
};

// The fields of Sample, Inner's flattened, in wire order.
const std::vector<Field> kFields {
    {"bool", "true", "true"},
    {"i8", "-128", "-128"},
    {"i16", "-300", "-300"},
    {"i32", "123456", "123456"},
    {"i64", "-9000000000", "-9000000000"},
    {"u8", "255", "255"},
    {"u16", "0xBEEF", "48879"},
    {"u32", "4000000000", "4000000000"},
    {"u64", "18446744073709551615", "18446744073709551615"},
    {"f32", "1.5", "1.5"},
    {"f64", " -2.25 ", "-2.25"},
    {"string", "hi, \"you\"\n", "\"hi, \\\"you\\\"\\x0a\""},
    {"u32[]", "1, 2,3", "[1, 2, 3]"},
    {"f64[3]", "0.5,1,-3e2", "[0.5, 1, -300]"},
    {"string[]", "a,b c", "[\"a\", \"b c\"]"},
    {"u16", "7", "7"},
    {"string", "", "\"\""},
};

Sample MakeSample()
{
    Sample sample;
    sample.flag = true;
    sample.i8 = -128;
    sample.i16 = -300;
    sample.i32 = 123456;
    sample.i64 = -9000000000LL;
    sample.u8 = 255;
    sample.u16 = 0xBEEF;
    sample.u32 = 4000000000U;
    sample.u64 = 18446744073709551615ULL;
    sample.f32 = 1.5F;
    sample.f64 = -2.25;
    sample.text = "hi, \"you\"\n";
    sample.counts = {1, 2, 3};
    sample.position = {0.5, 1, -300};
    sample.names = {"a", "b c"};
    sample.inner.id = 7;
    return sample;
}

void SameBytesAsTheCodec()
{
    transport::ByteBuffer encoded;
    for (const auto& field : kFields) {
        assert(tools::EncodeField(Type(field.type), field.value, encoded).ok());
    }
    assert(encoded == Encode(MakeSample()));
}

void DecodesTheCodecsBytes()
{
    std::vector<tools::FieldType> types;
    for (const auto& field : kFields) {
        types.push_back(Type(field.type));
    }
    const auto bytes = Encode(MakeSample());
    auto decoded = tools::DecodeFields(types, transport::ByteView(bytes));
    assert(decoded.ok());
    assert(decoded.value().size() == kFields.size());
    for (std::size_t index = 0; index < kFields.size(); ++index) {
        if (decoded.value()[index] != kFields[index].decoded) {
            std::cerr << "field " << index << ": " << decoded.value()[index] << '\n';
        }
        assert(decoded.value()[index] == kFields[index].decoded);
    }

    // A field list that differs from the writer's fails, as in the codec.
    types.pop_back();
    auto short_list = tools::DecodeFields(types, transport::ByteView(bytes));
    assert(short_list.status().code() == core::StatusCode::kDataLoss);
    assert(short_list.status().message().find("4 bytes left over") != std::string::npos);
    types.push_back(Type("string"));
    types.push_back(Type("u8"));
    auto long_list = tools::DecodeFields(types, transport::ByteView(bytes));
    assert(long_list.status().code() == core::StatusCode::kDataLoss);
    assert(long_list.status().message().find("field 18 (u8): message ends inside a field") != std::string::npos);
}

void FieldTypes()
{
    for (const auto* good : {"bool", "u64", "f32[]", "string[]", "i16[4]", "u8[0x10]"}) {
        assert(tools::ParseFieldType(good).ok());
    }
    for (const auto* bad : {"", "u128", "int", "u8[0]", "u8[-1]", "bool[", "f64]", "u8[x]", "Pose"}) {
        assert(tools::ParseFieldType(bad).status().code() == core::StatusCode::kInvalidArgument);
    }
    assert(tools::ToString(Type("u8[0x10]")) == "u8[16]");
    assert(tools::ToString(Type(" f64[] ")) == "f64[]");
}

void BadValues()
{
    const auto fails = [](const char* type, const char* value) {
        transport::ByteBuffer bytes;
        return tools::EncodeField(Type(type), value, bytes).code() == core::StatusCode::kInvalidArgument;
    };
    assert(fails("u8", "256"));
    assert(fails("u8", "-1"));
    assert(fails("i8", "128"));
    assert(fails("i8", "-129"));
    assert(fails("u16", "0x10000"));
    assert(fails("u64", "18446744073709551616"));
    assert(fails("i32", "1.5"));
    assert(fails("i32", ""));
    assert(fails("bool", "yes"));
    assert(fails("f32", "1e40"));
    assert(fails("f64", "1e400"));
    assert(fails("f64", "x"));
    assert(fails("f64[3]", "1,2"));
    assert(fails("u8[]", "1,,2"));

    // An empty vector is a zero count; i8 -128 is its two's complement.
    transport::ByteBuffer bytes;
    assert(tools::EncodeField(Type("u32[]"), "", bytes).ok());
    assert(tools::EncodeField(Type("i8"), "-128", bytes).ok());
    assert((bytes == transport::ByteBuffer {0, 0, 0, 0, 0x80}));
}

void BadBytes()
{
    const auto decode = [](const char* type, transport::ByteBuffer bytes) {
        return tools::DecodeFields({Type(type)}, transport::ByteView(bytes));
    };
    assert(decode("bool", {2}).status().code() == core::StatusCode::kDataLoss);
    assert(decode("u32", {1, 2}).status().code() == core::StatusCode::kDataLoss);
    // A count larger than what is left fails before allocating.
    assert(decode("u64[]", {0xff, 0xff, 0xff, 0x7f}).status().code() == core::StatusCode::kDataLoss);
    assert(decode("string", {5, 0, 0, 0, 'a'}).status().code() == core::StatusCode::kDataLoss);
    auto empty = tools::DecodeFields({}, transport::ByteView());
    assert(empty.ok() && empty.value().empty());
}

}  // namespace

int main()
{
    SameBytesAsTheCodec();
    DecodesTheCodecsBytes();
    FieldTypes();
    BadValues();
    BadBytes();
    std::cout << "portable fields tests passed\n";
    return 0;
}
