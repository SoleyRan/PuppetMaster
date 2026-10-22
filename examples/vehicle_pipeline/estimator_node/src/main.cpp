// Estimation team's process: fuses sensor data into a vehicle state.
//
//     estimator_node [--deployment deploy.json] share/estimator_node/estimator.json
//
// Imports resolve through PUPPETMASTER_CONFIG_PATH or --config-path; the
// deployment file decides transports and shared-memory sizes.

#include <memory>

#include <puppet_master/configuration/json_launcher.h>
#include <puppet_master/transport/shm/shm_transport.h>

#include "estimator.h"

namespace config = puppet_master::configuration;
namespace core = puppet_master::core;
namespace runtime = puppet_master::runtime;
namespace shm = puppet_master::transport::shm;

template <typename T>
config::ComponentFactory Factory()
{
    return [](const runtime::ComponentSpec& spec) {
        return core::Result<runtime::ComponentPtr>(runtime::ComponentPtr(std::make_shared<T>(spec)));
    };
}

int main(int argc, char** argv)
{
    config::ComponentFactoryRegistry factories;
    factories.Register("state_estimator", Factory<estimator_node::StateEstimator>());

    config::ApplicationOptions options;
    options.register_project_transports = [](runtime::RuntimeContext& context, const config::ProjectConfig& project) {
        auto shm_options = shm::OptionsFromProject(project);
        if (!shm_options.ok()) {
            return shm_options.status();
        }
        return shm::RegisterTransport(context, core::TransportName::Unsafe("shm"), std::move(shm_options).value());
    };
    return config::RunJsonApplication(argc, argv, factories, options);
}
