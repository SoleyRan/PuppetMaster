#include <puppet_master/transport/fastdds/fastdds_transport.h>

#include "byte_payload_type.h"

#include <puppet_master/runtime/context.h>

#include <fastdds/dds/domain/DomainParticipant.hpp>
#include <fastdds/dds/domain/DomainParticipantFactory.hpp>
#include <fastdds/dds/domain/qos/DomainParticipantQos.hpp>
#include <fastdds/dds/publisher/DataWriter.hpp>
#include <fastdds/dds/publisher/Publisher.hpp>
#include <fastdds/dds/publisher/qos/DataWriterQos.hpp>
#include <fastdds/dds/subscriber/DataReader.hpp>
#include <fastdds/dds/subscriber/DataReaderListener.hpp>
#include <fastdds/dds/subscriber/SampleInfo.hpp>
#include <fastdds/dds/subscriber/Subscriber.hpp>
#include <fastdds/dds/subscriber/qos/DataReaderQos.hpp>
#include <fastdds/dds/topic/Topic.hpp>
#include <fastdds/dds/topic/TypeSupport.hpp>
#if PUPPETMASTER_FASTDDS_V3
#include <fastdds/rtps/transport/UDPv4TransportDescriptor.hpp>
#include <fastdds/rtps/transport/shared_mem/SharedMemTransportDescriptor.hpp>
#else
#include <fastdds/rtps/transport/UDPv4TransportDescriptor.h>
#include <fastdds/rtps/transport/shared_mem/SharedMemTransportDescriptor.h>
#endif

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <puppet_master/transport/fastdds/policy_mapping.h>

namespace puppet_master::transport::fastdds {

namespace dds = detail::dds;
namespace rtps = detail::dds_rtps;

namespace {

#if PUPPETMASTER_FASTDDS_V3
constexpr dds::ReturnCode_t kRetOk = dds::RETCODE_OK;
#else
const eprosima::fastrtps::types::ReturnCode_t kRetOk =
    eprosima::fastrtps::types::ReturnCode_t::RETCODE_OK;
#endif

core::TimePoint Now()
{
    return std::chrono::time_point_cast<core::Nanoseconds>(core::SteadyClock::now());
}

bool IsSameDescriptor(const MessageDescriptor& lhs, const MessageDescriptor& rhs)
{
    return lhs.type_name == rhs.type_name && lhs.encoding == rhs.encoding;
}

void ApplyEndpointQos(const QosProfile& profile,
                      dds::ReliabilityQosPolicy& reliability,
                      dds::HistoryQosPolicy& history,
                      dds::DurabilityQosPolicy& durability)
{
    reliability.kind = profile.reliability == ReliabilityKind::kReliable
        ? dds::RELIABLE_RELIABILITY_QOS
        : dds::BEST_EFFORT_RELIABILITY_QOS;

    if (profile.history == HistoryKind::kKeepAll) {
        history.kind = dds::KEEP_ALL_HISTORY_QOS;
    } else {
        history.kind = dds::KEEP_LAST_HISTORY_QOS;
        history.depth = static_cast<std::int32_t>(profile.history_depth);
    }

    durability.kind = profile.durability == DurabilityKind::kTransientLocal
        ? dds::TRANSIENT_LOCAL_DURABILITY_QOS
        : dds::VOLATILE_DURABILITY_QOS;
}

// State shared between a FastDdsReader handle, its DDS listener and the
// owning transport. Two locks keep the listener thread away from DDS calls:
// signal_mutex guards notification state only, entity_mutex guards the
// DataReader pointer and every DDS call made through it.
struct ReaderState {
    class Listener final : public dds::DataReaderListener {
    public:
        explicit Listener(ReaderState& state) : state_(state) {}

        void on_data_available(dds::DataReader* /*reader*/) override
        {
            DataAvailableCallback callback;
            {
                std::lock_guard<std::mutex> lock(state_.signal_mutex);
                ++state_.generation;
                callback = state_.callback;
            }
            state_.cv.notify_all();
            if (callback) {
                callback();
            }
        }

    private:
        ReaderState& state_;
    };

