#include <puppet_master/compat/itage_facade.h>

#include <chrono>
#include <cstring>
#include <limits>
#include <utility>

namespace puppet_master::compat::itage {

namespace {

core::Status FitsLegacyReturnValue(std::size_t size)
{
    if (size > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return core::Status::OutOfRange("payload size exceeds legacy int return range");
    }
    return core::Status::Ok();
}

std::int64_t ComputeTimeDiffMs(const transport::MessageMetadata& metadata)
{
    if (metadata.source_timestamp == core::TimePoint {}) {
        return 0;
    }

    const auto now = std::chrono::time_point_cast<core::Nanoseconds>(core::SteadyClock::now());
    const auto diff = now - metadata.source_timestamp;
    if (diff <= core::Nanoseconds::zero()) {
        return 0;
    }

    return std::chrono::duration_cast<std::chrono::milliseconds>(diff).count();
}

class CompatWriter final : public WriterBase {
public:
    CompatWriter(
        transport::WriterPtr writer,
        transport::EndpointConfig endpoint,
        std::size_t fixed_payload_size)
        : writer_(std::move(writer)),
          endpoint_(std::move(endpoint)),
          fixed_payload_size_(fixed_payload_size)
    {
    }

    int Write(void* data, std::size_t len = 0) override
    {
        const auto payload_size = len == 0 && fixed_payload_size_ > 0 ? fixed_payload_size_ : len;
        if (data == nullptr && payload_size != 0) {
            last_status_ = core::Status::InvalidArgument("legacy writer data is null");
            return -1;
        }

        auto status = FitsLegacyReturnValue(payload_size);
        if (!status.ok()) {
            last_status_ = std::move(status);
            return -1;
        }

        status = writer_->Write(transport::ByteView::From(data, payload_size));
        last_status_ = status;
        if (!status.ok()) {
            return -1;
        }

        return static_cast<int>(payload_size);
    }

    const core::Status& last_status() const noexcept override
    {
        return last_status_;
    }

    const transport::EndpointConfig& endpoint() const noexcept override
    {
        return endpoint_;
    }

private:
    transport::WriterPtr writer_;
    transport::EndpointConfig endpoint_;
    std::size_t fixed_payload_size_ {0};
    core::Status last_status_;
};

class CompatReader final : public ReaderBase {
public:
    CompatReader(
        transport::ReaderPtr reader,
        transport::EndpointConfig endpoint,
        TopicOptions options)
        : reader_(std::move(reader)),
          endpoint_(std::move(endpoint)),
          options_(std::move(options))
    {
    }

    void SetCallBack(const std::function<void()>& callback) override
    {
        last_status_ = reader_->SetDataAvailableCallback(callback);
    }

    int Read(void* data, std::int64_t& time_diff_ms) override
    {
        if (options_.fixed_payload_size == 0) {
            last_status_ = core::Status::FailedPrecondition(
                "legacy Read(void*, time_diff) requires TopicOptions::fixed_payload_size");
            return -1;
        }

        return Read(data, options_.fixed_payload_size, time_diff_ms);
    }

    int Read(void* data, std::size_t capacity, std::int64_t& time_diff_ms) override
    {
        time_diff_ms = 0;

        transport::ReadOptions read_options;
        read_options.wait = options_.wait_on_read;
        read_options.timeout = options_.read_timeout;

        auto message = reader_->Read(read_options);
        if (!message.ok()) {
            last_status_ = message.status();
            return -1;
        }

        const auto payload_size = message.value().payload.size();
        auto status = FitsLegacyReturnValue(payload_size);
        if (!status.ok()) {
            last_status_ = std::move(status);
            return -1;
        }

        if (payload_size > capacity) {
            last_status_ = core::Status::ResourceExhausted(
                "legacy reader buffer is smaller than the message payload");
            return -1;
        }

        if (data == nullptr && payload_size != 0) {
            last_status_ = core::Status::InvalidArgument("legacy reader data is null");
            return -1;
        }

        if (payload_size != 0) {
            std::memcpy(data, message.value().payload.data(), payload_size);
        }

        time_diff_ms = ComputeTimeDiffMs(message.value().metadata);
        last_status_ = core::Status::Ok();
        return static_cast<int>(payload_size);
    }

    const core::Status& last_status() const noexcept override
    {
        return last_status_;
    }

