#include <cassert>
#include <cstdint>
#include <string>

#include <puppet_master/puppet_master.h>

namespace compat = puppet_master::compat::itage;
namespace core = puppet_master::core;

namespace {

struct SpeedSample {
    double speed_mps {0.0};
    std::int32_t gear {0};
};

compat::TopicOptions MakeSpeedTopicOptions()
{
    compat::TopicOptions options;
    options.message_type = "test.SpeedSample";
    options.encoding = "application/x-struct";
    options.fixed_payload_size = sizeof(SpeedSample);
    options.message_policy.freshness = core::FreshnessPolicy::kQueued;
    options.message_policy.queue_depth = 4;
    return options;
}

void FixedSizeStructCanUseLegacyReadAndWrite()
{
    compat::Node node("legacy_speed_node");
    assert(node.last_status().ok());

    const auto options = MakeSpeedTopicOptions();
    auto reader = node.CreateReader("/compat/speed", options);
    auto writer = node.CreateWriter("/compat/speed", options);
    assert(reader);
    assert(writer);

    int callback_count = 0;
    reader->SetCallBack([&callback_count]() {
        ++callback_count;
    });
    assert(reader->last_status().ok());

    SpeedSample sample;
    sample.speed_mps = 12.5;
    sample.gear = 3;

    assert(writer->Write(&sample) == static_cast<int>(sizeof(SpeedSample)));
    assert(writer->last_status().ok());
    assert(callback_count == 1);

    SpeedSample received;
    std::int64_t time_diff_ms = -1;
    assert(reader->Read(&received, time_diff_ms) == static_cast<int>(sizeof(SpeedSample)));
    assert(reader->last_status().ok());
    assert(received.speed_mps == sample.speed_mps);
    assert(received.gear == sample.gear);
    assert(time_diff_ms >= 0);
}

void RawBytesCanUseExplicitCapacityRead()
{
    compat::Node node("legacy_bytes_node");
    assert(node.last_status().ok());

    compat::TopicOptions options;
    options.message_type = "test.RawBytes";
    options.encoding = "text/plain";
    options.message_policy.freshness = core::FreshnessPolicy::kQueued;
    options.message_policy.queue_depth = 4;

    auto reader = node.CreateReader("/compat/raw", options);
    auto writer = node.CreateWriter("/compat/raw", options);
    assert(reader);
    assert(writer);

    const std::string payload = "speed=12.5";
    const auto written = writer->Write(const_cast<char*>(payload.data()), payload.size());
    assert(written == static_cast<int>(payload.size()));

    char buffer[32] {};
    std::int64_t time_diff_ms = -1;
    const auto read_size = reader->Read(buffer, sizeof(buffer), time_diff_ms);
    assert(read_size == static_cast<int>(payload.size()));
    assert(std::string(buffer, buffer + read_size) == payload);
}

void AttributePointerCanCarryTopicOptions()
{
    compat::Node node("legacy_attribute_node");
    assert(node.last_status().ok());

    auto options = MakeSpeedTopicOptions();
    auto reader = node.CreateReader("/compat/attribute", nullptr, &options);
    auto writer = node.CreateWriter("/compat/attribute", nullptr, &options);
    assert(reader);
    assert(writer);

    SpeedSample sample;
    sample.speed_mps = 8.0;
    sample.gear = 2;

    assert(writer->Write(&sample) == static_cast<int>(sizeof(SpeedSample)));

    SpeedSample received;
    std::int64_t time_diff_ms = -1;
    assert(reader->Read(&received, time_diff_ms) == static_cast<int>(sizeof(SpeedSample)));
    assert(received.speed_mps == sample.speed_mps);
    assert(received.gear == sample.gear);
}

void UnsafeLegacyReadRequiresFixedPayloadSize()
{
    compat::Node node("legacy_safe_read_node");
    assert(node.last_status().ok());

    compat::TopicOptions options;
    options.message_type = "test.RawBytes";
    options.encoding = "text/plain";

    auto reader = node.CreateReader("/compat/no_fixed_size", options);
    assert(reader);

    char buffer[8] {};
    std::int64_t time_diff_ms = -1;
    assert(reader->Read(buffer, time_diff_ms) == -1);
    assert(reader->last_status().code() == core::StatusCode::kFailedPrecondition);
}

void InvalidTopicIsReportedOnNode()
{
    compat::Node node("legacy_invalid_topic_node");
    assert(node.last_status().ok());

    auto writer = node.CreateWriter("/compat/has space", compat::TopicOptions {});
    assert(!writer);
    assert(node.last_status().code() == core::StatusCode::kInvalidArgument);
}

}  // namespace

int main()
{
    FixedSizeStructCanUseLegacyReadAndWrite();
    RawBytesCanUseExplicitCapacityRead();
    AttributePointerCanCarryTopicOptions();
    UnsafeLegacyReadRequiresFixedPayloadSize();
    InvalidTopicIsReportedOnNode();
    return 0;
}
