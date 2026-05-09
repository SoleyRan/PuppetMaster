#include <puppet_master/transport/inmemory/inmemory_transport.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace puppet_master::transport::inmemory {

namespace {

core::TimePoint Now()
{
    return std::chrono::time_point_cast<core::Nanoseconds>(core::SteadyClock::now());
}

bool IsSameDescriptor(const MessageDescriptor& lhs, const MessageDescriptor& rhs)
{
    return lhs.type_name == rhs.type_name && lhs.encoding == rhs.encoding;
}

core::Status ValidateInMemoryEndpoint(const EndpointConfig& endpoint)
{
    auto status = endpoint.Validate();
    if (!status.ok()) {
        return status;
    }

    if (endpoint.topic.transport != core::TransportKind::kInMemory) {
        return core::Status::InvalidArgument("in-memory transport requires topic transport kind to be in_memory");
    }

    return core::Status::Ok();
}

bool UsesUnboundedQueue(const core::MessagePolicy& policy)
{
    return policy.freshness == core::FreshnessPolicy::kQueued
        && policy.retention == core::RetentionPolicy::kKeepAll;
}

std::size_t QueueCapacity(const core::MessagePolicy& policy)
{
    if (policy.freshness == core::FreshnessPolicy::kLatest) {
        return 1;
    }
    return policy.queue_depth;
}

}  // namespace

class InMemoryReader final : public Reader {
public:
    class Mailbox {
    public:
        Mailbox(core::TopicName topic, MessageDescriptor descriptor, core::MessagePolicy policy)
            : topic_(std::move(topic)), descriptor_(std::move(descriptor)), policy_(policy)
        {
        }

        const core::TopicName& topic_name() const noexcept
        {
            return topic_;
        }

        const MessageDescriptor& message_descriptor() const noexcept
        {
            return descriptor_;
        }

        core::Result<Message> Read(ReadOptions options)
        {
            std::unique_lock<std::mutex> lock(mutex_);

            if (options.wait) {
                if (options.timeout > core::Nanoseconds::zero()) {
                    const bool ready = data_available_.wait_for(lock, options.timeout, [this]() {
                        return closed_ || !messages_.empty();
                    });
                    if (!ready) {
                        return core::Result<Message>::FromStatus(
                            core::Status::DeadlineExceeded("timed out waiting for in-memory message"));
                    }
                } else {
                    data_available_.wait(lock, [this]() {
                        return closed_ || !messages_.empty();
                    });
                }
            }

            if (messages_.empty()) {
                return core::Result<Message>::FromStatus(
                    core::Status::Unavailable("no in-memory message is available"));
            }

            Message message = std::move(messages_.front());
            messages_.pop_front();
            space_available_.notify_all();
            return message;
        }

        core::Status SetDataAvailableCallback(DataAvailableCallback callback)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            callback_ = std::move(callback);
            return core::Status::Ok();
        }

        core::Status Push(const Message& message)
        {
            DataAvailableCallback callback;

            {
                std::unique_lock<std::mutex> lock(mutex_);
                if (closed_) {
                    return core::Status::FailedPrecondition("in-memory reader mailbox is closed");
                }

                if (policy_.freshness == core::FreshnessPolicy::kLatest) {
                    messages_.clear();
                    messages_.push_back(message);
                } else {
                    auto status = PushQueued(lock, message);
                    if (!status.ok()) {
                        return status;
                    }
                }

                callback = callback_;
            }

            data_available_.notify_all();
            if (callback) {
                callback();
            }
            return core::Status::Ok();
        }

