#include "portable_fields.h"

#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace puppet_master::tools {

namespace {

using transport::ByteBuffer;
using transport::ByteView;

struct ScalarName {
    const char* name;
    Scalar scalar;
};

constexpr ScalarName kScalars[] = {
    {"bool", Scalar::kBool}, {"i8", Scalar::kI8},   {"i16", Scalar::kI16}, {"i32", Scalar::kI32},
    {"i64", Scalar::kI64},   {"u8", Scalar::kU8},   {"u16", Scalar::kU16}, {"u32", Scalar::kU32},
    {"u64", Scalar::kU64},   {"f32", Scalar::kF32}, {"f64", Scalar::kF64}, {"string", Scalar::kString},
};

const char* NameOf(Scalar scalar)
{
    for (const auto& entry : kScalars) {
        if (entry.scalar == scalar) {
            return entry.name;
        }
    }
    return "?";
}

// Bytes on the wire; a string's is its count, before the bytes.
std::size_t WidthOf(Scalar scalar)
{
    switch (scalar) {
        case Scalar::kBool:
        case Scalar::kI8:
        case Scalar::kU8:
            return 1;
        case Scalar::kI16:
        case Scalar::kU16:
            return 2;
        case Scalar::kI32:
        case Scalar::kU32:
        case Scalar::kF32:
        case Scalar::kString:
            return 4;
        case Scalar::kI64:
        case Scalar::kU64:
        case Scalar::kF64:
            return 8;
    }
    return 1;
}

bool IsSigned(Scalar scalar)
{
    return scalar == Scalar::kI8 || scalar == Scalar::kI16 || scalar == Scalar::kI32 || scalar == Scalar::kI64;
}

void Put(std::uint64_t bits, std::size_t width, ByteBuffer& out)
{
    for (std::size_t index = 0; index < width; ++index) {
        out.push_back(static_cast<core::Byte>(bits >> (8 * index)));
    }
}

std::string_view Trim(std::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return text;
}

// Decimal with an optional sign, or 0x hex, within `width` bytes. `bits`
// gets the two's complement of a negative value.
bool ParseInteger(std::string_view text, bool is_signed, std::size_t width, std::uint64_t& bits)
{
    bool negative = false;
    if (!text.empty() && (text.front() == '-' || text.front() == '+')) {
        negative = text.front() == '-';
        text.remove_prefix(1);
    }
    if (negative && !is_signed) {
        return false;
    }
    std::uint64_t base = 10;
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        base = 16;
        text.remove_prefix(2);
    }
    if (text.empty()) {
        return false;
    }
    std::uint64_t magnitude = 0;
    for (const char c : text) {
        std::uint64_t digit = 0;
        if (c >= '0' && c <= '9') {
            digit = static_cast<std::uint64_t>(c - '0');
        } else if (base == 16 && c >= 'a' && c <= 'f') {
            digit = static_cast<std::uint64_t>(c - 'a' + 10);
        } else if (base == 16 && c >= 'A' && c <= 'F') {
            digit = static_cast<std::uint64_t>(c - 'A' + 10);
        } else {
            return false;
        }
        if (magnitude > (std::numeric_limits<std::uint64_t>::max() - digit) / base) {
            return false;
        }
        magnitude = magnitude * base + digit;
    }
    const auto bit_width = 8 * width;
    if (is_signed) {
        const std::uint64_t limit = std::uint64_t {1} << (bit_width - 1);  // |minimum|
        if (negative ? magnitude > limit : magnitude >= limit) {
            return false;
        }
        bits = negative ? ~magnitude + 1 : magnitude;
        return true;
    }
    if (bit_width < 64 && (magnitude >> bit_width) != 0) {
        return false;
    }
    bits = magnitude;
    return true;
}

bool ParseFloat(std::string_view text, double& value)
{
    if (text.empty() || text.front() == ' ' || text.front() == '\t') {
        return false;
    }
    const std::string copy(text);
    char* end = nullptr;
    errno = 0;
    value = std::strtod(copy.c_str(), &end);
    return end == copy.c_str() + copy.size() && !(errno == ERANGE && std::isinf(value));
}

// Empty on success, else why `text` is not a `scalar`.
std::string EncodeScalar(Scalar scalar, std::string_view text, ByteBuffer& out)
{
    switch (scalar) {
        case Scalar::kBool:
            if (text == "true" || text == "1") {
                out.push_back(1);
                return {};
            }
            if (text == "false" || text == "0") {
                out.push_back(0);
                return {};
            }
            return "not true, false, 1, or 0";
        case Scalar::kString:
            if (text.size() > std::numeric_limits<std::uint32_t>::max()) {
                return "longer than 2^32-1 bytes";
            }
            Put(text.size(), 4, out);
            out.insert(out.end(), text.begin(), text.end());
            return {};
        case Scalar::kF32:
        case Scalar::kF64: {
            double value = 0;
            if (!ParseFloat(text, value)) {
                return "not a number";
            }
            if (scalar == Scalar::kF64) {
                std::uint64_t bits = 0;
                std::memcpy(&bits, &value, sizeof(bits));
                Put(bits, 8, out);
                return {};
            }
            if (std::isfinite(value) && std::fabs(value) > static_cast<double>(std::numeric_limits<float>::max())) {
                return "out of range for f32";
            }
            const auto narrow = static_cast<float>(value);
            std::uint32_t bits = 0;
            std::memcpy(&bits, &narrow, sizeof(bits));
            Put(bits, 4, out);
            return {};
        }
        default: {
            std::uint64_t bits = 0;
            if (!ParseInteger(text, IsSigned(scalar), WidthOf(scalar), bits)) {
                return std::string("not a ") + NameOf(scalar) + " (decimal or 0x hex, in range)";
            }
            Put(bits, WidthOf(scalar), out);
            return {};
        }
    }
}

