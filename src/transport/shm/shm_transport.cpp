#include <puppet_master/transport/shm/shm_transport.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <puppet_master/runtime/context.h>

#include "platform.h"
#include "segment.h"

namespace puppet_master::transport::shm {

namespace {

constexpr auto kSegmentInitTimeout = std::chrono::seconds(1);

core::TimePoint Now()
{
    return std::chrono::time_point_cast<core::Nanoseconds>(core::SteadyClock::now());
}

std::uint64_t ReaderDepth(const core::MessagePolicy& policy)
{
    return policy.freshness == core::FreshnessPolicy::kLatest ? 1 : policy.queue_depth;
}

struct ReaderState {
    std::mutex mutex;
    std::condition_variable cv;
    std::uint64_t cursor {1};
    std::uint64_t depth {1};
    std::uint64_t notified_up_to {0};  // newest sequence already announced by callback
    std::uint64_t generation {0};      // bumped on new data, wakes blocking reads
    DataAvailableCallback callback;
    bool closed {false};

    void Close() noexcept
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            closed = true;
            callback = {};
        }
        cv.notify_all();
    }
};

// One topic as seen by this process: the mapped segment, this process's
// participant entry, and the dispatcher that turns notifications into reader
// wakeups and callbacks.
class TopicState final {
public:
    static core::Result<std::shared_ptr<TopicState>> Open(
        core::DomainId domain, const EndpointConfig& endpoint, const TopicOptions& topic_options,
        core::Nanoseconds poll_interval)
    {
        using ResultType = core::Result<std::shared_ptr<TopicState>>;
        auto segment = detail::Segment::Open(
            domain,
            detail::Segment::Identity {endpoint.topic.name.str(), endpoint.message},
            detail::SegmentGeometry {topic_options.slot_payload_size, topic_options.slot_count},
            kSegmentInitTimeout);
        if (!segment.ok()) {
            return ResultType::FromStatus(segment.status());
        }
        auto participant = segment.value()->ClaimParticipant();
        if (!participant.ok()) {
            return ResultType::FromStatus(participant.status());
        }
        auto notifier = detail::Notifier::Open(
            &segment.value()->header().notify_word, segment.value()->name(), participant.value());
        if (!notifier.ok()) {
            segment.value()->ReleaseParticipant(participant.value());
            return ResultType::FromStatus(notifier.status());
        }
        return ResultType(std::shared_ptr<TopicState>(new TopicState(
            endpoint.message, std::move(segment).value(), participant.value(), std::move(notifier).value(),
            poll_interval)));
    }

    ~TopicState()
    {
        Close();
    }

    TopicState(const TopicState&) = delete;
    TopicState& operator=(const TopicState&) = delete;

    const MessageDescriptor& descriptor() const noexcept { return descriptor_; }
    detail::Segment& segment() const noexcept { return *segment_; }
    bool closed() const noexcept { return closed_.load(std::memory_order_acquire); }

    core::Status AddReader(const std::shared_ptr<TopicState>& self, const std::shared_ptr<ReaderState>& reader)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed()) {
            return core::Status::Unavailable("shm topic is closed");
        }
        readers_.push_back(reader);
        if (!dispatcher_.joinable()) {
            dispatcher_ = std::thread(&TopicState::DispatchLoop, std::weak_ptr<TopicState>(self),
                                      segment_->WriteSequence());
        }
        return core::Status::Ok();
    }

    void RemoveReader(const std::shared_ptr<ReaderState>& reader)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        readers_.erase(std::remove(readers_.begin(), readers_.end(), reader), readers_.end());
    }

    core::Status Publish(ByteView payload, WriteOptions options)
    {
        if (closed()) {
            return core::Status::Unavailable("shm writer is closed");
        }
        auto status = payload.Validate();
        if (!status.ok()) {
            return status;
        }
        return Committed(segment_->Publish(payload, Timestamp(options)));
    }

    core::Status PublishWith(std::size_t capacity, const FillFunction& fill, WriteOptions options)
    {
        if (closed()) {
            return core::Status::Unavailable("shm writer is closed");
        }
        if (!fill) {
            return core::Status::InvalidArgument("fill function must not be empty");
        }
        return Committed(segment_->PublishWith(capacity, fill, Timestamp(options)));
    }

    void Close() noexcept
    {
        std::vector<std::shared_ptr<ReaderState>> readers;
        std::thread dispatcher;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closed_.exchange(true, std::memory_order_acq_rel)) {
                return;
            }
            readers.swap(readers_);
            dispatcher.swap(dispatcher_);
        }
        for (const auto& reader : readers) {
            reader->Close();
        }
        if (dispatcher.joinable()) {
            notifier_->NotifyAll({participant_});
            if (dispatcher.get_id() == std::this_thread::get_id()) {
                // Close() on the dispatcher thread (from a callback or the
                // destructor): it cannot join itself. DispatchLoop returns
                // without touching this object once it sees closed().
                dispatcher.detach();
            } else {
                dispatcher.join();
            }
        }
        segment_->ReleaseParticipant(participant_);
    }