        void Close() noexcept
        {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                closed_ = true;
            }
            data_available_.notify_all();
            space_available_.notify_all();
        }

    private:
        core::Status PushQueued(std::unique_lock<std::mutex>& lock, const Message& message)
        {
            if (UsesUnboundedQueue(policy_)) {
                messages_.push_back(message);
                return core::Status::Ok();
            }

            const auto capacity = QueueCapacity(policy_);
            if (messages_.size() < capacity) {
                messages_.push_back(message);
                return core::Status::Ok();
            }

            switch (policy_.overflow) {
                case core::QueueOverflowPolicy::kDropOldest:
                    messages_.pop_front();
                    messages_.push_back(message);
                    return core::Status::Ok();
                case core::QueueOverflowPolicy::kDropNewest:
                    return core::Status::Ok();
                case core::QueueOverflowPolicy::kReject:
                    return core::Status::ResourceExhausted("in-memory reader queue is full");
                case core::QueueOverflowPolicy::kBlock:
                    space_available_.wait(lock, [this, capacity]() {
                        return closed_ || messages_.size() < capacity;
                    });
                    if (closed_) {
                        return core::Status::FailedPrecondition("in-memory reader mailbox is closed");
                    }
                    messages_.push_back(message);
                    return core::Status::Ok();
            }

            return core::Status::Internal("unrecognized in-memory queue overflow policy");
        }

        core::TopicName topic_;
        MessageDescriptor descriptor_;
        core::MessagePolicy policy_;
        std::mutex mutex_;
        std::condition_variable data_available_;
        std::condition_variable space_available_;
        std::deque<Message> messages_;
        DataAvailableCallback callback_;
        bool closed_ {false};
    };

    explicit InMemoryReader(std::shared_ptr<Mailbox> mailbox)
        : mailbox_(std::move(mailbox))
    {
    }

    ~InMemoryReader() override
    {
        if (mailbox_) {
            mailbox_->Close();
        }
    }

    const core::TopicName& topic_name() const noexcept override
    {
        return mailbox_->topic_name();
    }

    const MessageDescriptor& message_descriptor() const noexcept override
    {
        return mailbox_->message_descriptor();
    }

    core::Result<Message> Read(ReadOptions options = {}) override
    {
        return mailbox_->Read(options);
    }

    core::Status SetDataAvailableCallback(DataAvailableCallback callback) override
    {
        return mailbox_->SetDataAvailableCallback(std::move(callback));
    }

private:
    std::shared_ptr<Mailbox> mailbox_;
};

class TopicChannel final {
public:
    TopicChannel(core::TopicName topic, MessageDescriptor descriptor)
        : topic_(std::move(topic)), descriptor_(std::move(descriptor))
    {
    }

    const MessageDescriptor& descriptor() const noexcept
    {
        return descriptor_;
    }

    bool MatchesDescriptor(const MessageDescriptor& descriptor) const
    {
        return IsSameDescriptor(descriptor_, descriptor);
    }

    core::Status AddReader(const std::shared_ptr<InMemoryReader::Mailbox>& mailbox)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) {
            return core::Status::FailedPrecondition("in-memory topic channel is closed: " + topic_.str());
        }

        readers_.push_back(mailbox);
        return core::Status::Ok();
    }

    core::Status Publish(ByteView payload, WriteOptions options)
    {
        auto status = payload.Validate();
        if (!status.ok()) {
            return status;
        }

        std::vector<std::shared_ptr<InMemoryReader::Mailbox>> readers;
        Message message;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closed_) {
                return core::Status::FailedPrecondition("in-memory topic channel is closed: " + topic_.str());
            }

            message.payload = CopyBytes(payload);
            message.metadata.sequence = next_sequence_++;
            message.metadata.source_timestamp =
                options.source_timestamp == core::TimePoint {} ? Now() : options.source_timestamp;
            message.metadata.reception_timestamp = Now();

            readers_.erase(
                std::remove_if(readers_.begin(), readers_.end(), [](const auto& reader) {
                    return reader.expired();
                }),
                readers_.end());

            readers.reserve(readers_.size());
            for (const auto& reader : readers_) {
                if (auto locked = reader.lock()) {
                    readers.push_back(std::move(locked));
                }
            }
        }

        for (const auto& reader : readers) {
            status = reader->Push(message);
            if (!status.ok()) {
                return status;
            }
        }

        return core::Status::Ok();
    }

    void Close() noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        for (const auto& reader : readers_) {
            if (auto locked = reader.lock()) {
                locked->Close();
            }
        }
        readers_.clear();
    }

private:
    core::TopicName topic_;
    MessageDescriptor descriptor_;
    std::mutex mutex_;
    std::vector<std::weak_ptr<InMemoryReader::Mailbox>> readers_;
    core::SequenceNumber next_sequence_ {1};
    bool closed_ {false};
};

class InMemoryWriter final : public Writer {
public:
    InMemoryWriter(core::TopicName topic, MessageDescriptor descriptor, std::shared_ptr<TopicChannel> channel)
        : topic_(std::move(topic)), descriptor_(std::move(descriptor)), channel_(std::move(channel))
    {
    }

    const core::TopicName& topic_name() const noexcept override
    {
        return topic_;
    }

    const MessageDescriptor& message_descriptor() const noexcept override
    {
        return descriptor_;
    }

