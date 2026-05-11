#include <puppet_master/runtime/context.h>

#include <map>
#include <mutex>
#include <string>
#include <utility>

#include <puppet_master/transport/inmemory/inmemory_transport.h>
#include <puppet_master/transport/registry.h>

namespace puppet_master::runtime {

namespace {

core::Status OpenTransportIfNeeded(const transport::TransportPtr& transport)
{
    if (!transport) {
        return core::Status::InvalidArgument("transport must not be null");
    }

    if (transport->is_open()) {
        return core::Status::Ok();
    }

    return transport->Open();
}

}  // namespace

core::Status RuntimeOptions::Validate() const
{
    return core::ValidateName("default transport name", default_transport_name.view());
}

struct RuntimeContext::Impl {
    explicit Impl(RuntimeOptions runtime_options)
        : options(std::move(runtime_options))
    {
    }

    core::Status RegisterTransport(transport::TransportPtr transport)
    {
        if (!transport) {
            return core::Status::InvalidArgument("transport must not be null");
        }

        const auto name = transport->name();
        const auto kind = transport->kind();

        if (IsOpen() || options.open_registered_transports) {
            auto status = OpenTransportIfNeeded(transport);
            if (!status.ok()) {
                return status;
            }
        }

        std::lock_guard<std::mutex> lock(mutex);
        const auto existing_kind = transport_by_kind.find(kind);
        if (existing_kind != transport_by_kind.end() && existing_kind->second != name) {
            return core::Status::AlreadyExists(
                std::string("transport kind already registered: ") + core::ToString(kind));
        }

        auto status = transports.Register(std::move(transport));
        if (!status.ok()) {
            return status;
        }

        transport_by_kind.emplace(kind, name);
        return core::Status::Ok();
    }

    core::Status EnsureDefaultTransports()
    {
        if (!options.register_inmemory_transport) {
            return core::Status::Ok();
        }

        {
            std::lock_guard<std::mutex> lock(mutex);
            if (transport_by_kind.find(core::TransportKind::kInMemory) != transport_by_kind.end()) {
                return core::Status::Ok();
            }
        }

        auto transport = std::make_shared<transport::inmemory::InMemoryTransport>(
            options.default_transport_name);
        return RegisterTransport(std::move(transport));
    }

    core::Status Open()
    {
        auto status = options.Validate();
        if (!status.ok()) {
            return status;
        }

        status = EnsureDefaultTransports();
        if (!status.ok()) {
            return status;
        }

        const auto names = ListTransportNames();
        for (const auto& name : names) {
            auto transport = FindTransport(name);
            if (!transport.ok()) {
                return transport.status();
            }

            status = OpenTransportIfNeeded(transport.value());
            if (!status.ok()) {
                return status;
            }
        }

        std::lock_guard<std::mutex> lock(mutex);
        open = true;
        return core::Status::Ok();
    }

    core::Status Close()
    {
        const auto names = ListTransportNames();
        for (const auto& name : names) {
            auto transport = FindTransport(name);
            if (transport.ok()) {
                transport.value()->Close();
            }
        }

        std::lock_guard<std::mutex> lock(mutex);
        open = false;
        return core::Status::Ok();
    }

    bool IsOpen() const noexcept
    {
        std::lock_guard<std::mutex> lock(mutex);
        return open;
    }

    core::Result<transport::TransportPtr> FindTransport(const core::TransportName& name) const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return transports.Find(name);
    }

    std::vector<core::TransportName> ListTransportNames() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return transports.ListNames();
    }

    core::Result<transport::TransportPtr> FindTransport(core::TransportKind kind) const
    {
        std::lock_guard<std::mutex> lock(mutex);
        const auto found = transport_by_kind.find(kind);
        if (found == transport_by_kind.end()) {
            return core::Result<transport::TransportPtr>::FromStatus(
                core::Status::NotFound(
                    std::string("transport kind is not registered: ") + core::ToString(kind)));
        }

        return transports.Find(found->second);
    }

    RuntimeOptions options;
    ComponentRegistry components;
    transport::TransportRegistry transports;
    std::map<core::TransportKind, core::TransportName> transport_by_kind;
    mutable std::mutex mutex;
    bool open {false};
};

RuntimeContext::RuntimeContext(RuntimeOptions options)
    : impl_(std::make_unique<Impl>(std::move(options)))
{
}

RuntimeContext::~RuntimeContext()
{
    try {
        Close();
    } catch (...) {
    }
}

core::Result<std::shared_ptr<RuntimeContext>> RuntimeContext::Create(RuntimeOptions options)
{
    auto status = options.Validate();
    if (!status.ok()) {
        return core::Result<std::shared_ptr<RuntimeContext>>::FromStatus(std::move(status));
    }

    auto context = std::make_shared<RuntimeContext>(std::move(options));
    status = context->Open();
    if (!status.ok()) {
        return core::Result<std::shared_ptr<RuntimeContext>>::FromStatus(std::move(status));
    }

    return context;
}

const RuntimeOptions& RuntimeContext::options() const noexcept
{
    return impl_->options;
}

core::Status RuntimeContext::Open()
{
    return impl_->Open();
}

core::Status RuntimeContext::Close()
{
    return impl_->Close();
}

bool RuntimeContext::is_open() const noexcept
{
    return impl_->IsOpen();
}

core::Status RuntimeContext::RegisterTransport(transport::TransportPtr transport)
{
    return impl_->RegisterTransport(std::move(transport));
}

core::Result<transport::TransportPtr> RuntimeContext::FindTransport(
    const core::TransportName& name) const
{
    return impl_->FindTransport(name);
}

std::vector<core::TransportName> RuntimeContext::ListTransportNames() const
{
    return impl_->ListTransportNames();
}

core::Status RuntimeContext::RegisterComponent(ComponentSpec spec)
{
    return impl_->components.Register(std::move(spec));
}

core::Result<ComponentSpec> RuntimeContext::FindComponent(const core::ComponentName& name) const
{
    return impl_->components.Find(name);
}

std::vector<core::ComponentName> RuntimeContext::ListComponentNames() const
{
    return impl_->components.ListNames();
}

core::Result<transport::ReaderPtr> RuntimeContext::CreateReader(
    const transport::EndpointConfig& endpoint)
{
    if (!is_open()) {
        return core::Result<transport::ReaderPtr>::FromStatus(
            core::Status::FailedPrecondition("runtime context must be open before creating readers"));
    }

    auto transport = impl_->FindTransport(endpoint.topic.transport);
    if (!transport.ok()) {
        return core::Result<transport::ReaderPtr>::FromStatus(transport.status());
    }

    return transport.value()->CreateReader(endpoint);
}

core::Result<transport::WriterPtr> RuntimeContext::CreateWriter(
    const transport::EndpointConfig& endpoint)
{
    if (!is_open()) {
        return core::Result<transport::WriterPtr>::FromStatus(
            core::Status::FailedPrecondition("runtime context must be open before creating writers"));
    }

    auto transport = impl_->FindTransport(endpoint.topic.transport);
    if (!transport.ok()) {
        return core::Result<transport::WriterPtr>::FromStatus(transport.status());
    }

    return transport.value()->CreateWriter(endpoint);
}

}  // namespace puppet_master::runtime
