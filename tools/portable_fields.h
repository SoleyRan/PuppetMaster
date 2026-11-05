#pragma once

// Encodes and decodes PortableCodec messages field by field from text, for
// tools that do not have the message's C++ type. The wire format is that of
// puppet_master/transport/portable_codec.h; see docs/typed-messages.md.

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <puppet_master/core/result.h>
#include <puppet_master/core/status.h>
#include <puppet_master/transport/message.h>

namespace puppet_master::tools {

enum class Scalar { kBool, kI8, kI16, kI32, kI64, kU8, kU16, kU32, kU64, kF32, kF64, kString };

// One field: a scalar or a string, a vector of them ("u32[]": a u32 count,
// then the elements), or a fixed array ("f64[3]": the elements only). A
// nested type with PortableFields is its fields in order on the wire, so it
// is written as those fields.
struct FieldType {
    enum class Shape { kOne, kVector, kArray };

    Scalar scalar {Scalar::kU8};
    Shape shape {Shape::kOne};
    std::size_t length {0};  // kArray
};

// "bool", "i8" to "i64", "u8" to "u64", "f32", "f64", or "string", with "[]"
// for a vector or "[N]" for an array.
core::Result<FieldType> ParseFieldType(std::string_view text);
std::string ToString(const FieldType& type);

// Appends `value` encoded as `type`. Integers are decimal or 0x hex; bools
// true, false, 1, or 0; strings are taken as they are. A vector's or array's
// elements are separated by commas, so string elements cannot contain one;
// an empty value is an empty vector.
core::Status EncodeField(const FieldType& type, std::string_view value, transport::ByteBuffer& out);

// Decodes `bytes` as `types` in order, into one text per field: numbers in
// decimal, strings quoted with \xNN for bytes that are not printable,
// vectors and arrays as "[a, b]". DataLoss, as PortableCodec, if the bytes
// end inside a field, a count exceeds them, a bool is not 0 or 1, or bytes
// are left over.
core::Result<std::vector<std::string>> DecodeFields(const std::vector<FieldType>& types, transport::ByteView bytes);

}  // namespace puppet_master::tools