private:
    TopicState(MessageDescriptor descriptor, std::shared_ptr<detail::Segment> segment, std::uint32_t participant,
               std::unique_ptr<detail::Notifier> notifier, core::Nanoseconds poll_interval) noexcept
        : descriptor_(std::move(descriptor)),
          segment_(std::move(segment)),
          participant_(participant),
          notifier_(std::move(notifier)),
          poll_interval_(poll_interval)
    {
    }

    static core::TimePoint Timestamp(const WriteOptions& options)
    {
        return options.source_timestamp == core::TimePoint {} ? Now() : options.source_timestamp;
    }

    core::Status Committed(const core::Result<std::uint64_t>& sequence)
    {
        if (!sequence.ok()) {
            return sequence.status();
        }
        notifier_->NotifyAll(segment_->ActiveParticipants());
        return core::Status::Ok();
    }

    // Holds a strong reference only for one iteration. If a reader callback
    // drops the last external reference, the destructor runs here when `self`
    // goes out of scope; Close() then detaches this thread, and the loop
    // returns without touching the object again.
    static void DispatchLoop(std::weak_ptr<TopicState> weak, std::uint64_t last_seen)
    {
        while (true) {
            auto self = weak.lock();
            if (!self || self->closed()) {
                return;
            }
            // Load the counter before the sequence: a publish after this point
            // changes the counter, so the wait below cannot miss it.
            const auto observed = self->notifier_->Load();
            const auto newest = self->segment_->WriteSequence();
            if (newest != last_seen) {
                last_seen = newest;
                self->Fanout(newest);
            }
            if (self->closed()) {
                return;
            }
            self->notifier_->Wait(observed, self->poll_interval_);
        }
    }

    void Fanout(std::uint64_t newest)
    {
        std::vector<std::shared_ptr<ReaderState>> readers;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            readers = readers_;
        }
        for (const auto& reader : readers) {
            DataAvailableCallback callback;
            std::uint64_t count = 0;
            {
                std::lock_guard<std::mutex> lock(reader->mutex);
                if (reader->closed || newest <= reader->notified_up_to) {
                    continue;
                }
                // One callback per new message, capped at what the reader can
                // still read; older messages are already overwritten for it.
                count = std::min(newest - reader->notified_up_to, reader->depth);
                reader->notified_up_to = newest;
                ++reader->generation;
                callback = reader->callback;
            }
            reader->cv.notify_all();
            for (std::uint64_t index = 0; callback && index < count; ++index) {
                try {
                    callback();
                } catch (...) {
                    // A throwing callback must not stop delivery to other readers.
                }
            }
        }
    }

    MessageDescriptor descriptor_;
    std::shared_ptr<detail::Segment> segment_;
    std::uint32_t participant_;
    std::unique_ptr<detail::Notifier> notifier_;
    core::Nanoseconds poll_interval_;

    std::mutex mutex_;
    std::vector<std::shared_ptr<ReaderState>> readers_;
    std::thread dispatcher_;
    std::atomic<bool> closed_ {false};
};

class ShmReader final : public Reader {
public:
    ShmReader(EndpointConfig endpoint, std::shared_ptr<TopicState> topic, std::shared_ptr<ReaderState> state)
        : endpoint_(std::move(endpoint)), topic_(std::move(topic)), state_(std::move(state))
    {
    }

    ~ShmReader() override
    {
        topic_->RemoveReader(state_);
        state_->Close();
    }

    const core::TopicName& topic_name() const noexcept override { return endpoint_.topic.name; }
    const MessageDescriptor& message_descriptor() const noexcept override { return endpoint_.message; }

    core::Result<Message> Read(ReadOptions options) override
    {
        const auto deadline = core::SteadyClock::now() + options.timeout;
        std::unique_lock<std::mutex> lock(state_->mutex);
        while (true) {
            if (state_->closed || topic_->closed()) {
                return core::Result<Message>::FromStatus(core::Status::Unavailable("shm reader is closed"));
            }
            const auto observed_generation = state_->generation;

            Message message;
            const auto outcome = topic_->segment().TryRead(state_->cursor, state_->depth, message);
            if (outcome.has_message) {
                message.metadata.reception_timestamp = Now();
                return core::Result<Message>(std::move(message));
            }
            if (!options.wait) {
                return core::Result<Message>::FromStatus(core::Status::Unavailable("no shm message available"));
            }
            if (topic_->segment().Pending(state_->cursor, state_->depth) != 0) {
                continue;  // raced with a writer; data is there, retry at once
            }

            const auto ready = [&] {
                return state_->closed || topic_->closed() || state_->generation != observed_generation;
            };
            if (options.timeout > core::Nanoseconds::zero()) {
                if (!state_->cv.wait_until(lock, deadline, ready)) {
                    return core::Result<Message>::FromStatus(
                        core::Status::DeadlineExceeded("timed out waiting for shm message"));
                }
            } else {
                state_->cv.wait(lock, ready);
            }
        }
    }

