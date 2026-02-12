#include "dds_node_impl.h"

PUPPET_MASTER_COMMUNICATION_NS_BEGIN

DDSNodeImpl::DDSNodeImpl(const std::string& node_name, TransType trans_type)
    : m_node_name(node_name), m_trans_type(trans_type) 
{
    eprosima::fastdds::dds::DomainParticipantQos participant_qos;
    
    auto& discovery_config = participant_qos.wire_protocol().builtin.discovery_config;
    discovery_config.discoveryProtocol = eprosima::fastdds::rtps::DiscoveryProtocol::SIMPLE;
    discovery_config.use_SIMPLE_EndpointDiscoveryProtocol = true;
    discovery_config.m_simpleEDP.use_PublicationReaderANDSubscriptionWriter = true;
    discovery_config.m_simpleEDP.use_PublicationWriterANDSubscriptionReader = true;
    discovery_config.leaseDuration = eprosima::fastdds::dds::c_TimeInfinite;

    participant_qos.entity_factory().autoenable_created_entities = true;
    participant_qos.name(m_node_name);
    participant_qos.transport().use_builtin_transports = false;

    InitTransports(participant_qos, trans_type);

    m_participant = eprosima::fastdds::dds::DomainParticipantFactory::get_instance()
        ->create_participant(DEFAULT_DOMAIN_ID, participant_qos);
    if (m_participant == nullptr) 
    {
        throw std::runtime_error("Failed to create DomainParticipant!");
    }
    m_participant->enable();
    LOG_Info() << "Created DomainParticipant: " << m_node_name;

    eprosima::fastdds::dds::PublisherQos pub_qos = eprosima::fastdds::dds::PUBLISHER_QOS_DEFAULT;
    pub_qos.entity_factory().autoenable_created_entities = false;
    m_reusable_publisher = m_participant->create_publisher(pub_qos);
    if (m_reusable_publisher == nullptr) 
    {
        throw std::runtime_error("Failed to create reusable Publisher!");
    }
    m_reusable_publisher->enable();
    LOG_Info() << "Created reusable Publisher";

    eprosima::fastdds::dds::SubscriberQos sub_qos = eprosima::fastdds::dds::SUBSCRIBER_QOS_DEFAULT;
    sub_qos.entity_factory().autoenable_created_entities = false;
    m_reusable_subscriber = m_participant->create_subscriber(sub_qos);
    if (m_reusable_subscriber == nullptr) 
    {
        throw std::runtime_error("Failed to create reusable Subscriber!");
    }
    m_reusable_subscriber->enable();
    LOG_Info() << "Created reusable Subscriber";
}

DDSNodeImpl::~DDSNodeImpl() 
{
    std::lock_guard<std::mutex> lg(m_topic_mutex);

    for (auto& [topic_name, topic] : m_topic_cache) 
    {
        if (topic != nullptr) 
            m_participant->delete_topic(topic);
    }
    m_topic_cache.clear();

    if (m_reusable_publisher != nullptr) 
    {
        m_participant->delete_publisher(m_reusable_publisher);
        m_reusable_publisher = nullptr;
    }
    if (m_reusable_subscriber != nullptr) 
    {
        m_participant->delete_subscriber(m_reusable_subscriber);
        m_reusable_subscriber = nullptr;
    }

    if (m_participant != nullptr) 
    {
        eprosima::fastdds::dds::DomainParticipantFactory::get_instance()->delete_participant(m_participant);
        m_participant = nullptr;
    }
    LOG_Info() << "DDSNodeImpl destroyed successfully";
}

void DDSNodeImpl::InitTransports(eprosima::fastdds::dds::DomainParticipantQos& participant_qos, TransType trans_type) 
{
    if (trans_type == TransType_SHM || trans_type == TransType_BOTH) 
    {
        auto shm_transport = std::make_shared<eprosima::fastdds::rtps::SharedMemTransportDescriptor>();
        shm_transport->segment_size(SHM_SEGMENT_SIZE);
        shm_transport->healthy_check_timeout_ms(1000);
        participant_qos.transport().user_transports.push_back(shm_transport);
        LOG_Info("Added SHM transport (segment size: 160MB)");
    }

    if (trans_type == TransType_UDP || trans_type == TransType_BOTH) 
    {
        auto udp_transport = std::make_shared<eprosima::fastdds::rtps::UDPv4TransportDescriptor>();
        udp_transport->sendBufferSize = UDP_BUFFER_SIZE;
        udp_transport->receiveBufferSize = UDP_BUFFER_SIZE;
        participant_qos.transport().user_transports.push_back(udp_transport);
        LOG_Info("Added UDP transport (buffer size: 16MB)");
    }
}

bool DDSNodeImpl::RegisterTopic(const std::string& topic_name, 
                                std::shared_ptr<eprosima::fastdds::dds::TypeSupport> type_support) 
{
    std::lock_guard<std::mutex> lg(m_topic_mutex);

    if (m_topic_cache.find(topic_name) != m_topic_cache.end())
        return true;

    type_support->register_type(m_participant);

    eprosima::fastdds::dds::TopicQos topic_qos = eprosima::fastdds::dds::TOPIC_QOS_DEFAULT;
    auto topic = m_participant->create_topic(topic_name, 
                                             type_support->get_type_name(), 
                                             topic_qos);
    if (topic == nullptr) 
    {
        LOG_Error(("Failed to create Topic: " + topic_name).c_str());
        return false;
    }

    m_topic_cache[topic_name] = topic;
    LOG_Info() << "Registered Topic: " << topic_name;
    return true;
}

