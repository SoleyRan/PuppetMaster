#pragma once

#include <string>
#include <string_view>

#include <puppet_master/configuration/configuration.h>
#include <puppet_master/export.h>

namespace puppet_master::configuration {

PUPPET_MASTER_API core::Result<ProjectConfig> ParseJsonConfig(std::string_view json);
PUPPET_MASTER_API core::Result<ProjectConfig> LoadJsonConfig(const std::string& path);

}  // namespace puppet_master::configuration
