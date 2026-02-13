#ifndef DDS_NODE_H
#define DDS_NODE_H

#include <fastdds/dds/domain/DomainParticipant.hpp>
#include <fastdds/dds/domain/DomainParticipantFactory.hpp>
#include <fastdds/dds/topic/Topic.hpp>
#include <fastdds/dds/topic/TypeSupport.hpp>
#include <fastdds/rtps/common/Locator.hpp>
#include <fastdds/rtps/transport/UDPv4TransportDescriptor.hpp>
#include <fastdds/rtps/transport/shared_mem/SharedMemTransportDescriptor.hpp>

#include <common/namespace_macros.h>
#include <common/attributes.h>
#include <common/common_macros.h>
#include <utils/logger/log.hpp>
#include <base/base_communication/node_base.h>

PUPPET_MASTER_COMMUNICATION_NS_BEGIN

constexpr size_t UDP_BUFFER_SIZE = 16 * 1024 * 1024;  // 16MB UDP缓冲区
constexpr size_t SHM_SEGMENT_SIZE = 160 * 1024 * 1024; // 160MB SHM段大小

using puppet_master::base::NodeBase;

class DDSNode : public NodeBase
{
public:
    explicit DDSNode(const std::string& node_name, TransType trans_type = TransType_BOTH);
    ~DDSNode();

    DDSNode(const DDSNode&) = delete;
    DDSNode& operator=(const DDSNode&) = delete;
    DDSNode(DDSNode&&) = delete;
    DDSNode& operator=(DDSNode&&) = delete;

public:
    std::shared_ptr<WriterBase> CreateWriter(std::string topic_name, void* data, void* attribute) override;
    std::shared_ptr<ReaderBase> CreateReader(std::string topic_name, void* data, void* attribute) override;

public:
    // 辅助接口：获取Participant（用于传输层监控）
    eprosima::fastdds::dds::DomainParticipant* GetParticipant() const {
        return participant_;
    }

private:
    // 私有方法：注册Topic（线程安全）
    bool RegisterTopic(const std::string& topic_name, 
                       std::shared_ptr<eprosima::fastdds::dds::TypeSupport> type_support);

    // 私有方法：初始化传输层（UDP+SHM）
    void InitTransports(eprosima::fastdds::dds::DomainParticipantQos& participant_qos, 
                        TransType trans_type);

private:
    eprosima::fastdds::dds::DomainParticipant* participant_ = nullptr;
    eprosima::fastdds::dds::Publisher* reusable_publisher_ = nullptr;    
    eprosima::fastdds::dds::Subscriber* reusable_subscriber_ = nullptr;  

    std::unordered_map<std::string, eprosima::fastdds::dds::Topic*> topic_cache_;
    std::mutex topic_mutex_; 

    std::string node_name_;
};



PUPPET_MASTER_COMMUNICATION_NS_END

#endif