    ReaderState() : listener(*this) {}

    std::mutex signal_mutex;
    std::condition_variable cv;
    DataAvailableCallback callback;
    std::uint64_t generation {0};
    bool closed {false};

    std::mutex entity_mutex;
    dds::Subscriber* subscriber {nullptr};
    dds::DataReader* reader {nullptr};
    std::uint64_t sequence {0};

    Listener listener;

    void Shutdown() noexcept
    {
        {
            std::lock_guard<std::mutex> lock(signal_mutex);
            closed = true;
        }
        cv.notify_all();

        std::lock_guard<std::mutex> lock(entity_mutex);
        if (reader != nullptr && subscriber != nullptr) {
            subscriber->delete_datareader(reader);
        }
        reader = nullptr;
        subscriber = nullptr;
    }
};

struct WriterState {
    std::mutex mutex;
    dds::Publisher* publisher {nullptr};
    dds::DataWriter* writer {nullptr};

    void Shutdown() noexcept
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (writer != nullptr && publisher != nullptr) {
            publisher->delete_datawriter(writer);
        }
        writer = nullptr;
        publisher = nullptr;
    }
};

class FastDdsReader final : public Reader {
public:
    FastDdsReader(EndpointConfig endpoint, std::shared_ptr<ReaderState> state)
        : endpoint_(std::move(endpoint)), state_(std::move(state))
    {
    }

    ~FastDdsReader() override
    {
        state_->Shutdown();
    }

    const core::TopicName& topic_name() const noexcept override
    {
        return endpoint_.topic.name;
    }

    const MessageDescriptor& message_descriptor() const noexcept override
    {
        return endpoint_.message;
    }

    core::Result<Message> Read(ReadOptions options) override
    {
        const auto deadline = Now() + options.timeout;
        while (true) {
            std::uint64_t observed_generation = 0;
            {
                std::lock_guard<std::mutex> lock(state_->signal_mutex);
                if (state_->closed) {
                    return core::Result<Message>::FromStatus(
                        core::Status::Unavailable("FastDDS reader is closed"));
                }
                observed_generation = state_->generation;
            }

            auto taken = TryTake();
            if (taken.ok() || taken.status().code() != core::StatusCode::kUnavailable || !options.wait) {
                return taken;
            }

            std::unique_lock<std::mutex> lock(state_->signal_mutex);
            const auto ready = [&] { return state_->closed || state_->generation != observed_generation; };
            if (options.timeout > core::Nanoseconds::zero()) {
                if (!state_->cv.wait_until(lock, deadline, ready)) {
                    return core::Result<Message>::FromStatus(
                        core::Status::DeadlineExceeded("timed out waiting for FastDDS message"));
                }
            } else {
                state_->cv.wait(lock, ready);
            }
        }
    }

    core::Status SetDataAvailableCallback(DataAvailableCallback callback) override
    {
        std::lock_guard<std::mutex> lock(state_->signal_mutex);
        state_->callback = std::move(callback);
        return core::Status::Ok();
    }

    core::Result<std::size_t> PendingMessageCount() const override
    {
        std::lock_guard<std::mutex> lock(state_->entity_mutex);
        if (state_->reader == nullptr) {
            return core::Result<std::size_t>::FromStatus(
                core::Status::Unavailable("FastDDS reader is closed"));
        }
        return core::Result<std::size_t>(static_cast<std::size_t>(state_->reader->get_unread_count()));
    }

private:
    core::Result<Message> TryTake()
    {
        std::lock_guard<std::mutex> lock(state_->entity_mutex);
        if (state_->reader == nullptr) {
            return core::Result<Message>::FromStatus(core::Status::Unavailable("FastDDS reader is closed"));
        }

        detail::BytePayload sample;
        dds::SampleInfo info;
        // Skip metadata-only samples (dispose / unregister notifications).
        while (state_->reader->take_next_sample(&sample, &info) == kRetOk) {
            if (!info.valid_data) {
                continue;
            }
            Message message;
            message.payload = std::move(sample);
            message.metadata.sequence = ++state_->sequence;
            message.metadata.reception_timestamp = Now();
            // Steady-clock source timestamps are process local and cannot be
            // carried across participants, so reception time is reported.
            message.metadata.source_timestamp = message.metadata.reception_timestamp;
            return core::Result<Message>(std::move(message));
        }

        return core::Result<Message>::FromStatus(core::Status::Unavailable("no FastDDS message available"));
    }