    core::Status Write(ByteView payload, WriteOptions options = {}) override
    {
        return channel_->Publish(payload, options);
    }

private:
    core::TopicName topic_;
    MessageDescriptor descriptor_;
    std::shared_ptr<TopicChannel> channel_;
};

struct InMemoryTransport::State {
    mutable std::mutex mutex;
    std::map<std::string, std::shared_ptr<TopicChannel>> channels;
    bool open {false};

    core::Status Open()
    {
        std::lock_guard<std::mutex> lock(mutex);
        open = true;
        return core::Status::Ok();
    }

    core::Status Close() noexcept
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!open && channels.empty()) {
            return core::Status::Ok();
        }

        open = false;
        for (const auto& entry : channels) {
            entry.second->Close();
        }
        channels.clear();
        return core::Status::Ok();
    }

    bool IsOpen() const noexcept
    {
        std::lock_guard<std::mutex> lock(mutex);
        return open;
    }

    core::Result<std::shared_ptr<TopicChannel>> GetOrCreateChannel(const EndpointConfig& endpoint)
    {
        auto status = ValidateInMemoryEndpoint(endpoint);
        if (!status.ok()) {
            return core::Result<std::shared_ptr<TopicChannel>>::FromStatus(std::move(status));
        }

        std::lock_guard<std::mutex> lock(mutex);
        if (!open) {
            return core::Result<std::shared_ptr<TopicChannel>>::FromStatus(
                core::Status::FailedPrecondition("in-memory transport must be open before creating endpoints"));
        }

        const auto& key = endpoint.topic.name.str();
        auto found = channels.find(key);
        if (found != channels.end()) {
            if (!found->second->MatchesDescriptor(endpoint.message)) {
                return core::Result<std::shared_ptr<TopicChannel>>::FromStatus(
                    core::Status::InvalidArgument(
                        "in-memory topic already exists with a different message descriptor"));
            }
            return found->second;
        }

        auto channel = std::make_shared<TopicChannel>(endpoint.topic.name, endpoint.message);
        channels.emplace(key, channel);
        return channel;
    }
};

InMemoryTransport::InMemoryTransport(core::TransportName name)
    : name_(std::move(name)), state_(std::make_shared<State>())
{
}

InMemoryTransport::~InMemoryTransport()
{
    Close();
}

const core::TransportName& InMemoryTransport::name() const noexcept
{
    return name_;
}

core::TransportKind InMemoryTransport::kind() const noexcept
{
    return core::TransportKind::kInMemory;
}

TransportCapabilities InMemoryTransport::capabilities() const noexcept
{
    TransportCapabilities capabilities;
    capabilities.kind = kind();
    capabilities.supports_callbacks = true;
    capabilities.supports_blocking_read = true;
    capabilities.supports_reliable_delivery = true;
    capabilities.supports_keep_all = true;
    capabilities.supports_zero_copy = false;
    return capabilities;
}

core::Status InMemoryTransport::Open()
{
    return state_->Open();
}

core::Status InMemoryTransport::Close() noexcept
{
    return state_->Close();
}

bool InMemoryTransport::is_open() const noexcept
{
    return state_->IsOpen();
}

core::Status InMemoryTransport::ValidateEndpoint(const EndpointConfig& endpoint) const
{
    return ValidateInMemoryEndpoint(endpoint);
}

core::Result<ReaderPtr> InMemoryTransport::CreateReader(const EndpointConfig& endpoint)
{
    auto channel = state_->GetOrCreateChannel(endpoint);
    if (!channel.ok()) {
        return core::Result<ReaderPtr>::FromStatus(channel.status());
    }

    auto mailbox = std::make_shared<InMemoryReader::Mailbox>(
        endpoint.topic.name,
        endpoint.message,
        endpoint.topic.message_policy);

    auto status = channel.value()->AddReader(mailbox);
    if (!status.ok()) {
        return core::Result<ReaderPtr>::FromStatus(std::move(status));
    }

    return std::static_pointer_cast<Reader>(std::make_shared<InMemoryReader>(std::move(mailbox)));
}

core::Result<WriterPtr> InMemoryTransport::CreateWriter(const EndpointConfig& endpoint)
{
    auto channel = state_->GetOrCreateChannel(endpoint);
    if (!channel.ok()) {
        return core::Result<WriterPtr>::FromStatus(channel.status());
    }

    return std::static_pointer_cast<Writer>(
        std::make_shared<InMemoryWriter>(endpoint.topic.name, endpoint.message, channel.value()));
}

}  // namespace puppet_master::transport::inmemory
