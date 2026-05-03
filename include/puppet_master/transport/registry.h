#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <puppet_master/core/result.h>
#include <puppet_master/core/status.h>
#include <puppet_master/core/types.h>
#include <puppet_master/transport/transport.h>

namespace puppet_master::transport {

class TransportRegistry {
public:
    core::Status Register(TransportPtr transport)
    {
        if (!transport) {
            return core::Status::InvalidArgument("transport must not be null");
        }

        const auto& key = transport->name().str();
        if (transports_.find(key) != transports_.end()) {
            return core::Status::AlreadyExists("transport already registered: " + key);
        }

        transports_.emplace(key, std::move(transport));
        return core::Status::Ok();
    }

    core::Result<TransportPtr> Find(const core::TransportName& name) const
    {
        const auto found = transports_.find(name.str());
        if (found == transports_.end()) {
            return core::Result<TransportPtr>::FromStatus(
                core::Status::NotFound("transport is not registered: " + name.str()));
        }
        return found->second;
    }

    core::Status Unregister(const core::TransportName& name)
    {
        if (transports_.erase(name.str()) == 0) {
            return core::Status::NotFound("transport is not registered: " + name.str());
        }
        return core::Status::Ok();
    }

    std::vector<core::TransportName> ListNames() const
    {
        std::vector<core::TransportName> names;
        names.reserve(transports_.size());

        for (const auto& entry : transports_) {
            names.push_back(core::TransportName::Unsafe(entry.first));
        }

        return names;
    }

    bool empty() const noexcept
    {
        return transports_.empty();
    }

    std::size_t size() const noexcept
    {
        return transports_.size();
    }

private:
    std::map<std::string, TransportPtr> transports_;
};

}  // namespace puppet_master::transport
