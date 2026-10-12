#include "byte_payload_type.h"

#include <cstring>
#include <limits>

namespace puppet_master::transport::fastdds::detail {

namespace {

// CDR little-endian encapsulation identifier.
constexpr std::uint16_t kEncapsulationCdrLe = 0x0001;
constexpr std::uint32_t kEncapsulationSize = 4;
constexpr std::uint32_t kLengthSize = 4;
constexpr std::uint32_t kHeaderSize = kEncapsulationSize + kLengthSize;
// Initial pool reservation; samples larger than this are handled by the
// PREALLOCATED_WITH_REALLOC history memory policy configured on endpoints.
constexpr std::uint32_t kInitialTypeSize = 256;

bool SerializedSize(const BytePayload& payload, std::uint32_t& size)
{
    if (payload.size() > std::numeric_limits<std::uint32_t>::max() - kHeaderSize) {
        return false;
    }
    size = kHeaderSize + static_cast<std::uint32_t>(payload.size());
    return true;
}

void WriteUint32Le(std::uint8_t* out, std::uint32_t value)
{
    out[0] = static_cast<std::uint8_t>(value & 0xFFu);
    out[1] = static_cast<std::uint8_t>((value >> 8) & 0xFFu);
    out[2] = static_cast<std::uint8_t>((value >> 16) & 0xFFu);
    out[3] = static_cast<std::uint8_t>((value >> 24) & 0xFFu);
}

std::uint32_t ReadUint32Le(const std::uint8_t* in)
{
    return static_cast<std::uint32_t>(in[0])
        | (static_cast<std::uint32_t>(in[1]) << 8)
        | (static_cast<std::uint32_t>(in[2]) << 16)
        | (static_cast<std::uint32_t>(in[3]) << 24);
}

bool SerializeInto(const BytePayload& sample, dds_rtps::SerializedPayload_t& payload)
{
    std::uint32_t size = 0;
    if (!SerializedSize(sample, size) || payload.max_size < size || payload.data == nullptr) {
        return false;
    }

    // Encapsulation header: 2-byte representation id (big-endian) + 2 option bytes.
    payload.data[0] = static_cast<std::uint8_t>((kEncapsulationCdrLe >> 8) & 0xFFu);
    payload.data[1] = static_cast<std::uint8_t>(kEncapsulationCdrLe & 0xFFu);
    payload.data[2] = 0;
    payload.data[3] = 0;
    WriteUint32Le(payload.data + kEncapsulationSize, static_cast<std::uint32_t>(sample.size()));
    if (!sample.empty()) {
        std::memcpy(payload.data + kHeaderSize, sample.data(), sample.size());
    }

    payload.encapsulation = kEncapsulationCdrLe;
    payload.length = size;
    return true;
}

bool DeserializeFrom(const dds_rtps::SerializedPayload_t& payload, BytePayload& sample)
{
    if (payload.data == nullptr || payload.length < kHeaderSize) {
        return false;
    }

    const std::uint32_t length = ReadUint32Le(payload.data + kEncapsulationSize);
    if (length > payload.length - kHeaderSize) {
        return false;
    }

    sample.assign(payload.data + kHeaderSize, payload.data + kHeaderSize + length);
    return true;
}

}  // namespace

BytePayloadType::BytePayloadType(const std::string& type_name)
{
#if PUPPETMASTER_FASTDDS_V3
    set_name(type_name);
    max_serialized_type_size = kInitialTypeSize;
    is_compute_key_provided = false;
#else
    setName(type_name.c_str());
    m_typeSize = kInitialTypeSize;
    m_isGetKeyDefined = false;
#endif
}

#if PUPPETMASTER_FASTDDS_V3

bool BytePayloadType::serialize(
    const void* const data,
    dds_rtps::SerializedPayload_t& payload,
    dds::DataRepresentationId_t /*data_representation*/)
{
    return data != nullptr && SerializeInto(*static_cast<const BytePayload*>(data), payload);
}

bool BytePayloadType::deserialize(dds_rtps::SerializedPayload_t& payload, void* data)
{
    return data != nullptr && DeserializeFrom(payload, *static_cast<BytePayload*>(data));
}

uint32_t BytePayloadType::calculate_serialized_size(
    const void* const data,
    dds::DataRepresentationId_t /*data_representation*/)
{
    std::uint32_t size = kHeaderSize;
    if (data != nullptr) {
        SerializedSize(*static_cast<const BytePayload*>(data), size);
    }
    return size;
}

void* BytePayloadType::create_data()
{
    return new BytePayload();
}

void BytePayloadType::delete_data(void* data)
{
    delete static_cast<BytePayload*>(data);
}

bool BytePayloadType::compute_key(
    dds_rtps::SerializedPayload_t& /*payload*/,
    dds_rtps::InstanceHandle_t& /*handle*/,
    bool /*force_md5*/)
{
    return false;
}

bool BytePayloadType::compute_key(
    const void* const /*data*/,
    dds_rtps::InstanceHandle_t& /*handle*/,
    bool /*force_md5*/)
{
    return false;
}

#else

bool BytePayloadType::serialize(void* data, dds_rtps::SerializedPayload_t* payload)
{
    return data != nullptr && payload != nullptr
        && SerializeInto(*static_cast<const BytePayload*>(data), *payload);
}

bool BytePayloadType::deserialize(dds_rtps::SerializedPayload_t* payload, void* data)
{
    return data != nullptr && payload != nullptr
        && DeserializeFrom(*payload, *static_cast<BytePayload*>(data));
}

std::function<uint32_t()> BytePayloadType::getSerializedSizeProvider(void* data)
{
    return [data]() -> uint32_t {
        std::uint32_t size = kHeaderSize;
        if (data != nullptr) {
            SerializedSize(*static_cast<const BytePayload*>(data), size);
        }
        return size;
    };
}

void* BytePayloadType::createData()
{
    return new BytePayload();
}

void BytePayloadType::deleteData(void* data)
{
    delete static_cast<BytePayload*>(data);
}

bool BytePayloadType::getKey(void* /*data*/, dds_rtps::InstanceHandle_t* /*handle*/, bool /*force_md5*/)
{
    return false;
}

#endif

}  // namespace puppet_master::transport::fastdds::detail
