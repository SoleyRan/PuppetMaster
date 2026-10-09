#include <cassert>
#include <chrono>
#include <memory>
#include <string>
#include <utility>

#include <puppet_master/puppet_master.h>
#include <puppet_master/transport/fastdds/fastdds_transport.h>

namespace core = puppet_master::core;
namespace configuration = puppet_master::configuration;
namespace runtime = puppet_master::runtime;
namespace scheduler = puppet_master::scheduler;
namespace transport = puppet_master::transport;
namespace fastdds = puppet_master::transport::fastdds;

namespace {

transport::EndpointConfig MakeEndpoint(const std::string& topic_name, const std::string& type_name)
{
    auto topic = core::TopicName::Create(topic_name);
    assert(topic.ok());
    core::MessagePolicy policy;
    policy.delivery = core::DeliveryGuarantee::kReliable;
    policy.queue_depth = 16;
    return transport::EndpointConfig {
        core::TopicSpec {topic.value(), core::TransportKind::kFastDds, policy},
        transport::MessageDescriptor {type_name, "application/octet-stream"}
    };
}

std::unique_ptr<fastdds::FastDdsTransport> MakeTransport()
{
    auto name = core::TransportName::Create("fastdds");
    assert(name.ok());
    return std::make_unique<fastdds::FastDdsTransport>(name.value());
}

class FastDdsProcessor final : public runtime::Component {
public:
    explicit FastDdsProcessor(runtime::ComponentSpec spec) : spec_(std::move(spec)) {}

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
        auto writer = context.CreateWriter(spec_.writers.front());
        if (!writer.ok()) {
            return writer.status();
        }
        reader_ = reader.value();
        writer_ = writer.value();
        return core::Status::Ok();
    }

    core::Status Execute(runtime::ComponentContext&) override
    {
        auto message = reader_->Read();
        if (!message.ok()) {
            return message.status();
        }
        const auto& bytes = message.value().payload;
        const std::string input(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        const std::string output = "processed:" + input;
        return writer_->Write(transport::ByteView::From(output.data(), output.size()));
    }

private:
    runtime::ComponentSpec spec_;
    transport::ReaderPtr reader_;
    transport::WriterPtr writer_;
};

void RuntimeContextCreatesFastDdsEndpoints()
{
    auto context = runtime::RuntimeContext::Create();
    assert(context.ok());

    auto name = core::TransportName::Create("fastdds");
    assert(name.ok());
    fastdds::Options invalid_options;
    invalid_options.participant_name.clear();
    assert(fastdds::RegisterTransport(*context.value(), name.value(), invalid_options).code() ==
           core::StatusCode::kInvalidArgument);
    assert(!context.value()->FindTransport(name.value()).ok());
    assert(fastdds::RegisterTransport(*context.value(), name.value()).ok());
    assert(context.value()->FindTransport(core::TransportName::Unsafe("fastdds")).ok());

    const auto endpoint = MakeEndpoint("/test/runtime-roundtrip", "test.RuntimeBytes");
    auto reader = context.value()->CreateReader(endpoint);
    auto writer = context.value()->CreateWriter(endpoint);
    assert(reader.ok());
    assert(writer.ok());

    const std::string payload = "runtime fastdds";
    transport::ReadOptions options;
    options.wait = true;
    options.timeout = std::chrono::milliseconds(200);

    bool received = false;
    for (int attempt = 0; attempt < 25 && !received; ++attempt) {
        assert(writer.value()->Write(transport::ByteView::From(payload.data(), payload.size())).ok());
        auto message = reader.value()->Read(options);
        if (message.ok()) {
            const auto& bytes = message.value().payload;
            assert(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()) == payload);
            received = true;
        }
    }
    assert(received);

    assert(context.value()->Close().ok());
    assert(reader.value()->Read().status().code() == core::StatusCode::kUnavailable);
}

void RejectsEndpointsBeforeOpen()
{
    auto transport = MakeTransport();
    const auto endpoint = MakeEndpoint("/test/closed", "test.Bytes");
    auto reader = transport->CreateReader(endpoint);
    assert(!reader.ok());
    assert(reader.status().code() == core::StatusCode::kFailedPrecondition);
}

void RejectsForeignTransportKind()
{
    auto transport = MakeTransport();
    auto endpoint = MakeEndpoint("/test/foreign", "test.Bytes");
    endpoint.topic.transport = core::TransportKind::kInMemory;
    assert(transport->ValidateEndpoint(endpoint).code() == core::StatusCode::kInvalidArgument);
}

void PublishSubscribeRoundTrip()
{
    auto transport = MakeTransport();
    assert(transport->Open().ok());
    assert(transport->is_open());

    const auto endpoint = MakeEndpoint("/test/roundtrip", "test.Bytes");
    auto reader = transport->CreateReader(endpoint);
    auto writer = transport->CreateWriter(endpoint);
    assert(reader.ok());
    assert(writer.ok());

    const std::string payload = "hello fastdds";
    transport::ReadOptions options;
    options.wait = true;
    options.timeout = std::chrono::milliseconds(200);

    // Intra-participant matching is asynchronous; retry until delivered.
    bool received = false;
    for (int attempt = 0; attempt < 25 && !received; ++attempt) {
        assert(writer.value()->Write(transport::ByteView::From(payload.data(), payload.size())).ok());
        auto message = reader.value()->Read(options);
        if (message.ok()) {
            const auto& bytes = message.value().payload;
            assert(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()) == payload);
            assert(message.value().metadata.sequence >= 1);
            received = true;
        }
    }
    assert(received);

    auto conflicting = transport->CreateReader(MakeEndpoint("/test/roundtrip", "test.Other"));
    assert(!conflicting.ok());
    assert(conflicting.status().code() == core::StatusCode::kInvalidArgument);

    assert(transport->Close().ok());
    assert(!transport->is_open());
    auto after_close = reader.value()->Read();
    assert(after_close.status().code() == core::StatusCode::kUnavailable);
    assert(writer.value()->Write(transport::ByteView::From(payload.data(), payload.size())).code() ==
           core::StatusCode::kUnavailable);
}

