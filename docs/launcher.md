# Launcher And Configuration Tools

A configured application has the same lifecycle every time: create the
runtime, create and register components, configure, initialize, and start them
in order, start the scheduler, run until asked to stop, then stop and shut down
in reverse. The launcher layer does that once, so an application supplies only
its component factories.

## JSON Applications

With `PuppetMaster::JsonConfigAdapter`, a complete `main()` is:

```cpp
#include <puppet_master/configuration/json_launcher.h>

int main(int argc, char** argv)
{
    configuration::ComponentFactoryRegistry factories;
    factories.Register("speed_sensor", MakeSpeedSensor);
    factories.Register("speed_monitor", MakeSpeedMonitor);
    return configuration::RunJsonApplication(argc, argv, factories);
}
```

Each component's `"type"` in the configuration selects its factory. The
resulting binary accepts:

| Command | Effect | Exit code |
|---|---|---|
| `app config.json` | Run until SIGINT or SIGTERM (or Ctrl+Break on Windows), then shut down cleanly | 0, or 1 on error |
| `app --validate config.json` | Check the configuration and that every component type has a factory | 0 or 1 |
| `app --describe config.json` | Print topics, components, and triggers | 0 or 1 |
| `app --help` | Usage | 0 |

Bad usage exits with 2. Errors are printed with the file name, and validation
reports every unknown component type, not only the first.

`demo/launcher_demo.cpp` with `demo/config/speed_pipeline.json` is a runnable
example: a periodic sensor publishes typed speed samples that a data-triggered
monitor prints.

## Application

`configuration::Application` is the same machinery without JSON, for
applications that build their `ProjectConfig` in code:

```cpp
auto app = configuration::Application::Create(std::move(project), factories);
if (!app.ok()) { /* invalid configuration or unknown component type */ }

app.value()->InstallSignalHandlers();   // optional: SIGINT/SIGTERM stop Run()
auto status = app.value()->Run();       // Start(), wait, Stop()
```

- `Create` validates the project and every component type, creates the
  runtime, and creates and registers all components. Nothing is started yet.
- `Start` configures, initializes, and starts components in declaration order,
  then starts the scheduler. If any step fails, everything already brought up
  is stopped and shut down in reverse order, including the component that
  failed, and the error names the component and the step.
- `Stop` stops the scheduler first, which drains queued work, then stops and
  shuts down components in reverse order and closes the transports. It keeps
  going after errors and returns the first one. It is idempotent, and the
  destructor calls it.
- `Run` is `Start`, then a wait for `RequestStop()`, then `Stop`.
  `RequestStop()` is safe from any thread, including component callbacks.
- `InstallSignalHandlers` makes SIGINT and SIGTERM call `RequestStop()`, and
  SIGBREAK on Windows, which is how `puppet_master_launch` stops a node there.
  Only one application per process can own the handlers.

An application runs once; create a new one to run again.

### Other Transports

The runtime registers the in-memory transport by default. Register others
through `ApplicationOptions::register_transports`, which runs after the
runtime opens and before components are created:

```cpp
configuration::ApplicationOptions options;
options.register_transports = [](runtime::RuntimeContext& context) {
    return transport::shm::RegisterTransport(context, core::TransportName::Unsafe("shm"));
};
return configuration::RunJsonApplication(argc, argv, factories, options);
```

