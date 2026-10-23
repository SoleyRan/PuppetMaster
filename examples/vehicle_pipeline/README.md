# Vehicle Pipeline Example

A vehicle state pipeline built the way separate teams would ship it: two
interface packages, three node packages, and a system integration. Each
package is its own CMake project, installed into a shared prefix and found
only through it.

```
sensor_node ──wheel, imu──► estimator_node ──state──► monitor_node
```

| Package | Owner | Contents |
|---|---|---|
| `sensor_interfaces` | sensor team | `WheelSpeed`, `ImuSample`; `share/sensor_interfaces/topics.json` |
| `estimation_interfaces` | estimation team | `VehicleState`; its `topics.json` imports the sensor topics |
| `sensor_node` | sensor team | wheel speed and IMU publishers |
| `estimator_node` | estimation team | fuses both inputs into `VehicleState` |
| `monitor_node` | monitoring team | checks states, counts sequence gaps |
| `integration/` | system integrator | `system.json`, `deploy.json`, run scripts |

Configuration is split into three layers, each owned by the team that
decides it:

- **Interface** (`topics.json` in an interface package): topic names, message
  types, encodings, and versions. Owned by the team that writes the topic.
- **Node** (`config/*.json` in a node package): components, parameters,
  triggers, and each reader's queue policy. It imports the interface files
  it needs and names no transport.
- **Deployment** (`integration/deploy.json`): which transport each topic
  uses and its shared-memory ring size. It can change where a topic runs,
  but not what it carries.

## Message Versions

`sensor_interfaces` takes `WHEEL_SPEED_VERSION` from its `CMakeLists.txt`
and generates both `messages.h` and `topics.json` from it, so the struct and
its declaration cannot drift. Raise it whenever the struct changes in a way
old readers would misread.

A process built against another version is refused rather than misreading:

- `puppet_master_config validate --system` reports the topic with both
  versions and the files that declare them;
- if the processes run anyway, the shared-memory segment rejects the second
  version when the endpoint is created.

`integration/check_version_skew.sh` builds a v3 sensor release next to the
v2 estimator and checks both cases.

## Build And Run

PuppetMaster must be installed with the JSON adapter (the shared-memory
adapter is on by default). On Linux:

```bash
cmake -S /path/to/PuppetMaster -B pm-build -DPUPPETMASTER_ENABLE_JSON_CONFIG=ON
cmake --build pm-build --parallel
cmake --install pm-build --prefix "$HOME/pm-install"

# Each package in its own build tree, installed into one prefix.
integration/build_all.sh "$HOME/pm-install" "$HOME/vehicle"

# System check, then three processes over shared memory for 3 seconds.
integration/run_pipeline.sh "$HOME/vehicle" "$HOME/pm-install/bin/puppet_master_config" 3
```

`integration/system.json` is the system manifest: it lists the three nodes in
start order with their binaries and config files, the import search path,
and the deployment. `run_pipeline.sh` reads everything from it, so the
system check, CI, and the launch use the same node list:

```bash
export VEHICLE_PREFIX="$HOME/vehicle"   # the manifest's ${VEHICLE_PREFIX}
puppet_master_config system validate integration/system.json
puppet_master_config system commands integration/system.json   # start commands
```

To run a node by hand, point the import search path at the shared `share`
directory and pass the deployment:

```bash
export PUPPETMASTER_CONFIG_PATH="$HOME/vehicle/share"
"$HOME/vehicle/bin/monitor_node" --deployment integration/deploy.json \
    "$HOME/vehicle/share/monitor_node/monitor.json"
```

`--config-path DIR` adds search directories on the command line, and every
node binary also accepts `--validate` and `--describe`.

## Known Limits

- Shared-memory segments persist in `/dev/shm` after the processes exit;
  `puppet_master_config shm list` shows them and `shm clean` removes those no
  running process uses. See
  [Listing And Cleaning Up](../../docs/shm-transport.md#listing-and-cleaning-up).
- `TrivialCodec` copies structs byte for byte, so every process must use the
  same compiler ABI. Versions catch layout changes the owner declares, not
  ABI differences between compilers; across those, use
  [`PortableCodec`](../../docs/typed-messages.md#portable-codec).
