# Configuration System

PuppetMaster now has a code-first configuration layer under
`include/puppet_master/configuration`. The goal is to make a small project easy
to assemble without scattering topic names, endpoint policies, and trigger
metadata across every component implementation.

This milestone intentionally does not introduce YAML, JSON, or TOML parsing
yet. The public model is the stable part. File formats can be added later as
thin loaders that produce the same `ProjectConfig`.

## Model

`ProjectConfig` is the top-level declaration:

- `TopicConfig` describes a logical topic id, transport kind, wire topic name,
  message type, encoding, and message policy.
- `ComponentConfig` describes a component name, readers, writers, and triggers.
- `TriggerConfig` describes manual, periodic, data-driven, or task-dependency
  trigger metadata.

Topic ids are local configuration names such as `speed`. They are resolved into
real middleware topic names such as `/vehicle/speed` when endpoint specs are
built.

```cpp
namespace config = puppet_master::configuration;
namespace core = puppet_master::core;

core::MessagePolicy policy;
policy.freshness = core::FreshnessPolicy::kQueued;
policy.queue_depth = 8;

config::ProjectConfig project;
project.AddTopic(config::TopicConfig {
    "speed",
    "/vehicle/speed",
    core::TransportKind::kInMemory,
    "demo.Speed",
    "text/plain",
    policy
});

config::TriggerConfig trigger;
trigger.kind = core::TriggerKind::kData;
trigger.dependency_policy = core::DependencyPolicy::kAny;
trigger.data_topics = {"speed"};

project.AddComponent(config::ComponentConfig {
    "speed_monitor",
    "runs when speed data arrives",
    {"speed"},
    {},
    {trigger}
});
```

## Validation

Configuration validation is strict by design:

- topic ids and component names must be non-empty and whitespace-free
- duplicated topic ids are rejected
- duplicated component names are rejected
- topic message type and encoding are required
- referenced topics must exist
- a data trigger topic must also be declared as a component reader
- periodic triggers must have a positive period
- trigger-specific fields cannot be attached to the wrong trigger kind

```cpp
auto status = project.Validate();
if (!status.ok()) {
    std::cerr << status.ToString() << '\n';
}
```

## Building Runtime Specs

Configuration objects can be converted into the runtime contracts that already
exist in PuppetMaster:

```cpp
auto endpoint = project.BuildEndpoint("speed");
auto component = project.BuildComponentSpec("speed_monitor");
auto options = project.ToRuntimeOptions();
```

This keeps user code away from repetitive `EndpointConfig`, `TopicSpec`, and
`TriggerSpec` construction while still preserving the typed runtime API.

For projects that only need to register declarative component specs, use:

```cpp
auto context = puppet_master::runtime::RuntimeContext::Create();
config::ApplyComponentSpecs(project, *context.value());
```

For executable components, build the component spec from configuration and let
the component return it from `Describe()`:

```cpp
class SpeedMonitor final : public puppet_master::runtime::Component {
public:
    explicit SpeedMonitor(puppet_master::runtime::ComponentSpec spec)
        : spec_(std::move(spec))
    {
    }

    puppet_master::runtime::ComponentSpec Describe() const override
    {
        return spec_;
    }

private:
    puppet_master::runtime::ComponentSpec spec_;
};
```

## String Parsers

The configuration layer also includes small enum parsers for future file
loaders and command-line tools:

```cpp
auto transport = config::ParseTransportKind("fastdds");
auto trigger = config::ParseTriggerKind("data");
auto freshness = config::ParseFreshnessPolicy("queued");
```

These helpers normalize case and surrounding whitespace but still report typed
`Status` errors when the input is unknown.

## Example

See `demo/configuration_system_demo.cpp` for a runnable single-process pipeline:

1. Declare a `ProjectConfig`.
2. Build a runtime context from it.
3. Build a component spec from configuration.
4. Register the executable component.
5. Use the scheduler to run the component when data arrives.