    EndpointConfig endpoint_;
    std::shared_ptr<ReaderState> state_;
};

class FastDdsWriter final : public Writer {
public:
    FastDdsWriter(EndpointConfig endpoint, std::shared_ptr<WriterState> state)
        : endpoint_(std::move(endpoint)), state_(std::move(state))
    {
    }

    ~FastDdsWriter() override
    {
        state_->Shutdown();
    }

    const core::TopicName& topic_name() const noexcept override
    {
        return endpoint_.topic.name;
    }

    const MessageDescriptor& message_descriptor() const noexcept override
    {
        return endpoint_.message;
    }

    core::Status Write(ByteView payload, WriteOptions /*options*/) override
    {
        auto status = payload.Validate();
        if (!status.ok()) {
            return status;
        }

        detail::BytePayload sample = CopyBytes(payload);

        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->writer == nullptr) {
            return core::Status::Unavailable("FastDDS writer is closed");
        }
#if PUPPETMASTER_FASTDDS_V3
        const bool written = state_->writer->write(&sample) == kRetOk;
#else
        const bool written = state_->writer->write(&sample);
#endif
        if (!written) {
            return core::Status::Unavailable("FastDDS DataWriter rejected the sample");
        }
        return core::Status::Ok();
    }

private:
    EndpointConfig endpoint_;
    std::shared_ptr<WriterState> state_;
};

}  // namespace

struct FastDdsTransport::Impl {
    struct TopicEntry {
        dds::Topic* topic {nullptr};
        MessageDescriptor message;
    };

    dds::DomainParticipant* participant {nullptr};
    dds::Publisher* publisher {nullptr};
    dds::Subscriber* subscriber {nullptr};

    std::mutex mutex;
    std::map<std::string, TopicEntry> topics;
    std::set<std::string> registered_types;
    std::vector<std::weak_ptr<ReaderState>> readers;
    std::vector<std::weak_ptr<WriterState>> writers;

    // Caller must hold mutex.
    core::Result<dds::Topic*> EnsureTopic(const EndpointConfig& endpoint)
    {
        const std::string topic_name = endpoint.topic.name.str();
        const std::string& type_name = endpoint.message.type_name;

        auto found = topics.find(topic_name);
        if (found != topics.end()) {
            if (!IsSameDescriptor(found->second.message, endpoint.message)) {
                return core::Result<dds::Topic*>::FromStatus(core::Status::InvalidArgument(
                    "topic already exists with a different message descriptor"));
            }
            return core::Result<dds::Topic*>(found->second.topic);
        }

        if (registered_types.count(type_name) == 0) {
            dds::TypeSupport type(new detail::BytePayloadType(type_name));
            if (type.register_type(participant) != kRetOk) {
                return core::Result<dds::Topic*>::FromStatus(
                    core::Status::Unavailable("failed to register FastDDS type " + type_name));
            }
            registered_types.insert(type_name);
        }

        dds::Topic* topic = participant->create_topic(topic_name, type_name, dds::TOPIC_QOS_DEFAULT);
        if (topic == nullptr) {
            return core::Result<dds::Topic*>::FromStatus(
                core::Status::Unavailable("failed to create FastDDS topic " + topic_name));
        }
        topics.emplace(topic_name, TopicEntry {topic, endpoint.message});
        return core::Result<dds::Topic*>(topic);
    }

    template <typename State>
    static void Prune(std::vector<std::weak_ptr<State>>& states)
    {
        states.erase(std::remove_if(states.begin(), states.end(),
                                    [](const std::weak_ptr<State>& state) { return state.expired(); }),
                     states.end());
    }
};

