#pragma once

#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

#include <puppet_master/puppet_master.h>
#include <puppet_master/runtime/parameters.h>
#include <puppet_master/runtime/typed_endpoints.h>
#include <puppet_master/transport/typed.h>

#include <sensor_interfaces/messages.h>

namespace sensor_node {

namespace core = puppet_master::core;
namespace runtime = puppet_master::runtime;
namespace transport = puppet_master::transport;
using sensor_interfaces::ImuSample;
using sensor_interfaces::WheelSpeed;

// Simulated drive: speed follows a slow sine around `cruise_speed`.
inline double SimulatedSpeed(double cruise_speed, std::uint64_t sample)
{
    return cruise_speed + 2.0 * std::sin(static_cast<double>(sample) * 0.01);
}

// Publishes a wheel speed sample on every periodic trigger.
class WheelSpeedSensor final : public runtime::Component {
public:
    explicit WheelSpeedSensor(runtime::ComponentSpec spec) : spec_(std::move(spec)) {}

    runtime::ComponentSpec Describe() const override { return spec_; }

    core::Status Configure(runtime::ComponentContext& context) override
    {
        auto cruise = runtime::GetParameter(spec_.parameters, "cruise_speed", 15.0);
        if (!cruise.ok()) {
            return cruise.status();
        }
        cruise_speed_ = cruise.value();
        auto writer = runtime::MakeTypedWriter<WheelSpeed>(context, spec_);
        if (!writer.ok()) {
            return writer.status();
        }
        writer_.emplace(std::move(writer).value());
        return core::Status::Ok();
    }

    core::Status Execute(runtime::ComponentContext&) override
    {
        ++sample_;
        return writer_->Write(WheelSpeed {SimulatedSpeed(cruise_speed_, sample_), sample_});
    }

private:
    runtime::ComponentSpec spec_;
    std::optional<transport::TypedWriter<WheelSpeed>> writer_;
    double cruise_speed_ {0.0};
    std::uint64_t sample_ {0};
};

// Publishes an IMU sample on every periodic trigger, consistent with the
// simulated speed profile.
class ImuSensor final : public runtime::Component {
public:
    explicit ImuSensor(runtime::ComponentSpec spec) : spec_(std::move(spec)) {}

    runtime::ComponentSpec Describe() const override { return spec_; }

    core::Status Configure(runtime::ComponentContext& context) override
    {
        auto cruise = runtime::GetParameter(spec_.parameters, "cruise_speed", 15.0);
        if (!cruise.ok()) {
            return cruise.status();
        }
        cruise_speed_ = cruise.value();
        auto writer = runtime::MakeTypedWriter<ImuSample>(context, spec_);
        if (!writer.ok()) {
            return writer.status();
        }
        writer_.emplace(std::move(writer).value());
        return core::Status::Ok();
    }

    core::Status Execute(runtime::ComponentContext&) override
    {
        ++sample_;
        const double previous = SimulatedSpeed(cruise_speed_, sample_ - 1);
        const double current = SimulatedSpeed(cruise_speed_, sample_);
        return writer_->Write(ImuSample {(current - previous) / 0.01, 0.05, sample_});
    }

private:
    runtime::ComponentSpec spec_;
    std::optional<transport::TypedWriter<ImuSample>> writer_;
    double cruise_speed_ {0.0};
    std::uint64_t sample_ {0};
};

}  // namespace sensor_node
