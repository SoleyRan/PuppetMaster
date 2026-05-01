#pragma once

#include <puppet_master/core/message_policy.h>
#include <puppet_master/core/result.h>
#include <puppet_master/core/status.h>
#include <puppet_master/core/types.h>
#include <puppet_master/export.h>
#include <puppet_master/version.h>

namespace puppet_master {

PUPPET_MASTER_API const char* ProjectName() noexcept;
PUPPET_MASTER_API const char* Version() noexcept;

}  // namespace puppet_master
