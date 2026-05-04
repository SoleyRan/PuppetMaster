#include <puppet_master/transport/fastdds/fastdds_transport.h>

#include <fastdds/dds/domain/DomainParticipant.hpp>
#include <fastdds/dds/domain/DomainParticipantFactory.hpp>
#include <fastdds/dds/domain/qos/DomainParticipantQos.hpp>
#include <fastdds/dds/publisher/Publisher.hpp>
#include <fastdds/dds/subscriber/Subscriber.hpp>
#include <fastdds/rtps/transport/UDPv4TransportDescriptor.hpp>
#include <fastdds/rtps/transport/shared_mem/SharedMemTransportDescriptor.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include <puppet_master/transport/fastdds/policy_mapping.h>

namespace puppet_master::transport::fastdds {

namespace dds = eprosima::fastdds::dds;
namespace rtps = eprosima::fastdds::rtps;

struct FastDdsTransport::Impl {
    dds::DomainParticipant* participant {nullptr};
    dds::Publisher* publisher {nullptr};
    dds::Subscriber* subscriber {nullptr};
};

namespace {

void ConfigureTransports(dds::DomainParticipantQos& qos, const Options& options)
{
    if (options.transport_mode == TransportMode::kDefault) {
        qos.transport().use_builtin_transports = true;
        return;
    }

    qos.transport().use_builtin_transports = false;

    if (options.transport_mode == TransportMode::kUdp || options.transport_mode == TransportMode::kHybrid) {
        auto udp = std::make_shared<rtps::UDPv4TransportDescriptor>();
        udp->sendBufferSize = static_cast<std::uint32_t>(options.udp_buffer_size);
        udp->receiveBufferSize = static_cast<std::uint32_t>(options.udp_buffer_size);
        qos.transport().user_transports.push_back(udp);
    }

    if (options.transport_mode == TransportMode::kSharedMemory
        || options.transport_mode == TransportMode::kHybrid) {
        auto shm = std::make_shared<rtps::SharedMemTransportDescriptor>();
        shm->segment_size(options.shm_segment_size);
        qos.transport().user_transports.push_back(shm);
    }
}

}  // namespace

FastDdsTransport::FastDdsTransport(core::TransportName name, Options options)
    : name_(std::move(name)), options_(std::move(options)), impl_(std::make_unique<Impl>())
{
}

FastDdsTransport::~FastDdsTransport()
{
    Close();
}

const core::TransportName& FastDdsTransport::name() const noexcept
{
    return name_;
}

core::TransportKind FastDdsTransport::kind() const noexcept
{
    return core::TransportKind::kFastDds;
}

TransportCapabilities FastDdsTransport::capabilities() const noexcept
{
    TransportCapabilities capabilities;
    capabilities.kind = kind();
    capabilities.supports_callbacks = true;
    capabilities.supports_blocking_read = false;
    capabilities.supports_reliable_delivery = true;
    capabilities.supports_keep_all = true;
    capabilities.supports_zero_copy = options_.data_sharing;
    return capabilities;
}

core::Status FastDdsTransport::Open()
{
    auto status = options_.Validate();
    if (!status.ok()) {
        return status;
    }

    if (is_open()) {
        return core::Status::Ok();
    }

    dds::DomainParticipantQos participant_qos;
    participant_qos.name(options_.participant_name);
    participant_qos.entity_factory().autoenable_created_entities = true;
    ConfigureTransports(participant_qos, options_);

    impl_->participant = dds::DomainParticipantFactory::get_instance()->create_participant(
        options_.domain_id,
        participant_qos);

    if (impl_->participant == nullptr) {
        return core::Status::Unavailable("failed to create FastDDS DomainParticipant");
    }

    impl_->publisher = impl_->participant->create_publisher(dds::PUBLISHER_QOS_DEFAULT);
    if (impl_->publisher == nullptr) {
        Close();
        return core::Status::Unavailable("failed to create FastDDS Publisher");
    }

    impl_->subscriber = impl_->participant->create_subscriber(dds::SUBSCRIBER_QOS_DEFAULT);
    if (impl_->subscriber == nullptr) {
        Close();
        return core::Status::Unavailable("failed to create FastDDS Subscriber");
    }

    return core::Status::Ok();
}

core::Status FastDdsTransport::Close() noexcept
{
    if (impl_ == nullptr || impl_->participant == nullptr) {
        return core::Status::Ok();
    }

    if (impl_->publisher != nullptr) {
        impl_->participant->delete_publisher(impl_->publisher);
        impl_->publisher = nullptr;
    }

    if (impl_->subscriber != nullptr) {
        impl_->participant->delete_subscriber(impl_->subscriber);
        impl_->subscriber = nullptr;
    }

    dds::DomainParticipantFactory::get_instance()->delete_participant(impl_->participant);
    impl_->participant = nullptr;
    return core::Status::Ok();
}

bool FastDdsTransport::is_open() const noexcept
{
    return impl_ != nullptr && impl_->participant != nullptr;
}

core::Status FastDdsTransport::ValidateEndpoint(const EndpointConfig& endpoint) const
{
    auto status = endpoint.Validate();
    if (!status.ok()) {
        return status;
    }

    auto qos = MapMessagePolicy(endpoint.topic.message_policy, options_.durability);
    if (!qos.ok()) {
        return qos.status();
    }

    return core::Status::Ok();
}

core::Result<ReaderPtr> FastDdsTransport::CreateReader(const EndpointConfig& endpoint)
{
    auto status = ValidateEndpoint(endpoint);
    if (!status.ok()) {
        return core::Result<ReaderPtr>::FromStatus(std::move(status));
    }

    if (!is_open()) {
        return core::Result<ReaderPtr>::FromStatus(
            core::Status::FailedPrecondition("FastDDS transport must be open before creating readers"));
    }

    return core::Result<ReaderPtr>::FromStatus(core::Status::Unsupported(
        "FastDDS typed reader binding is not implemented yet; provide native type support in the next adapter step"));
}

core::Result<WriterPtr> FastDdsTransport::CreateWriter(const EndpointConfig& endpoint)
{
    auto status = ValidateEndpoint(endpoint);
    if (!status.ok()) {
        return core::Result<WriterPtr>::FromStatus(std::move(status));
    }

    if (!is_open()) {
        return core::Result<WriterPtr>::FromStatus(
            core::Status::FailedPrecondition("FastDDS transport must be open before creating writers"));
    }

    return core::Result<WriterPtr>::FromStatus(core::Status::Unsupported(
        "FastDDS typed writer binding is not implemented yet; provide native type support in the next adapter step"));
}

const Options& FastDdsTransport::options() const noexcept
{
    return options_;
}

}  // namespace puppet_master::transport::fastdds
