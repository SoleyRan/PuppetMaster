#pragma once

#include <string>
#include <utility>
#include <vector>

#include <puppet_master/core/status.h>
#include <puppet_master/core/types.h>
#include <puppet_master/transport/transport.h>

namespace puppet_master::runtime {

// ComponentSpec describes a runtime participant before it is scheduled. It is
// intentionally declarative: lifecycle callbacks and executable task bodies
// will be attached by later runtime and scheduler milestones.
struct ComponentSpec {
    core::ComponentName name;
    std::string description;
    std::vector<transport::EndpointConfig> readers;
    std::vector<transport::EndpointConfig> writers;
    std::vector<core::TriggerSpec> triggers;

    core::Status Validate() const
    {
        auto status = core::ValidateName("component name", name.view());
        if (!status.ok()) {
            return status;
        }

        status = ValidateEndpoints(readers, "reader");
        if (!status.ok()) {
            return status;
        }

        status = ValidateEndpoints(writers, "writer");
        if (!status.ok()) {
            return status;
        }

        for (const auto& trigger : triggers) {
            status = trigger.Validate();
            if (!status.ok()) {
                return status;
            }
        }

        return core::Status::Ok();
    }

private:
    static core::Status ValidateEndpoints(
        const std::vector<transport::EndpointConfig>& endpoints,
        const char* endpoint_kind)
    {
        for (const auto& endpoint : endpoints) {
            auto status = endpoint.Validate();
            if (!status.ok()) {
                return core::Status::InvalidArgument(
                    std::string(endpoint_kind) + " endpoint is invalid: " + status.ToString());
            }
        }

        return core::Status::Ok();
    }
};

}  // namespace puppet_master::runtime
