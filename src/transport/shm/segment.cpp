#include "segment.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>
#include <thread>
#include <utility>

#if defined(__SANITIZE_THREAD__)
#define PUPPETMASTER_SHM_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define PUPPETMASTER_SHM_TSAN 1
#endif
#endif

namespace puppet_master::transport::shm::detail {

namespace {

constexpr std::size_t kSlotPayloadOffset = kSlotAlignment;
static_assert(sizeof(SlotHeader) <= kSlotPayloadOffset, "slot header must fit before the payload");

// Rounds up to a multiple of kSlotAlignment, or returns 0 on overflow.
std::size_t AlignUp(std::size_t value)
{
    if (value > std::numeric_limits<std::size_t>::max() - (kSlotAlignment - 1)) {
        return 0;
    }
    return (value + kSlotAlignment - 1) / kSlotAlignment * kSlotAlignment;
}

// The seqlock copies race with a concurrent writer by design; the sequence
// re-check discards torn results. Under ThreadSanitizer the copies use relaxed
// atomic byte accesses so the intended race is not reported.
void CopyToShared(unsigned char* destination, const unsigned char* source, std::size_t size)
{
#ifdef PUPPETMASTER_SHM_TSAN
    for (std::size_t index = 0; index < size; ++index) {
        __atomic_store_n(destination + index, source[index], __ATOMIC_RELAXED);
    }
#else
    if (size != 0) {
        std::memcpy(destination, source, size);
    }
#endif
}

void CopyFromShared(unsigned char* destination, const unsigned char* source, std::size_t size)
{
#ifdef PUPPETMASTER_SHM_TSAN
    for (std::size_t index = 0; index < size; ++index) {
        destination[index] = __atomic_load_n(source + index, __ATOMIC_RELAXED);
    }
#else
    if (size != 0) {
        std::memcpy(destination, source, size);
    }
#endif
}

bool FitsName(const std::string& value)
{
    return value.size() < kNameCapacity;
}

void StoreName(char (&field)[kNameCapacity], const std::string& value)
{
    std::memcpy(field, value.data(), value.size());
    field[value.size()] = '\0';
}

// Compares a NUL-terminated header field without trusting its terminator.
bool NameEquals(const char (&field)[kNameCapacity], const std::string& value)
{
    const void* end = std::memchr(field, '\0', kNameCapacity);
    if (end == nullptr) {
        return false;
    }
    const auto length = static_cast<std::size_t>(static_cast<const char*>(end) - field);
    return length == value.size() && std::memcmp(field, value.data(), length) == 0;
}

std::string RemoveHint(const std::string& name)
{
    return " (segment " + name + "; remove it with shm::RemoveSegment if it is stale)";
}

std::uint32_t Kind(std::uint32_t state)
{
    return state & kParticipantKindMask;
}

std::uint32_t Generation(std::uint32_t state)
{
    return state & ~kParticipantKindMask;
}

std::uint32_t NextGeneration(std::uint32_t state)
{
    return Generation(state) + (kParticipantKindMask + 1);
}

}  // namespace

std::size_t SlotStride(std::size_t slot_payload_size)
{
    if (slot_payload_size > std::numeric_limits<std::size_t>::max() - kSlotPayloadOffset) {
        return 0;
    }
    return AlignUp(kSlotPayloadOffset + slot_payload_size);
}

std::size_t HeaderSize()
{
    return AlignUp(sizeof(SegmentHeader));
}

std::size_t SegmentSize(const SegmentGeometry& geometry)
{
    const auto stride = SlotStride(geometry.slot_payload_size);
    if (stride == 0 || geometry.slot_count == 0) {
        return 0;
    }
    const auto header = HeaderSize();
    if (geometry.slot_count > (std::numeric_limits<std::size_t>::max() - header) / stride) {
        return 0;
    }
    return header + geometry.slot_count * stride;
}

std::uint64_t ClampCursor(std::uint64_t cursor, std::uint64_t newest, std::uint64_t depth) noexcept
{
    const std::uint64_t oldest_kept = newest >= depth ? newest - depth + 1 : 1;
    return std::max(cursor, oldest_kept);
}

core::Result<std::shared_ptr<Segment>> Segment::Open(
    core::DomainId domain, const Identity& identity, const SegmentGeometry& geometry,
    core::Nanoseconds init_timeout)
{
    using ResultType = core::Result<std::shared_ptr<Segment>>;
    if (!FitsName(identity.topic) || !FitsName(identity.descriptor.type_name)
        || !FitsName(identity.descriptor.encoding)) {
        return ResultType::FromStatus(core::Status::InvalidArgument(
            "shm topic, type name, and encoding must each be shorter than 256 bytes"));
    }
    const auto size = SegmentSize(geometry);
    if (size == 0 || geometry.slot_count > std::numeric_limits<std::uint32_t>::max()) {
        return ResultType::FromStatus(core::Status::InvalidArgument("shm segment geometry is too large"));
    }

    const auto name = ObjectName(domain, identity.topic);
    auto region = SharedRegion::CreateOrOpen(name, size, init_timeout);
    if (!region.ok()) {
        return ResultType::FromStatus(region.status());
    }
    auto* header = static_cast<SegmentHeader*>(region.value()->data());

    if (region.value()->created()) {
        header->layout_version = kLayoutVersion;
        header->header_size = static_cast<std::uint32_t>(HeaderSize());
        header->slot_payload_size = geometry.slot_payload_size;
        header->slot_stride = SlotStride(geometry.slot_payload_size);
        header->slot_count = static_cast<std::uint32_t>(geometry.slot_count);
        StoreName(header->topic_name, identity.topic);
        StoreName(header->type_name, identity.descriptor.type_name);
        StoreName(header->encoding, identity.descriptor.encoding);
        auto status = WriterLock::InitializeStorage(header->lock_storage);
        if (!status.ok()) {
            // Unlink so the next opener does not wait for a magic that never comes.
            SharedRegion::Remove(name);
            return ResultType::FromStatus(std::move(status));
        }
        header->write_seq.store(0, std::memory_order_relaxed);
        // Publishing the magic last makes the header visible to openers.
        header->magic.store(kMagic, std::memory_order_release);
    } else {
        if (region.value()->size() < HeaderSize()) {
            return ResultType::FromStatus(core::Status::FailedPrecondition(
                "shm segment is smaller than its header" + RemoveHint(name)));
        }
        const auto deadline = core::SteadyClock::now() + init_timeout;
        while (header->magic.load(std::memory_order_acquire) != kMagic) {
            if (core::SteadyClock::now() >= deadline) {
                return ResultType::FromStatus(core::Status::FailedPrecondition(
                    "shm segment was never initialized" + RemoveHint(name)));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (header->layout_version != kLayoutVersion || header->header_size != HeaderSize()) {
            return ResultType::FromStatus(core::Status::FailedPrecondition(
                "shm segment has an incompatible layout version" + RemoveHint(name)));
        }
        if (!NameEquals(header->topic_name, identity.topic)) {
            return ResultType::FromStatus(core::Status::FailedPrecondition(
                "shm segment name collides with another topic" + RemoveHint(name)));
        }
        if (header->slot_payload_size != geometry.slot_payload_size || header->slot_count != geometry.slot_count
            || header->slot_stride != SlotStride(geometry.slot_payload_size)) {
            return ResultType::FromStatus(core::Status::FailedPrecondition(
                "shm topic " + identity.topic + " exists with slot size "
                + std::to_string(header->slot_payload_size) + " x " + std::to_string(header->slot_count)
                + " slots; this process requested " + std::to_string(geometry.slot_payload_size) + " x "
                + std::to_string(geometry.slot_count) + RemoveHint(name)));
        }
        if (!NameEquals(header->type_name, identity.descriptor.type_name)
            || !NameEquals(header->encoding, identity.descriptor.encoding)) {
            return ResultType::FromStatus(core::Status::InvalidArgument(
                "shm topic already exists with a different message descriptor: " + identity.topic));
        }
        if (region.value()->size() < size) {
            return ResultType::FromStatus(core::Status::FailedPrecondition(
                "shm segment is smaller than its declared geometry" + RemoveHint(name)));
        }
    }

    auto lock = WriterLock::Open(header->lock_storage, name);
    if (!lock.ok()) {
        return ResultType::FromStatus(lock.status());
    }
    return ResultType(std::shared_ptr<Segment>(
        new Segment(std::move(region).value(), std::move(lock).value(), name, geometry)));
}

Segment::Segment(std::unique_ptr<SharedRegion> region, std::unique_ptr<WriterLock> lock, std::string name,
                 SegmentGeometry geometry) noexcept
    : region_(std::move(region)),
      lock_(std::move(lock)),
      name_(std::move(name)),
      geometry_(geometry),
      header_(static_cast<SegmentHeader*>(region_->data())),
      slots_(static_cast<unsigned char*>(region_->data()) + HeaderSize()),
      stride_(SlotStride(geometry.slot_payload_size))
{
}

Segment::~Segment() = default;

std::uint64_t Segment::WriteSequence() const noexcept
{
    return header_->write_seq.load(std::memory_order_acquire);
}

unsigned char* Segment::SlotAt(std::uint64_t sequence) const noexcept
{
    return slots_ + static_cast<std::size_t>(sequence % geometry_.slot_count) * stride_;
}

core::Result<std::uint64_t> Segment::Publish(ByteView payload, core::TimePoint source_timestamp)
{
    using ResultType = core::Result<std::uint64_t>;
    if (payload.size() > geometry_.slot_payload_size) {
        return ResultType::FromStatus(core::Status::OutOfRange(
            "payload of " + std::to_string(payload.size()) + " bytes exceeds the shm slot size of "
            + std::to_string(geometry_.slot_payload_size)));
    }

    // A previous owner that died mid-write never advanced write_seq, so its
    // half-written slot is invisible to readers and is simply reused here.
    auto locked = lock_->Lock();
    if (!locked.ok()) {
        return ResultType::FromStatus(locked.status());
    }

    const std::uint64_t sequence = header_->write_seq.load(std::memory_order_relaxed) + 1;
    unsigned char* slot = SlotAt(sequence);
    auto* slot_header = reinterpret_cast<SlotHeader*>(slot);

    slot_header->seq.store(sequence | kWritingBit, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    slot_header->payload_size.store(payload.size(), std::memory_order_relaxed);
    slot_header->source_timestamp_ns.store(source_timestamp.time_since_epoch().count(), std::memory_order_relaxed);
    CopyToShared(slot + kSlotPayloadOffset, payload.data(), payload.size());
    if (crash_hook_ != nullptr) {
        crash_hook_();  // test-only: dies here with the lock held and the slot half-written
    }
    slot_header->seq.store(sequence, std::memory_order_release);
    header_->write_seq.store(sequence, std::memory_order_release);

    lock_->Unlock();
    return ResultType(sequence);
}

ReadOutcome Segment::TryRead(std::uint64_t& cursor, std::uint64_t depth, Message& message) const
{
    ReadOutcome outcome;
    const std::uint64_t ring = geometry_.slot_count;
    const std::uint64_t effective_depth = std::max<std::uint64_t>(1, std::min<std::uint64_t>(depth, ring));

    // Each pass returns or advances the cursor. Concurrent writers can keep
    // moving the newest sequence, so the passes are bounded; a caller that
    // gets no message simply tries again.
    for (std::uint64_t pass = 0; pass < ring + 16; ++pass) {
        const std::uint64_t newest = WriteSequence();
        if (cursor > newest) {
            return outcome;
        }
        const std::uint64_t clamped = ClampCursor(cursor, newest, effective_depth);
        outcome.dropped += clamped - cursor;
        cursor = clamped;

        const unsigned char* slot = SlotAt(cursor);
        const auto* slot_header = reinterpret_cast<const SlotHeader*>(slot);
        const std::uint64_t before = slot_header->seq.load(std::memory_order_acquire);
        if (before != cursor) {
            // Overwritten, being overwritten, or corrupt: this message is lost.
            ++outcome.dropped;
            ++cursor;
            continue;
        }

        const std::uint64_t size = slot_header->payload_size.load(std::memory_order_relaxed);
        const std::int64_t timestamp = slot_header->source_timestamp_ns.load(std::memory_order_relaxed);
        bool valid = size <= geometry_.slot_payload_size;
        if (valid) {
            message.payload.resize(static_cast<std::size_t>(size));
            CopyFromShared(message.payload.data(), slot + kSlotPayloadOffset, static_cast<std::size_t>(size));
        }
        std::atomic_thread_fence(std::memory_order_acquire);
        const std::uint64_t after = slot_header->seq.load(std::memory_order_relaxed);
        if (!valid || after != before) {
            ++outcome.dropped;
            ++cursor;
            continue;
        }

        message.metadata.sequence = cursor;
        message.metadata.source_timestamp = core::TimePoint(core::Nanoseconds(timestamp));
        ++cursor;
        outcome.has_message = true;
        return outcome;
    }
    return outcome;
}

std::uint64_t Segment::Pending(std::uint64_t cursor, std::uint64_t depth) const noexcept
{
    const std::uint64_t newest = WriteSequence();
    if (cursor > newest) {
        return 0;
    }
    const std::uint64_t effective_depth =
        std::max<std::uint64_t>(1, std::min<std::uint64_t>(depth, geometry_.slot_count));
    return newest - ClampCursor(cursor, newest, effective_depth) + 1;
}

core::Result<std::uint32_t> Segment::ClaimParticipant()
{
    ReclaimDeadParticipants();
    const auto self = CurrentProcess();
    for (std::uint32_t index = 0; index < kMaxParticipants; ++index) {
        auto& entry = header_->participants[index];
        std::uint32_t state = entry.state.load(std::memory_order_acquire);
        if (Kind(state) != kParticipantFree) {
            continue;
        }
        const std::uint32_t claiming = NextGeneration(state) | kParticipantClaiming;
        if (!entry.state.compare_exchange_strong(state, claiming, std::memory_order_acq_rel)) {
            continue;
        }
        entry.pid.store(self.pid, std::memory_order_relaxed);
        entry.start_time.store(self.start_time, std::memory_order_relaxed);
        entry.state.store(Generation(claiming) | kParticipantActive, std::memory_order_release);
        return core::Result<std::uint32_t>(index);
    }
    return core::Result<std::uint32_t>::FromStatus(core::Status::ResourceExhausted(
        "shm topic already has " + std::to_string(kMaxParticipants) + " participant processes"));
}

void Segment::ReleaseParticipant(std::uint32_t index) noexcept
{
    if (index >= kMaxParticipants) {
        return;
    }
    auto& entry = header_->participants[index];
    const std::uint32_t state = entry.state.load(std::memory_order_acquire);
    entry.pid.store(0, std::memory_order_relaxed);
    entry.start_time.store(0, std::memory_order_relaxed);
    entry.state.store(Generation(state) | kParticipantFree, std::memory_order_release);
}

std::vector<std::uint32_t> Segment::ActiveParticipants() const
{
    std::vector<std::uint32_t> active;
    for (std::uint32_t index = 0; index < kMaxParticipants; ++index) {
        if (Kind(header_->participants[index].state.load(std::memory_order_acquire)) == kParticipantActive) {
            active.push_back(index);
        }
    }
    return active;
}

void Segment::ReclaimDeadParticipants() noexcept
{
    for (auto& entry : header_->participants) {
        std::uint32_t state = entry.state.load(std::memory_order_acquire);
        if (Kind(state) != kParticipantActive) {
            continue;
        }
        const ProcessIdentity identity {
            entry.pid.load(std::memory_order_relaxed),
            entry.start_time.load(std::memory_order_relaxed),
        };
        if (!IsProcessAlive(identity)) {
            // The generation in `state` makes this fail if the entry was
            // released and claimed again since it was read.
            entry.state.compare_exchange_strong(
                state, Generation(state) | kParticipantFree, std::memory_order_acq_rel);
        }
    }
}
}  // namespace puppet_master::transport::shm::detail
