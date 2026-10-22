#pragma once

// Wire types shared by every process of the pipeline. All are trivially
// copyable, so TrivialCodec carries them; every process must be built with the
// same compiler ABI.

#include <cstdint>

#include <puppet_master/transport/typed.h>

namespace vehicle {

struct WheelSpeed {
    double meters_per_second;
    std::uint64_t sample;
};

struct ImuSample {
    double longitudinal_acceleration;  // m/s^2
    double yaw_rate;                   // rad/s
    std::uint64_t sample;
};

struct VehicleState {
    double speed;         // fused, m/s
    double acceleration;  // m/s^2
    double yaw_rate;      // rad/s
    std::uint64_t wheel_sample;
    std::uint64_t imu_sample;
};

}  // namespace vehicle

template <>
struct puppet_master::transport::MessageTypeName<vehicle::WheelSpeed> {
    static constexpr const char* value = "vehicle.WheelSpeed";
};

template <>
struct puppet_master::transport::MessageTypeName<vehicle::ImuSample> {
    static constexpr const char* value = "vehicle.ImuSample";
};

template <>
struct puppet_master::transport::MessageTypeName<vehicle::VehicleState> {
    static constexpr const char* value = "vehicle.VehicleState";
};
