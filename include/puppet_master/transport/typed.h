#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>

#include <puppet_master/core/result.h>
#include <puppet_master/core/status.h>
#include <puppet_master/transport/message.h>
#include <puppet_master/transport/transport.h>

// Typed readers and writers over the byte-oriented transport contract.
//
// A message type T is usable once Codec<T> is defined. Trivially copyable
// structs work out of the box by declaring a type name:
//
//     struct VehicleSpeed { double mps; std::int64_t stamp_ns; };
//     template <> struct puppet_master::transport::MessageTypeName<VehicleSpeed> {
//         static constexpr const char* value = "demo.VehicleSpeed";
//     };
//
// Other types specialize Codec<T> with their own serialization. See
// docs/typed-messages.md.
namespace puppet_master::transport {

// Names the wire type for T. Specialize with a `static constexpr const char*
// value`. The name lands in MessageDescriptor::type_name, so endpoints for
// different message types cannot share a topic by accident. The primary
// template is empty so a missing specialization is detected, not an error.
//
// An optional `static constexpr std::uint32_t version` becomes
// MessageDescriptor::version. Raise it whenever the struct changes in a way
// that old readers would misread, so processes built from different releases
// refuse to connect.
template <typename T>
struct MessageTypeName {};

namespace detail {

template <typename T, typename = void>
struct HasMessageTypeName : std::false_type {};

template <typename T>
struct HasMessageTypeName<T, std::void_t<decltype(MessageTypeName<T>::value)>> : std::true_type {};

template <typename T, typename = void>
struct MessageTypeVersion {
    static constexpr std::uint32_t value = 0;
};

template <typename T>
struct MessageTypeVersion<T, std::void_t<decltype(MessageTypeName<T>::version)>> {
    static constexpr std::uint32_t value = MessageTypeName<T>::version;
};

template <typename C, typename = void>
struct HasCodecVersion : std::false_type {};

template <typename C>
struct HasCodecVersion<C, std::void_t<decltype(C::Version())>> : std::true_type {};

}  // namespace detail

// Raw byte copy of a trivially copyable struct. The layout must match on both
// sides, so writer and reader need the same compiler ABI and endianness; use a
// custom codec for anything that crosses a host or language boundary.
template <typename T>
struct TrivialCodec {
    static_assert(std::is_trivially_copyable_v<T>, "TrivialCodec requires a trivially copyable type");
    static_assert(std::is_default_constructible_v<T>, "TrivialCodec requires a default constructible type");
    static_assert(detail::HasMessageTypeName<T>::value,
                  "specialize transport::MessageTypeName<T> or provide a transport::Codec<T>");

    static std::string TypeName() { return MessageTypeName<T>::value; }
    static std::string Encoding() { return "application/x-puppetmaster-trivial"; }
    static std::uint32_t Version() { return detail::MessageTypeVersion<T>::value; }

    static std::size_t MaxSize(const T&) { return sizeof(T); }

    static core::Result<std::size_t> Encode(const T& value, MutableByteView buffer)
    {
        if (buffer.size() < sizeof(T)) {
            return core::Result<std::size_t>::FromStatus(
                core::Status::OutOfRange("buffer is smaller than " + TypeName()));
        }
        std::memcpy(buffer.data(), &value, sizeof(T));
        return core::Result<std::size_t>(sizeof(T));
    }

