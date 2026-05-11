#pragma once

#include <cstddef>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <puppet_master/core/result.h>
#include <puppet_master/core/status.h>
#include <puppet_master/core/types.h>
#include <puppet_master/runtime/component.h>

namespace puppet_master::runtime {

// ComponentRegistry is a small, thread-safe catalog for runtime component
// declarations. It stores specs by value so callers cannot mutate registered
// state without going through explicit registry operations.
class ComponentRegistry {
public:
    ComponentRegistry() = default;

    ComponentRegistry(const ComponentRegistry&) = delete;
    ComponentRegistry& operator=(const ComponentRegistry&) = delete;

    core::Status Register(ComponentSpec spec)
    {
        auto status = spec.Validate();
        if (!status.ok()) {
            return status;
        }

        const auto key = spec.name.str();
        std::lock_guard<std::mutex> lock(mutex_);
        if (components_.find(key) != components_.end()) {
            return core::Status::AlreadyExists("component already registered: " + key);
        }

        components_.emplace(key, std::move(spec));
        return core::Status::Ok();
    }

    core::Result<ComponentSpec> Find(const core::ComponentName& name) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = components_.find(name.str());
        if (found == components_.end()) {
            return core::Result<ComponentSpec>::FromStatus(
                core::Status::NotFound("component is not registered: " + name.str()));
        }

        return found->second;
    }

    core::Status Unregister(const core::ComponentName& name)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (components_.erase(name.str()) == 0) {
            return core::Status::NotFound("component is not registered: " + name.str());
        }

        return core::Status::Ok();
    }

    std::vector<core::ComponentName> ListNames() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<core::ComponentName> names;
        names.reserve(components_.size());

        for (const auto& entry : components_) {
            names.push_back(core::ComponentName::Unsafe(entry.first));
        }

        return names;
    }

    bool empty() const noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return components_.empty();
    }

    std::size_t size() const noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return components_.size();
    }

private:
    mutable std::mutex mutex_;
    std::map<std::string, ComponentSpec> components_;
};

}  // namespace puppet_master::runtime
