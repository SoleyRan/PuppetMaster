#include <cassert>
#include <memory>
#include <string>
#include <utility>

#include <puppet_master/puppet_master.h>

namespace core = puppet_master::core;
namespace runtime = puppet_master::runtime;
namespace transport = puppet_master::transport;

namespace {

core::ComponentName MakeComponentName(const std::string& value)
{
    auto name = core::ComponentName::Create(value);
    assert(name.ok());
    return name.value();
}

core::TopicName MakeTopicName(const std::string& value)
{
    auto name = core::TopicName::Create(value);
    assert(name.ok());
    return name.value();
}

transport::EndpointConfig MakeEndpoint(const std::string& topic)
{
    core::MessagePolicy policy;
    policy.freshness = core::FreshnessPolicy::kQueued;
    policy.retention = core::RetentionPolicy::kKeepLast;
    policy.queue_depth = 4;

    return transport::EndpointConfig {
        core::TopicSpec {MakeTopicName(topic), core::TransportKind::kInMemory, policy},
        transport::MessageDescriptor {"test.ComponentPayload", "text/plain"}
    };
}

std::string PayloadToString(const transport::ByteBuffer& payload)
{
    if (payload.empty()) {
        return {};
    }

    return std::string(reinterpret_cast<const char*>(payload.data()), payload.size());
}

class SpeedProducer final : public runtime::Component {
public:
    SpeedProducer(core::ComponentName name, transport::EndpointConfig endpoint)
        : spec_ {std::move(name), "publishes speed samples", {}, {std::move(endpoint)}, {}}
    {
    }

    runtime::ComponentSpec Describe() const override
    {
        return spec_;
    }

    core::Status Configure(runtime::ComponentContext& context) override
    {
        auto writer = context.CreateWriter(spec_.writers.front());
        if (!writer.ok()) {
            return writer.status();
        }

        writer_ = writer.value();
        ++configure_count_;
        return core::Status::Ok();
    }

    core::Status Initialize(runtime::ComponentContext&) override
    {
        ++initialize_count_;
        return core::Status::Ok();
    }

    core::Status Start(runtime::ComponentContext&) override
    {
        ++start_count_;
        return core::Status::Ok();
    }

    core::Status Execute(runtime::ComponentContext&) override
    {
        ++execute_count_;
        const std::string payload = "speed=" + std::to_string(execute_count_);
        return writer_->Write(transport::ByteView::From(payload.data(), payload.size()));
    }

    core::Status Stop(runtime::ComponentContext&) override
    {
        ++stop_count_;
        return core::Status::Ok();
    }

    core::Status Shutdown(runtime::ComponentContext&) override
    {
        ++shutdown_count_;
        return core::Status::Ok();
    }

    int configure_count() const noexcept
    {
        return configure_count_;
    }

    int initialize_count() const noexcept
    {
        return initialize_count_;
    }

    int start_count() const noexcept
    {
        return start_count_;
    }

    int execute_count() const noexcept
    {
        return execute_count_;
    }

    int stop_count() const noexcept
    {
        return stop_count_;
    }

    int shutdown_count() const noexcept
    {
        return shutdown_count_;
    }

private:
    runtime::ComponentSpec spec_;
    transport::WriterPtr writer_;
    int configure_count_ {0};
    int initialize_count_ {0};
    int start_count_ {0};
    int execute_count_ {0};
    int stop_count_ {0};
    int shutdown_count_ {0};
};

class SpeedConsumer final : public runtime::Component {
public:
    SpeedConsumer(core::ComponentName name, transport::EndpointConfig endpoint)
        : spec_ {std::move(name), "reads speed samples", {std::move(endpoint)}, {}, {}}
    {
        spec_.triggers = {core::TriggerSpec {
            core::TriggerKind::kData,
            {},
            {},
            {spec_.readers.front().topic.name},
            {}
        }};
    }

    runtime::ComponentSpec Describe() const override
    {
        return spec_;
    }

    core::Status Configure(runtime::ComponentContext& context) override
    {
        auto reader = context.CreateReader(spec_.readers.front());
        if (!reader.ok()) {
            return reader.status();
        }

        reader_ = reader.value();
        return core::Status::Ok();
    }