namespace {

void ConfigureTransports(const Options& options, dds::DomainParticipantQos& qos)
{
    if (options.transport_mode == TransportMode::kDefault) {
        qos.transport().use_builtin_transports = true;
        return;
    }

    qos.transport().use_builtin_transports = false;
    if (options.transport_mode == TransportMode::kUdp || options.transport_mode == TransportMode::kHybrid) {
        auto udp = std::make_shared<eprosima::fastdds::rtps::UDPv4TransportDescriptor>();
        udp->sendBufferSize = static_cast<std::uint32_t>(options.udp_buffer_size);
        udp->receiveBufferSize = static_cast<std::uint32_t>(options.udp_buffer_size);
        qos.transport().user_transports.push_back(udp);
    }
    if (options.transport_mode == TransportMode::kSharedMemory ||
        options.transport_mode == TransportMode::kHybrid) {
        auto shm = std::make_shared<eprosima::fastdds::rtps::SharedMemTransportDescriptor>();
        shm->segment_size(static_cast<std::uint32_t>(options.shm_segment_size));
        qos.transport().user_transports.push_back(shm);
    }
}

}  // namespace

core::Status RegisterTransport(
    runtime::RuntimeContext& runtime,
    core::TransportName name,
    Options options)
{
    auto status = options.Validate();
    if (!status.ok()) {
        return status;
    }

    return runtime.RegisterTransport(
        std::make_shared<FastDdsTransport>(std::move(name), std::move(options)));
}

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
    TransportCapabilities caps;
    caps.kind = core::TransportKind::kFastDds;
    caps.supports_callbacks = true;
    caps.supports_blocking_read = true;
    caps.supports_reliable_delivery = true;
    caps.supports_keep_all = true;
    // Byte payloads are unbounded, so FastDDS data-sharing never engages.
    caps.supports_zero_copy = false;
    return caps;
}

core::Status FastDdsTransport::Open()
{
    auto status = options_.Validate();
    if (!status.ok()) {
        return status;
    }
    if (impl_->participant != nullptr) {
        return core::Status::Ok();
    }

    dds::DomainParticipantQos qos = dds::PARTICIPANT_QOS_DEFAULT;
    qos.name(options_.participant_name);
    ConfigureTransports(options_, qos);

    impl_->participant =
        dds::DomainParticipantFactory::get_instance()->create_participant(options_.domain_id, qos);
    if (impl_->participant == nullptr) {
        return core::Status::Unavailable("failed to create FastDDS DomainParticipant");
    }
    impl_->publisher = impl_->participant->create_publisher(dds::PUBLISHER_QOS_DEFAULT);
    impl_->subscriber = impl_->participant->create_subscriber(dds::SUBSCRIBER_QOS_DEFAULT);
    if (impl_->publisher == nullptr || impl_->subscriber == nullptr) {
        Close();
        return core::Status::Unavailable("failed to create FastDDS publisher/subscriber");
    }
    return core::Status::Ok();
}

core::Status FastDdsTransport::Close() noexcept
{
    if (impl_->participant == nullptr) {
        return core::Status::Ok();
    }

    std::vector<std::weak_ptr<ReaderState>> readers;
    std::vector<std::weak_ptr<WriterState>> writers;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        readers.swap(impl_->readers);
        writers.swap(impl_->writers);
    }
    // Endpoint handles may outlive the transport; detach them from DDS first.
    for (auto& weak : readers) {
        if (auto state = weak.lock()) {
            state->Shutdown();
        }
    }
    for (auto& weak : writers) {
        if (auto state = weak.lock()) {
            state->Shutdown();
        }
    }

    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        for (auto& entry : impl_->topics) {
            impl_->participant->delete_topic(entry.second.topic);
        }
        impl_->topics.clear();
        impl_->registered_types.clear();
    }

    if (impl_->publisher != nullptr) {
        impl_->participant->delete_publisher(impl_->publisher);
    }
    if (impl_->subscriber != nullptr) {
        impl_->participant->delete_subscriber(impl_->subscriber);
    }
    dds::DomainParticipantFactory::get_instance()->delete_participant(impl_->participant);
    impl_->publisher = nullptr;
    impl_->subscriber = nullptr;
    impl_->participant = nullptr;
    return core::Status::Ok();
}

