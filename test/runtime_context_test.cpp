#include <cassert>
#include <memory>
#include <string>

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

transport::EndpointConfig MakeEndpoint(const std::string& topic, core::MessagePolicy policy = {})
{
    return transport::EndpointConfig {
        core::TopicSpec {MakeTopicName(topic), core::TransportKind::kInMemory, policy},
        transport::MessageDescriptor {"test.RuntimePayload", "text/plain"}
    };
}

core::Status WriteString(const transport::WriterPtr& writer, const std::string& payload)
{
    return writer->Write(transport::ByteView::From(payload.data(), payload.size()));
}

std::string PayloadToString(const transport::ByteBuffer& payload)
{
    if (payload.empty()) {
        return {};
    }
    return std::string(reinterpret_cast<const char*>(payload.data()), payload.size());
}

void RuntimeContextCreatesDefaultInMemoryTransport()
{
    auto context = runtime::RuntimeContext::Create();
    assert(context.ok());
    assert(context.value()->is_open());

    const auto transport_names = context.value()->ListTransportNames();
    assert(transport_names.size() == 1);
    assert(transport_names.front().str() == "inmemory");

    auto transport = context.value()->FindTransport(transport_names.front());
    assert(transport.ok());
    assert(transport.value()->kind() == core::TransportKind::kInMemory);
    assert(transport.value()->is_open());
}

void RuntimeContextRegistersComponents()
{
    auto context = runtime::RuntimeContext::Create();
    assert(context.ok());

    const auto endpoint = MakeEndpoint("/runtime/speed");
    runtime::ComponentSpec producer {
        MakeComponentName("speed_producer"),
        "publishes speed samples",
        {},
        {endpoint},
        {}
    };

    runtime::ComponentSpec consumer {
        MakeComponentName("speed_consumer"),
        "reads speed samples",
        {endpoint},
        {},
        {core::TriggerSpec {core::TriggerKind::kData, {}, {}, {endpoint.topic.name}, {}}}
    };

    assert(context.value()->RegisterComponent(producer).ok());
    assert(context.value()->RegisterComponent(consumer).ok());
    assert(context.value()->ListComponentNames().size() == 2);

    auto found = context.value()->FindComponent(MakeComponentName("speed_consumer"));
    assert(found.ok());
    assert(found.value().readers.size() == 1);

    auto duplicate = context.value()->RegisterComponent(producer);
    assert(!duplicate.ok());
    assert(duplicate.code() == core::StatusCode::kAlreadyExists);
}

void RuntimeContextCreatesEndpointsThroughTransport()
{
    auto context = runtime::RuntimeContext::Create();
    assert(context.ok());

    core::MessagePolicy policy;
    policy.freshness = core::FreshnessPolicy::kQueued;
    policy.queue_depth = 4;

    const auto endpoint = MakeEndpoint("/runtime/pubsub", policy);
    auto reader = context.value()->CreateReader(endpoint);
    auto writer = context.value()->CreateWriter(endpoint);
    assert(reader.ok());
    assert(writer.ok());

    assert(WriteString(writer.value(), "speed=12.5").ok());

    auto message = reader.value()->Read();
    assert(message.ok());
    assert(PayloadToString(message.value().payload) == "speed=12.5");
}

void RuntimeContextReportsMissingTransportKind()
{
    runtime::RuntimeOptions options;
    options.register_inmemory_transport = false;

    auto context = runtime::RuntimeContext::Create(options);
    assert(context.ok());
    assert(context.value()->ListTransportNames().empty());

    auto writer = context.value()->CreateWriter(MakeEndpoint("/runtime/missing_transport"));
    assert(!writer.ok());
    assert(writer.status().code() == core::StatusCode::kNotFound);
}

void ClosedRuntimeContextRejectsEndpointCreation()
{
    auto context = runtime::RuntimeContext::Create();
    assert(context.ok());
    assert(context.value()->Close().ok());
    assert(!context.value()->is_open());

    auto reader = context.value()->CreateReader(MakeEndpoint("/runtime/closed"));
    assert(!reader.ok());
    assert(reader.status().code() == core::StatusCode::kFailedPrecondition);
}

}  // namespace

int main()
{
    RuntimeContextCreatesDefaultInMemoryTransport();
    RuntimeContextRegistersComponents();
    RuntimeContextCreatesEndpointsThroughTransport();
    RuntimeContextReportsMissingTransportKind();
    ClosedRuntimeContextRejectsEndpointCreation();
    return 0;
}
