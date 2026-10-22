# Configuration System

PuppetMaster now has a code-first configuration layer under
`include/puppet_master/configuration`. The goal is to make a small project easy
to assemble without scattering topic names, endpoint policies, and trigger
metadata across every component implementation.

The C++ model is the stable part. File formats are thin loaders that produce
the same `ProjectConfig`; JSON is available through an optional adapter (see
[JSON Loading](#json-loading)).

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

## Component Parameters

`ComponentConfig::parameters` holds a component's own settings, such as gains
or limits, as a `runtime::ParameterMap` of key to text. They are copied into
`ComponentSpec::parameters`, and the component reads them with
`puppet_master/runtime/parameters.h`:

```cpp
auto gain = runtime::GetParameter(spec_.parameters, "wheel_gain", 0.8);  // fallback when unset
auto port = runtime::GetRequiredParameter<std::uint16_t>(spec_.parameters, "port");
if (!gain.ok()) {
    return gain.status();  // InvalidArgument naming the key
}
```

`GetParameter<T>` converts to `std::string`, `bool` (`true`/`false`),
integers (range-checked), and floating point, and returns the fallback when
the key is missing. `GetRequiredParameter<T>` returns `NotFound` instead. The
configuration layer does not interpret parameters; each component checks its
own in `Configure`.

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

When built with `PUPPETMASTER_ENABLE_JSON_CONFIG=ON`, the demo accepts an
optional `config.json` path and loads the project from it instead.

That demo wires the lifecycle by hand to show each step. Applications should
use `configuration::Application` or `RunJsonApplication`, which do it for
them; see [Launcher And Configuration Tools](launcher.md).

## JSON Loading

`PuppetMaster::JsonConfigAdapter` is built when
`PUPPETMASTER_ENABLE_JSON_CONFIG=ON` and requires an installed `nlohmann_json`
package. It exposes two functions in
`puppet_master/configuration/json_loader.h`:

```cpp
auto from_text = config::ParseJsonConfig(json_text);
auto from_file = config::LoadJsonConfig("project.json");
```

Both return `core::Result<ProjectConfig>` and run `ProjectConfig::Validate()`
before returning. Malformed JSON, wrong value types, and unknown fields return
`InvalidArgument` with the offending path. A file that cannot be opened returns
`Unavailable`.

The JSON fields mirror the C++ model. Durations use nanosecond integers
(`period_ns`, `deadline_ns`), and enum values use the same names accepted by the
string parsers. A component's `"parameters"` is an object of strings, numbers,
and booleans; numbers and booleans are stored as their JSON text, so `30`,
`0.5`, and `true` read back through `GetParameter` as written:

```json
{
    "scheduler": {"worker_count": 2},
    "topics": [{
        "id": "speed",
        "name": "/vehicle/speed",
        "transport": "in_memory",
        "message_type": "demo.Speed",
        "encoding": "text/plain",
        "message_policy": {"freshness": "queued", "queue_depth": 8}
    }],
    "components": [{
        "name": "speed_monitor",
        "readers": ["speed"],
        "parameters": {"max_speed": 40.0},
        "triggers": [{"kind": "data", "data_topics": ["speed"]}]
    }]
}
```

## Multi-File Configuration

When processes are built and released by different teams, no single file can
describe the system. Configuration then splits into three layers, each kept
by the team that decides it:

| Layer | Contents | Kept by |
|---|---|---|
| Interface | topic names, message types, encodings, versions | the team that writes the topic, in its interface package |
| Node | components, parameters, triggers, reader policies | each team, in its node package |
| Deployment | each topic's transport and transport settings | the system integrator |

`examples/vehicle_pipeline` is a complete system built this way.

### Imports

An interface file declares only topics, and is installed with its package,
for example to `<prefix>/share/sensor_interfaces/topics.json`:

```json
{
    "topics": [
        {"id": "wheel", "name": "/vehicle/wheel_speed", "message_type": "vehicle.WheelSpeed",
         "message_version": 2, "encoding": "application/x-puppetmaster-trivial"}
    ]
}
```

A node file imports it instead of repeating the declaration:

```json
{
    "imports": ["sensor_interfaces/topics.json"],
    "components": [{"name": "wheel_speed_sensor", "type": "wheel_speed_sensor", "writers": ["wheel"], ...}]
}
```

`LoadJsonConfig` resolves each import against the importing file's
directory first, then against each search path in order. Search paths come
from `JsonLoadOptions::search_paths`, the `PUPPETMASTER_CONFIG_PATH`
environment variable (`:`-separated, `;` on Windows), or `--config-path DIR`
on launchers and `puppet_master_config`. Point them at the `share`
directories of installed packages.

- Imported files may import others; the topics land in declaration order. A
  file reached twice is read once, and a cycle is an error.
- Imported files may contain only `imports` and `topics`.
- A topic id declared twice, in any file, is an error that names the file of
  the first declaration.
- Errors inside an imported file name that file.
- `ParseJsonConfig` has no file to resolve against, so `imports` is an error
  there.

### Reader Policies

Queue depth and freshness are a reader's choice, not part of a topic's
identity, so they belong to the node. A reader may be an object naming the
topic, with policy fields that start from the topic's own policy:

```json
"readers": [
    {"topic": "wheel", "freshness": "queued", "queue_depth": 8},
    "imu"
]
```

The plain `"imu"` keeps the topic's policy. The override applies to this
component's reader only. In C++ it is `ComponentConfig::reader_policies`,
keyed by topic id, and every key must also be in `readers`.

### Deployment Files

A deployment file decides where topics run. It is applied after the project
loads, with `JsonLoadOptions::deployment_path` or `--deployment FILE`:

```json
{
    "defaults": {"transport": "shm", "transport_parameters": {"domain_id": 0}},
    "topics": [
        {"name": "/vehicle/wheel_speed", "transport_parameters": {"slot_payload_size": 256, "slot_count": 32}}
    ]
}
```

- Topics are matched by wire name, which is what interface packages publish.
  Topics a process does not declare are skipped, so one deployment serves
  every process.
- Only `transport` and `transport_parameters` can be set. A deployment that
  tries to change a topic's type, encoding, or version is rejected: those
  belong to the interface.
- `transport_parameters` lands in `TopicConfig::transport_parameters` as
  text. Each transport adapter reads its own keys; the shared-memory adapter
  reads them with `shm::OptionsFromProject` (see
  [Shared-Memory Transport](shm-transport.md#configuration)).

### System Validation

Each process loads only its own file, so a problem between processes, such as
a topic nobody writes, is invisible to it. `ValidateSystem` checks several
loaded projects together; `puppet_master_config validate --system` runs it on
files:

```bash
PUPPETMASTER_CONFIG_PATH=/opt/vehicle/share \
puppet_master_config validate --system --deployment deploy.json \
    /opt/vehicle/share/sensor_node/sensors.json \
    /opt/vehicle/share/estimator_node/estimator.json \
    /opt/vehicle/share/monitor_node/monitor.json
```

It reports as errors a topic declared with different types, encodings,
versions, or transports in different processes; a topic that is read but
written by no process; and a topic written by more than one process. A topic
written but read by no process is a warning. Topics that a process declares
but does not use are ignored.

Run it where every process's configuration is available: in system
integration CI, and before starting the processes on a target.
