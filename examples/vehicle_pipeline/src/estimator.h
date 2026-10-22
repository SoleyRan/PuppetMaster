#pragma once

#include <cstdint>
#include <optional>
#include <utility>

#include <puppet_master/puppet_master.h>
#include <puppet_master/runtime/parameters.h>
#include <puppet_master/runtime/typed_endpoints.h>
#include <puppet_master/transport/typed.h>

#include "messages.h"

namespace vehicle {

namespace core = puppet_master::core;
namespace runtime = puppet_master::runtime;
namespace transport = puppet_master::transport;

// Fuses wheel speed and IMU into a vehicle state. Runs whenever either input
// arrives and publishes once per run with the newest value of each.
class StateEstimator final : public runtime::Component {
public:
    explicit StateEstimator(runtime::ComponentSpec spec) : spec_(std::move(spec)) {}

    runtime::ComponentSpec Describe() const override { return spec_; }

    core::Status Configure(runtime::ComponentContext& context) override
    {
        auto gain = runtime::GetParameter(spec_.parameters, "wheel_gain", 0.8);
        if (!gain.ok()) {
            return gain.status();
        }
        if (gain.value() < 0.0 || gain.value() > 1.0) {
            return core::Status::InvalidArgument("wheel_gain must be within [0, 1]");
        }
        wheel_gain_ = gain.value();

        auto wheel = runtime::MakeTypedReader<WheelSpeed>(context, spec_);
        if (!wheel.ok()) {
            return wheel.status();
        }
        wheel_.emplace(std::move(wheel).value());
        auto imu = runtime::MakeTypedReader<ImuSample>(context, spec_);
        if (!imu.ok()) {
            return imu.status();
        }
        imu_.emplace(std::move(imu).value());
        auto state = runtime::MakeTypedWriter<VehicleState>(context, spec_);
        if (!state.ok()) {
            return state.status();
        }
        state_.emplace(std::move(state).value());
        return core::Status::Ok();
    }

    core::Status Execute(runtime::ComponentContext&) override
    {
        bool updated = false;
        for (auto message = wheel_->Read(); message.ok(); message = wheel_->Read()) {
            last_wheel_ = message.value().value;
            updated = true;
        }
        for (auto message = imu_->Read(); message.ok(); message = imu_->Read()) {
            last_imu_ = message.value().value;
            updated = true;
        }
        if (!updated || !last_wheel_ || !last_imu_) {
            return core::Status::Ok();
        }

        // Complementary filter: trust the wheels, correct with integrated IMU.
        const double predicted = speed_ + last_imu_->longitudinal_acceleration * 0.01;
        speed_ = wheel_gain_ * last_wheel_->meters_per_second + (1.0 - wheel_gain_) * predicted;
        return state_->Write(VehicleState {
            speed_, last_imu_->longitudinal_acceleration, last_imu_->yaw_rate,
            last_wheel_->sample, last_imu_->sample});
    }

private:
    runtime::ComponentSpec spec_;
    std::optional<transport::TypedReader<WheelSpeed>> wheel_;
    std::optional<transport::TypedReader<ImuSample>> imu_;
    std::optional<transport::TypedWriter<VehicleState>> state_;
    std::optional<WheelSpeed> last_wheel_;
    std::optional<ImuSample> last_imu_;
    double wheel_gain_ {0.8};
    double speed_ {0.0};
};

}  // namespace vehicle
