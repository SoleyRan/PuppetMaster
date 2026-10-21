#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "segment.h"

// Under ThreadSanitizer, PublishWith fills a private buffer instead of the
// slot, so the in-place address check does not apply.
#if defined(__SANITIZE_THREAD__)
#define PUPPETMASTER_SHM_TEST_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define PUPPETMASTER_SHM_TEST_TSAN 1
#endif
#endif

namespace core = puppet_master::core;
namespace detail = puppet_master::transport::shm::detail;
namespace transport = puppet_master::transport;

namespace {

using namespace std::chrono_literals;

struct TestSegment {
    std::string topic;
    std::shared_ptr<detail::Segment> segment;

    ~TestSegment()
    {
        segment.reset();
        detail::SharedRegion::Remove(detail::ObjectName(0, topic));
    }
};

std::string UniqueTopic(const char* tag)
{
    static int counter = 0;
    return std::string("/pm/segment-test/") + tag + "/"
        + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "/"
        + std::to_string(++counter);
}

detail::Segment::Identity Identity(const std::string& topic)
{
    return detail::Segment::Identity {topic, transport::MessageDescriptor {"test.Bytes", "application/octet-stream"}};
}

core::Result<std::shared_ptr<detail::Segment>> Open(const std::string& topic, std::size_t payload, std::size_t slots)
{
    return detail::Segment::Open(0, Identity(topic), detail::SegmentGeometry {payload, slots}, 1s);
}

TestSegment Make(const char* tag, std::size_t payload = 64, std::size_t slots = 4)
{
    TestSegment test {UniqueTopic(tag), nullptr};
    auto segment = Open(test.topic, payload, slots);
    assert(segment.ok());
    test.segment = segment.value();
    return test;
}

void Publish(detail::Segment& segment, const std::string& text, core::TimePoint timestamp = core::TimePoint {})
{
    auto sequence = segment.Publish(transport::ByteView::From(text.data(), text.size()), timestamp);
    assert(sequence.ok());
}

std::string Text(const transport::Message& message)
{
    return std::string(reinterpret_cast<const char*>(message.payload.data()), message.payload.size());
}

void GeometryAndClamp()
{
    assert(detail::SlotStride(0) == 64);
    assert(detail::SlotStride(1) == 128);
    assert(detail::SlotStride(64) == 128);
    assert(detail::SlotStride(65) == 192);
    assert(detail::HeaderSize() % 64 == 0);
    assert(detail::SegmentSize({64, 4}) == detail::HeaderSize() + 4 * 128);
    assert(detail::SegmentSize({64, 0}) == 0);
    assert(detail::SegmentSize({static_cast<std::size_t>(-1), 1}) == 0);
    assert(detail::SegmentSize({1024, static_cast<std::size_t>(-1) / 64}) == 0);

    assert(detail::ClampCursor(1, 0, 1) == 1);
    assert(detail::ClampCursor(1, 5, 1) == 5);
    assert(detail::ClampCursor(1, 5, 3) == 3);
    assert(detail::ClampCursor(4, 5, 3) == 4);
    assert(detail::ClampCursor(1, 2, 8) == 1);
}

void PublishThenReadInOrder()
{
    auto test = Make("order");
    std::uint64_t cursor = 1;
    transport::Message message;
    assert(!test.segment->TryRead(cursor, 4, message).has_message);
    assert(test.segment->Pending(cursor, 4) == 0);

    Publish(*test.segment, "one", core::TimePoint(core::Nanoseconds(111)));
    Publish(*test.segment, "two");
    assert(test.segment->WriteSequence() == 2);
    assert(test.segment->Pending(cursor, 4) == 2);

    auto outcome = test.segment->TryRead(cursor, 4, message);
    assert(outcome.has_message && outcome.dropped == 0);
    assert(Text(message) == "one");
    assert(message.metadata.sequence == 1);
    assert(message.metadata.source_timestamp == core::TimePoint(core::Nanoseconds(111)));

    outcome = test.segment->TryRead(cursor, 4, message);
    assert(outcome.has_message && Text(message) == "two" && message.metadata.sequence == 2);
    assert(!test.segment->TryRead(cursor, 4, message).has_message);
    assert(cursor == 3);
}

void EmptyAndMaxSizePayloads()
{
    auto test = Make("sizes", 8, 2);
    std::uint64_t cursor = 1;
    transport::Message message;

    Publish(*test.segment, "");
    assert(test.segment->TryRead(cursor, 2, message).has_message);
    assert(message.payload.empty());

    Publish(*test.segment, "12345678");
    assert(test.segment->TryRead(cursor, 2, message).has_message);
    assert(Text(message) == "12345678");

    const std::string too_big = "123456789";
    auto rejected = test.segment->Publish(transport::ByteView::From(too_big.data(), too_big.size()), {});
    assert(rejected.status().code() == core::StatusCode::kOutOfRange);
    assert(test.segment->WriteSequence() == 2);
}

void LatestAndDepthClampCountDrops()
{
    auto test = Make("depth", 16, 4);
    for (int index = 1; index <= 6; ++index) {
        Publish(*test.segment, std::to_string(index));
    }

    // kLatest: depth 1 jumps to the newest.
    std::uint64_t latest = 1;
    transport::Message message;
    auto outcome = test.segment->TryRead(latest, 1, message);
    assert(outcome.has_message && Text(message) == "6" && outcome.dropped == 5);

    // Depth 3: starts three back from the newest.
    std::uint64_t queued = 1;
    assert(test.segment->Pending(queued, 3) == 3);
    outcome = test.segment->TryRead(queued, 3, message);
    assert(outcome.has_message && Text(message) == "4" && outcome.dropped == 3);
    assert(test.segment->TryRead(queued, 3, message).has_message && Text(message) == "5");
    assert(test.segment->TryRead(queued, 3, message).has_message && Text(message) == "6");

    // Depth above the ring is capped to the ring.
    std::uint64_t capped = 1;
    assert(test.segment->Pending(capped, 100) == 4);
    outcome = test.segment->TryRead(capped, 100, message);
    assert(outcome.has_message && Text(message) == "3" && outcome.dropped == 2);
}

void OverwrittenAndInterruptedSlotsAreSkipped()
{
    auto test = Make("skip", 16, 4);
    Publish(*test.segment, "a");
    Publish(*test.segment, "b");

    // Simulate a writer that died mid-write over sequence 1's slot, as the
    // next lap would: marks slot 1 % 4 with the writing bit for sequence 5.
    auto* slots = reinterpret_cast<unsigned char*>(&test.segment->header()) + detail::HeaderSize();
    auto* slot1 = reinterpret_cast<detail::SlotHeader*>(slots + 1 * detail::SlotStride(16));
    slot1->seq.store(5 | detail::kWritingBit);

    std::uint64_t cursor = 1;
    transport::Message message;
    auto outcome = test.segment->TryRead(cursor, 4, message);
    assert(outcome.has_message && Text(message) == "b" && outcome.dropped == 1);

    // A corrupt payload size is treated as a lost message, never read.
    Publish(*test.segment, "c");
    auto* slot3 = reinterpret_cast<detail::SlotHeader*>(slots + 3 * detail::SlotStride(16));
    slot3->payload_size.store(1u << 20);
    outcome = test.segment->TryRead(cursor, 4, message);
    assert(!outcome.has_message && outcome.dropped == 1);
    assert(cursor == 4);
}

void PublishWithFillsTheSlotInPlace()
{
    auto test = Make("fill", 16, 4);
    const auto* slots = reinterpret_cast<const unsigned char*>(&test.segment->header()) + detail::HeaderSize();
    const unsigned char* filled_at = nullptr;

    auto published = test.segment->PublishWith(
        16,
        [&](transport::MutableByteView buffer) {
            assert(buffer.size() == 16);
            filled_at = buffer.data();
            std::memcpy(buffer.data(), "in-place", 8);
            return core::Result<std::size_t>(8);
        },
        core::TimePoint(core::Nanoseconds(7)));
    assert(published.ok() && published.value() == 1);
#ifndef PUPPETMASTER_SHM_TEST_TSAN
    // Sequence 1 lives in slot 1; its payload starts one header past the slot.
    assert(filled_at == slots + detail::SlotStride(16) + 64);
#else
    (void)slots;
#endif

    std::uint64_t cursor = 1;
    transport::Message message;
    assert(test.segment->TryRead(cursor, 4, message).has_message);
    assert(Text(message) == "in-place");
    assert(message.metadata.source_timestamp == core::TimePoint(core::Nanoseconds(7)));

    // A failed, throwing, or oversized fill publishes nothing and keeps the
    // previous message readable.
    auto failed = test.segment->PublishWith(
        16, [](transport::MutableByteView) { return core::Result<std::size_t>::FromStatus(core::Status::Cancelled("no")); },
        {});
    assert(failed.status().code() == core::StatusCode::kCancelled);
    auto thrown = test.segment->PublishWith(
        16, [](transport::MutableByteView) -> core::Result<std::size_t> { throw std::runtime_error("boom"); }, {});
    assert(thrown.status().code() == core::StatusCode::kInternal);
    auto oversized = test.segment->PublishWith(
        8, [](transport::MutableByteView) { return core::Result<std::size_t>(9); }, {});
    assert(oversized.status().code() == core::StatusCode::kOutOfRange);
    auto too_big = test.segment->PublishWith(
        17, [](transport::MutableByteView) { return core::Result<std::size_t>(0); }, {});
    assert(too_big.status().code() == core::StatusCode::kOutOfRange);
    assert(test.segment->WriteSequence() == 1);

    // The lock was released after each failure, and the sequence is reused.
    Publish(*test.segment, "next");
    assert(test.segment->WriteSequence() == 2);
    assert(test.segment->TryRead(cursor, 4, message).has_message && Text(message) == "next");
}

void AttachValidatesIdentityAndGeometry()
{
    auto test = Make("attach", 64, 4);
    Publish(*test.segment, "shared");

    auto same = Open(test.topic, 64, 4);
    assert(same.ok());
    std::uint64_t cursor = 1;
    transport::Message message;
    assert(same.value()->TryRead(cursor, 4, message).has_message && Text(message) == "shared");

    auto other_size = Open(test.topic, 128, 4);
    assert(other_size.status().code() == core::StatusCode::kFailedPrecondition);
    auto other_count = Open(test.topic, 64, 8);
    assert(other_count.status().code() == core::StatusCode::kFailedPrecondition);

    auto other_type = detail::Segment::Open(
        0, detail::Segment::Identity {test.topic, transport::MessageDescriptor {"test.Other", "application/octet-stream"}},
        detail::SegmentGeometry {64, 4}, 1s);
    assert(other_type.status().code() == core::StatusCode::kInvalidArgument);

    const std::string long_name(300, 'x');
    assert(Open("/" + long_name, 64, 4).status().code() == core::StatusCode::kInvalidArgument);
}

void ParticipantsClaimReleaseAndReclaim()
{
    auto test = Make("participants");
    std::vector<std::uint32_t> claimed;
    for (std::uint32_t index = 0; index < detail::kMaxParticipants; ++index) {
        auto participant = test.segment->ClaimParticipant();
        assert(participant.ok());
        claimed.push_back(participant.value());
    }
    assert(test.segment->ActiveParticipants().size() == detail::kMaxParticipants);
    assert(test.segment->ClaimParticipant().status().code() == core::StatusCode::kResourceExhausted);

    test.segment->ReleaseParticipant(claimed[5]);
    auto again = test.segment->ClaimParticipant();
    assert(again.ok() && again.value() == claimed[5]);

    // An entry left by a dead process is reclaimed on the next claim.
    auto& entry = test.segment->header().participants[claimed[9]];
    entry.pid.store(0);
    auto reclaimed = test.segment->ClaimParticipant();
    assert(reclaimed.ok() && reclaimed.value() == claimed[9]);

    for (const auto index : claimed) {
        test.segment->ReleaseParticipant(index);
    }
    assert(test.segment->ActiveParticipants().empty());
}

// Readers racing a writer over a small ring must never return a torn or
// out-of-order payload. Each payload repeats its own sequence number.
void ConcurrentReadersSeeConsistentPayloads()
{
    auto test = Make("stress", 256, 4);
    constexpr std::uint64_t kMessages = 20000;
    std::atomic<bool> done {false};
    std::atomic<std::uint64_t> received {0};

    std::vector<std::thread> readers;
    for (int reader = 0; reader < 3; ++reader) {
        readers.emplace_back([&, depth = reader == 0 ? 1u : 4u] {
            std::uint64_t cursor = 1;
            std::uint64_t last = 0;
            transport::Message message;
            while (!done.load() || cursor <= test.segment->WriteSequence()) {
                if (!test.segment->TryRead(cursor, depth, message).has_message) {
                    std::this_thread::yield();
                    continue;
                }
                const auto sequence = message.metadata.sequence;
                assert(sequence > last);
                last = sequence;
                assert(message.payload.size() == 8 + sequence % 200);
                for (std::size_t offset = 0; offset + 8 <= message.payload.size(); offset += 8) {
                    std::uint64_t value = 0;
                    std::memcpy(&value, message.payload.data() + offset, 8);
                    assert(value == sequence);
                }
                received.fetch_add(1);
            }
        });
    }

    std::vector<unsigned char> payload;
    for (std::uint64_t sequence = 1; sequence <= kMessages; ++sequence) {
        payload.assign(8 + sequence % 200, 0);
        for (std::size_t offset = 0; offset + 8 <= payload.size(); offset += 8) {
            std::memcpy(payload.data() + offset, &sequence, 8);
        }
        auto published = test.segment->Publish(transport::ByteView(payload.data(), payload.size()), {});
        assert(published.ok() && published.value() == sequence);
    }
    done.store(true);
    for (auto& reader : readers) {
        reader.join();
    }
    assert(received.load() > 0);
}

}  // namespace

int main()
{
    GeometryAndClamp();
    PublishThenReadInOrder();
    EmptyAndMaxSizePayloads();
    LatestAndDepthClampCountDrops();
    OverwrittenAndInterruptedSlotsAreSkipped();
    PublishWithFillsTheSlotInPlace();
    AttachValidatesIdentityAndGeometry();
    ParticipantsClaimReleaseAndReclaim();
    ConcurrentReadersSeeConsistentPayloads();
    std::cout << "shm segment tests passed\n";
    return 0;
}
