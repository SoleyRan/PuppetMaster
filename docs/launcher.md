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

`puppet_master_topic` looks at the shared-memory topics of a running system.
It is built and installed with the JSON and shared-memory adapters.

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

## Library Helpers

These are in the core library and need no JSON:

- `ValidateSystem(projects)` returns a `SystemReport` with the problems
  between processes; see [System Validation](configuration.md#system-validation).
- `ValidateProject(project, &factories)` returns a `ValidationReport` with
  every problem found. Pass no registry to skip component-type checks.
- `DescribeProject(project)` returns the summary that `--describe` prints.
