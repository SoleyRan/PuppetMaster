# Observability

PuppetMaster exposes communication and scheduling state through one
runtime-wide `observability::Observer`. The observer is deliberately independent
from logging, metrics, and tracing products so an application can choose its
own exporters without changing middleware code.

## Architecture

Runtime instrumentation emits typed events into the observer:

```text
transport reader/writer ----\
                             > Observer ---> event callback
scheduler ------------------/            ---> metrics callback
                                          ---> structured log callback
                                          ---> point-in-time snapshot
```

The callbacks are the stable extension boundary. A Prometheus exporter can
consume snapshots, a Chrome trace exporter can consume events, and a JSON
logger can consume `LogRecord` values.

## Collected Metrics

Topic metrics include:

- published and received message counts
- published and received byte counts
- lifetime-average message and byte throughput
- last, average, and maximum end-to-end latency
- current and maximum reader queue depth

Task metrics include:

- execution count
- failure count
- last, average, and maximum execution time
- deadline miss count

Latency uses the message source timestamp and the local steady clock. A
transport that cannot preserve compatible timestamps should report no latency
instead of fabricating one.

Queue depth is an optional reader capability. The in-memory transport reports
it directly; external transports may leave it unsupported until they can
provide a meaningful value.

For periodic triggers, the configured period is currently used as the
execution deadline. Manual and data triggers collect execution time but do not
infer a deadline.

## Runtime Hooks

Callbacks can be supplied when the runtime is created:

```cpp
runtime::RuntimeOptions options;
options.observability_options.event_callback =
    [](const observability::Event& event) {
        // Forward lightweight trace data to an asynchronous exporter.
    };
options.observability_options.metrics_callback =
    [](const observability::MetricsSnapshot& snapshot) {
        // Export or display the current aggregate metrics.
    };

auto context = runtime::RuntimeContext::Create(std::move(options));
context.value()->observer()->PublishMetrics();
```

Callbacks execute synchronously on the calling thread. They should do a small
amount of work or enqueue data for an asynchronous exporter. Exceptions thrown
by callbacks are contained and never escape into middleware execution.

Applications can also call `Snapshot()` at any time. Snapshots are copies and
can be formatted or exported without holding the observer lock.

## Default Logging

PuppetMaster includes a small standard-library console logger. It is enabled by
default and does not require GoodLog, Boost.Log, or another logging package.

Middleware records are written to `std::clog` with timestamp, severity,
component, event, message, and structured fields:

```text
[2026-07-24 14:30:21.083]<warning>[puppet_master][component=localization][event=task_deadline_missed] component execution exceeded its periodic deadline execution_us=8068 deadline_us=5000
```

The stream-style compatibility API is available from an explicit header:

```cpp
#include <puppet_master/logging/log.h>

LOG_Info() << "runtime started";
LOG_Warn() << "queue depth=" << queue_depth;
LOG_Error() << "transport failed";
```

These macros write through the same process-wide sink used by the default
Observer callback. Applications can replace the sink without changing call
sites:

```cpp
logging::SetSink([](const observability::LogRecord& record) {
    // Forward to another logging backend.
});
```

Set `observability::Options::use_default_log_sink` to `false` when an Observer
should remain silent until a callback is installed explicitly.

## Unified Logs With GoodLog

GoodLog is an optional adapter because its Boost.Log, OpenSSL, and ZLIB
dependencies should not be forced on applications that only need metrics or
trace events.

Enable it during configuration:

```bash
cmake -S . -B build \
  -DPUPPETMASTER_ENABLE_GOODLOG=ON
cmake --build build
```

CMake first searches for an installed `GoodLog` package. If one is not found,
it fetches the pinned revision from:

<https://github.com/SoleyRan/GoodLog>

For an offline build, copy GoodLog locally and point FetchContent to it:

```bash
cmake -S . -B build \
  -DPUPPETMASTER_ENABLE_GOODLOG=ON \
  -DFETCHCONTENT_SOURCE_DIR_GOODLOG=/path/to/GoodLog
```

Downstream targets link the adapter explicitly:

```cmake
find_package(PuppetMaster CONFIG REQUIRED)
target_link_libraries(
    my_runtime
    PRIVATE
        PuppetMaster::PuppetMaster
        PuppetMaster::GoodLogAdapter
)
```

Install the sink after creating the runtime:

```cpp
#include <puppet_master/observability/goodlog_sink.h>

observability::goodlog::SinkOptions log_options;
log_options.directory = "/tmp/puppet_master/";
log_options.console_level = observability::LogLevel::kInfo;
log_options.file_level = observability::LogLevel::kDebug;

auto status = observability::goodlog::InstallSink(
    context->observer(),
    log_options);
```

`InstallSink()` replaces the process-wide built-in sink. After installation,
both middleware `Observer::Log()` records and `LOG_Info()` style calls are
written by GoodLog.

Middleware log messages share this shape:

```text
[puppet_master][component=slow_localization][event=task_deadline_missed]
component execution exceeded its periodic deadline execution_us=8071 deadline_us=5000
```

GoodLog adds timestamp, severity, source location, terminal colors, file
rotation, and optional gzip or AES-256-GCM handling around that message.

## Demo

The observability demo publishes and receives local messages, runs a periodic
task that intentionally exceeds its deadline, and then prints the resulting
topic and task metrics:

```bash
./build/demo/puppet_master_observability_demo
```

With `PUPPETMASTER_ENABLE_GOODLOG=ON`, the demo installs the GoodLog sink.
Otherwise it uses the built-in console logger. The default GoodLog directory is:

```text
/tmp/puppet_master/
```

## Exporter Direction

Future exporters should remain separate adapters:

- Prometheus: convert `MetricsSnapshot` fields to counters, gauges, and
  histograms.
- JSON log: serialize `LogRecord` without changing runtime instrumentation.
- Chrome trace: convert task and topic events into trace events with timestamps
  and durations.

Keeping these exporters outside the core prevents production integrations from
adding dependencies or blocking behavior to every PuppetMaster application.
