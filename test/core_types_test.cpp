#include <cassert>
#include <chrono>
#include <string>

#include <puppet_master/puppet_master.h>

namespace pm = puppet_master::core;

int main()
{
    const auto status = pm::Status::Unavailable("transport is not registered");
    assert(!status.ok());
    assert(status.code() == pm::StatusCode::kUnavailable);
    assert(status.ToString() == "unavailable: transport is not registered");
    assert(std::string(pm::StatusCodeName(pm::StatusCode::kOk)) == "ok");

    pm::Result<int> answer(42);
    assert(answer.ok());
    assert(answer.value() == 42);

    auto failed_answer =
        pm::Result<int>::FromStatus(pm::Status::NotFound("topic does not exist"));
    assert(!failed_answer.ok());
    assert(failed_answer.status().code() == pm::StatusCode::kNotFound);

    pm::Result<void> done;
    assert(done.ok());

    auto topic = pm::TopicName::Create("/vehicle/speed");
    assert(topic.ok());
    assert(topic.value().str() == "/vehicle/speed");

    auto bad_topic = pm::TopicName::Create("bad topic");
    assert(!bad_topic.ok());
    assert(bad_topic.status().code() == pm::StatusCode::kInvalidArgument);

    pm::MessagePolicy policy;
    assert(policy.Validate().ok());
    assert(std::string(pm::ToString(policy.delivery)) == "best_effort");
    policy.delivery = pm::DeliveryGuarantee::kReliable;
    policy.retention = pm::RetentionPolicy::kKeepLast;
    policy.freshness = pm::FreshnessPolicy::kQueued;
    policy.overflow = pm::QueueOverflowPolicy::kBlock;
    policy.queue_depth = 0;
    assert(!policy.Validate().ok());

    pm::TriggerSpec trigger;
    trigger.kind = pm::TriggerKind::kPeriodic;
    trigger.period = std::chrono::milliseconds(20);
    trigger.data_dependencies.push_back(topic.value());
    assert(trigger.period == std::chrono::milliseconds(20));
    assert(std::string(pm::ToString(trigger.kind)) == "periodic");

    return 0;
}
