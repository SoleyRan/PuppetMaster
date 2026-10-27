#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <puppet_master/core/result.h>
#include <puppet_master/core/status.h>
#include <puppet_master/runtime/component.h>
#include <puppet_master/runtime/context.h>
#include <puppet_master/service/service.h>
#include <puppet_master/service/typed_service.h>

// Servers and clients for the services a component declares in its
// ComponentSpec, so component code names a service, not its transport or
// queue settings, which come from configuration.
namespace puppet_master::service {

namespace detail {

inline core::Result<ServiceSpec> FindServiceSpec(
    const std::vector<ServiceSpec>& specs, const std::string& name, const char* kind)
{
    for (const auto& spec : specs) {
        if (spec.name.str() == name) {
            return core::Result<ServiceSpec>(spec);
        }
    }
    return core::Result<ServiceSpec>::FromStatus(
        core::Status::NotFound(std::string("component declares no ") + kind + " for service " + name));
}

// kCaller when one of the spec's service triggers names the service: the
// scheduler then runs the component on requests, and the component serves
// them in Execute() with ServePending. kOwnThread otherwise.
inline ServerOptions OptionsFor(const runtime::ComponentSpec& spec, const ServiceSpec& service)
{
    ServerOptions options;
    for (const auto& trigger : spec.triggers) {
        if (trigger.kind != core::TriggerKind::kService) {
            continue;
        }
        for (const auto& name : trigger.service_dependencies) {
            if (name == service.name) {
                options.mode = ServeMode::kCaller;
            }
        }
    }
    return options;
}

}  // namespace detail

// Server for `name`, which the spec must list in `servers`. If a service
// trigger of the spec names it, the server is ServeMode::kCaller and the
// component must call ServePending() in Execute(); see
// docs/services.md#scheduled-servers.
inline core::Result<std::unique_ptr<Server>> CreateServer(
    runtime::ComponentContext& context, const runtime::ComponentSpec& spec, const std::string& name, Handler handler)
{
    auto service = detail::FindServiceSpec(spec.servers, name, "server");
    if (!service.ok()) {
        return core::Result<std::unique_ptr<Server>>::FromStatus(service.status());
    }
    const auto options = detail::OptionsFor(spec, service.value());
    return Server::Create(context.runtime(), std::move(service).value(), std::move(handler), options);
}

// Client for `name`, which the spec must list in `clients`.
inline core::Result<std::unique_ptr<Client>> CreateClient(
    runtime::ComponentContext& context, const runtime::ComponentSpec& spec, const std::string& name)
{
    auto service = detail::FindServiceSpec(spec.clients, name, "client");
    if (!service.ok()) {
        return core::Result<std::unique_ptr<Client>>::FromStatus(service.status());
    }
    return Client::Create(context.runtime(), std::move(service).value());
}

// Typed variants. The configured request and reply descriptors must match
// the types' codecs, versions included.
template <typename Request, typename Reply>
core::Result<std::unique_ptr<Server>> CreateTypedServer(
    runtime::ComponentContext& context, const runtime::ComponentSpec& spec, const std::string& name,
    TypedHandler<Request, Reply> handler)
{
    using ResultType = core::Result<std::unique_ptr<Server>>;
    auto service = detail::FindServiceSpec(spec.servers, name, "server");
    if (!service.ok()) {
        return ResultType::FromStatus(service.status());
    }
    auto status = detail::CheckTypedSpec<Request, Reply>(service.value());
    if (!status.ok()) {
        return ResultType::FromStatus(std::move(status));
    }
    const auto options = detail::OptionsFor(spec, service.value());
    return MakeTypedServer<Request, Reply>(
        context.runtime(), std::move(service).value(), std::move(handler), options);
}

template <typename Request, typename Reply>
core::Result<TypedClient<Request, Reply>> CreateTypedClient(
    runtime::ComponentContext& context, const runtime::ComponentSpec& spec, const std::string& name)
{
    using ResultType = core::Result<TypedClient<Request, Reply>>;
    auto service = detail::FindServiceSpec(spec.clients, name, "client");
    if (!service.ok()) {
        return ResultType::FromStatus(service.status());
    }
    auto status = detail::CheckTypedSpec<Request, Reply>(service.value());
    if (!status.ok()) {
        return ResultType::FromStatus(std::move(status));
    }
    return MakeTypedClient<Request, Reply>(context.runtime(), std::move(service).value());
}

}  // namespace puppet_master::service
