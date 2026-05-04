#include "dds_reader.h"

PUPPET_MASTER_COMMUNICATION_NS_BEGIN

// ========================ReaderListener========================
void ReaderListener::SetCallBack(const std::function<void()> &func)
{
    v_func_.emplace_back(func);
}

void ReaderListener::on_subscription_matched(
    eprosima::fastdds::dds::DataReader *reader, const eprosima::fastdds::dds::SubscriptionMatchedStatus &info)
{
    if (info.current_count_change == 1) {
        LOG_Info()<<"Current matched count: " << info.current_count << " [ Topic: " << reader->get_topicdescription()->get_name() << " ]";
        matched_ = info.current_count;
    } else if (info.current_count_change == -1) {
        LOG_Warn()<<"Unmatched: current matched count: " << info.current_count << " [ Topic: " << reader->get_topicdescription()->get_name() << " ]";
        matched_ = info.current_count;
    } else {
        LOG_Error() << "Match error!";
        matched_ = -1;
    }
}

uint32_t ReaderListener::CurrentMatched() const
{
    return matched_;
}

void ReaderListener::on_data_available(eprosima::fastdds::dds::DataReader *reader)
{
    LOG_Trace() << "recv topic name: " << reader->get_topicdescription()->get_name();
    for (auto &x: v_func_) 
    {
        x();
    }
}

// ========================DDSReaderBase========================
DDSReaderBase::~DDSReaderBase()
{
    if (reader_ != nullptr) 
    {
        reader_->close();
        delete reader_;
    }
    if (listener_ != nullptr) 
    {
        delete listener_;
    }
}

void DDSReaderBase::SetCallBack(const std::function<void()> &_func) 
{
    listener_->SetCallBack(_func);

}

void DDSReaderBase::SetReader(eprosima::fastdds::dds::DataReader* reader, bool is_fresh)
{
    is_fresh_ = is_fresh;
    if (reader == nullptr) {
        throw std::runtime_error("reader is nullptr");
    }
    reader_ = reader;
    listener_ = new ReaderListener();
    reader_->set_listener(listener_);
    reader_->enable();
}



PUPPET_MASTER_COMMUNICATION_NS_END