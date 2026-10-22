#pragma once

#include <algorithm>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include <puppet_master/core/result.h>
#include <puppet_master/core/status.h>
#include <puppet_master/runtime/component.h>
#include <puppet_master/transport/typed.h>

// Typed endpoint helpers: build a typed reader or writer from a ComponentSpec
// by matching the message type T. The spec's readers/writers must contain
// exactly one endpoint whose message descriptor matches T's codec.

namespace puppet_master::runtime {

namespace detail {

template <typename T>
core::Result<transport::EndpointConfig> FindEndpointForType(
    const std::vector<transport::EndpointConfig>& endpoints,
    const char* endpoint_kind)
{
    const auto expected = transport::DescriptorFor<T>();
    const auto found = std::find_if(endpoints.begin(), endpoints.end(),
        [&expected](const transport::EndpointConfig& endpoint) {
            return endpoint.message == expected;
        });
    if (found == endpoints.end()) {
        return core::Result<transport::EndpointConfig>::FromStatus(core::Status::NotFound(
            std::string("component has no ") + endpoint_kind + " for " + expected.ToString()));
    }
    const auto again = std::find_if(std::next(found), endpoints.end(),
        [&expected](const transport::EndpointConfig& endpoint) {
            return endpoint.message == expected;
        });
    if (again != endpoints.end()) {
        return core::Result<transport::EndpointConfig>::FromStatus(core::Status::InvalidArgument(
            std::string("component has several ") + endpoint_kind + "s for " + expected.ToString() + " ("
            + found->topic.name.str() + ", " + again->topic.name.str() + "); create them by topic instead"));
    }
    return core::Result<transport::EndpointConfig>(*found);
}

}  // namespace detail

template <typename T>
core::Result<transport::TypedWriter<T>> MakeTypedWriter(
    ComponentContext& context,
    const ComponentSpec& spec)
{
    auto endpoint = detail::FindEndpointForType<T>(spec.writers, "writer");
    if (!endpoint.ok()) {
        return core::Result<transport::TypedWriter<T>>::FromStatus(endpoint.status());
    }
    auto writer = context.CreateWriter(endpoint.value());
    if (!writer.ok()) {
        return core::Result<transport::TypedWriter<T>>::FromStatus(writer.status());
    }
    return transport::TypedWriter<T>::Wrap(std::move(writer).value());
}

template <typename T>
core::Result<transport::TypedReader<T>> MakeTypedReader(
    ComponentContext& context,
    const ComponentSpec& spec)
{
    auto endpoint = detail::FindEndpointForType<T>(spec.readers, "reader");
    if (!endpoint.ok()) {
        return core::Result<transport::TypedReader<T>>::FromStatus(endpoint.status());
    }
    auto reader = context.CreateReader(endpoint.value());
    if (!reader.ok()) {
        return core::Result<transport::TypedReader<T>>::FromStatus(reader.status());
    }
    return transport::TypedReader<T>::Wrap(std::move(reader).value());
}

}  // namespace puppet_master::runtime
