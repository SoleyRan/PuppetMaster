#pragma once

#include <puppet_master/compat/itage_facade.h>
#include <puppet_master/configuration/configuration.h>
#include <puppet_master/core/message_policy.h>
#include <puppet_master/core/result.h>
#include <puppet_master/core/status.h>
#include <puppet_master/core/types.h>
#include <puppet_master/export.h>
#include <puppet_master/runtime/component.h>
#include <puppet_master/runtime/context.h>
#include <puppet_master/runtime/registry.h>
#include <puppet_master/scheduler/scheduler.h>
#include <puppet_master/transport/message.h>
#include <puppet_master/transport/registry.h>
#include <puppet_master/transport/inmemory/inmemory_transport.h>
#include <puppet_master/transport/transport.h>
#include <puppet_master/version.h>

namespace puppet_master {

PUPPET_MASTER_API const char* ProjectName() noexcept;
PUPPET_MASTER_API const char* Version() noexcept;

}  // namespace puppet_master