    core::Status SetDataAvailableCallback(DataAvailableCallback callback) override
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->closed || topic_->closed()) {
            return core::Status::Unavailable("shm reader is closed");
        }
        state_->callback = std::move(callback);
        return core::Status::Ok();
    }

    core::Result<std::size_t> PendingMessageCount() const override
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->closed || topic_->closed()) {
            return core::Result<std::size_t>::FromStatus(core::Status::Unavailable("shm reader is closed"));
        }
        return core::Result<std::size_t>(
            static_cast<std::size_t>(topic_->segment().Pending(state_->cursor, state_->depth)));
    }

private:
    EndpointConfig endpoint_;
    std::shared_ptr<TopicState> topic_;
    std::shared_ptr<ReaderState> state_;
};

class ShmWriter final : public Writer {
public:
    ShmWriter(EndpointConfig endpoint, std::shared_ptr<TopicState> topic)
        : endpoint_(std::move(endpoint)), topic_(std::move(topic))
    {
    }

    const core::TopicName& topic_name() const noexcept override { return endpoint_.topic.name; }
    const MessageDescriptor& message_descriptor() const noexcept override { return endpoint_.message; }

    core::Status Write(ByteView payload, WriteOptions options) override
    {
        return topic_->Publish(payload, options);
    }

    // Zero-copy: `fill` writes straight into the shared-memory slot.
    core::Status WriteWith(std::size_t capacity, const FillFunction& fill, WriteOptions options) override
    {
        return topic_->PublishWith(capacity, fill, options);
    }

private:
    EndpointConfig endpoint_;
    std::shared_ptr<TopicState> topic_;
};

}  // namespace

core::Status TopicOptions::Validate() const
{
    if (slot_payload_size == 0) {
        return core::Status::InvalidArgument("shm slot_payload_size must be greater than zero");
    }
    if (slot_count == 0) {
        return core::Status::InvalidArgument("shm slot_count must be greater than zero");
    }
    if (detail::SegmentSize(detail::SegmentGeometry {slot_payload_size, slot_count}) == 0
        || slot_count > std::numeric_limits<std::uint32_t>::max()) {
        return core::Status::InvalidArgument("shm topic geometry is too large");
    }
    return core::Status::Ok();
}

core::Status Options::Validate() const
{
    auto status = default_topic.Validate();
    if (!status.ok()) {
        return status;
    }
    for (const auto& entry : topics) {
        status = entry.second.Validate();
        if (!status.ok()) {
            return core::Status::InvalidArgument(status.message() + " (topic " + entry.first + ")");
        }
    }
    if (wait_poll_interval <= core::Nanoseconds::zero()) {
        return core::Status::InvalidArgument("shm wait_poll_interval must be greater than zero");
    }
    return core::Status::Ok();
}

const TopicOptions& Options::ForTopic(const std::string& topic) const
{
    const auto found = topics.find(topic);
    return found == topics.end() ? default_topic : found->second;
}

core::Status RegisterTransport(runtime::RuntimeContext& runtime, core::TransportName name, Options options)
{
    auto status = options.Validate();
    if (!status.ok()) {
        return status;
    }
    return runtime.RegisterTransport(std::make_shared<ShmTransport>(std::move(name), std::move(options)));
}

core::Status RemoveSegment(core::DomainId domain_id, const std::string& topic)
{
    return detail::SharedRegion::Remove(detail::ObjectName(domain_id, topic));
}

struct ShmTransport::Impl {
    mutable std::mutex mutex;
    bool open {false};
    std::map<std::string, std::shared_ptr<TopicState>> topics;
};

ShmTransport::ShmTransport(core::TransportName name, Options options)
    : name_(std::move(name)), options_(std::move(options)), impl_(std::make_shared<Impl>())
{
}

ShmTransport::~ShmTransport()
{
    Close();
}

const core::TransportName& ShmTransport::name() const noexcept
{
    return name_;
}

core::TransportKind ShmTransport::kind() const noexcept
{
    return core::TransportKind::kIpc;
}

TransportCapabilities ShmTransport::capabilities() const noexcept
{
    TransportCapabilities caps;
    caps.kind = core::TransportKind::kIpc;
    caps.supports_callbacks = true;
    caps.supports_blocking_read = true;
    caps.supports_reliable_delivery = true;
    caps.supports_keep_all = false;
    caps.supports_zero_copy = true;  // Writer::WriteWith fills the slot in place
    return caps;
}