std::vector<std::string_view> SplitCommas(std::string_view text)
{
    std::vector<std::string_view> parts;
    if (text.empty()) {
        return parts;
    }
    std::size_t start = 0;
    while (true) {
        const auto comma = text.find(',', start);
        parts.push_back(text.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start));
        if (comma == std::string_view::npos) {
            return parts;
        }
        start = comma + 1;
    }
}

// The shortest decimal that reads back as the same value.
std::string FormatFloat(double value, bool single)
{
    if (std::isnan(value)) {
        return "nan";
    }
    if (std::isinf(value)) {
        return value < 0 ? "-inf" : "inf";
    }
    char text[64];
    int precision = 1;
    for (; precision < 17; ++precision) {
        std::snprintf(text, sizeof(text), "%.*g", precision, value);
        const double back = std::strtod(text, nullptr);
        if (single ? static_cast<float>(back) == static_cast<float>(value) : back == value) {
            break;
        }
    }
    // %g switches to an exponent once the integer part has more digits than
    // the precision: print -300, not -3e+02, while the digits stay exact.
    const int exponent = value == 0 ? 0 : static_cast<int>(std::floor(std::log10(std::fabs(value))));
    if (exponent >= precision && exponent < 17) {
        precision = exponent + 1;
    }
    std::snprintf(text, sizeof(text), "%.*g", precision, value);
    return text;
}

std::string Quote(const core::Byte* data, std::size_t size)
{
    std::string text = "\"";
    for (std::size_t index = 0; index < size; ++index) {
        const auto byte = data[index];
        if (byte == '"' || byte == '\\') {
            text += '\\';
            text += static_cast<char>(byte);
        } else if (byte >= 0x20 && byte < 0x7f) {
            text += static_cast<char>(byte);
        } else {
            char escaped[8];
            std::snprintf(escaped, sizeof(escaped), "\\x%02x", static_cast<unsigned>(byte));
            text += escaped;
        }
    }
    return text + "\"";
}

class Decoder {
public:
    explicit Decoder(ByteView bytes) : bytes_(bytes) {}

    core::Status Field(const FieldType& type, std::string& text)
    {
        if (type.shape == FieldType::Shape::kOne) {
            return One(type.scalar, text);
        }
        std::size_t count = type.length;
        if (type.shape == FieldType::Shape::kVector) {
            std::uint64_t bits = 0;
            auto status = Take(4, bits);
            if (!status.ok()) {
                return status;
            }
            count = static_cast<std::size_t>(bits);
            // Each element takes at least one byte (a string, its count).
            if (count > (bytes_.size() - offset_) / WidthOf(type.scalar)) {
                return core::Status::DataLoss("element count " + std::to_string(count) + " exceeds the message");
            }
        }
        text = "[";
        for (std::size_t index = 0; index < count; ++index) {
            std::string element;
            auto status = One(type.scalar, element);
            if (!status.ok()) {
                return status;
            }
            text += (index == 0 ? "" : ", ") + element;
        }
        text += "]";
        return core::Status::Ok();
    }

    std::size_t left() const noexcept { return bytes_.size() - offset_; }

private:
    core::Status Take(std::size_t width, std::uint64_t& bits)
    {
        if (left() < width) {
            return core::Status::DataLoss("message ends inside a field");
        }
        bits = 0;
        for (std::size_t index = 0; index < width; ++index) {
            bits |= static_cast<std::uint64_t>(bytes_.data()[offset_ + index]) << (8 * index);
        }
        offset_ += width;
        return core::Status::Ok();
    }

