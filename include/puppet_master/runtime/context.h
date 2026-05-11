#pragma once

#include <memory>
#include <vector>

#include <puppet_master/core/result.h>
#include <puppet_master/core/status.h>
#include <puppet_master/core/types.h>
#include <puppet_master/runtime/component.h>
#include <puppet_master/runtime/registry.h>
#include <puppet_master/transport/transport.h>

namespace puppet_master::runtime {

struct RuntimeOptions {
    core::TransportName default_transport_name {core::TransportName::Unsafe("inmemory")};
    bool register_inmemory_transport {true};
    bool open_registered_transports {true};

    core::Status Validate() const;
};

// RuntimeContext is the central assembly point for the middleware runtime. It
// owns registries and transport instances, but it does not execute scheduling
// policy yet. Keeping it small makes the next scheduler milestone easier to
// test against both local and external transports.
class RuntimeContext final {
public:
    explicit RuntimeContext(RuntimeOptions options = {});
    ~RuntimeContext();

    RuntimeContext(const RuntimeContext&) = delete;
    RuntimeContext& operator=(const RuntimeContext&) = delete;
    RuntimeContext(RuntimeContext&&) = delete;
    RuntimeContext& operator=(RuntimeContext&&) = delete;

    static core::Result<std::shared_ptr<RuntimeContext>> Create(RuntimeOptions options = {});

    const RuntimeOptions& options() const noexcept;

    core::Status Open();
    core::Status Close();
    bool is_open() const noexcept;

    core::Status RegisterTransport(transport::TransportPtr transport);
    core::Result<transport::TransportPtr> FindTransport(const core::TransportName& name) const;
    std::vector<core::TransportName> ListTransportNames() const;

    core::Status RegisterComponent(ComponentSpec spec);
    core::Result<ComponentSpec> FindComponent(const core::ComponentName& name) const;
    std::vector<core::ComponentName> ListComponentNames() const;

    core::Result<transport::ReaderPtr> CreateReader(const transport::EndpointConfig& endpoint);
    core::Result<transport::WriterPtr> CreateWriter(const transport::EndpointConfig& endpoint);

private:
    struct Impl;

    std::unique_ptr<Impl> impl_;
};

}  // namespace puppet_master::runtime
