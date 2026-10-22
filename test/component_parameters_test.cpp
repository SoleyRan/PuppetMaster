#include <cassert>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include <puppet_master/configuration/configuration.h>
#include <puppet_master/puppet_master.h>
#include <puppet_master/runtime/parameters.h>
#include <puppet_master/runtime/typed_endpoints.h>
#include <puppet_master/transport/typed.h>

namespace config = puppet_master::configuration;
namespace core = puppet_master::core;
namespace runtime = puppet_master::runtime;
namespace transport = puppet_master::transport;

struct Speed {
    double value;
};

struct Heading {
    double value;
};

template <>
struct puppet_master::transport::MessageTypeName<Speed> {
    static constexpr const char* value = "test.Speed";
};

template <>
struct puppet_master::transport::MessageTypeName<Heading> {
    static constexpr const char* value = "test.Heading";
};

namespace {

void ParametersConvert()
{
    const runtime::ParameterMap parameters {
        {"name", "front"}, {"count", "42"}, {"negative", "-7"}, {"gain", "0.25"},
        {"enabled", "true"}, {"disabled", "false"}, {"padded", " 5"}, {"big", "300"},
    };

    assert(runtime::GetParameter(parameters, "name", "rear").value() == "front");
    assert(runtime::GetParameter(parameters, "count", 0).value() == 42);
    assert(runtime::GetParameter<std::int64_t>(parameters, "negative", 0).value() == -7);
    assert(runtime::GetParameter(parameters, "gain", 1.0).value() == 0.25);
    assert(runtime::GetParameter(parameters, "enabled", false).value());
    assert(!runtime::GetParameter(parameters, "disabled", true).value());

    // Missing keys fall back; required ones are NotFound.
    assert(runtime::GetParameter(parameters, "missing", 9).value() == 9);
    assert(runtime::GetParameter(parameters, "missing", "x").value() == "x");
    assert(runtime::GetRequiredParameter<int>(parameters, "missing").status().code() == core::StatusCode::kNotFound);
    assert(runtime::GetRequiredParameter<int>(parameters, "count").value() == 42);

    // Values that do not convert name the key.
    const auto wrong = runtime::GetParameter(parameters, "name", 0);
    assert(wrong.status().code() == core::StatusCode::kInvalidArgument);
    assert(wrong.status().message().find("name") != std::string::npos);
    assert(!runtime::GetParameter(parameters, "gain", 0).ok());
    assert(!runtime::GetParameter(parameters, "count", false).ok());
    assert(!runtime::GetParameter(parameters, "padded", 0).ok());
    assert(!runtime::GetParameter<std::uint8_t>(parameters, "big", 0).ok());
    assert(!runtime::GetParameter<std::uint32_t>(parameters, "negative", 0).ok());
    assert(!runtime::GetParameter(parameters, "name", 0.0).ok());
    assert(runtime::GetParameter(parameters, "count", 0.0).value() == 42.0);
}

void ConfigurationCarriesParameters()
{
    config::ProjectConfig project;
    assert(project.AddTopic(config::TopicConfig {"speed", "/speed", core::TransportKind::kInMemory, "test.Speed",
                                                 "application/x-puppetmaster-trivial", {}}).ok());
    config::ComponentConfig component {"monitor", "", {"speed"}, {}, {}};
    component.parameters = {{"limit", "30"}};
    assert(project.AddComponent(component).ok());

    auto spec = project.BuildComponentSpec("monitor");
    assert(spec.ok());
    assert(runtime::GetParameter(spec.value().parameters, "limit", 0).value() == 30);
}

core::TopicSpec Topic(const std::string& name)
{
    return core::TopicSpec {core::TopicName::Unsafe(name), core::TransportKind::kInMemory, {}};
}

void TypedEndpointsFromSpec()
{
    auto context = runtime::RuntimeContext::Create();
    assert(context.ok());
    runtime::ComponentContext component_context(core::ComponentName::Unsafe("fusion"), *context.value());

    runtime::ComponentSpec spec {core::ComponentName::Unsafe("fusion"), "",
        {transport::EndpointFor<Speed>(Topic("/speed"))},
        {transport::EndpointFor<Heading>(Topic("/heading"))},
        {}, {}};

    auto speed_reader = runtime::MakeTypedReader<Speed>(component_context, spec);
    auto heading_writer = runtime::MakeTypedWriter<Heading>(component_context, spec);
    assert(speed_reader.ok() && heading_writer.ok());
    assert(speed_reader.value().topic_name().str() == "/speed");
    assert(heading_writer.value().topic_name().str() == "/heading");

    // Direction matters: Speed is only read, Heading only written.
    const auto no_writer = runtime::MakeTypedWriter<Speed>(component_context, spec);
    assert(no_writer.status().code() == core::StatusCode::kNotFound);
    assert(no_writer.status().message().find("test.Speed") != std::string::npos);
    assert(runtime::MakeTypedReader<Heading>(component_context, spec).status().code() == core::StatusCode::kNotFound);

    // Two endpoints of one type are ambiguous.
    spec.readers.push_back(transport::EndpointFor<Speed>(Topic("/speed/rear")));
    const auto ambiguous = runtime::MakeTypedReader<Speed>(component_context, spec);
    assert(ambiguous.status().code() == core::StatusCode::kInvalidArgument);
    assert(ambiguous.status().message().find("/speed/rear") != std::string::npos);
    assert(context.value()->Close().ok());
}

}  // namespace

int main()
{
    ParametersConvert();
    ConfigurationCarriesParameters();
    TypedEndpointsFromSpec();
    return 0;
}
