// One binary for every node of the vehicle pipeline: the configuration file
// decides which components run in this process and over which transport.
//
//     vehicle_node config/sensors.json      # wheel + IMU sensors
//     vehicle_node config/estimator.json    # state estimator
//     vehicle_node config/monitor.json      # state monitor
//     vehicle_node config/single_process.json
//
// Also accepts --validate and --describe; see RunJsonApplication.

#include <memory>

#include <puppet_master/configuration/json_launcher.h>
#include <puppet_master/transport/shm/shm_transport.h>

#include "estimator.h"
#include "monitor.h"
#include "sensors.h"

namespace config = puppet_master::configuration;
namespace core = puppet_master::core;
namespace runtime = puppet_master::runtime;
namespace shm = puppet_master::transport::shm;

namespace {

template <typename T>
config::ComponentFactory Factory()
{
    return [](const runtime::ComponentSpec& spec) {
        return core::Result<runtime::ComponentPtr>(runtime::ComponentPtr(std::make_shared<T>(spec)));
    };
}

}  // namespace

int main(int argc, char** argv)
{
    config::ComponentFactoryRegistry factories;
    factories.Register("wheel_speed_sensor", Factory<vehicle::WheelSpeedSensor>());
    factories.Register("imu_sensor", Factory<vehicle::ImuSensor>());
    factories.Register("state_estimator", Factory<vehicle::StateEstimator>());
    factories.Register("state_monitor", Factory<vehicle::StateMonitor>());

    config::ApplicationOptions options;
    options.register_transports = [](runtime::RuntimeContext& context) {
        return shm::RegisterTransport(context, core::TransportName::Unsafe("shm"));
    };
    return config::RunJsonApplication(argc, argv, factories, options);
}
