#include <cstdint>
#include <iostream>

#include <puppet_master/puppet_master.h>

namespace compat = puppet_master::compat::itage;
namespace core = puppet_master::core;

namespace {

struct SpeedSample {
    double speed_mps {0.0};
    std::int32_t gear {0};
};

int Fail(const char* message, const core::Status& status)
{
    std::cerr << message << ": " << status.ToString() << '\n';
    return 1;
}

}  // namespace

int main()
{
    compat::TopicOptions topic;
    topic.message_type = "demo.SpeedSample";
    topic.encoding = "application/x-struct";
    topic.fixed_payload_size = sizeof(SpeedSample);
    topic.message_policy.freshness = core::FreshnessPolicy::kQueued;
    topic.message_policy.queue_depth = 4;

    compat::Node node("legacy_speed_node");
    if (!node.last_status().ok()) {
        return Fail("failed to create compatibility node", node.last_status());
    }

    auto reader = node.CreateReader("/compat/demo/speed", topic);
    if (!reader) {
        return Fail("failed to create compatibility reader", node.last_status());
    }

    auto writer = node.CreateWriter("/compat/demo/speed", topic);
    if (!writer) {
        return Fail("failed to create compatibility writer", node.last_status());
    }

    reader->SetCallBack([]() {
        std::cout << "compat callback: data available\n";
    });

    SpeedSample sample;
    sample.speed_mps = 12.5;
    sample.gear = 3;

    if (writer->Write(&sample) < 0) {
        return Fail("failed to write compatibility sample", writer->last_status());
    }

    SpeedSample received;
    std::int64_t time_diff_ms = 0;
    if (reader->Read(&received, time_diff_ms) < 0) {
        return Fail("failed to read compatibility sample", reader->last_status());
    }

    std::cout << "speed: " << received.speed_mps << " m/s\n";
    std::cout << "gear: " << received.gear << '\n';
    std::cout << "time diff: " << time_diff_ms << " ms\n";
    return 0;
}
