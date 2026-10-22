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
| `app config.json` | Run until SIGINT or SIGTERM, then shut down cleanly | 0, or 1 on error |
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
- `InstallSignalHandlers` makes SIGINT and SIGTERM call `RequestStop()`. Only
  one application per process can own the handlers.

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
```

`validate` accepts several files, prints `ok` or every problem per file, and
exits with 1 if any file fails. It checks everything except component types,
because factories live in the application binary; use `app --validate` for
those.

`validate --system` also checks the files together as one system, for
example every node of a release in integration CI. See
[System Validation](configuration.md#system-validation). Both commands take
`--config-path` and `--deployment` like a launcher.

## Library Helpers

These are in the core library and need no JSON:

- `ValidateSystem(projects)` returns a `SystemReport` with the problems
  between processes; see [System Validation](configuration.md#system-validation).
- `ValidateProject(project, &factories)` returns a `ValidationReport` with
  every problem found. Pass no registry to skip component-type checks.
- `DescribeProject(project)` returns the summary that `--describe` prints.
