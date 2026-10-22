#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <puppet_master/configuration/configuration.h>
#include <puppet_master/export.h>

namespace puppet_master::configuration {

// Where `"imports"` are looked up. An import is first resolved against the
// directory of the file that names it, then against each search path in
// order, so an installed interface package is found as
// "<package>/topics.json" under its share directory.
struct JsonLoadOptions {
    std::vector<std::string> search_paths;
    // A deployment file applied after the project loads: it may override
    // each topic's transport and transport parameters, but not its identity.
    // Empty means none.
    std::string deployment_path;
};

// Search paths from the PUPPETMASTER_CONFIG_PATH environment variable,
// separated by ':' (';' on Windows). Empty entries are skipped.
PUPPET_MASTER_API std::vector<std::string> SearchPathsFromEnvironment();

// Parses a project from text. `"imports"` is rejected because there is no
// file to resolve it against; use LoadJsonConfig.
PUPPET_MASTER_API core::Result<ProjectConfig> ParseJsonConfig(std::string_view json);

// Loads a project file and everything it imports, then applies the deployment
// file if one is set.
PUPPET_MASTER_API core::Result<ProjectConfig> LoadJsonConfig(const std::string& path);
PUPPET_MASTER_API core::Result<ProjectConfig> LoadJsonConfig(const std::string& path, const JsonLoadOptions& options);

}  // namespace puppet_master::configuration
