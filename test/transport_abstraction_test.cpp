#include <cassert>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <puppet_master/puppet_master.h>

namespace core = puppet_master::core;
namespace transport = puppet_master::transport;

class FakeReader final : public transport::Reader {
public:
    FakeReader(core::TopicName topic, transport::MessageDescriptor descriptor)
        : topic_(std::move(topic)), descriptor_(std::move(descriptor))
    {
    }

    const core::TopicName& topic_name() const noexcept override
    {
        return topic_;
    }

    const transport::MessageDescriptor& message_descriptor() const noexcept override
    {
        return descriptor_;
    }

    core::Result<transport::Message> Read(transport::ReadOptions) override
    {
        if (messages_.empty()) {
            return core::Result<transport::Message>::FromStatus(
                core::Status::Unavailable("no message is available"));
        }

        auto message = std::move(messages_.front());
        messages_.erase(messages_.begin());
        return message;
    }

    core::Status SetDataAvailableCallback(transport::DataAvailableCallback callback) override
    {
        callback_ = std::move(callback);
        return core::Status::Ok();
    }

    void Push(transport::ByteView payload)
    {
        transport::Message message;
        message.payload = transport::CopyBytes(payload);
        messages_.push_back(std::move(message));

        if (callback_) {
            callback_();
        }
    }

private:
    core::TopicName topic_;
    transport::MessageDescriptor descriptor_;
    std::vector<transport::Message> messages_;
    transport::DataAvailableCallback callback_;
};

class FakeWriter final : public transport::Writer {
public:
    FakeWriter(core::TopicName topic, transport::MessageDescriptor descriptor)
        : topic_(std::move(topic)), descriptor_(std::move(descriptor))
    {
    }

    const core::TopicName& topic_name() const noexcept override
    {
        return topic_;
    }

    const transport::MessageDescriptor& message_descriptor() const noexcept override
    {
        return descriptor_;
    }

    core::Status Write(transport::ByteView payload, transport::WriteOptions) override
    {
        auto status = payload.Validate();
        if (!status.ok()) {
            return status;
        }

        last_payload_ = transport::CopyBytes(payload);
        return core::Status::Ok();
    }

    const transport::ByteBuffer& last_payload() const noexcept
    {
        return last_payload_;
    }

private:
    core::TopicName topic_;
    transport::MessageDescriptor descriptor_;
    transport::ByteBuffer last_payload_;
};

class FakeTransport final : public transport::Transport {
public:
    explicit FakeTransport(core::TransportName name)
        : name_(std::move(name))
    {
    }

    const core::TransportName& name() const noexcept override
    {
        return name_;
    }

    core::TransportKind kind() const noexcept override
    {
        return core::TransportKind::kInMemory;
    }

    transport::TransportCapabilities capabilities() const noexcept override
    {
        transport::TransportCapabilities caps;
        caps.kind = kind();
        caps.supports_callbacks = true;
        caps.supports_blocking_read = false;
        caps.supports_reliable_delivery = false;
        caps.supports_keep_all = false;
        caps.supports_zero_copy = false;
        return caps;
    }

    core::Status Open() override
    {
        is_open_ = true;
        return core::Status::Ok();
    }

    core::Status Close() noexcept override
    {
        is_open_ = false;
        return core::Status::Ok();
    }

    bool is_open() const noexcept override
    {
        return is_open_;
    }

    core::Result<transport::ReaderPtr> CreateReader(const transport::EndpointConfig& endpoint) override
    {
        auto status = ValidateEndpoint(endpoint);
        if (!status.ok()) {
            return core::Result<transport::ReaderPtr>::FromStatus(std::move(status));
        }

        reader_ = std::make_shared<FakeReader>(endpoint.topic.name, endpoint.message);
        return std::static_pointer_cast<transport::Reader>(reader_);
    }

    core::Result<transport::WriterPtr> CreateWriter(const transport::EndpointConfig& endpoint) override
    {
        auto status = ValidateEndpoint(endpoint);
        if (!status.ok()) {
            return core::Result<transport::WriterPtr>::FromStatus(std::move(status));
        }

        writer_ = std::make_shared<FakeWriter>(endpoint.topic.name, endpoint.message);
        return std::static_pointer_cast<transport::Writer>(writer_);
    }

    std::shared_ptr<FakeReader> reader() const noexcept
    {
        return reader_;
    }

    std::shared_ptr<FakeWriter> writer() const noexcept
    {
        return writer_;
    }

private:
    core::TransportName name_;
    bool is_open_ {false};
    std::shared_ptr<FakeReader> reader_;
    std::shared_ptr<FakeWriter> writer_;
};

int main()
{
    const auto topic = core::TopicName::Create("/vehicle/speed");
    assert(topic.ok());

    const auto transport_name = core::TransportName::Create("fake");
    assert(transport_name.ok());

    core::TopicSpec topic_spec {
        topic.value(),
        core::TransportKind::kInMemory,
        core::MessagePolicy {}
    };

    transport::EndpointConfig endpoint {
        topic_spec,
        transport::MessageDescriptor {"vehicle.Speed", "application/octet-stream"}
    };
    assert(endpoint.Validate().ok());

    auto fake_transport = std::make_shared<FakeTransport>(transport_name.value());
    assert(fake_transport->Open().ok());
    assert(fake_transport->is_open());
    assert(fake_transport->capabilities().supports_callbacks);

    transport::TransportRegistry registry;
    assert(registry.Register(fake_transport).ok());
    assert(!registry.Register(fake_transport).ok());
    assert(registry.size() == 1);

    auto found = registry.Find(transport_name.value());
    assert(found.ok());
    assert(found.value()->kind() == core::TransportKind::kInMemory);

    auto reader = fake_transport->CreateReader(endpoint);
    auto writer = fake_transport->CreateWriter(endpoint);
    assert(reader.ok());
    assert(writer.ok());

    bool callback_called = false;
    assert(reader.value()->SetDataAvailableCallback([&callback_called]() {
        callback_called = true;
    }).ok());

    const std::string payload = "speed=12.5";
    const auto write_status =
        writer.value()->Write(transport::ByteView::From(payload.data(), payload.size()));
    assert(write_status.ok());
    assert(fake_transport->writer()->last_payload().size() == payload.size());

    fake_transport->reader()->Push(transport::ByteView::From(payload.data(), payload.size()));
    assert(callback_called);

    auto message = reader.value()->Read();
    assert(message.ok());
    assert(message.value().payload.size() == payload.size());

    auto empty_read = reader.value()->Read();
    assert(!empty_read.ok());
    assert(empty_read.status().code() == core::StatusCode::kUnavailable);

    assert(registry.Unregister(transport_name.value()).ok());
    assert(registry.empty());

    return 0;
}
