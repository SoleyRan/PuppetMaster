#ifndef ATTRIBUTES_H
#define ATTRIBUTES_H

#include <iostream>
#include <vector>
#include <string>
#include <fastdds/dds/core/policy/QosPolicies.hpp>
#include <fastdds/rtps/attributes/ResourceManagement.hpp>

enum CommType:uint8_t
{
    ZMQ_COMMTYPE,
    DDS_COMMTYPE,
    IPC_COMMTYPE
};

enum EntityType:uint8_t
{
    READER_EntityType,
    WRITER_EntityType
};

enum TaskType:uint8_t
{
    PERIOD_TRIGGER,
    DATA_TRIGGER,
    TASK_TRIGGER,
    USER_TRIGGER
};

enum TransType:uint8_t
{
    TransType_UDP,
    TransType_SHM,
    TransType_BOTH
};

struct TaskAttribute
{
    timespec period;
    std::vector<std::string> data_depend;
    std::vector<std::string> task_depend;
};

using ReliabilityQosKind = eprosima::fastdds::dds::ReliabilityQosPolicyKind;
using HistoryQosKind = eprosima::fastdds::dds::HistoryQosPolicyKind;
using DurabilityQosKind = eprosima::fastdds::dds::DurabilityQosPolicyKind;

/**
 * @brief FastDDS QoS配置文件结构体
 * @note 封装DDS发布/订阅的核心QoS策略参数，适配传感器数据传输场景
 */
struct QosProfile
{
    // 可靠性策略（默认：尽力而为）
    ReliabilityQosKind reliability = ReliabilityQosKind::BEST_EFFORT_RELIABILITY_QOS;
    // 历史缓存策略（默认：保留最后N个）
    HistoryQosKind history = HistoryQosKind::KEEP_LAST_HISTORY_QOS;
    // 历史缓存深度（仅KEEP_LAST生效，默认：10）
    int history_depth = 10;
    // 持久性策略（默认：易失性，不缓存）
    DurabilityQosKind durability = DurabilityQosKind::VOLATILE_DURABILITY_QOS;
};

struct DDSAttribute
{
    TransType trans_type {TransType_UDP}; // 传输方式
    bool is_fresh {true}; // 是否指取最新的数据
    QosProfile qos_profile; // qos 配置，一般采用默认配置
    void* dds_reader {nullptr}; 
};

#endif