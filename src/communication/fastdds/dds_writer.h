#ifndef PUB_NODE_H
#define PUB_NODE_H

#include <fastdds/dds/publisher/DataWriter.hpp>
#include <fastdds/dds/publisher/DataWriterListener.hpp>
#include <fastdds/dds/publisher/Publisher.hpp>

#include <base/base_communication/writer_base.h>
#include <puppet_master/logging/log.h>

PUPPET_MASTER_COMMUNICATION_NS_BEGIN

class WriterListener : public eprosima::fastdds::dds::DataWriterListener
{
private:
    uint32_t matched_;

public:
    WriterListener() : matched_(0) {}
    ~WriterListener() = default;

    uint32_t CurrentMatched() const;

public:
    void on_publication_matched(
        eprosima::fastdds::dds::DataWriter *writer, const eprosima::fastdds::dds::PublicationMatchedStatus &info) override;

    void on_offered_incompatible_qos(
        eprosima::fastdds::dds::DataWriter *writer, const eprosima::fastdds::dds::OfferedIncompatibleQosStatus &status) override;

};

using puppet_master::base::WriterBase;

class DDSWriter: public WriterBase
{
public:
    DDSWriter() = default;
    ~DDSWriter();
    DDSWriter(const WriterBase&) = delete;
    DDSWriter& operator=(const DDSWriter&) = delete;
    DDSWriter(DDSWriter&&) = delete;
    DDSWriter& operator=(DDSWriter&&) = delete;

    void SetWriter(eprosima::fastdds::dds::DataWriter* writer);

    int Write(void* data, size_t len = 0) override;

private:
    eprosima::fastdds::dds::DataWriter* writer_ = nullptr;
    eprosima::fastdds::dds::DataWriterListener* listener_ = nullptr;
    bool is_zero_copy_ = false;
};

PUPPET_MASTER_COMMUNICATION_NS_END

#endif