`register_project_transports` is the same hook with the loaded project, for
transports that take settings from it, such as shared-memory ring sizes from
a deployment file; see [Configuration](shm-transport.md#configuration).

### Imports And Deployments

`RunJsonApplication` loads with `LoadJsonConfig`, so `"imports"` resolve
through `PUPPETMASTER_CONFIG_PATH`, and it takes two more options:

| Option | Effect |
|---|---|
| `--config-path DIR` | also look for imports in DIR; repeatable, searched after the environment |
| `--deployment FILE` | apply a deployment file: per-topic transport and transport settings |

See [Multi-File Configuration](configuration.md#multi-file-configuration).

## Configuration Tool

`puppet_master_config` checks configuration files without building the
application, for CI and code review. It is built and installed with the JSON
adapter.

```bash
puppet_master_config validate config/*.json
puppet_master_config validate --system --deployment deploy.json nodes/*.json
puppet_master_config describe config/vehicle.json
puppet_master_config system validate system.json
puppet_master_config shm clean --dry-run
```

`validate` accepts several files, prints `ok` or every problem per file, and
exits with 1 if any file fails. It checks everything except component types,
because factories live in the application binary; use `app --validate` for
those.

`validate --system` also checks the files together as one system, for
example every node of a release in integration CI. See
[System Validation](configuration.md#system-validation). Both commands take
`--config-path` and `--deployment` like a launcher.

`system validate|describe|commands system.json` does the same from a system
manifest, which lists every node with its binary, config, and the search
path and deployment; `system commands` prints each node's start command for
launch scripts. See [System Manifests](configuration.md#system-manifests).

`shm list` and `shm clean` show and remove leftover shared-memory segments
(when the shared-memory adapter is built). See
[Listing And Cleaning Up](shm-transport.md#listing-and-cleaning-up).

## System Launcher

`puppet_master_launch` runs every node of a
[system manifest](configuration.md#system-manifests) and supervises them. It
is built and installed with the JSON adapter, next to `puppet_master_config`.

```bash
puppet_master_launch system.json
puppet_master_launch --log-dir logs system.json
```

| Option | Effect |
|---|---|
| `--log-dir DIR` | write each node's output to `DIR/<node>.log`, appended across restarts |
| `--no-validate` | skip the check of the nodes together before starting |

It runs as follows:

1. Check the manifest like `puppet_master_config system validate`, and exit
   with 1 on any error.
2. Start the nodes in manifest order, waiting each node's `start_delay_ms`
   before the next. Each node runs in a process group of its own.
3. Supervise them. A node that exits is restarted by its `restart` policy
   after `restart_delay_ms`. The delay doubles for each restart that follows
   a run shorter than 10 s, up to 30 s. A longer run resets the delay and the
   `max_restarts` count.
4. On SIGINT or SIGTERM (Ctrl+C or Ctrl+Break on Windows), stop the running
   nodes in reverse start order. Each node gets SIGTERM (Ctrl+Break on
   Windows), then is killed if it is still running after `stop_timeout_ms`.

The system also stops when a `required` node ends for good: it exited and
its policy allows no restart, or it failed to start. A node with
`"required": false` is left stopped and the others keep running. When every
node has ended, the launcher exits too.

The launcher prints one line per event to stderr: `started`, `start-failed`,
`exited`, `restarting`, `gave-up`, `stopping`, `killed`, and `stopped`. It
exits with 0 after a stop request or when every node ended successfully. It
exits with 1 when a required node failed or any node ended with a failure.

Component types are not checked here, because only each node's binary knows
its factories. Run `node --validate` in CI for that, as
`examples/vehicle_pipeline/integration/run_pipeline.sh` does before it
starts the system.

In C++, `configuration::Supervisor` in
`puppet_master/configuration/supervisor.h` does the same. Its
`ProcessLauncher` interface lets tests replace real processes.

## Topic Tool

`puppet_master_topic` looks at the shared-memory topics of a running system,
and its FastDDS topics when built with that adapter (see
[FastDDS Topics](#fastdds-topics)), and at its services (see
[Services](#services)). It is built and installed with the JSON and
shared-memory adapters.

```bash
puppet_master_topic list
puppet_master_topic list --system system.json
puppet_master_topic echo /vehicle/state --count 5
puppet_master_topic hz /vehicle/state --window-ms 500
```

| Command | Effect |
|---|---|
| `list` | the topics with a segment on this host: type, ring size, state, messages written, attached pids (Linux) |
| `list --system FILE` | the topics the manifest's nodes declare, with their ring size and which components write and read them |
| `echo TOPIC` | each message's sequence, size, and latency, and its first `--bytes N` bytes in hex (32 by default), or `--text` |
| `hz TOPIC` | every `--window-ms` (1000 by default): the rate, bytes per second, shortest and longest interval, and losses |

`echo` and `hz` subscribe like any other reader and change nothing for the
writer. They start after the newest message and queue up to one ring. A
message they fall behind on is overwritten in the ring and shown as lost.
Intervals are measured between publish times, so the tool's own delays do
not show up as jitter.

The topic's type and ring size come from its segment header, which needs
Linux and a writer that has started. With `--system FILE` they come from
the manifest's nodes and deployment instead, which also works on Windows
and before the writer starts. `--domain N` picks a domain when a topic
exists in several.

`echo --count N` stops after N messages. With `--timeout-ms`, it exits with
1 if fewer arrived, so scripts can wait for traffic. `hz --duration-ms N`
stops after N ms. Ctrl+C stops either one.

### FastDDS Topics

When PuppetMaster is built with the FastDDS adapter as well, the tool also
reads and writes FastDDS topics. They are found only with `--system FILE`,
since they have no segment to read, and are joined in domain `--domain N`
(0 by default): the FastDDS domain is chosen in each application's code, not
in its configuration. The tool's reader asks for the topic's `delivery`, so
it matches the system's writers.

```bash
puppet_master_topic list --system system.json --domain 3
puppet_master_topic echo /vehicle/objects --system system.json --domain 3 --text
```

FastDDS carries neither the writer's sequence numbers nor its publish times,
so `echo` and `hz` show no losses and no latency, and intervals are measured
by arrival. A tool built without FastDDS reports that it cannot read such a
topic.

The tool also records and replays topics:

| Command | Effect |
|---|---|
| `record [TOPIC...] -o FILE` | write the topics' messages to FILE; with no topic named, every shm topic found (or the system's, with `--system`); stops after `--count N` messages, `--duration-ms N`, or Ctrl+C |
| `replay FILE [TOPIC...]` | publish the recording's messages again, all or the topics named, at the recorded pace times `--rate X` (`0`: at once) |
| `info FILE` | the recording's topics, message counts, sizes, rates, and length |

See [Recordings](recording.md).

### Services

The tool also looks at a system's services and calls them. Both commands
need `--system FILE`; `--domain N` picks the domain as for topics.

```bash
puppet_master_topic service list --system system.json
puppet_master_topic service call /map/query --system system.json --data "a b" --text
puppet_master_topic service call /map/query --system system.json --hex "01 00 2a"
```

| Command | Effect |
|---|---|
| `service list` | each service the manifest's nodes declare: request and reply types, transport, which components serve and call it, and whether a server is up, with its instance id and the age of its newest heartbeat |
| `service call SERVICE` | send one request, the bytes of `--data TEXT`, of `--hex HEX`, of `--field TYPE:VALUE`s (see [Typed Requests](#typed-requests)), or none, and print the reply's size, round trip, and bytes (in hex, or `--text`; all of them unless `--bytes N`), or its fields with `--reply-fields` |

`service list` reads each service's presence topic only, so it creates no
client and no reply topic. It listens for three heartbeat intervals (at
least 1 s on FastDDS, for discovery), or `--timeout-ms N`. When no heartbeat
arrives in that time, on shared memory it reads the last presence message
left in the topic's segment (`shm::ReadNewestMessage`, without attaching):

| `server:` | Meaning |
|---|---|
| `up, instance ID, heartbeat N ms ago` | a heartbeat within three intervals |
| `stopped (instance ID said goodbye N ms ago)` | the server closed in order |
| `down (instance ID stopped without a goodbye; last heartbeat N ms ago)` | its heartbeats stopped: a crash, a kill, or a server too busy to keep its interval |
| `down (no heartbeat in N ms)` | nothing to read: never started, on FastDDS, or on Windows after its last process closed the topic |

Clients send no heartbeats, so `clients` lists the components that declare
the service, not the processes running.

`service call` uses an ordinary `service::Client`, so the server handles the
request like any other. It waits up to `--timeout-ms N` (2000 by default)
for a server and its reply, then exits with 1 and the call's status, such as
`unavailable` (no live server), `deadline_exceeded`, or the handler's own
error and message.

#### Typed Requests

For a service whose messages use the [portable codec](typed-messages.md#portable-codec),
`--field TYPE:VALUE` builds the request field by field, in the order of the
type's `PortableFields`, and `--reply-fields TYPE,...` decodes the reply the
same way. The tool has no C++ types of its own, so the field list is yours to
match:

```bash
# struct PoseQuery { std::string frame; std::uint32_t id; std::vector<double> weights; };
# struct Pose { double x, y; std::string frame; std::array<float, 2> covariance; bool valid; };
puppet_master_topic service call /map/pose --system system.json \
    --field string:map --field u32:7 --field "f64[]:1, 2.5" \
    --reply-fields "f64,f64,string,f32[2],bool"
```

```text
call /map/pose: server 7763ad3f9789659a, request 31 B
reply 32 B in 0.6 ms
  1 f64: 10.5
  2 f64: 3.5
  3 string: "map"
  4 f32[2]: [0.25, -1]
  5 bool: true
```

| Type | Value |
|---|---|
| `bool` | `true`, `false`, `1`, or `0` |
| `i8` `i16` `i32` `i64`, `u8` `u16` `u32` `u64` | decimal or `0x` hex, within the type's range |
| `f32`, `f64` | a decimal number, such as `1.5` or `-3e2` |
| `string` | the rest of the argument, as it is |
| `T[]` | a vector: elements separated by commas; empty for none |
| `T[N]` | an array of exactly N elements |

A field of a nested type is its own fields in order, so list those. Enums are
their underlying integer type. String elements of a vector cannot contain a
comma.

A request missing a field comes back as the server's decode error
(`data_loss: ...: message ends inside a field`). A reply that does not match
`--reply-fields` exits with 1, names the first field that failed or the bytes
left over, and prints the reply's bytes so the list can be corrected. A
change to a message's fields also changes its version, so check the
`request` and `reply` versions `service list` shows.

## Library Helpers

These are in the core library and need no JSON:

- `ValidateSystem(projects)` returns a `SystemReport` with the problems
  between processes; see [System Validation](configuration.md#system-validation).
- `ValidateProject(project, &factories)` returns a `ValidationReport` with
  every problem found. Pass no registry to skip component-type checks.
- `DescribeProject(project)` returns the summary that `--describe` prints.