std::shared_ptr<TGPub> DDSNodeImpl::CreateWriter(const std::string& topic_name, 
                                                 void* data_type, 
                                                 void* attribute) 
{
    if (data_type == nullptr || attribute == nullptr) 
    {
        throw std::invalid_argument("data_type or attribute is nullptr!");
    }

    auto type_support = std::make_shared<eprosima::fastdds::dds::TypeSupport>(
        static_cast<eprosima::fastdds::dds::TopicDataType*>(data_type)
    );
    auto dds_attr = static_cast<DDSAttribute*>(attribute);

    if (!RegisterTopic(topic_name, type_support)) 
    {
        throw std::runtime_error("Failed to register Topic for Writer: " + topic_name);
    }

    eprosima::fastdds::dds::DataWriterQos writer_qos;
    writer_qos.endpoint().history_memory_policy = eprosima::fastdds::rtps::PREALLOCATED_WITH_REALLOC_MEMORY_MODE;
    writer_qos.data_sharing().automatic();
    writer_qos.reliability().kind = dds_attr->qos_profile.reliability;
    writer_qos.history().kind = dds_attr->qos_profile.history;
    writer_qos.history().depth = dds_attr->qos_profile.history_depth;
    writer_qos.durability().kind = dds_attr->qos_profile.durability;
    writer_qos.resource_limits().max_samples = 1000;
    writer_qos.resource_limits().max_instances = 10;
    writer_qos.resource_limits().max_samples_per_instance = 100;
    writer_qos.representation().m_value.push_back(eprosima::fastdds::dds::DataRepresentationId::XCDR_DATA_REPRESENTATION);
    writer_qos.publish_mode().kind = eprosima::fastdds::dds::ASYNCHRONOUS_PUBLISH_MODE;
    writer_qos.publish_mode().thread_pool_size = 8;  // 适配多Writer并发

    auto writer = m_reusable_publisher->create_datawriter(
        m_topic_cache[topic_name], 
        writer_qos
    );
    if (writer == nullptr) 
    {
        throw std::runtime_error("Failed to create DataWriter for Topic: " + topic_name);
    }

    auto puber = std::make_shared<TGPub>(attribute);
    puber->set_writer(writer);

    LOG_Info() << "Created DataWriter for Topic: " << topic_name;
    return puber;
}

std::shared_ptr<DDSReaderBase> DDSNodeImpl::CreateReader(const std::string& topic_name, 
                                                         void* data_type, 
                                                         void* attribute) {
    if (data_type == nullptr || attribute == nullptr) 
    {
        throw std::invalid_argument("data_type or attribute is nullptr!");
    }

    auto type_support = std::make_shared<eprosima::fastdds::dds::TypeSupport>(
        static_cast<eprosima::fastdds::dds::TopicDataType*>(data_type)
    );
    auto dds_attr = static_cast<DDSAttribute*>(attribute);

    if (!RegisterTopic(topic_name, type_support)) 
    {
        throw std::runtime_error("Failed to register Topic for Reader: " + topic_name);
    }

    eprosima::fastdds::dds::DataReaderQos reader_qos;
    reader_qos.endpoint().history_memory_policy = eprosima::fastdds::rtps::PREALLOCATED_WITH_REALLOC_MEMORY_MODE;
    reader_qos.data_sharing().automatic();
    reader_qos.reliability().kind = dds_attr->qos_profile.reliability;
    reader_qos.history().kind = dds_attr->qos_profile.history;
    reader_qos.history().depth = dds_attr->qos_profile.history_depth;
    reader_qos.durability().kind = dds_attr->qos_profile.durability;
    reader_qos.resource_limits().max_samples = 1000;
    reader_qos.resource_limits().max_instances = 10;
    reader_qos.resource_limits().max_samples_per_instance = 100;
    reader_qos.representation().m_value.push_back(eprosima::fastdds::dds::DataRepresentationId::XCDR_DATA_REPRESENTATION);
    auto reader = m_reusable_subscriber->create_datareader(
        m_topic_cache[topic_name], 
        reader_qos
    );
    if (reader == nullptr) 
    {
        throw std::runtime_error("Failed to create DataReader for Topic: " + topic_name);
    }

    std::shared_ptr<DDSReaderBase> suber;
    if (dds_attr->attr == nullptr) 
    {
        suber = std::make_shared<DDSReaderBase>();
    } 
    else 
    {
        suber = std::shared_ptr<DDSReaderBase>(static_cast<DDSReaderBase*>(dds_attr->attr));
    }
    suber->set_reader(reader, dds_attr->is_fresh);

    LOG_Info << "Created DataReader for Topic: " << topic_name;
    return suber;
}


PUPPET_MASTER_COMMUNICATION_NS_END