    const transport::EndpointConfig& endpoint() const noexcept override
    {
        return endpoint_;
    }

private:
    transport::ReaderPtr reader_;
    transport::EndpointConfig endpoint_;
    TopicOptions options_;
    core::Status last_status_;
};

}  // namespace

core::Status TopicOptions::Validate() const
{
    if (message_type.empty()) {
        return core::Status::InvalidArgument("compat topic message_type must not be empty");
    }

    if (encoding.empty()) {
        return core::Status::InvalidArgument("compat topic encoding must not be empty");
    }

    if (read_timeout < core::Nanoseconds::zero()) {
        return core::Status::InvalidArgument("compat topic read_timeout must not be negative");
    }

    return message_policy.Validate();
}

core::Status NodeOptions::Validate() const
{
    auto status = runtime_options.Validate();
    if (!status.ok()) {
        return status;
    }

    return default_topic_options.Validate();
}

Node::Node(std::string node_name, NodeOptions options)
    : node_name_(std::move(node_name)),
      options_(std::move(options))
{
    auto status = core::ValidateName("compat node name", node_name_);
    if (!status.ok()) {
        last_status_ = std::move(status);
        return;
    }

    status = options_.Validate();
    if (!status.ok()) {
        last_status_ = std::move(status);
        return;
    }

    auto context = runtime::RuntimeContext::Create(options_.runtime_options);
    if (!context.ok()) {
        last_status_ = context.status();
        return;
    }

    context_ = context.value();
    last_status_ = core::Status::Ok();
}

core::Result<std::shared_ptr<Node>> Node::Create(std::string node_name, NodeOptions options)
{
    auto node = std::make_shared<Node>(std::move(node_name), std::move(options));
    if (!node->last_status().ok()) {
        return core::Result<std::shared_ptr<Node>>::FromStatus(node->last_status());
    }

    return node;
}

const std::string& Node::name() const noexcept
{
    return node_name_;
}

std::shared_ptr<runtime::RuntimeContext> Node::context() const noexcept
{
    return context_;
}

const core::Status& Node::last_status() const noexcept
{
    return last_status_;
}

core::Result<transport::EndpointConfig> Node::BuildEndpoint(
    std::string topic_name,
    const TopicOptions& options) const
{
    auto status = options.Validate();
    if (!status.ok()) {
        return core::Result<transport::EndpointConfig>::FromStatus(std::move(status));
    }

    auto topic = core::TopicName::Create(std::move(topic_name));
    if (!topic.ok()) {
        return core::Result<transport::EndpointConfig>::FromStatus(topic.status());
    }

    transport::EndpointConfig endpoint {
        core::TopicSpec {topic.value(), options.transport, options.message_policy},
        transport::MessageDescriptor {options.message_type, options.encoding}
    };

    status = endpoint.Validate();
    if (!status.ok()) {
        return core::Result<transport::EndpointConfig>::FromStatus(std::move(status));
    }

    return endpoint;
}

std::shared_ptr<WriterBase> Node::CreateWriter(
    std::string topic_name,
    const TopicOptions& options)
{
    if (!context_) {
        return nullptr;
    }

    auto endpoint = BuildEndpoint(std::move(topic_name), options);
    if (!endpoint.ok()) {
        last_status_ = endpoint.status();
        return nullptr;
    }

    auto writer = context_->CreateWriter(endpoint.value());
    if (!writer.ok()) {
        last_status_ = writer.status();
        return nullptr;
    }

    last_status_ = core::Status::Ok();
    return std::make_shared<CompatWriter>(
        writer.value(),
        endpoint.value(),
        options.fixed_payload_size);
}

std::shared_ptr<ReaderBase> Node::CreateReader(
    std::string topic_name,
    const TopicOptions& options)
{
    if (!context_) {
        return nullptr;
    }

    auto endpoint = BuildEndpoint(std::move(topic_name), options);
    if (!endpoint.ok()) {
        last_status_ = endpoint.status();
        return nullptr;
    }

    auto reader = context_->CreateReader(endpoint.value());
    if (!reader.ok()) {
        last_status_ = reader.status();
        return nullptr;
    }

    last_status_ = core::Status::Ok();
    return std::make_shared<CompatReader>(
        reader.value(),
        endpoint.value(),
        options);
}

std::shared_ptr<WriterBase> Node::CreateWriter(
    std::string topic_name,
    void* data,
    void* attribute)
{
    (void)data;
    return CreateWriter(std::move(topic_name), ResolveTopicOptions(attribute));
}

std::shared_ptr<ReaderBase> Node::CreateReader(
    std::string topic_name,
    void* data,
    void* attribute)
{
    (void)data;
    return CreateReader(std::move(topic_name), ResolveTopicOptions(attribute));
}

TopicOptions Node::ResolveTopicOptions(const void* attribute) const
{
    if (attribute == nullptr) {
        return options_.default_topic_options;
    }

    return *static_cast<const TopicOptions*>(attribute);
}

}  // namespace puppet_master::compat::itage