    core::Status Execute(runtime::ComponentContext&) override
    {
        auto message = reader_->Read();
        if (!message.ok()) {
            return message.status();
        }

        last_payload_ = PayloadToString(message.value().payload);
        return core::Status::Ok();
    }

    const std::string& last_payload() const noexcept
    {
        return last_payload_;
    }

private:
    runtime::ComponentSpec spec_;
    transport::ReaderPtr reader_;
    std::string last_payload_;
};

class FailingComponent final : public runtime::Component {
public:
    explicit FailingComponent(core::ComponentName name)
        : spec_ {std::move(name), "fails during start", {}, {}, {}}
    {
    }

    runtime::ComponentSpec Describe() const override
    {
        return spec_;
    }

    core::Status Start(runtime::ComponentContext&) override
    {
        return core::Status::Internal("start failed");
    }

private:
    runtime::ComponentSpec spec_;
};

void ComponentLifecycleRunsInOrder()
{
    auto context = runtime::RuntimeContext::Create();
    assert(context.ok());

    const auto endpoint = MakeEndpoint("/component/speed");
    auto producer = std::make_shared<SpeedProducer>(MakeComponentName("producer"), endpoint);
    auto consumer = std::make_shared<SpeedConsumer>(MakeComponentName("consumer"), endpoint);

    assert(context.value()->RegisterComponent(producer).ok());
    assert(context.value()->RegisterComponent(consumer).ok());

    auto state = context.value()->GetComponentState(MakeComponentName("producer"));
    assert(state.ok());
    assert(state.value() == runtime::ComponentState::kCreated);

    auto execute_too_early = context.value()->ExecuteComponent(MakeComponentName("producer"));
    assert(!execute_too_early.ok());
    assert(execute_too_early.code() == core::StatusCode::kFailedPrecondition);

    assert(context.value()->ConfigureComponent(MakeComponentName("producer")).ok());
    assert(context.value()->InitializeComponent(MakeComponentName("producer")).ok());
    assert(context.value()->StartComponent(MakeComponentName("producer")).ok());

    assert(context.value()->ConfigureComponent(MakeComponentName("consumer")).ok());
    assert(context.value()->InitializeComponent(MakeComponentName("consumer")).ok());
    assert(context.value()->StartComponent(MakeComponentName("consumer")).ok());

    assert(context.value()->ExecuteComponent(MakeComponentName("producer")).ok());
    assert(context.value()->ExecuteComponent(MakeComponentName("consumer")).ok());
    assert(consumer->last_payload() == "speed=1");

    assert(context.value()->StopComponent(MakeComponentName("producer")).ok());
    assert(context.value()->ShutdownComponent(MakeComponentName("producer")).ok());

    state = context.value()->GetComponentState(MakeComponentName("producer"));
    assert(state.ok());
    assert(state.value() == runtime::ComponentState::kShutdown);

    assert(producer->configure_count() == 1);
    assert(producer->initialize_count() == 1);
    assert(producer->start_count() == 1);
    assert(producer->execute_count() == 1);
    assert(producer->stop_count() == 1);
    assert(producer->shutdown_count() == 1);
}

void ComponentFailureMovesStateToError()
{
    auto context = runtime::RuntimeContext::Create();
    assert(context.ok());

    auto component = std::make_shared<FailingComponent>(MakeComponentName("failing"));
    assert(context.value()->RegisterComponent(component).ok());
    assert(context.value()->ConfigureComponent(MakeComponentName("failing")).ok());
    assert(context.value()->InitializeComponent(MakeComponentName("failing")).ok());

    auto start = context.value()->StartComponent(MakeComponentName("failing"));
    assert(!start.ok());

    auto state = context.value()->GetComponentState(MakeComponentName("failing"));
    assert(state.ok());
    assert(state.value() == runtime::ComponentState::kError);
}

}  // namespace

int main()
{
    ComponentLifecycleRunsInOrder();
    ComponentFailureMovesStateToError();
    return 0;
}
