#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include <puppet_master/configuration/application.h>
#include <puppet_master/configuration/configuration.h>
#include <puppet_master/configuration/json_loader.h>
#include <puppet_master/core/result.h>
#include <puppet_master/export.h>

// A system manifest lists every process of one system, so the system check,
// launch scripts, and CI read one file instead of each keeping its own list.
// Part of PuppetMaster::JsonConfigAdapter. See
// docs/configuration.md#system-manifests.
namespace puppet_master::configuration {

struct NodeManifest {
    std::string name;
    // Program to start. Resolved on load: an absolute path is kept, a name is
    // looked up in the manifest's bin_paths and kept as is if not found, for
    // PATH lookup when started.
    std::string executable;
    // The node's project file. Resolved on load like an import: next to the
    // manifest, then in the manifest's config_paths.
    std::string config;
    std::vector<std::string> arguments;  // passed after the launcher options
    // How long to wait after starting this node before starting the next.
    std::chrono::milliseconds start_delay {0};

    // Supervision, for puppet_master_launch.
    enum class Restart : std::uint8_t {
        kNever = 0,   // an exit is final
        kOnFailure,   // restart after a non-zero exit or a signal
        kAlways,      // restart after any exit
    };
    Restart restart {Restart::kNever};
    // Restarts allowed before giving up on the node; 0 means no limit. A run
    // longer than SupervisorOptions::stable_run resets the count.
    std::uint32_t max_restarts {5};
    // Wait before a restart. It doubles for each restart after a short run
    // (SupervisorOptions::stable_run, 10 s by default), up to 30 s, and is
    // reset by a long one.
    std::chrono::milliseconds restart_delay {std::chrono::milliseconds(500)};
    // A required node that exits for good (no restart left) stops the whole
    // system; an optional one is left stopped.
    bool required {true};
    // After asking the node to stop, how long to wait before killing it.
    std::chrono::milliseconds stop_timeout {std::chrono::milliseconds(5000)};
};

PUPPET_MASTER_API const char* ToString(NodeManifest::Restart restart) noexcept;

struct SystemManifest {
    std::string name;
    std::string path;  // the manifest file
    // Absolute after loading; relative entries resolve against the manifest.
    std::vector<std::string> config_paths;
    std::vector<std::string> bin_paths;
    std::string deployment;  // empty if none
    std::vector<NodeManifest> nodes;  // start order

    // Load options for this system's node files: config_paths, then
    // PUPPETMASTER_CONFIG_PATH, and the deployment.
    PUPPET_MASTER_API JsonLoadOptions LoadOptions() const;

    // The command that starts `node`: executable, the launcher's
    // --config-path and --deployment options, the node's own arguments, and
    // its config file.
    PUPPET_MASTER_API std::vector<std::string> CommandFor(const NodeManifest& node) const;
};

// Reads a manifest and resolves its paths. Fails on unknown fields,
// duplicate node names, and node configs that cannot be found; node files
// are located but not parsed.
PUPPET_MASTER_API core::Result<SystemManifest> LoadSystemManifest(const std::string& path);

struct LoadedNode {
    std::string name;
    ProjectConfig project;
};

// Loads every node's project with the manifest's search paths and
// deployment. Fails on the first node that does not load, naming it.
PUPPET_MASTER_API core::Result<std::vector<LoadedNode>> LoadSystemNodes(const SystemManifest& manifest);

// ValidateSystem over every node of the manifest, labelled by node name.
PUPPET_MASTER_API core::Result<SystemReport> ValidateSystemManifest(const SystemManifest& manifest);

}  // namespace puppet_master::configuration
