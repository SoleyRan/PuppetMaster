#pragma once

#include <memory>
#include <string>
#include <utility>

#include <puppet_master/core/result.h>
#include <puppet_master/service/service.h>
#include <puppet_master/transport/typed.h>

// Typed services: requests and replies encoded with transport::Codec<T>, so
// message versions declared on the types apply to the service.
namespace puppet_master::service {

namespace detail {

template <typename T>
core::Result<transport::ByteBuffer> EncodeMessage(const T& value)
{
    transport::ByteBuffer buffer(transport::Codec<T>::MaxSize(value));
    auto used = transport::detail::InvokeFill(
        [&value](transport::MutableByteView view) { return transport::Codec<T>::Encode(value, view); },
        transport::MutableByteView(buffer.data(), buffer.size()));
    if (!used.ok()) {
        return core::Result<transport::ByteBuffer>::FromStatus(used.status());
    }
    buffer.resize(used.value());
    return core::Result<transport::ByteBuffer>(std::move(buffer));
}

}  // namespace detail

namespace detail {

// A configured spec must carry what this binary's types encode; otherwise
// the binary would misread the other side.
template <typename Request, typename Reply>
core::Status CheckTypedSpec(const ServiceSpec& spec)
{
    const auto request = transport::DescriptorFor<Request>();
    const auto reply = transport::DescriptorFor<Reply>();
    if (spec.request != request) {
        return core::Status::InvalidArgument("service " + spec.name.str() + " is configured with request "
                                             + spec.request.ToString() + ", but this binary was built for "
                                             + request.ToString());
    }
    if (spec.reply != reply) {
        return core::Status::InvalidArgument("service " + spec.name.str() + " is configured with reply "
                                             + spec.reply.ToString() + ", but this binary was built for "
                                             + reply.ToString());
    }
    return core::Status::Ok();
}

}  // namespace detail

template <typename Request, typename Reply>
ServiceSpec TypedServiceSpec(core::TopicName name, core::TransportKind transport)
{
    ServiceSpec spec(std::move(name));
    spec.transport = transport;
    spec.request = transport::DescriptorFor<Request>();
    spec.reply = transport::DescriptorFor<Reply>();
    return spec;
}

template <typename Request, typename Reply>
class TypedClient {
public:
    explicit TypedClient(std::unique_ptr<Client> client) : client_(std::move(client)) {}

    core::Result<Reply> Call(const Request& request, core::Nanoseconds timeout)
    {
        auto encoded = detail::EncodeMessage(request);
        if (!encoded.ok()) {
            return core::Result<Reply>::FromStatus(encoded.status());
        }
        auto reply = client_->Call(transport::ByteView(encoded.value()), timeout);
        if (!reply.ok()) {
            return core::Result<Reply>::FromStatus(reply.status());
        }
        return transport::Codec<Reply>::Decode(transport::ByteView(reply.value()));
    }

    Client& raw() noexcept { return *client_; }

private:
    std::unique_ptr<Client> client_;
};

template <typename Request, typename Reply>
using TypedHandler = std::function<core::Result<Reply>(const Request&)>;

// A request that fails to decode is answered with the decode error, not
// passed to the handler.
template <typename Request, typename Reply>
core::Result<std::unique_ptr<Server>> MakeTypedServer(
    runtime::RuntimeContext& runtime, ServiceSpec spec, TypedHandler<Request, Reply> handler)
{
    if (!handler) {
        return core::Result<std::unique_ptr<Server>>::FromStatus(
            core::Status::InvalidArgument("service handler must not be empty"));
    }
    return Server::Create(runtime, std::move(spec), [handler = std::move(handler)](const service::Request& raw) {
        auto request = transport::Codec<Request>::Decode(raw.payload);
        if (!request.ok()) {
            return core::Result<transport::ByteBuffer>::FromStatus(request.status());
        }
        auto reply = handler(request.value());
        if (!reply.ok()) {
            return core::Result<transport::ByteBuffer>::FromStatus(reply.status());
        }
        return detail::EncodeMessage(reply.value());
    });
}

template <typename Request, typename Reply>
core::Result<std::unique_ptr<Server>> MakeTypedServer(
    runtime::RuntimeContext& runtime, core::TopicName name, core::TransportKind transport,
    TypedHandler<Request, Reply> handler)
{
    return MakeTypedServer<Request, Reply>(
        runtime, TypedServiceSpec<Request, Reply>(std::move(name), transport), std::move(handler));
}

template <typename Request, typename Reply>
core::Result<TypedClient<Request, Reply>> MakeTypedClient(runtime::RuntimeContext& runtime, ServiceSpec spec)
{
    auto client = Client::Create(runtime, std::move(spec));
    if (!client.ok()) {
        return core::Result<TypedClient<Request, Reply>>::FromStatus(client.status());
    }
    return core::Result<TypedClient<Request, Reply>>(TypedClient<Request, Reply>(std::move(client).value()));
}

template <typename Request, typename Reply>
core::Result<TypedClient<Request, Reply>> MakeTypedClient(
    runtime::RuntimeContext& runtime, core::TopicName name, core::TransportKind transport)
{
    return MakeTypedClient<Request, Reply>(runtime, TypedServiceSpec<Request, Reply>(std::move(name), transport));
}

}  // namespace puppet_master::service