    core::Status One(Scalar scalar, std::string& text)
    {
        std::uint64_t bits = 0;
        auto status = Take(WidthOf(scalar), bits);
        if (!status.ok()) {
            return status;
        }
        switch (scalar) {
            case Scalar::kBool:
                if (bits > 1) {
                    return core::Status::DataLoss("bool byte is " + std::to_string(bits));
                }
                text = bits == 1 ? "true" : "false";
                return core::Status::Ok();
            case Scalar::kString: {
                const auto size = static_cast<std::size_t>(bits);
                if (size > left()) {
                    return core::Status::DataLoss("element count " + std::to_string(size) + " exceeds the message");
                }
                text = Quote(bytes_.data() + offset_, size);
                offset_ += size;
                return core::Status::Ok();
            }
            case Scalar::kF32: {
                const auto narrow_bits = static_cast<std::uint32_t>(bits);
                float value = 0;
                std::memcpy(&value, &narrow_bits, sizeof(value));
                text = FormatFloat(value, true);
                return core::Status::Ok();
            }
            case Scalar::kF64: {
                double value = 0;
                std::memcpy(&value, &bits, sizeof(value));
                text = FormatFloat(value, false);
                return core::Status::Ok();
            }
            default:
                break;
        }
        if (IsSigned(scalar)) {
            const auto width = 8 * WidthOf(scalar);
            // Sign-extend from the field's width.
            const auto shift = 64 - width;
            const auto value = static_cast<std::int64_t>(bits << shift) >> shift;
            text = std::to_string(value);
        } else {
            text = std::to_string(bits);
        }
        return core::Status::Ok();
    }

    ByteView bytes_;
    std::size_t offset_ {0};
};

}  // namespace

core::Result<FieldType> ParseFieldType(std::string_view text)
{
    using ResultType = core::Result<FieldType>;
    const auto invalid = [&text] {
        return ResultType::FromStatus(core::Status::InvalidArgument(
            "field type " + std::string(text) +
            " is not bool, i8-i64, u8-u64, f32, f64, or string, optionally with [] or [N]"));
    };
    FieldType type;
    auto name = Trim(text);
    if (name.empty()) {
        return ResultType::FromStatus(core::Status::InvalidArgument("empty field type"));
    }
    const auto bracket = name.find('[');
    if (bracket != std::string_view::npos) {
        if (name.back() != ']') {
            return invalid();
        }
        const auto inside = name.substr(bracket + 1, name.size() - bracket - 2);
        name = name.substr(0, bracket);
        if (inside.empty()) {
            type.shape = FieldType::Shape::kVector;
        } else {
            std::uint64_t length = 0;
            if (inside.front() == '+' || !ParseInteger(inside, false, 4, length) || length == 0) {
                return invalid();
            }
            type.shape = FieldType::Shape::kArray;
            type.length = static_cast<std::size_t>(length);
        }
    }
    for (const auto& entry : kScalars) {
        if (name == entry.name) {
            type.scalar = entry.scalar;
            return ResultType(type);
        }
    }
    return invalid();
}

std::string ToString(const FieldType& type)
{
    std::string text = NameOf(type.scalar);
    if (type.shape == FieldType::Shape::kVector) {
        text += "[]";
    } else if (type.shape == FieldType::Shape::kArray) {
        text += "[" + std::to_string(type.length) + "]";
    }
    return text;
}

core::Status EncodeField(const FieldType& type, std::string_view value, ByteBuffer& out)
{
    const auto failed = [&type](const std::string& element, const std::string& why) {
        return core::Status::InvalidArgument(ToString(type) + " value \"" + element + "\": " + why);
    };
    if (type.shape == FieldType::Shape::kOne) {
        // Strings are taken as they are; numbers may have spaces around them.
        const auto text = type.scalar == Scalar::kString ? value : Trim(value);
        const auto why = EncodeScalar(type.scalar, text, out);
        return why.empty() ? core::Status::Ok() : failed(std::string(value), why);
    }
    const auto elements = SplitCommas(value);
    if (type.shape == FieldType::Shape::kArray && elements.size() != type.length) {
        return failed(std::string(value),
                      "has " + std::to_string(elements.size()) + " elements, not " + std::to_string(type.length));
    }
    if (type.shape == FieldType::Shape::kVector) {
        if (elements.size() > std::numeric_limits<std::uint32_t>::max()) {
            return failed(std::string(value), "more than 2^32-1 elements");
        }
        Put(elements.size(), 4, out);
    }
    for (const auto element : elements) {
        const auto text = type.scalar == Scalar::kString ? element : Trim(element);
        const auto why = EncodeScalar(type.scalar, text, out);
        if (!why.empty()) {
            return failed(std::string(element), why);
        }
    }
    return core::Status::Ok();
}

core::Result<std::vector<std::string>> DecodeFields(const std::vector<FieldType>& types, ByteView bytes)
{
    using ResultType = core::Result<std::vector<std::string>>;
    Decoder decoder(bytes);
    std::vector<std::string> fields;
    for (std::size_t index = 0; index < types.size(); ++index) {
        std::string text;
        auto status = decoder.Field(types[index], text);
        if (!status.ok()) {
            return ResultType::FromStatus(core::Status::DataLoss(
                "field " + std::to_string(index + 1) + " (" + ToString(types[index]) + "): " + status.message()));
        }
        fields.push_back(std::move(text));
    }
    if (decoder.left() != 0) {
        return ResultType::FromStatus(core::Status::DataLoss(
            std::to_string(decoder.left()) + " bytes left over after " + std::to_string(types.size()) +
            " fields; the reply has other fields"));
    }
    return ResultType(std::move(fields));
}

}  // namespace puppet_master::tools
