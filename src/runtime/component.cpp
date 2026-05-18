#include <puppet_master/runtime/component.h>

#include <utility>

#include <puppet_master/runtime/context.h>

namespace puppet_master::runtime {

ComponentContext::ComponentContext(core::ComponentName component_name, RuntimeContext& runtime)
    : component_name_(std::move(component_name)), runtime_(&runtime)
{
}

const core::ComponentName& ComponentContext::component_name() const noexcept
{
    return component_name_;
}

RuntimeContext& ComponentContext::runtime() noexcept
{
    return *runtime_;
}

const RuntimeContext& ComponentContext::runtime() const noexcept
{
    return *runtime_;
}

core::Result<transport::ReaderPtr> ComponentContext::CreateReader(
    const transport::EndpointConfig& endpoint)
{
    return runtime_->CreateReader(endpoint);
}

core::Result<transport::WriterPtr> ComponentContext::CreateWriter(
    const transport::EndpointConfig& endpoint)
{
    return runtime_->CreateWriter(endpoint);
}

core::Status Component::Configure(ComponentContext&)
{
    return core::Status::Ok();
}

core::Status Component::Initialize(ComponentContext&)
{
    return core::Status::Ok();
}

core::Status Component::Start(ComponentContext&)
{
    return core::Status::Ok();
}

core::Status Component::Execute(ComponentContext&)
{
    return core::Status::Ok();
}

core::Status Component::Stop(ComponentContext&)
{
    return core::Status::Ok();
}

core::Status Component::Shutdown(ComponentContext&)
{
    return core::Status::Ok();
}

}  // namespace puppet_master::runtime
