#ifndef DDS_NODE_IMPL_H
#define DDS_NODE_IMPL_H

#include <common/namespace_macros.h>
#include <common/attributes.h>

PUPPET_MASTER_COMMUNICATION_NS_BEGIN

constexpr int DEFAULT_DOMAIN_ID = 0;  // 默认DDS域ID
constexpr size_t UDP_BUFFER_SIZE = 16 * 1024 * 1024;  // 16MB UDP缓冲区
constexpr size_t SHM_SEGMENT_SIZE = 160 * 1024 * 1024; // 160MB SHM段大小

class DDSNodeImpl 
{
public:
    explicit DDSNodeImpl(const std::string& node_name, TransType trans_type = TransType_BOTH);
    ~DDSNodeImpl();

    DDSNodeImpl(const DDSNodeImpl&) = delete;
    DDSNodeImpl& operator=(const DDSNodeImpl&) = delete;
    DDSNodeImpl(DDSNodeImpl&&) = delete;
    DDSNodeImpl& operator=(DDSNodeImpl&&) = delete;

    // 核心接口：创建Writer/Reader（复用Publisher/Subscriber）
    std::shared_ptr<TGPub> CreateWriter(const std::string& topic_name, 
                                        void* data_type, 
                                        void* attribute);
    std::shared_ptr<DDSReaderBase> CreateReader(const std::string& topic_name, 
                                                void* data_type, 
                                                void* attribute);

    // 辅助接口：获取Participant（用于传输层监控）
    eprosima::fastdds::dds::DomainParticipant* GetParticipant() const {
        return m_participant;
    }

private:
    // 私有方法：注册Topic（线程安全）
    bool RegisterTopic(const std::string& topic_name, 
                       std::shared_ptr<eprosima::fastdds::dds::TypeSupport> type_support);

    // 私有方法：初始化传输层（UDP+SHM）
    void InitTransports(eprosima::fastdds::dds::DomainParticipantQos& participant_qos, 
                        TransType trans_type);

    // 核心成员：复用的Publisher/Subscriber（全局唯一）
    eprosima::fastdds::dds::DomainParticipant* m_participant = nullptr;
    eprosima::fastdds::dds::Publisher* m_reusable_publisher = nullptr;    // 复用的发布者
    eprosima::fastdds::dds::Subscriber* m_reusable_subscriber = nullptr;  // 复用的订阅者

    // 缓存：已注册的Topic（避免重复创建）
    std::unordered_map<std::string, eprosima::fastdds::dds::Topic*> m_topic_cache;
    std::mutex m_topic_mutex;  // 保护Topic缓存的线程安全锁

    // 配置参数
    std::string m_node_name;
    TransType m_trans_type;
};



PUPPET_MASTER_COMMUNICATION_NS_END

#endif