#pragma once

#include <fastdds/dds/topic/TopicDataType.hpp>

#include <cstdint>
#include <functional>
#include <string>

#include <puppet_master/transport/message.h>

// FastDDS 3.x is found through the `fastdds` CMake package and 2.x through
// `fastrtps`; the CMake target defines PUPPETMASTER_FASTDDS_V3 accordingly.
#ifndef PUPPETMASTER_FASTDDS_V3
#define PUPPETMASTER_FASTDDS_V3 0
#endif

namespace puppet_master::transport::fastdds::detail {

namespace dds = eprosima::fastdds::dds;

#if PUPPETMASTER_FASTDDS_V3
namespace dds_rtps = eprosima::fastdds::rtps;
#else
namespace dds_rtps = eprosima::fastrtps::rtps;
#endif

// Sample type carried by the FastDDS endpoints: an opaque byte buffer.
using BytePayload = ByteBuffer;

// Wire layout: 4-byte CDR encapsulation header, uint32 little-endian payload
// length, then the raw payload bytes.
class BytePayloadType final : public dds::TopicDataType {
public:
    explicit BytePayloadType(const std::string& type_name);

#if PUPPETMASTER_FASTDDS_V3
    bool serialize(
        const void* const data,
        dds_rtps::SerializedPayload_t& payload,
        dds::DataRepresentationId_t data_representation) override;
    bool deserialize(dds_rtps::SerializedPayload_t& payload, void* data) override;
    uint32_t calculate_serialized_size(
        const void* const data,
        dds::DataRepresentationId_t data_representation) override;
    void* create_data() override;
    void delete_data(void* data) override;
    bool compute_key(
        dds_rtps::SerializedPayload_t& payload,
        dds_rtps::InstanceHandle_t& handle,
        bool force_md5 = false) override;
    bool compute_key(
        const void* const data,
        dds_rtps::InstanceHandle_t& handle,
        bool force_md5 = false) override;
#else
    bool serialize(void* data, dds_rtps::SerializedPayload_t* payload) override;
    bool deserialize(dds_rtps::SerializedPayload_t* payload, void* data) override;
    std::function<uint32_t()> getSerializedSizeProvider(void* data) override;
    void* createData() override;
    void deleteData(void* data) override;
    bool getKey(void* data, dds_rtps::InstanceHandle_t* handle, bool force_md5 = false) override;
#endif
};

}  // namespace puppet_master::transport::fastdds::detail