void ConfigurationAndSchedulerFastDdsRoundTrip()
{
    configuration::ProjectConfig project;
    project.runtime_options.open_registered_transports = true;
    assert(project.AddTopic(configuration::TopicConfig {
        "input", "/test/component-input", core::TransportKind::kFastDds,
        "test.Bytes", "application/octet-stream", {}}).ok());
    assert(project.AddTopic(configuration::TopicConfig {
        "output", "/test/component-output", core::TransportKind::kFastDds,
        "test.Bytes", "application/octet-stream", {}}).ok());
    assert(project.AddComponent(configuration::ComponentConfig {
        "fastdds_processor", "FastDDS processing component", {"input"}, {"output"},
        {{core::TriggerKind::kData, {}, core::DependencyPolicy::kAll, {"input"}, {}}}}).ok());
    assert(project.Validate().ok());

    auto spec = project.BuildComponentSpec("fastdds_processor");
    assert(spec.ok());
    assert(spec.value().Validate().ok());
    assert(spec.value().readers.size() == 1);
    assert(spec.value().writers.size() == 1);
    assert(spec.value().triggers.size() == 1);
    assert(spec.value().triggers.front().kind == core::TriggerKind::kData);

    auto context = runtime::RuntimeContext::Create(project.runtime_options);
    assert(context.ok());
    auto transport_name = core::TransportName::Create("fastdds");
    assert(transport_name.ok());
    assert(fastdds::RegisterTransport(*context.value(), transport_name.value()).ok());

    auto component = std::make_shared<FastDdsProcessor>(spec.value());
    assert(context.value()->RegisterComponent(component).ok());
    assert(context.value()->ConfigureComponent(spec.value().name).ok());
    assert(context.value()->InitializeComponent(spec.value().name).ok());
    assert(context.value()->StartComponent(spec.value().name).ok());

    auto input_writer = context.value()->CreateWriter(spec.value().readers.front());
    auto output_reader = context.value()->CreateReader(spec.value().writers.front());
    assert(input_writer.ok());
    assert(output_reader.ok());

    scheduler::Scheduler scheduler(*context.value());
    assert(scheduler.RegisterAllComponents().ok());
    assert(scheduler.Start().ok());

    const std::string payload = "fastdds payload";
    const std::string expected = "processed:" + payload;
    transport::ReadOptions options;
    options.wait = true;
    options.timeout = std::chrono::milliseconds(200);
    bool received = false;
    for (int attempt = 0; attempt < 30 && !received; ++attempt) {
        assert(input_writer.value()->Write(
            transport::ByteView::From(payload.data(), payload.size())).ok());
        auto output = output_reader.value()->Read(options);
        if (output.ok()) {
            const auto& bytes = output.value().payload;
            assert(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()) == expected);
            received = true;
        }
    }
    assert(received);
    assert(scheduler.WaitIdle(std::chrono::milliseconds(500)).ok());
    assert(scheduler.Stop().ok());

    auto state = context.value()->GetComponentState(spec.value().name);
    assert(state.ok());
    assert(state.value() == runtime::ComponentState::kStarted);
    const auto stats = scheduler.stats();
    assert(stats.registered_components == 1);
    assert(stats.dispatched_events >= 1);

    const auto snapshot = context.value()->observer()->Snapshot();
    bool input_observed = false;
    bool output_observed = false;
    for (const auto& topic : snapshot.topics) {
        if (topic.topic_name == "/test/component-input") {
            input_observed = topic.messages_published > 0 && topic.messages_received > 0;
        }
        if (topic.topic_name == "/test/component-output") {
            output_observed = topic.messages_published > 0 && topic.messages_received > 0;
        }
    }
    assert(input_observed);
    assert(output_observed);
    bool task_observed = false;
    for (const auto& task : snapshot.tasks) {
        if (task.task_name == "fastdds_processor") {
            task_observed = task.executions > 0;
        }
    }
    assert(task_observed);

    assert(context.value()->StopComponent(spec.value().name).ok());
    assert(context.value()->ShutdownComponent(spec.value().name).ok());
    state = context.value()->GetComponentState(spec.value().name);
    assert(state.ok());
    assert(state.value() == runtime::ComponentState::kShutdown);
    assert(context.value()->Close().ok());
}

}  // namespace

int main()
{
    RejectsEndpointsBeforeOpen();
    RejectsForeignTransportKind();
    PublishSubscribeRoundTrip();
    RuntimeContextCreatesFastDdsEndpoints();
    ConfigurationAndSchedulerFastDdsRoundTrip();
    return 0;
}
