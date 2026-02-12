
#ifndef QOS_PROFILE_CONF_H
#define QOS_PROFILE_CONF_H

#include <common/namespace_macros.h>
#include <common/attributes.h>

PUPPET_MASTER_UTILS_NS_BEGIN
/**
 * @brief 创建自定义QoS配置
 * @param reliability 可靠性策略（BEST_EFFORT/RELIABLE）
 * @param history 历史缓存策略（KEEP_LAST/KEEP_ALL）
 * @param history_depth 缓存深度（KEEP_LAST时生效，KEEP_ALL填0）
 * @param durability 持久性策略（VOLATILE/TRANSIENT_LOCAL等）
 * @return 配置完成的QosProfile对象
 */
inline QosProfile CreateQosProfile(
    const ReliabilityQosKind& reliability,
    const HistoryQosKind& history,
    const int history_depth,
    const DurabilityQosKind& durability)
{
    QosProfile profile;
    profile.reliability = reliability;
    profile.history = history;
    profile.history_depth = history_depth;
    profile.durability = durability;
    return profile;
}

// ========================= 预定义QoS配置宏（格式对齐，补充注释） =========================
/**
 * @brief 默认QoS配置：尽力而为、保留最后1个、易失性（通用基础配置）
 */
#define QOS_PROFILE_DEFAULT     CreateQosProfile( \
    ReliabilityQosKind::BEST_EFFORT_RELIABILITY_QOS, \
    HistoryQosKind::KEEP_LAST_HISTORY_QOS, \
    1, \
    DurabilityQosKind::VOLATILE_DURABILITY_QOS)

/**
 * @brief 事件类QoS配置：可靠传输、保留最后5个、易失性（如传感器触发事件）
 */
#define QOS_PROFILE_EVENT       CreateQosProfile( \
    ReliabilityQosKind::RELIABLE_RELIABILITY_QOS, \
    HistoryQosKind::KEEP_LAST_HISTORY_QOS, \
    5, \
    DurabilityQosKind::VOLATILE_DURABILITY_QOS)

/**
 * @brief 本地事件QoS配置：可靠传输、保留最后5个、本地持久化（迟加入读者可获取）
 */
#define QOS_PROFILE_EVENT_LOCAL CreateQosProfile( \
    ReliabilityQosKind::RELIABLE_RELIABILITY_QOS, \
    HistoryQosKind::KEEP_LAST_HISTORY_QOS, \
    5, \
    DurabilityQosKind::TRANSIENT_LOCAL_DURABILITY_QOS)

/**
 * @brief 周期数据QoS配置：尽力而为、保留最后50个、易失性（如常规传感器周期数据）
 */
#define QOS_PROFILE_PERIOD      CreateQosProfile( \
    ReliabilityQosKind::BEST_EFFORT_RELIABILITY_QOS, \
    HistoryQosKind::KEEP_LAST_HISTORY_QOS, \
    50, \
    DurabilityQosKind::VOLATILE_DURABILITY_QOS)

/**
 * @brief 大体积周期数据QoS配置：尽力而为、保留最后1000个、易失性（如点云/图像）
 */
#define QOS_PROFILE_PERIOD_BIG  CreateQosProfile( \
    ReliabilityQosKind::BEST_EFFORT_RELIABILITY_QOS, \
    HistoryQosKind::KEEP_LAST_HISTORY_QOS, \
    1000, \
    DurabilityQosKind::VOLATILE_DURABILITY_QOS)

/**
 * @brief EPU周期数据QoS配置：可靠传输、保留最后1000个、易失性（EPU模块专用）
 */
#define QOS_PROFILE_PERIOD_EPU  CreateQosProfile( \
    ReliabilityQosKind::RELIABLE_RELIABILITY_QOS, \
    HistoryQosKind::KEEP_LAST_HISTORY_QOS, \
    1000, \
    DurabilityQosKind::VOLATILE_DURABILITY_QOS)

/**
 * @brief 全样本保留QoS配置：可靠传输、保留所有样本、本地持久化
 * @note 迟加入的DataReader可获取历史样本；Writer无匹配Reader时写满会阻塞
 */
#define QOS_KEEP_ALL_SAMPLES    CreateQosProfile( \
    ReliabilityQosKind::RELIABLE_RELIABILITY_QOS, \
    HistoryQosKind::KEEP_ALL_HISTORY_QOS, \
    0, \
    DurabilityQosKind::TRANSIENT_LOCAL_DURABILITY_QOS)

/**
 * @brief RPC定制QoS配置：可靠传输、保留所有样本（深度10）、易失性（RPC通信专用）
 */
#define QOS_RPC_CUSTOMIZATION   CreateQosProfile( \
    ReliabilityQosKind::RELIABLE_RELIABILITY_QOS, \
    HistoryQosKind::KEEP_ALL_HISTORY_QOS, \
    10, \
    DurabilityQosKind::VOLATILE_DURABILITY_QOS)

/**
 * @brief ROS2对齐QoS配置：可靠传输、保留最后1个、易失性（兼容ROS2消息格式）
 */
#define QOS_PROFILE_ROSMESSAGE  CreateQosProfile( \
    ReliabilityQosKind::RELIABLE_RELIABILITY_QOS, \
    HistoryQosKind::KEEP_LAST_HISTORY_QOS, \
    1, \
    DurabilityQosKind::VOLATILE_DURABILITY_QOS)

/**
 * @brief 发现机制QoS配置：可靠传输、保留最后1个、本地持久化（DDS发现专用）
 */
#define QOS_PROFILE_DISCOVERY   CreateQosProfile( \
    ReliabilityQosKind::RELIABLE_RELIABILITY_QOS, \
    HistoryQosKind::KEEP_LAST_HISTORY_QOS, \
    1, \
    DurabilityQosKind::TRANSIENT_LOCAL_DURABILITY_QOS)

PUPPET_MASTER_UTILS_NS_END

#endif