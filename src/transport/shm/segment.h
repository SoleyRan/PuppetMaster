#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <puppet_master/core/result.h>
#include <puppet_master/core/status.h>
#include <puppet_master/core/types.h>
#include <puppet_master/transport/message.h>
#include <puppet_master/transport/transport.h>

#include "platform.h"

// Shared-memory layout and seqlock ring of one topic. Internal; see the
// "Segment Layout" and "Publish And Read Protocol" sections of
// docs/shm-transport-design.md.
namespace puppet_master::transport::shm::detail {

inline constexpr std::uint64_t kMagic = 0x3130304d48534d50ULL;  // "PMSHM001", little-endian
inline constexpr std::uint32_t kLayoutVersion = 1;
inline constexpr std::size_t kNameCapacity = 256;
inline constexpr std::uint64_t kWritingBit = 1ULL << 63;
inline constexpr std::size_t kSlotAlignment = 64;

struct SegmentGeometry {
    std::size_t slot_payload_size {0};
    std::size_t slot_count {0};
};

// `state` packs a 2-bit kind with a 30-bit generation that changes on every
// claim, so a stale compare-and-swap cannot free an entry that was reclaimed
// and claimed again in between.
struct ParticipantEntry {
    std::atomic<std::uint32_t> state;
    std::atomic<std::uint32_t> pid;
    std::atomic<std::uint64_t> start_time;
};

inline constexpr std::uint32_t kParticipantFree = 0;
inline constexpr std::uint32_t kParticipantClaiming = 1;
inline constexpr std::uint32_t kParticipantActive = 2;
inline constexpr std::uint32_t kParticipantKindMask = 3;

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324)  // padding from alignas is intentional: keeps write_seq off the name fields' cache lines
#endif
struct SegmentHeader {
    std::atomic<std::uint64_t> magic;
    std::uint32_t layout_version;
    std::uint32_t header_size;
    std::uint64_t slot_payload_size;
    std::uint64_t slot_stride;
    std::uint32_t slot_count;
    std::uint32_t reserved0;
    char topic_name[kNameCapacity];
    char type_name[kNameCapacity];
    char encoding[kNameCapacity];
    alignas(kSlotAlignment) std::atomic<std::uint64_t> write_seq;
    std::atomic<std::uint32_t> notify_word;
    std::uint32_t reserved1;
    alignas(WriterLock::kStorageAlignment) unsigned char lock_storage[WriterLock::kStorageSize];
    ParticipantEntry participants[kMaxParticipants];
};
#ifdef _MSC_VER
#pragma warning(pop)
#endif

// Fields other than `seq` are written inside the seqlock and read
// speculatively, so they are relaxed atomics rather than plain words.
struct SlotHeader {
    std::atomic<std::uint64_t> seq;
    std::atomic<std::int64_t> source_timestamp_ns;
    std::atomic<std::uint64_t> payload_size;
};

static_assert(std::atomic<std::uint64_t>::is_always_lock_free, "shared atomics must be lock-free");
static_assert(sizeof(std::atomic<std::uint64_t>) == sizeof(std::uint64_t), "shared atomics must be plain words");

std::size_t SlotStride(std::size_t slot_payload_size);
std::size_t HeaderSize();
// Total bytes for the geometry, or 0 if it overflows size_t.
std::size_t SegmentSize(const SegmentGeometry& geometry);

// Result of one reader pass over the ring.
struct ReadOutcome {
    bool has_message {false};
    std::uint64_t dropped {0};  // sequences skipped by freshness, overwrite, or a torn copy
};

// One mapped topic segment. Thread-safe: writers serialize through the
// cross-process lock, readers only touch their own cursor.
class Segment {
public:
    struct Identity {
        std::string topic;
        MessageDescriptor descriptor;
    };

    // Creates the segment or attaches to an existing one, verifying that its
    // layout, geometry, and identity match.
    static core::Result<std::shared_ptr<Segment>> Open(
        core::DomainId domain, const Identity& identity, const SegmentGeometry& geometry,
        core::Nanoseconds init_timeout);

    ~Segment();
    Segment(const Segment&) = delete;
    Segment& operator=(const Segment&) = delete;

    const SegmentGeometry& geometry() const noexcept { return geometry_; }
    const std::string& name() const noexcept { return name_; }
    SegmentHeader& header() const noexcept { return *header_; }
    std::uint64_t WriteSequence() const noexcept;

    // Writer protocol. Returns the committed sequence.
    core::Result<std::uint64_t> Publish(ByteView payload, core::TimePoint source_timestamp);

    // Zero-copy variant: `fill` writes the message directly into the slot
    // while this process holds the cross-process writer lock, so it should be
    // short and must not publish to the same topic. A failed fill publishes
    // nothing.
    core::Result<std::uint64_t> PublishWith(
        std::size_t capacity, const FillFunction& fill, core::TimePoint source_timestamp);

    // Reader protocol. `cursor` is the next sequence the reader wants; `depth`
    // bounds how far back it may start. On success the payload, sequence, and
    // source timestamp are written to `message`.
    ReadOutcome TryRead(std::uint64_t& cursor, std::uint64_t depth, Message& message) const;

    // Unread messages for a reader at `cursor`, after the depth clamp.
    std::uint64_t Pending(std::uint64_t cursor, std::uint64_t depth) const noexcept;

    // Participant table. Claims reclaim entries of dead processes first.
    core::Result<std::uint32_t> ClaimParticipant();
    void ReleaseParticipant(std::uint32_t index) noexcept;
    std::vector<std::uint32_t> ActiveParticipants() const;

    // Test-only: called inside Publish after the payload copy, before the
    // commit, with the writer lock held. Crash tests use it to kill a writer
    // at the worst moment. Not thread-safe to change while publishing.
    void SetCrashHookForTesting(void (*hook)()) noexcept { crash_hook_ = hook; }

private:
    Segment(std::unique_ptr<SharedRegion> region, std::unique_ptr<WriterLock> lock, std::string name,
            SegmentGeometry geometry) noexcept;

    unsigned char* SlotAt(std::uint64_t sequence) const noexcept;
    void ReclaimDeadParticipants() noexcept;

    std::unique_ptr<SharedRegion> region_;
    std::unique_ptr<WriterLock> lock_;
    std::string name_;
    SegmentGeometry geometry_;
    SegmentHeader* header_;
    unsigned char* slots_;
    std::size_t stride_;
    void (*crash_hook_)() {nullptr};
};

// Applies the freshness clamp: the first sequence a reader at `cursor` should
// consider when the newest is `newest`. Pure, for unit tests.
std::uint64_t ClampCursor(std::uint64_t cursor, std::uint64_t newest, std::uint64_t depth) noexcept;

}  // namespace puppet_master::transport::shm::detail
