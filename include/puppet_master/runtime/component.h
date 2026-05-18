#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <puppet_master/core/result.h>
#include <puppet_master/core/status.h>
#include <puppet_master/core/types.h>
#include <puppet_master/transport/transport.h>

namespace puppet_master::runtime {

class RuntimeContext;

enum class ComponentState : std::uint8_t {
    kCreated = 0,
    kConfigured,
    kInitialized,
    kStarted,
    kStopped,
    kShutdown,
    kError,
};

inline const char* ToString(ComponentState state) noexcept
{
    switch (state) {
        case ComponentState::kCreated:
            return "created";
        case ComponentState::kConfigured:
            return "configured";
        case ComponentState::kInitialized:
            return "initialized";
        case ComponentState::kStarted:
            return "started";
        case ComponentState::kStopped:
            return "stopped";
        case ComponentState::kShutdown:
            return "shutdown";
        case ComponentState::kError:
            return "error";
    }

    return "unknown";
}

// ComponentSpec describes a runtime participant before it is scheduled. It is
// intentionally declarative. A Component instance can return this spec to tell
// the runtime which endpoints and triggers it needs before scheduling starts.
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

// ComponentContext is passed to algorithm modules during lifecycle and execute
// callbacks. It gives modules access to runtime services without exposing the
// runtime implementation details or global singletons.
class ComponentContext {
public:
    ComponentContext(core::ComponentName component_name, RuntimeContext& runtime);

    const core::ComponentName& component_name() const noexcept;
    RuntimeContext& runtime() noexcept;
    const RuntimeContext& runtime() const noexcept;

    core::Result<transport::ReaderPtr> CreateReader(const transport::EndpointConfig& endpoint);
    core::Result<transport::WriterPtr> CreateWriter(const transport::EndpointConfig& endpoint);

private:
    core::ComponentName component_name_;
    RuntimeContext* runtime_ {nullptr};
};

// Component is the base interface for user algorithm modules. It keeps the
// runtime-facing contract small: modules describe their endpoints, then receive
// explicit lifecycle callbacks and an Execute() hook for the future scheduler.
class Component {
public:
    Component(const Component&) = delete;
    Component& operator=(const Component&) = delete;
    Component(Component&&) = delete;
    Component& operator=(Component&&) = delete;
    virtual ~Component() = default;

    virtual ComponentSpec Describe() const = 0;

    virtual core::Status Configure(ComponentContext& context);
    virtual core::Status Initialize(ComponentContext& context);
    virtual core::Status Start(ComponentContext& context);
    virtual core::Status Execute(ComponentContext& context);
    virtual core::Status Stop(ComponentContext& context);
    virtual core::Status Shutdown(ComponentContext& context);

protected:
    Component() = default;
};

using ComponentPtr = std::shared_ptr<Component>;

}  // namespace puppet_master::runtime
