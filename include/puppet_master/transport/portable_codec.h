#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <puppet_master/core/result.h>
#include <puppet_master/core/status.h>
#include <puppet_master/transport/message.h>
#include <puppet_master/transport/typed.h>

// A codec that does not depend on struct layout, padding, compiler, or host
// byte order, for messages between processes built with different compilers
// or running on different architectures. See docs/typed-messages.md.
//
// A type lists its fields once, in wire order, by specializing
// PortableFields:
//
//     struct Pose { double x; double y; std::string frame; std::vector<float> covariance; };
//
//     template <> struct puppet_master::transport::PortableFields<Pose> {
//         template <typename Message, typename Visitor>
//         static void Visit(Message& pose, Visitor&& field)
//         {
//             field(pose.x);
//             field(pose.y);
//             field(pose.frame);
//             field(pose.covariance);
//         }
//     };
//     template <> struct puppet_master::transport::Codec<Pose> : PortableCodec<Pose> {};
//
// Wire format, all little-endian: bool as one byte (0 or 1); integers in
// their own width; float and double as IEEE 754 bits; enums as their
// underlying type; std::string and std::vector as a u32 element count, then
// the elements; std::array as its elements; nested types with
// PortableFields as their fields in order. Adding, removing, or reordering a
// field changes the format, so raise the type's version with it.
namespace puppet_master::transport {

template <typename T>
struct PortableFields {};

namespace detail {

template <typename T, typename = void>
struct HasPortableFields : std::false_type {};

// Detected by whether Visit accepts a no-op visitor on a T&.
struct PortableProbe {
    template <typename Field>
    void operator()(Field&) const
    {
    }
};

template <typename T>
struct HasPortableFields<T, std::void_t<decltype(PortableFields<T>::Visit(std::declval<T&>(), PortableProbe {}))>>
    : std::true_type {};

template <typename T>
struct IsVector : std::false_type {};
template <typename T, typename A>
struct IsVector<std::vector<T, A>> : std::true_type {};

template <typename T>
struct IsArray : std::false_type {};
template <typename T, std::size_t N>
struct IsArray<std::array<T, N>> : std::true_type {};

// Floats must be IEEE 754 for their bits to mean the same on both sides.
static_assert(std::numeric_limits<float>::is_iec559 && sizeof(float) == 4, "PortableCodec needs IEEE 754 float");
static_assert(std::numeric_limits<double>::is_iec559 && sizeof(double) == 8, "PortableCodec needs IEEE 754 double");

template <typename T>
constexpr bool kPortableScalar = std::is_same_v<T, bool> || std::is_integral_v<T> || std::is_floating_point_v<T>
    || std::is_enum_v<T>;

template <typename T>
struct PortableSupported
    : std::bool_constant<kPortableScalar<T> || std::is_same_v<T, std::string> || HasPortableFields<T>::value> {};
template <typename T, typename A>
struct PortableSupported<std::vector<T, A>> : PortableSupported<T> {};
// std::vector<bool> packs bits behind proxy references; use
// std::vector<std::uint8_t> instead.
template <typename A>
struct PortableSupported<std::vector<bool, A>> : std::false_type {};
template <typename T, std::size_t N>
struct PortableSupported<std::array<T, N>> : PortableSupported<T> {};

template <typename T>
std::uint64_t ScalarBits(const T& value)
{
    if constexpr (std::is_same_v<T, bool>) {
        return value ? 1 : 0;
    } else if constexpr (std::is_enum_v<T>) {
        return ScalarBits(static_cast<std::underlying_type_t<T>>(value));
    } else if constexpr (std::is_floating_point_v<T>) {
        using Bits = std::conditional_t<sizeof(T) == 4, std::uint32_t, std::uint64_t>;
        Bits bits;
        std::memcpy(&bits, &value, sizeof(bits));
        return bits;
    } else {
        return static_cast<std::uint64_t>(static_cast<std::make_unsigned_t<T>>(value));
    }
}

template <typename T>
constexpr std::size_t ScalarSize()
{
    if constexpr (std::is_same_v<T, bool>) {
        return 1;
    } else if constexpr (std::is_enum_v<T>) {
        return sizeof(std::underlying_type_t<T>);
    } else {
        return sizeof(T);
    }
}

// Counts the encoded size. Never fails.
class PortableSizer {
public:
    template <typename T>
    void operator()(const T& value)
    {
        static_assert(PortableSupported<T>::value,
                      "PortableCodec: unsupported field type; use bool, integers, float, double, enums, "
                      "std::string, std::vector, std::array, or a type with PortableFields");
        if constexpr (kPortableScalar<T>) {
            size_ += ScalarSize<T>();
        } else if constexpr (std::is_same_v<T, std::string>) {
            size_ += 4 + value.size();
        } else if constexpr (IsVector<T>::value) {
            size_ += 4;
            Elements(value);
        } else if constexpr (IsArray<T>::value) {
            Elements(value);
        } else {
            PortableFields<T>::Visit(value, *this);
        }
    }