bool FastDdsTransport::is_open() const noexcept
{
    return impl_->participant != nullptr;
}

core::Status FastDdsTransport::ValidateEndpoint(const EndpointConfig& endpoint) const
{
    auto status = endpoint.Validate();
    if (!status.ok()) {
        return status;
    }
    if (endpoint.topic.transport != core::TransportKind::kFastDds) {
        return core::Status::InvalidArgument("endpoint topic is not bound to the FastDDS transport");
    }
    return MapMessagePolicy(endpoint.topic.message_policy, options_.durability).status();
}

core::Result<ReaderPtr> FastDdsTransport::CreateReader(const EndpointConfig& endpoint)
{
    auto status = ValidateEndpoint(endpoint);
    if (!status.ok()) {
        return core::Result<ReaderPtr>::FromStatus(status);
    }
    if (!is_open()) {
        return core::Result<ReaderPtr>::FromStatus(
            core::Status::FailedPrecondition("FastDDS transport must be open before creating readers"));
    }
    const QosProfile profile = MapMessagePolicy(endpoint.topic.message_policy, options_.durability).value();

    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto topic = impl_->EnsureTopic(endpoint);
    if (!topic.ok()) {
        return core::Result<ReaderPtr>::FromStatus(topic.status());
    }

    dds::DataReaderQos qos = dds::DATAREADER_QOS_DEFAULT;
    ApplyEndpointQos(profile, qos.reliability(), qos.history(), qos.durability());
    qos.endpoint().history_memory_policy = rtps::PREALLOCATED_WITH_REALLOC_MEMORY_MODE;

    auto state = std::make_shared<ReaderState>();
    state->subscriber = impl_->subscriber;
    state->reader = impl_->subscriber->create_datareader(topic.value(), qos, &state->listener);
    if (state->reader == nullptr) {
        return core::Result<ReaderPtr>::FromStatus(
            core::Status::Unavailable("failed to create FastDDS DataReader for " + endpoint.topic.name.str()));
    }

    Impl::Prune(impl_->readers);
    impl_->readers.push_back(state);
    return core::Result<ReaderPtr>(std::make_shared<FastDdsReader>(endpoint, std::move(state)));
}

core::Result<WriterPtr> FastDdsTransport::CreateWriter(const EndpointConfig& endpoint)
{
    auto status = ValidateEndpoint(endpoint);
    if (!status.ok()) {
        return core::Result<WriterPtr>::FromStatus(status);
    }
    if (!is_open()) {
        return core::Result<WriterPtr>::FromStatus(
            core::Status::FailedPrecondition("FastDDS transport must be open before creating writers"));
    }
    const QosProfile profile = MapMessagePolicy(endpoint.topic.message_policy, options_.durability).value();

    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto topic = impl_->EnsureTopic(endpoint);
    if (!topic.ok()) {
        return core::Result<WriterPtr>::FromStatus(topic.status());
    }

    dds::DataWriterQos qos = dds::DATAWRITER_QOS_DEFAULT;
    ApplyEndpointQos(profile, qos.reliability(), qos.history(), qos.durability());
    qos.endpoint().history_memory_policy = rtps::PREALLOCATED_WITH_REALLOC_MEMORY_MODE;
    if (options_.async_publish) {
        qos.publish_mode().kind = dds::ASYNCHRONOUS_PUBLISH_MODE;
    }

    auto state = std::make_shared<WriterState>();
    state->publisher = impl_->publisher;
    state->writer = impl_->publisher->create_datawriter(topic.value(), qos);
    if (state->writer == nullptr) {
        return core::Result<WriterPtr>::FromStatus(
            core::Status::Unavailable("failed to create FastDDS DataWriter for " + endpoint.topic.name.str()));
    }

    Impl::Prune(impl_->writers);
    impl_->writers.push_back(state);
    return core::Result<WriterPtr>(std::make_shared<FastDdsWriter>(endpoint, std::move(state)));
}

const Options& FastDdsTransport::options() const noexcept
{
    return options_;
}

}  // namespace puppet_master::transport::fastdds