core::Status ShmTransport::Open()
{
    auto status = options_.Validate();
    if (!status.ok()) {
        return status;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->open = true;
    return core::Status::Ok();
}

core::Status ShmTransport::Close() noexcept
{
    std::map<std::string, std::shared_ptr<TopicState>> topics;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->open = false;
        topics.swap(impl_->topics);
    }
    for (auto& entry : topics) {
        entry.second->Close();
    }
    return core::Status::Ok();
}

bool ShmTransport::is_open() const noexcept
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->open;
}

core::Status ShmTransport::ValidateEndpoint(const EndpointConfig& endpoint) const
{
    auto status = endpoint.Validate();
    if (!status.ok()) {
        return status;
    }
    if (endpoint.topic.transport != core::TransportKind::kIpc) {
        return core::Status::InvalidArgument("shm transport requires topic transport kind to be ipc");
    }
    const auto& policy = endpoint.topic.message_policy;
    if (policy.freshness == core::FreshnessPolicy::kQueued && policy.retention == core::RetentionPolicy::kKeepAll) {
        return core::Status::Unsupported("shm transport cannot keep all messages in a fixed ring");
    }
    if (policy.overflow != core::QueueOverflowPolicy::kDropOldest) {
        return core::Status::Unsupported("shm transport supports only drop-oldest overflow");
    }
    const auto& topic_options = options_.ForTopic(endpoint.topic.name.str());
    if (ReaderDepth(policy) > topic_options.slot_count) {
        return core::Status::InvalidArgument(
            "shm queue_depth " + std::to_string(policy.queue_depth) + " exceeds slot_count "
            + std::to_string(topic_options.slot_count) + " for topic " + endpoint.topic.name.str());
    }
    return core::Status::Ok();
}

namespace {

core::Result<std::shared_ptr<TopicState>> GetOrOpenTopic(
    std::mutex& mutex, bool& open, std::map<std::string, std::shared_ptr<TopicState>>& topics,
    const Options& options, const EndpointConfig& endpoint, const char* what)
{
    using ResultType = core::Result<std::shared_ptr<TopicState>>;
    std::lock_guard<std::mutex> lock(mutex);
    if (!open) {
        return ResultType::FromStatus(core::Status::FailedPrecondition(
            std::string("shm transport must be open before creating ") + what));
    }
    const auto& key = endpoint.topic.name.str();
    const auto found = topics.find(key);
    if (found != topics.end()) {
        if (found->second->descriptor() != endpoint.message) {
            return ResultType::FromStatus(core::Status::InvalidArgument(
                "shm topic " + key + " carries " + found->second->descriptor().ToString() + ", not "
                + endpoint.message.ToString()));
        }
        return ResultType(found->second);
    }
    auto topic = TopicState::Open(options.domain_id, endpoint, options.ForTopic(key), options.wait_poll_interval);
    if (!topic.ok()) {
        return topic;
    }
    topics.emplace(key, topic.value());
    return topic;
}

}  // namespace

core::Result<ReaderPtr> ShmTransport::CreateReader(const EndpointConfig& endpoint)
{
    auto status = ValidateEndpoint(endpoint);
    if (!status.ok()) {
        return core::Result<ReaderPtr>::FromStatus(std::move(status));
    }
    auto topic = GetOrOpenTopic(impl_->mutex, impl_->open, impl_->topics, options_, endpoint, "readers");
    if (!topic.ok()) {
        return core::Result<ReaderPtr>::FromStatus(topic.status());
    }

    // Readers are volatile: they start after the newest committed message.
    auto state = std::make_shared<ReaderState>();
    const auto newest = topic.value()->segment().WriteSequence();
    state->cursor = newest + 1;
    state->notified_up_to = newest;
    state->depth = ReaderDepth(endpoint.topic.message_policy);

    status = topic.value()->AddReader(topic.value(), state);
    if (!status.ok()) {
        return core::Result<ReaderPtr>::FromStatus(std::move(status));
    }
    return core::Result<ReaderPtr>(std::make_shared<ShmReader>(endpoint, topic.value(), std::move(state)));
}

core::Result<WriterPtr> ShmTransport::CreateWriter(const EndpointConfig& endpoint)
{
    auto status = ValidateEndpoint(endpoint);
    if (!status.ok()) {
        return core::Result<WriterPtr>::FromStatus(std::move(status));
    }
    auto topic = GetOrOpenTopic(impl_->mutex, impl_->open, impl_->topics, options_, endpoint, "writers");
    if (!topic.ok()) {
        return core::Result<WriterPtr>::FromStatus(topic.status());
    }
    return core::Result<WriterPtr>(std::make_shared<ShmWriter>(endpoint, topic.value()));
}

const Options& ShmTransport::options() const noexcept
{
    return options_;
}
}  // namespace puppet_master::transport::shm