    std::size_t size() const noexcept { return size_; }

private:
    template <typename Container>
    void Elements(const Container& container)
    {
        using Element = typename Container::value_type;
        if constexpr (kPortableScalar<Element>) {
            size_ += container.size() * ScalarSize<Element>();
        } else {
            for (const auto& element : container) {
                (*this)(element);
            }
        }
    }

    std::size_t size_ {0};
};

class PortableWriter {
public:
    explicit PortableWriter(MutableByteView buffer) : buffer_(buffer) {}

    template <typename T>
    void operator()(const T& value)
    {
        if (!status_.ok()) {
            return;
        }
        if constexpr (kPortableScalar<T>) {
            Put(ScalarBits(value), ScalarSize<T>());
        } else if constexpr (std::is_same_v<T, std::string>) {
            Count(value.size());
            Bytes(value.data(), value.size());
        } else if constexpr (IsVector<T>::value) {
            Count(value.size());
            for (const auto& element : value) {
                (*this)(element);
            }
        } else if constexpr (IsArray<T>::value) {
            for (const auto& element : value) {
                (*this)(element);
            }
        } else {
            PortableFields<T>::Visit(value, *this);
        }
    }

    const core::Status& status() const noexcept { return status_; }
    std::size_t used() const noexcept { return offset_; }

private:
    bool Reserve(std::size_t size)
    {
        if (buffer_.size() - offset_ < size) {
            status_ = core::Status::OutOfRange("portable encoding does not fit the buffer");
            return false;
        }
        return true;
    }

    void Put(std::uint64_t bits, std::size_t size)
    {
        if (!Reserve(size)) {
            return;
        }
        for (std::size_t index = 0; index < size; ++index) {
            buffer_.data()[offset_ + index] = static_cast<core::Byte>(bits >> (8 * index));
        }
        offset_ += size;
    }

    void Count(std::size_t count)
    {
        if (count > std::numeric_limits<std::uint32_t>::max()) {
            status_ = core::Status::OutOfRange("portable encoding: more than 2^32-1 elements");
            return;
        }
        Put(count, 4);
    }

    void Bytes(const char* data, std::size_t size)
    {
        if (!status_.ok() || !Reserve(size)) {
            return;
        }
        if (size != 0) {
            std::memcpy(buffer_.data() + offset_, data, size);
        }
        offset_ += size;
    }

    MutableByteView buffer_;
    std::size_t offset_ {0};
    core::Status status_ {core::Status::Ok()};
};

class PortableReader {
public:
    explicit PortableReader(ByteView bytes) : bytes_(bytes) {}

