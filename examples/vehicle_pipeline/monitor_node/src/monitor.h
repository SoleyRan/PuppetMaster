#pragma once

#include <csignal>
#include <cstdint>
#include <iostream>
#include <optional>
#include <utility>

#include <puppet_master/puppet_master.h>
#include <puppet_master/runtime/parameters.h>
#include <puppet_master/runtime/typed_endpoints.h>
#include <puppet_master/transport/typed.h>

#include <estimation_interfaces/messages.h>

namespace monitor_node {

namespace core = puppet_master::core;
namespace runtime = puppet_master::runtime;
namespace transport = puppet_master::transport;
using estimation_interfaces::VehicleState;

// Consumes vehicle states, checks them, and reports lost messages. With
// `max_states` set, stops the process after that many states.
class StateMonitor final : public runtime::Component {
public:
    explicit StateMonitor(runtime::ComponentSpec spec) : spec_(std::move(spec)) {}

    runtime::ComponentSpec Describe() const override { return spec_; }

    core::Status Configure(runtime::ComponentContext& context) override
    {
        auto max_states = runtime::GetParameter<std::int64_t>(spec_.parameters, "max_states", 0);
        auto max_speed = runtime::GetParameter(spec_.parameters, "max_speed", 40.0);
        if (!max_states.ok()) {
            return max_states.status();
        }
        if (!max_speed.ok()) {
            return max_speed.status();
        }
        max_states_ = static_cast<std::uint64_t>(max_states.value());
        max_speed_ = max_speed.value();
        auto reader = runtime::MakeTypedReader<VehicleState>(context, spec_);
        if (!reader.ok()) {
            return reader.status();
        }
        reader_.emplace(std::move(reader).value());
        return core::Status::Ok();
    }

    core::Status Execute(runtime::ComponentContext&) override
    {
        for (auto message = reader_->Read(); message.ok(); message = reader_->Read()) {
            const auto& state = message.value().value;
            const auto sequence = message.value().metadata.sequence;
            if (last_sequence_ != 0 && sequence > last_sequence_ + 1) {
                lost_ += sequence - last_sequence_ - 1;
            }
            last_sequence_ = sequence;
            if (state.speed < 0.0 || state.speed > max_speed_) {
                ++out_of_range_;
            }
            ++received_;
            if (received_ % 50 == 0 || received_ == max_states_) {
                std::cout << "state " << received_ << ": speed " << state.speed << " m/s, wheel #"
                          << state.wheel_sample << ", imu #" << state.imu_sample << std::endl;
            }
            if (max_states_ != 0 && received_ == max_states_) {
                std::raise(SIGINT);  // the launcher turns this into a clean shutdown
            }
        }
        return core::Status::Ok();
    }

    core::Status Shutdown(runtime::ComponentContext&) override
    {
        std::cout << "monitor summary: received " << received_ << ", lost " << lost_ << ", out of range "
                  << out_of_range_ << std::endl;
        return core::Status::Ok();
    }

private:
    runtime::ComponentSpec spec_;
    std::optional<transport::TypedReader<VehicleState>> reader_;
    std::uint64_t max_states_ {0};
    double max_speed_ {40.0};
    std::uint64_t received_ {0};
    std::uint64_t lost_ {0};
    std::uint64_t out_of_range_ {0};
    std::uint64_t last_sequence_ {0};
};

}  // namespace monitor_node
