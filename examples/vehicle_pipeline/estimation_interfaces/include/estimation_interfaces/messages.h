#pragma once

// Wire types published by the estimation team. Keep VehicleState's version in
// step with message_version in topics.json.

#include <cstdint>

#include <puppet_master/transport/typed.h>

namespace estimation_interfaces {

struct VehicleState {
    double speed;         // fused, m/s
    double acceleration;  // m/s^2
    double yaw_rate;      // rad/s
    std::uint64_t wheel_sample;
    std::uint64_t imu_sample;
};

}  // namespace estimation_interfaces

template <>
struct puppet_master::transport::MessageTypeName<estimation_interfaces::VehicleState> {
    static constexpr const char* value = "vehicle.VehicleState";
    static constexpr std::uint32_t version = 1;
};
