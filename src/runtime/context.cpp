#include <puppet_master/runtime/context.h>

#include <initializer_list>
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

bool IsOneOf(ComponentState state, std::initializer_list<ComponentState> allowed_states)
{
    for (const auto allowed : allowed_states) {
        if (state == allowed) {
            return true;
        }
    }

    return false;
}

core::Status InvalidTransition(
    const core::ComponentName& name,
    const char* operation,
    ComponentState state)
{
    return core::Status::FailedPrecondition(
        std::string("cannot ") + operation + " component " + name.str()
        + " from state " + ToString(state));
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

    core::Status RegisterComponentInstance(ComponentPtr component)
    {
        if (!component) {
            return core::Status::InvalidArgument("component must not be null");
        }

        auto spec = component->Describe();
        auto status = spec.Validate();
        if (!status.ok()) {
            return status;
        }

        const auto name = spec.name;
        status = components.Register(std::move(spec));
        if (!status.ok()) {
            return status;
        }

        std::lock_guard<std::mutex> lock(mutex);
        component_instances.emplace(name.str(), std::move(component));
        component_states.emplace(name.str(), ComponentState::kCreated);
        return core::Status::Ok();
    }

    core::Result<ComponentPtr> FindComponentInstance(const core::ComponentName& name) const
    {
        std::lock_guard<std::mutex> lock(mutex);
        const auto found = component_instances.find(name.str());
        if (found == component_instances.end()) {
            return core::Result<ComponentPtr>::FromStatus(
                core::Status::NotFound("component instance is not registered: " + name.str()));
        }

        return found->second;
    }

    core::Result<ComponentState> GetComponentState(const core::ComponentName& name) const
    {
        std::lock_guard<std::mutex> lock(mutex);
        const auto found = component_states.find(name.str());
        if (found == component_states.end()) {
            return core::Result<ComponentState>::FromStatus(
                core::Status::NotFound("component state is not registered: " + name.str()));
        }

        return found->second;
    }

    core::Status SetComponentState(const core::ComponentName& name, ComponentState state)
    {
        std::lock_guard<std::mutex> lock(mutex);
        const auto found = component_states.find(name.str());
        if (found == component_states.end()) {
            return core::Status::NotFound("component state is not registered: " + name.str());
        }

        found->second = state;
        return core::Status::Ok();
    }

    RuntimeOptions options;
    ComponentRegistry components;
    transport::TransportRegistry transports;
    std::map<core::TransportKind, core::TransportName> transport_by_kind;
    std::map<std::string, ComponentPtr> component_instances;
    std::map<std::string, ComponentState> component_states;
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

core::Status RuntimeContext::RegisterComponent(ComponentPtr component)
{
    return impl_->RegisterComponentInstance(std::move(component));
}

core::Result<ComponentSpec> RuntimeContext::FindComponent(const core::ComponentName& name) const
{
    return impl_->components.Find(name);
}

core::Result<ComponentPtr> RuntimeContext::FindComponentInstance(const core::ComponentName& name) const
{
    return impl_->FindComponentInstance(name);
}

core::Result<ComponentState> RuntimeContext::GetComponentState(const core::ComponentName& name) const
{
    return impl_->GetComponentState(name);
}

std::vector<core::ComponentName> RuntimeContext::ListComponentNames() const
{
    return impl_->components.ListNames();
}

core::Status RuntimeContext::ConfigureComponent(const core::ComponentName& name)
{
    auto component = FindComponentInstance(name);
    if (!component.ok()) {
        return component.status();
    }

    auto state = GetComponentState(name);
    if (!state.ok()) {
        return state.status();
    }

    if (!IsOneOf(state.value(), {ComponentState::kCreated, ComponentState::kStopped})) {
        return InvalidTransition(name, "configure", state.value());
    }

    ComponentContext context(name, *this);
    auto status = component.value()->Configure(context);
    impl_->SetComponentState(name, status.ok() ? ComponentState::kConfigured : ComponentState::kError);
    return status;
}

core::Status RuntimeContext::InitializeComponent(const core::ComponentName& name)
{
    auto component = FindComponentInstance(name);
    if (!component.ok()) {
        return component.status();
    }

    auto state = GetComponentState(name);
    if (!state.ok()) {
        return state.status();
    }

    if (!IsOneOf(state.value(), {ComponentState::kConfigured})) {
        return InvalidTransition(name, "initialize", state.value());
    }

    ComponentContext context(name, *this);
    auto status = component.value()->Initialize(context);
    impl_->SetComponentState(name, status.ok() ? ComponentState::kInitialized : ComponentState::kError);
    return status;
}

core::Status RuntimeContext::StartComponent(const core::ComponentName& name)
{
    auto component = FindComponentInstance(name);
    if (!component.ok()) {
        return component.status();
    }

    auto state = GetComponentState(name);
    if (!state.ok()) {
        return state.status();
    }

    if (!IsOneOf(state.value(), {ComponentState::kInitialized, ComponentState::kStopped})) {
        return InvalidTransition(name, "start", state.value());
    }

    ComponentContext context(name, *this);
    auto status = component.value()->Start(context);
    impl_->SetComponentState(name, status.ok() ? ComponentState::kStarted : ComponentState::kError);
    return status;
}

core::Status RuntimeContext::ExecuteComponent(const core::ComponentName& name)
{
    auto component = FindComponentInstance(name);
    if (!component.ok()) {
        return component.status();
    }

    auto state = GetComponentState(name);
    if (!state.ok()) {
        return state.status();
    }

    if (!IsOneOf(state.value(), {ComponentState::kStarted})) {
        return InvalidTransition(name, "execute", state.value());
    }

    ComponentContext context(name, *this);
    auto status = component.value()->Execute(context);
    if (!status.ok()) {
        impl_->SetComponentState(name, ComponentState::kError);
    }
    return status;
}

core::Status RuntimeContext::StopComponent(const core::ComponentName& name)
{
    auto component = FindComponentInstance(name);
    if (!component.ok()) {
        return component.status();
    }

    auto state = GetComponentState(name);
    if (!state.ok()) {
        return state.status();
    }

    if (!IsOneOf(state.value(), {ComponentState::kStarted})) {
        return InvalidTransition(name, "stop", state.value());
    }

    ComponentContext context(name, *this);
    auto status = component.value()->Stop(context);
    impl_->SetComponentState(name, status.ok() ? ComponentState::kStopped : ComponentState::kError);
    return status;
}

core::Status RuntimeContext::ShutdownComponent(const core::ComponentName& name)
{
    auto component = FindComponentInstance(name);
    if (!component.ok()) {
        return component.status();
    }

    auto state = GetComponentState(name);
    if (!state.ok()) {
        return state.status();
    }

    if (!IsOneOf(
            state.value(),
            {ComponentState::kCreated,
             ComponentState::kConfigured,
             ComponentState::kInitialized,
             ComponentState::kStopped,
             ComponentState::kError})) {
        return InvalidTransition(name, "shutdown", state.value());
    }

    ComponentContext context(name, *this);
    auto status = component.value()->Shutdown(context);
    impl_->SetComponentState(name, status.ok() ? ComponentState::kShutdown : ComponentState::kError);
    return status;
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
