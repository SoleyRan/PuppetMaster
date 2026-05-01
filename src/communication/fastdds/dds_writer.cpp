#include "dds_writer.h"

PUPPET_MASTER_COMMUNICATION_NS_BEGIN

// ========================WriterListener========================
void WriterListener::on_publication_matched(
    eprosima::fastdds::dds::DataWriter *writer, const eprosima::fastdds::dds::PublicationMatchedStatus &info)
{
    if (info.current_count_change == 1) 
    {
        LOG_Info() << "Matched! Current matched count: " << info.current_count << " [ Topic: " << writer->get_topic()->get_name() << " ]";
        matched_ = info.current_count;
    } 
    else if (info.current_count_change == -1) 
    {
        LOG_Warn() << "UnMatched! Current matched count: " << info.current_count << " [ Topic: " << writer->get_topic()->get_name() << " ]";
        matched_ = info.current_count;
    } 
    else 
    {
        LOG_Error() <<  "A fatal match error.";
        matched_ = -1;
    }
}

void WriterListener::on_offered_incompatible_qos(
    eprosima::fastdds::dds::DataWriter *writer, const eprosima::fastdds::dds::OfferedIncompatibleQosStatus &status)
{
    (void)status;
    LOG_Warn() << "Incompatible QoS. [ Topic: " << writer->get_topic()->get_name() << " ]";
}

uint32_t WriterListener::CurrentMatched() const
{
    return matched_;
}

// ========================DDSWriter========================
DDSWriter::~DDSWriter()
{
    if(writer_ != nullptr)
    {
        writer_->close();
        delete writer;
    }
    if(listener_ != nullptr)
        delete listener_;
}

void DDSWriter::SetWriter(eprosima::fastdds::dds::DataWriter *writer)
{
    writer_ = writer;
    listener_ = new WriterListener();
    writer->set_listener(listener);
    writer->enable();
    void* sample = nullptr;
    auto ret = writer_->loan_sample(sample);
    if (eprosima::fastdds::dds::RETCODE_OK == ret)
    {
       is_zero_copy_ = true;
       if (writer->discard_loan(sample) != eprosima::fastdds::dds::RETCODE_OK)
       {
            LOG_Warn() << "Discard loan error" ;
       }
       LOG_Info() << "Topic : "<< writer->get_topic()->get_name()<<", will use zero copy" ;
    }
    else
    {
        is_zero_copy_ = false;
    }
}

int DDSWriter::Write(void *data, size_t len) 
{
    void* sample = nullptr;
    eprosima::fastdds::dds::ReturnCode_t ret;
    bool loaned = false; 
    if (is_zero_copy_) 
    {
        ret = writer_->loan_sample(sample);
        loaned = (puppet_likely(ret == eprosima::fastdds::dds::RETCODE_OK));
        if (!loaned) 
        {
            LOG_Warn() << "Loan sample error " << ret ;
            return -1;
        }
        size_t copy_size = len > 0 ? len : writer_->get_type()->max_serialized_type_size;
        memcpy(sample, data, copy_size);
    } 
    else 
    {
        sample = data; 
    }

    ret = writer_->write(sample);
    if (puppet_unlikely(ret != eprosima::fastdds::dds::RETCODE_OK))
    {
        LOG_Warn() << "Write error" << ret ;
        // 关键：若write失败（未成功发送），必须discard_loan释放样本
        if (loaned) 
        {
            writer_->discard_loan(sample);
        }
        return -ret;
    }

    // write成功：中间件自动回收样本，无需手动释放！
    return 0;
}

PUPPET_MASTER_COMMUNICATION_NS_END