    template <typename T>
    void operator()(T& value)
    {
        if (!status_.ok()) {
            return;
        }
        if constexpr (std::is_same_v<T, bool>) {
            const auto bits = Take(1);
            if (status_.ok() && bits > 1) {
                status_ = core::Status::DataLoss("portable decoding: bool byte is " + std::to_string(bits));
                return;
            }
            value = bits == 1;
        } else if constexpr (std::is_enum_v<T>) {
            std::underlying_type_t<T> raw {};
            (*this)(raw);
            value = static_cast<T>(raw);
        } else if constexpr (std::is_floating_point_v<T>) {
            using Bits = std::conditional_t<sizeof(T) == 4, std::uint32_t, std::uint64_t>;
            const auto bits = static_cast<Bits>(Take(sizeof(T)));
            std::memcpy(&value, &bits, sizeof(value));
        } else if constexpr (std::is_integral_v<T>) {
            value = static_cast<T>(static_cast<std::make_unsigned_t<T>>(Take(sizeof(T))));
        } else if constexpr (std::is_same_v<T, std::string>) {
            const auto count = Count(1);
            if (!status_.ok()) {
                return;
            }
            value.assign(reinterpret_cast<const char*>(bytes_.data() + offset_), count);
            offset_ += count;
        } else if constexpr (IsVector<T>::value) {
            using Element = typename T::value_type;
            // Each element takes at least one byte, so a count larger than
            // what is left is corrupt; checking first keeps a bad count from
            // allocating gigabytes.
            const auto count = Count(kPortableScalar<Element> ? ScalarSize<Element>() : 1);
            if (!status_.ok()) {
                return;
            }
            value.clear();
            value.resize(count);
            for (auto& element : value) {
                (*this)(element);
            }
        } else if constexpr (IsArray<T>::value) {
            for (auto& element : value) {
                (*this)(element);
            }
        } else {
            PortableFields<T>::Visit(value, *this);
        }
    }

    // Decoded messages must use every byte; leftovers mean the writer's
    // field list differs from ours.
    core::Status Finish() const
    {
        if (!status_.ok()) {
            return status_;
        }
        if (offset_ != bytes_.size()) {
            return core::Status::DataLoss("portable decoding: " + std::to_string(bytes_.size() - offset_)
                                          + " bytes left over; the writer's field list differs");
        }
        return core::Status::Ok();
    }

private:
    std::uint64_t Take(std::size_t size)
    {
        if (bytes_.size() - offset_ < size) {
            status_ = core::Status::DataLoss("portable decoding: message ends inside a field");
            return 0;
        }
        std::uint64_t bits = 0;
        for (std::size_t index = 0; index < size; ++index) {
            bits |= static_cast<std::uint64_t>(bytes_.data()[offset_ + index]) << (8 * index);
        }
        offset_ += size;
        return bits;
    }

    std::size_t Count(std::size_t element_size)
    {
        const auto count = static_cast<std::size_t>(Take(4));
        if (status_.ok() && count > (bytes_.size() - offset_) / element_size) {
            status_ = core::Status::DataLoss("portable decoding: element count " + std::to_string(count)
                                             + " exceeds the message");
        }
        return count;
    }

    ByteView bytes_;
    std::size_t offset_ {0};
    core::Status status_ {core::Status::Ok()};
};

}  // namespace detail

// Codec<T> for types with PortableFields and a MessageTypeName.
template <typename T>
struct PortableCodec {
    static_assert(detail::HasPortableFields<T>::value, "PortableCodec<T> requires PortableFields<T>");
    static_assert(detail::HasMessageTypeName<T>::value, "PortableCodec<T> requires MessageTypeName<T>");
    static_assert(std::is_default_constructible_v<T>, "PortableCodec<T> requires a default constructible type");

    static std::string TypeName() { return MessageTypeName<T>::value; }
    static std::string Encoding() { return "application/x-puppetmaster-portable"; }
    static std::uint32_t Version() { return detail::MessageTypeVersion<T>::value; }

    static std::size_t MaxSize(const T& value)
    {
        detail::PortableSizer sizer;
        sizer(value);
        return sizer.size();
    }

    static core::Result<std::size_t> Encode(const T& value, MutableByteView buffer)
    {
        detail::PortableWriter writer(buffer);
        writer(value);
        if (!writer.status().ok()) {
            return core::Result<std::size_t>::FromStatus(writer.status());
        }
        return core::Result<std::size_t>(writer.used());
    }

    static core::Result<T> Decode(ByteView bytes)
    {
        T value {};
        detail::PortableReader reader(bytes);
        reader(value);
        auto status = reader.Finish();
        if (!status.ok()) {
            return core::Result<T>::FromStatus(core::Status::Error(status.code(), TypeName() + ": " + status.message()));
        }
        return core::Result<T>(std::move(value));
    }
};

}  // namespace puppet_master::transport
