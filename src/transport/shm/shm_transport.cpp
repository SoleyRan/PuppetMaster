#include <puppet_master/transport/shm/shm_transport.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <puppet_master/configuration/configuration.h>
#include <puppet_master/runtime/context.h>
#include <puppet_master/runtime/parameters.h>

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
    // `create`: whether this endpoint may create the segment. Writers of a
    // reader-owned topic may not; they only attach to the reader's.
    static core::Result<std::shared_ptr<TopicState>> Open(
        core::DomainId domain, const EndpointConfig& endpoint, const TopicOptions& topic_options,
        core::Nanoseconds poll_interval, bool create)
    {
        using ResultType = core::Result<std::shared_ptr<TopicState>>;
        auto segment = detail::Segment::Open(
            domain,
            detail::Segment::Identity {endpoint.topic.name.str(), endpoint.message},
            detail::SegmentGeometry {topic_options.slot_payload_size, topic_options.slot_count},
            kSegmentInitTimeout, create);
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
            endpoint.message, endpoint.topic.lifetime, std::move(segment).value(), participant.value(),
            std::move(notifier).value(), poll_interval)));
    }

    ~TopicState()
    {
        Close();
    }

    TopicState(const TopicState&) = delete;
    TopicState& operator=(const TopicState&) = delete;

    const MessageDescriptor& descriptor() const noexcept { return descriptor_; }
    core::TopicLifetime lifetime() const noexcept { return lifetime_; }
    detail::Segment& segment() const noexcept { return *segment_; }
    bool closed() const noexcept { return closed_.load(std::memory_order_acquire); }
    bool reader_owned() const noexcept { return lifetime_ == core::TopicLifetime::kReaderOwned; }
    // A reader-owned topic whose reader has gone; it takes no new endpoints.
    bool gone() const noexcept { return reader_owned() && segment_->retired(); }

    core::Status AddReader(const std::shared_ptr<TopicState>& self, const std::shared_ptr<ReaderState>& reader)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed()) {
            return core::Status::Unavailable("shm topic is closed");
        }
        readers_.push_back(reader);
        had_reader_ = true;
        if (!dispatcher_.joinable()) {
            dispatcher_ = std::thread(&TopicState::DispatchLoop, std::weak_ptr<TopicState>(self),
                                      segment_->WriteSequence());
        }
        return core::Status::Ok();
    }

    // The last reader of a reader-owned topic takes the topic with it, even
    // while writers in this process still hold it: they fail from now on.
    void RemoveReader(const std::shared_ptr<ReaderState>& reader)
    {
        bool last = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            readers_.erase(std::remove(readers_.begin(), readers_.end(), reader), readers_.end());
            last = readers_.empty();
        }
        if (last && reader_owned()) {
            RetireOnce();
        }
    }

    core::Status Publish(ByteView payload, WriteOptions options)
    {
        if (closed()) {
            return core::Status::Unavailable("shm writer is closed");
        }
        if (gone()) {
            return Gone();
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
        if (gone()) {
            return Gone();
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
        bool had_reader = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closed_.exchange(true, std::memory_order_acq_rel)) {
                return;
            }
            readers.swap(readers_);
            dispatcher.swap(dispatcher_);
            had_reader = had_reader_;
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
        if (!reader_owned()) {
            return;
        }
        if (!readers.empty() || had_reader) {
            RetireOnce();  // closed with the transport, not reader by reader
        } else if (!segment_->retired() && !segment_->HasLiveParticipant()) {
            // Only writers here, and every process that was attached has
            // exited: the reader crashed without retiring its topic. The
            // last one out removes the name, so nothing is left behind.
            detail::SharedRegion::Remove(segment_->name());
        }
    }

private:
    TopicState(MessageDescriptor descriptor, core::TopicLifetime lifetime, std::shared_ptr<detail::Segment> segment,
               std::uint32_t participant, std::unique_ptr<detail::Notifier> notifier,
               core::Nanoseconds poll_interval) noexcept
        : descriptor_(std::move(descriptor)),
          lifetime_(lifetime),
          segment_(std::move(segment)),
          participant_(participant),
          notifier_(std::move(notifier)),
          poll_interval_(poll_interval)
    {
    }

    core::Status Gone() const
    {
        return core::Status::NotFound("shm topic in segment " + segment_->name() + " was closed by its reader");
    }

    // Marks the segment retired, so writers attached in other processes stop,
    // wakes them, and removes its name, so none can attach any more.
    void RetireOnce() noexcept
    {
        if (retired_by_me_.exchange(true)) {
            return;
        }
        segment_->Retire();
        notifier_->NotifyAll(segment_->ActiveParticipants());
        detail::SharedRegion::Remove(segment_->name());
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
    core::TopicLifetime lifetime_;
    std::shared_ptr<detail::Segment> segment_;
    std::uint32_t participant_;
    std::unique_ptr<detail::Notifier> notifier_;
    core::Nanoseconds poll_interval_;

    std::mutex mutex_;
    std::vector<std::shared_ptr<ReaderState>> readers_;
    bool had_reader_ {false};  // guarded by mutex_
    std::thread dispatcher_;
    std::atomic<bool> closed_ {false};
    std::atomic<bool> retired_by_me_ {false};
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
    for (const auto& entry : topic_prefixes) {
        status = entry.second.Validate();
        if (!status.ok()) {
            return core::Status::InvalidArgument(status.message() + " (topics under " + entry.first + ")");
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
    if (found != topics.end()) {
        return found->second;
    }
    const TopicOptions* best = &default_topic;
    std::size_t best_length = 0;
    for (const auto& [prefix, options] : topic_prefixes) {
        if (prefix.size() > best_length && topic.compare(0, prefix.size(), prefix) == 0) {
            best = &options;
            best_length = prefix.size();
        }
    }
    return *best;
}

core::Status RegisterTransport(runtime::RuntimeContext& runtime, core::TransportName name, Options options)
{
    auto status = options.Validate();
    if (!status.ok()) {
        return status;
    }
    return runtime.RegisterTransport(std::make_shared<ShmTransport>(std::move(name), std::move(options)));
}

core::Result<Options> OptionsFromProject(const configuration::ProjectConfig& project, Options base)
{
    using ResultType = core::Result<Options>;

    // Each ipc topic with settings, plus the topics of each ipc service with
    // settings, which share the service's: request and presence by name, and
    // the per-client reply topics by the prefix `<service>/reply/`.
    struct Entry {
        std::string label;
        std::vector<std::string> topics;
        const runtime::ParameterMap* parameters;
        std::string prefix;
    };
    std::vector<Entry> entries;
    for (const auto& topic : project.topics) {
        if (topic.transport == core::TransportKind::kIpc && !topic.transport_parameters.empty()) {
            entries.push_back(Entry {"topic " + topic.name, {topic.name}, &topic.transport_parameters, {}});
        }
    }
    for (const auto& service : project.services) {
        if (service.transport == core::TransportKind::kIpc && !service.transport_parameters.empty()) {
            entries.push_back(Entry {"service " + service.name,
                                     {service.name + "/request", service.name + "/reply", service.name + "/presence"},
                                     &service.transport_parameters, service.name + "/reply/"});
        }
    }

    bool domain_set = false;
    for (const auto& entry_info : entries) {
        const auto invalid = [&entry_info](const core::Status& status) {
            return ResultType::FromStatus(core::Status::InvalidArgument(
                "shm settings of " + entry_info.label + ": " + status.message()));
        };
        for (const auto& entry : *entry_info.parameters) {
            if (entry.first != "slot_payload_size" && entry.first != "slot_count" && entry.first != "domain_id") {
                return invalid(core::Status::InvalidArgument("unknown parameter " + entry.first
                                                             + "; expected slot_payload_size, slot_count, or domain_id"));
            }
        }
        const auto& parameters = *entry_info.parameters;
        auto domain = runtime::GetParameter<core::DomainId>(parameters, "domain_id", base.domain_id);
        if (!domain.ok()) {
            return invalid(domain.status());
        }
        if (parameters.count("domain_id") != 0) {
            // base.domain_id holds the first topic's value once domain_set.
            if (domain_set && domain.value() != base.domain_id) {
                return invalid(core::Status::InvalidArgument(
                    "domain_id " + std::to_string(domain.value()) + " differs from " + std::to_string(base.domain_id)
                    + " set on another topic; one transport has one domain"));
            }
            base.domain_id = domain.value();
            domain_set = true;
        }
        if (parameters.count("slot_payload_size") == 0 && parameters.count("slot_count") == 0) {
            continue;
        }
        TopicOptions geometry = base.ForTopic(entry_info.topics.front());
        auto payload = runtime::GetParameter<std::size_t>(parameters, "slot_payload_size", geometry.slot_payload_size);
        auto slots = runtime::GetParameter<std::size_t>(parameters, "slot_count", geometry.slot_count);
        if (!payload.ok()) {
            return invalid(payload.status());
        }
        if (!slots.ok()) {
            return invalid(slots.status());
        }
        geometry.slot_payload_size = payload.value();
        geometry.slot_count = slots.value();
        auto status = geometry.Validate();
        if (!status.ok()) {
            return invalid(status);
        }
        for (const auto& topic : entry_info.topics) {
            base.topics[topic] = geometry;
        }
        if (!entry_info.prefix.empty()) {
            base.topic_prefixes[entry_info.prefix] = geometry;
        }
    }
    auto status = base.Validate();
    if (!status.ok()) {
        return ResultType::FromStatus(std::move(status));
    }
    return ResultType(std::move(base));
}

core::Status RemoveSegment(core::DomainId domain_id, const std::string& topic)
{
    return detail::SharedRegion::Remove(detail::ObjectName(domain_id, topic));
}

const char* ToString(SegmentInfo::State state) noexcept
{
    switch (state) {
        case SegmentInfo::State::kInUse:
            return "in_use";
        case SegmentInfo::State::kUnused:
            return "unused";
        case SegmentInfo::State::kUninitialized:
            return "uninitialized";
        case SegmentInfo::State::kIncompatible:
            return "incompatible";
    }
    return "unknown";
}

namespace {

// "/pm.<domain>.<hash>" -> domain. List() only returns names of that form.
core::DomainId DomainOf(const std::string& object_name)
{
    const auto start = object_name.find('.') + 1;
    const auto end = object_name.find('.', start);
    return static_cast<core::DomainId>(std::stoul(object_name.substr(start, end - start)));
}

SegmentInfo Describe(const std::string& object_name, const detail::SegmentInspection& inspection)
{
    SegmentInfo info;
    info.object_name = object_name;
    info.domain_id = DomainOf(object_name);
    info.size_bytes = inspection.size;
    info.problem = inspection.problem;
    switch (inspection.state) {
        case detail::SegmentInspection::State::kUninitialized:
            info.state = SegmentInfo::State::kUninitialized;
            return info;
        case detail::SegmentInspection::State::kIncompatible:
            info.state = SegmentInfo::State::kIncompatible;
            return info;
        case detail::SegmentInspection::State::kValid:
            break;
    }
    info.topic = inspection.topic;
    info.descriptor = inspection.descriptor;
    info.geometry = TopicOptions {inspection.geometry.slot_payload_size, inspection.geometry.slot_count};
    info.messages_written = inspection.write_sequence;
    for (const auto& participant : inspection.participants) {
        if (detail::IsProcessAlive(participant)) {
            info.live_pids.push_back(participant.pid);
        } else {
            ++info.dead_participants;
        }
    }
    info.state = info.live_pids.empty() ? SegmentInfo::State::kUnused : SegmentInfo::State::kInUse;
    return info;
}

}  // namespace

core::Result<std::vector<SegmentInfo>> ListSegments(std::optional<core::DomainId> domain_id)
{
    using ResultType = core::Result<std::vector<SegmentInfo>>;
    auto names = detail::SharedRegion::List(domain_id);
    if (!names.ok()) {
        return ResultType::FromStatus(names.status());
    }
    std::vector<SegmentInfo> segments;
    for (const auto& name : names.value()) {
        auto inspection = detail::InspectSegment(name);
        if (!inspection.ok()) {
            if (inspection.status().code() == core::StatusCode::kNotFound) {
                continue;  // removed since it was listed
            }
            return ResultType::FromStatus(core::Status::Error(
                inspection.status().code(), name + ": " + inspection.status().message()));
        }
        segments.push_back(Describe(name, inspection.value()));
    }
    return ResultType(std::move(segments));
}

core::Result<std::vector<SegmentInfo>> RemoveUnusedSegments(std::optional<core::DomainId> domain_id)
{
    using ResultType = core::Result<std::vector<SegmentInfo>>;
    auto segments = ListSegments(domain_id);
    if (!segments.ok()) {
        return segments;
    }
    std::vector<SegmentInfo> removed;
    for (auto& segment : segments.value()) {
        if (segment.state == SegmentInfo::State::kInUse) {
            continue;
        }
        auto status = detail::SharedRegion::Remove(segment.object_name);
        if (!status.ok()) {
            return ResultType::FromStatus(
                core::Status::Error(status.code(), segment.object_name + ": " + status.message()));
        }
        removed.push_back(std::move(segment));
    }
    return ResultType(std::move(removed));
}

struct ShmTransport::Impl {
    mutable std::mutex mutex;
    bool open {false};
    // Readers and writers own their topic; the map only finds it, so a topic
    // is unmapped, and its participant entry released, with its last
    // endpoint in this process. Expired entries are swept on each new one.
    std::map<std::string, std::weak_ptr<TopicState>> topics;
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
    std::map<std::string, std::weak_ptr<TopicState>> topics;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->open = false;
        topics.swap(impl_->topics);
    }
    for (auto& entry : topics) {
        if (auto topic = entry.second.lock()) {
            topic->Close();
        }
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

// Writers of a reader-owned topic only attach: they fail with NotFound when
// its reader has not created it, or has closed it.
core::Result<std::shared_ptr<TopicState>> GetOrOpenTopic(
    std::mutex& mutex, bool& open, std::map<std::string, std::weak_ptr<TopicState>>& topics,
    const Options& options, const EndpointConfig& endpoint, bool writer)
{
    const char* what = writer ? "writers" : "readers";
    const bool reader_owned = endpoint.topic.lifetime == core::TopicLifetime::kReaderOwned;
    using ResultType = core::Result<std::shared_ptr<TopicState>>;
    std::lock_guard<std::mutex> lock(mutex);
    if (!open) {
        return ResultType::FromStatus(core::Status::FailedPrecondition(
            std::string("shm transport must be open before creating ") + what));
    }
    for (auto entry = topics.begin(); entry != topics.end();) {
        entry = entry->second.expired() ? topics.erase(entry) : std::next(entry);
    }
    const auto& key = endpoint.topic.name.str();
    const auto found = topics.find(key);
    if (found != topics.end()) {
        auto existing = found->second.lock();
        if (existing) {
            if (existing->lifetime() != endpoint.topic.lifetime) {
                return ResultType::FromStatus(core::Status::InvalidArgument(
                    "shm topic " + key + " is " + core::ToString(existing->lifetime()) + " in this process, not "
                    + core::ToString(endpoint.topic.lifetime)));
            }
            if (existing->descriptor() != endpoint.message) {
                return ResultType::FromStatus(core::Status::InvalidArgument(
                    "shm topic " + key + " carries " + existing->descriptor().ToString() + ", not "
                    + endpoint.message.ToString()));
            }
            if (!existing->gone()) {
                return ResultType(std::move(existing));
            }
            // Retired: the old topic stays with the endpoints that still hold
            // it. A new reader starts a fresh one under the name, and a new
            // writer attaches to that fresh one, or is NotFound.
        }
        topics.erase(found);
    }
    auto topic = TopicState::Open(options.domain_id, endpoint, options.ForTopic(key), options.wait_poll_interval,
                                  !(writer && reader_owned));
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
    auto topic = GetOrOpenTopic(impl_->mutex, impl_->open, impl_->topics, options_, endpoint, false);
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
    auto topic = GetOrOpenTopic(impl_->mutex, impl_->open, impl_->topics, options_, endpoint, true);
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
