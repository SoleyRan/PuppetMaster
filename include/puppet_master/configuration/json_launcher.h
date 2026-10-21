#pragma once

#include <puppet_master/configuration/application.h>
#include <puppet_master/export.h>

namespace puppet_master::configuration {

// Complete main() for a JSON-configured application:
//
//     int main(int argc, char** argv)
//     {
//         configuration::ComponentFactoryRegistry factories;
//         factories.Register("speed_monitor", MakeSpeedMonitor);
//         return configuration::RunJsonApplication(argc, argv, factories);
//     }
//
// Command line: `[--validate | --describe] <config.json>`. Without a flag it
// runs until SIGINT or SIGTERM, then shuts down cleanly. Returns the process
// exit code: 0 on success, 1 on a configuration or runtime error, 2 on bad
// usage. Part of PuppetMaster::JsonConfigAdapter.
PUPPET_MASTER_API int RunJsonApplication(
    int argc, char** argv, const ComponentFactoryRegistry& factories, ApplicationOptions options = {});

}  // namespace puppet_master::configuration
