# Vehicle Pipeline Example

A downstream project that uses an installed PuppetMaster through
`find_package`, the way a real application would. It runs a small vehicle
state pipeline either in one process or split across three processes over
shared memory.

```
sensors.json                 estimator.json               monitor.json
wheel_speed_sensor ─┐
                    ├─ shm ─► state_estimator ─ shm ─► state_monitor
imu_sensor ─────────┘
```

- `wheel_speed_sensor`, `imu_sensor`: periodic triggers at 100 Hz, publish
  simulated `WheelSpeed` and `ImuSample`.
- `state_estimator`: data trigger on any of its inputs; fuses the newest
  values into a `VehicleState`.
- `state_monitor`: data trigger; checks each state, counts sequence gaps, and
  prints a summary at shutdown. With `max_states` set it stops the process.

One binary, `vehicle_node`, serves every node; the configuration file decides
which components run and over which transport. Component settings such as
`cruise_speed` and `wheel_gain` come from each component's `"parameters"`.

## Build

PuppetMaster must be installed with the JSON adapter (the shared-memory
adapter is on by default):

```bash
cmake -S /path/to/PuppetMaster -B pm-build -DPUPPETMASTER_ENABLE_JSON_CONFIG=ON
cmake --build pm-build --parallel
cmake --install pm-build --prefix "$HOME/pm-install"

cmake -S examples/vehicle_pipeline -B example-build -DCMAKE_PREFIX_PATH="$HOME/pm-install"
cmake --build example-build
```

## Run

In one process, over the in-memory transport; stops after 100 states:

```bash
example-build/vehicle_node config/single_process.json
```

As three processes over shared memory, each in its own terminal and stopped
with Ctrl+C:

```bash
example-build/vehicle_node config/monitor.json
example-build/vehicle_node config/estimator.json
example-build/vehicle_node config/sensors.json
```

`run_pipeline.sh` does the same on Linux: it validates the three files,
starts the processes, stops them after a few seconds (producers first), and
fails unless the monitor received states with no sequence gaps and none out
of range.

```bash
./run_pipeline.sh example-build/vehicle_node 3
```

Every binary also accepts `--validate` and `--describe`.

## Known Limits

- Each process's file repeats the declarations of the topics it shares with
  other processes; nothing checks that they agree until the shared-memory
  segment rejects a mismatched descriptor or geometry at runtime.
- Shared-memory segments persist in `/dev/shm` after the processes exit; see
  [Segment Lifetime](../../docs/shm-transport.md#segment-lifetime).
- `TrivialCodec` copies structs byte for byte, so every process must be built
  with the same compiler ABI.
