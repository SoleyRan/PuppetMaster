#ifndef DDS_READER_H
#define DDS_READER_H

#include <fastdds/dds/subscriber/DataReader.hpp>
#include <fastdds/dds/subscriber/DataReaderListener.hpp>
#include <fastdds/dds/subscriber/SampleInfo.hpp>
#include <fastdds/dds/subscriber/Subscriber.hpp>
#include <fastdds/dds/subscriber/qos/DataReaderQos.hpp>

#include <base/base_communication/reader_base.h>
#include <utils/logger/log.hpp>

PUPPET_MASTER_COMMUNICATION_NS_BEGIN

class ReaderListener : public eprosima::fastdds::dds::DataReaderListener
{
public:
    ReaderListener() = default;
    ~ReaderListener() = default;

    void SetCallBack(const std::function<void()> &func);

    uint32_t CurrentMatched() const;

public:
    void on_subscription_matched(eprosima::fastdds::dds::DataReader *reader, const eprosima::fastdds::dds::SubscriptionMatchedStatus &info) override;

private:
    void on_data_available(eprosima::fastdds::dds::DataReader *reader) override;

private:
    std::vector<std::function<void()>> v_func_;
    uint32_t matched_{0};
};

using puppet_master::base::ReaderBase;

class DDSReaderBase : public ReaderBase
{
public:
    DDSReaderBase() = default;
    ~DDSReaderBase();
    DDSReaderBase(const WriterBase&) = delete;
    DDSReaderBase& operator=(const DDSReaderBase&) = delete;
    DDSReaderBase(DDSReaderBase&&) = delete;
    DDSReaderBase& operator=(DDSReaderBase&&) = delete;

public:
    void SetCallBack(const std::function<void()> &_func) override;
    int Read(void *data, int64_t& time_diff) override;

private:
    eprosima::fastdds::dds::DataReader* reader_ = nullptr;
    eprosima::fastdds::dds::DataReaderListener *listener_ = nullptr;
};


template<typename T>
class DDSReader : public DDSReaderBase
{
public:
    int Read(void* data, int64_t& time_diff) override 
    {
        FASTDDS_SEQUENCE(DataSeq, T);
        DataSeq _data;
        eprosima::fastdds::dds::SampleInfoSeq infos;

        // 初始化时间差（默认无数据的情况）
        auto calc_time_diff = [&]() {
            auto dif_temp = static_cast<int64_t>(TimeStamp::MillisecondsSinceEpoch()) - last_time_;
            time_diff = (dif_temp < 0 ? 0 : dif_temp);
        };

        // ========== 核心分支：根据is_fresh_选择读取策略 ==========
        eprosima::fastdds::dds::ReturnCode_t ret;
        if (is_fresh_) {
            // 策略1：is_fresh=true → 读取所有未读数据，只保留最新的1条
            ret = reader_->take(_data, infos, eprosima::fastdds::dds::LENGTH_UNLIMITED);
        } else {
            // 策略2：is_fresh=false → 只读取1条未读数据，不保证最新（原有逻辑）
            ret = reader_->take(_data, infos, 1);
        }

        // 读取失败（无数据）：计算时间差，返回-1
        if (ret != eprosima::fastdds::dds::RETCODE_OK) 
        {
            calc_time_diff();
            return -1;
        }

        // 初始化：默认无有效数据
        bool has_valid_data = false;
        T latest_data;
        int64_t latest_source_ts = 0;
        int64_t latest_reception_ts = 0;

        if (is_fresh_) {
            // ========== is_fresh=true：遍历所有数据，只保留最新的有效数据 ==========
            for (size_t i = 0; i < infos.length(); ++i) {
                if (infos[i].valid_data) {
                    // 覆盖为最新数据（缓存是时间倒序，最后一条/任意覆盖都可，这里直接覆盖）
                    latest_data = _data[i];
                    latest_source_ts = static_cast<int64_t>(infos[i].source_timestamp.to_ns() * 1e-6);
                    latest_reception_ts = static_cast<int64_t>(infos[i].reception_timestamp.to_ns() * 1e-6);
                    has_valid_data = true;
                }
            }
        } else {
            // ========== is_fresh=false：原有逻辑，只检查1条数据 ==========
            if (infos.length() > 0 && infos[0].valid_data) {
                latest_data = _data[0];
                latest_source_ts = static_cast<int64_t>(infos[0].source_timestamp.to_ns() * 1e-6);
                latest_reception_ts = static_cast<int64_t>(infos[0].reception_timestamp.to_ns() * 1e-6);
                has_valid_data = true;
            }
        }

        // ========== 关键：无论哪种策略，都要归还借出的内存 ==========
        if (reader_->return_loan(_data, infos) != eprosima::fastdds::dds::RETCODE_OK) {
            LOG_Warn() << "[error] return_loan failed for topic: " << reader_->get_topic()->get_name();
        }

        // ========== 处理最终结果 ==========
        if (has_valid_data) {
            // 拷贝最新数据到用户缓冲区
            *static_cast<T*>(data) = latest_data;
            last_time_ = latest_source_ts;
            // 计算时间差
            calc_time_diff();
            // 日志输出
            LOG_Trace() << "source_timestamp: " << latest_source_ts 
                      << ", reception_timestamp: " << latest_reception_ts
                      << ", is_fresh: " << (is_fresh_ ? "true" : "false");
            return 0;
        } else {
            // 无有效数据
            calc_time_diff();
            return -1;
        }
    }
};


PUPPET_MASTER_COMMUNICATION_NS_END

#endif