    static core::Result<T> Decode(ByteView bytes)
    {
        if (bytes.size() != sizeof(T)) {
            return core::Result<T>::FromStatus(core::Status::DataLoss(
                TypeName() + " expects " + std::to_string(sizeof(T)) + " bytes, received "
                + std::to_string(bytes.size())));
        }
        T value;
        std::memcpy(&value, bytes.data(), sizeof(T));
        return core::Result<T>(value);
    }
};

// Serialization for T. Specialize to support non-trivial types. A codec
// provides:
//
//     static std::string TypeName();
//     static std::string Encoding();
//     static std::uint32_t Version();          // optional; 0 when absent
//     static std::size_t MaxSize(const T&);   // upper bound on Encode's output
//     static core::Result<std::size_t> Encode(const T&, MutableByteView);
//     static core::Result<T> Decode(ByteView);
//
// The primary template uses TrivialCodec, which accepts only trivially
// copyable types with a MessageTypeName.
template <typename T>
struct Codec : TrivialCodec<T> {};

// Descriptor for T's endpoints, derived from its codec.
template <typename T>
MessageDescriptor DescriptorFor()
{
    std::uint32_t version = 0;
    if constexpr (detail::HasCodecVersion<Codec<T>>::value) {
        version = Codec<T>::Version();
    }
    return MessageDescriptor {Codec<T>::TypeName(), Codec<T>::Encoding(), version};
}

// Builds an endpoint for T on `topic`.
template <typename T>
EndpointConfig EndpointFor(core::TopicSpec topic)
{
    return EndpointConfig {std::move(topic), DescriptorFor<T>()};
}

// Checks that an endpoint was declared for T; used by the typed wrappers.
template <typename T>
core::Status ValidateTypedDescriptor(const MessageDescriptor& descriptor)
{
    const auto expected = DescriptorFor<T>();
    if (descriptor != expected) {
        return core::Status::InvalidArgument(
            "endpoint carries " + descriptor.ToString() + ", but this binary was built for " + expected.ToString());
    }
    return core::Status::Ok();
}

template <typename T>
struct TypedMessage {
    T value;
    MessageMetadata metadata;
};

// Typed view over a byte writer. Encodes through WriteWith, so zero-copy
// backends serialize straight into transport memory.
template <typename T>
class TypedWriter {
public:
    // Wraps `writer`, checking that its descriptor matches T's codec.
    static core::Result<TypedWriter> Wrap(WriterPtr writer)
    {
        if (!writer) {
            return core::Result<TypedWriter>::FromStatus(core::Status::InvalidArgument("writer must not be null"));
        }
        auto status = ValidateTypedDescriptor<T>(writer->message_descriptor());
        if (!status.ok()) {
            return core::Result<TypedWriter>::FromStatus(std::move(status));
        }
        return core::Result<TypedWriter>(TypedWriter(std::move(writer)));
    }

    core::Status Write(const T& value, WriteOptions options = {})
    {
        return writer_->WriteWith(
            Codec<T>::MaxSize(value),
            [&value](MutableByteView buffer) { return Codec<T>::Encode(value, buffer); },
            options);
    }

    const core::TopicName& topic_name() const noexcept { return writer_->topic_name(); }
    const WriterPtr& raw() const noexcept { return writer_; }

private:
    explicit TypedWriter(WriterPtr writer) : writer_(std::move(writer)) {}

    WriterPtr writer_;
};

// Typed view over a byte reader. A message that fails to decode is consumed
// and reported as DataLoss; the next Read() continues with the next message.
template <typename T>
class TypedReader {
public:
    static core::Result<TypedReader> Wrap(ReaderPtr reader)
    {
        if (!reader) {
            return core::Result<TypedReader>::FromStatus(core::Status::InvalidArgument("reader must not be null"));
        }
        auto status = ValidateTypedDescriptor<T>(reader->message_descriptor());
        if (!status.ok()) {
            return core::Result<TypedReader>::FromStatus(std::move(status));
        }
        return core::Result<TypedReader>(TypedReader(std::move(reader)));
    }

    core::Result<TypedMessage<T>> Read(ReadOptions options = {})
    {
        auto message = reader_->Read(options);
        if (!message.ok()) {
            return core::Result<TypedMessage<T>>::FromStatus(message.status());
        }
        auto decoded = Codec<T>::Decode(ByteView(message.value().payload));
        if (!decoded.ok()) {
            return core::Result<TypedMessage<T>>::FromStatus(decoded.status());
        }
        return core::Result<TypedMessage<T>>(
            TypedMessage<T> {std::move(decoded).value(), message.value().metadata});
    }

    core::Status SetDataAvailableCallback(DataAvailableCallback callback)
    {
        return reader_->SetDataAvailableCallback(std::move(callback));
    }

    core::Result<std::size_t> PendingMessageCount() const { return reader_->PendingMessageCount(); }

    const core::TopicName& topic_name() const noexcept { return reader_->topic_name(); }
    const ReaderPtr& raw() const noexcept { return reader_; }

private:
    explicit TypedReader(ReaderPtr reader) : reader_(std::move(reader)) {}

    ReaderPtr reader_;
};

// Wraps a creation result, so typed endpoints read as one call:
//     auto speed = transport::MakeTypedReader<VehicleSpeed>(context.CreateReader(endpoint));
template <typename T>
core::Result<TypedWriter<T>> MakeTypedWriter(core::Result<WriterPtr> writer)
{
    if (!writer.ok()) {
        return core::Result<TypedWriter<T>>::FromStatus(writer.status());
    }
    return TypedWriter<T>::Wrap(std::move(writer).value());
}

template <typename T>
core::Result<TypedReader<T>> MakeTypedReader(core::Result<ReaderPtr> reader)
{
    if (!reader.ok()) {
        return core::Result<TypedReader<T>>::FromStatus(reader.status());
    }
    return TypedReader<T>::Wrap(std::move(reader).value());
}

}  // namespace puppet_master::transport
