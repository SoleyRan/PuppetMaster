#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <puppet_master/core/result.h>
#include <puppet_master/core/status.h>
#include <puppet_master/core/types.h>

// Internal OS layer for the shared-memory transport. Nothing here is public
// API; see docs/shm-transport-design.md for the protocol built on top of it.
namespace puppet_master::transport::shm::detail {

inline constexpr std::uint32_t kMaxParticipants = 64;

std::uint64_t Fnv1a64(std::string_view text) noexcept;

// Names the OS objects of one topic: "/pm.<domain>.<hash>" on Linux and
// "Local\pm.<domain>.<hash>" on Windows. A suffix names companion objects.
std::string ObjectName(core::DomainId domain, std::string_view topic, std::string_view suffix = {});

// A named, zero-filled shared mapping.
class SharedRegion {
public:
    // Creates the region with at least `size` bytes, or opens an existing one,
    // which keeps its original size. With `create` false it only opens an
    // existing one, and is NotFound if there is none. Callers validate size()
    // themselves. On Linux, waits up to `timeout` for a concurrent creator to
    // size it.
    static core::Result<std::unique_ptr<SharedRegion>> CreateOrOpen(
        const std::string& name, std::size_t size, core::Nanoseconds timeout, bool create = true);

    // Unlinks the name so the next CreateOrOpen creates a fresh region;
    // existing mappings stay valid. A missing name is not an error. No-op on
    // Windows, which frees a region when its last handle closes.
    static core::Status Remove(const std::string& name);

    // Maps an existing region read-only, for inspection. NotFound if it does
    // not exist. Unsupported on Windows.
    static core::Result<std::unique_ptr<SharedRegion>> OpenReadOnly(const std::string& name);

    // Names of the regions this layer created in `domain` (every domain if
    // none), as passed to CreateOrOpen. Unsupported on Windows, which does
    // not keep regions after their processes exit.
    static core::Result<std::vector<std::string>> List(std::optional<core::DomainId> domain);

    ~SharedRegion();
    SharedRegion(const SharedRegion&) = delete;
    SharedRegion& operator=(const SharedRegion&) = delete;

    void* data() const noexcept { return data_; }
    std::size_t size() const noexcept { return size_; }
    bool created() const noexcept { return created_; }

private:
    SharedRegion(void* data, std::size_t size, bool created, void* native) noexcept
        : data_(data), size_(size), created_(created), native_(native)
    {
    }

    void* data_;
    std::size_t size_;
    bool created_;
    void* native_;
};

// Cross-process writer mutex. On Linux it is a robust pthread mutex stored in
// `storage` inside a shared region; on Windows a named kernel mutex. It is not
// recursive, and on Windows it must be unlocked by the thread that locked it.
class WriterLock {
public:
    static constexpr std::size_t kStorageSize = 64;
    static constexpr std::size_t kStorageAlignment = 8;

    // Run once by the region creator before other processes can see it.
    static core::Status InitializeStorage(void* storage);

    static core::Result<std::unique_ptr<WriterLock>> Open(void* storage, const std::string& name);

    ~WriterLock();
    WriterLock(const WriterLock&) = delete;
    WriterLock& operator=(const WriterLock&) = delete;

    // Blocks until acquired. true means the previous owner died while holding
    // the lock, so the state it protected may be half-written.
    core::Result<bool> Lock();
    void Unlock() noexcept;

private:
    WriterLock(void* storage, void* native) noexcept : storage_(storage), native_(native) {}

    void* storage_;
    void* native_;
};

// Wakes participants in other processes. `word` is a change counter in shared
// memory; each participant waits on its own channel, identified by an index
// below kMaxParticipants.
class Notifier {
public:
    enum class WaitResult : std::uint8_t {
        kNotified = 0,
        kTimeout,
    };

    static core::Result<std::unique_ptr<Notifier>> Open(
        std::atomic<std::uint32_t>* word, const std::string& name, std::uint32_t self_index);

    ~Notifier();
    Notifier(const Notifier&) = delete;
    Notifier& operator=(const Notifier&) = delete;

    std::uint32_t Load() const noexcept;

    // Bumps the counter and wakes the listed participants. Linux wakes every
    // waiter on the word regardless of `indices`.
    void NotifyAll(const std::vector<std::uint32_t>& indices) noexcept;

    // Waits until the counter differs from `observed` or the timeout expires.
    // kNotified may be spurious; callers re-check their own state.
    WaitResult Wait(std::uint32_t observed, core::Nanoseconds timeout) noexcept;

private:
    Notifier(std::atomic<std::uint32_t>* word, std::string name, void* own_event) noexcept
        : word_(word), name_(std::move(name)), own_event_(own_event)
    {
    }

    std::atomic<std::uint32_t>* word_;
    std::string name_;
    void* own_event_;
    std::mutex peers_mutex_;
    void* peers_[kMaxParticipants] {};
};

struct ProcessIdentity {
    std::uint32_t pid {0};
    std::uint64_t start_time {0};
};

ProcessIdentity CurrentProcess();

// false only when the process has exited or its pid now names another
// process; an exited but unreaped Linux process counts as exited. A process
// that cannot be inspected is reported alive.
bool IsProcessAlive(const ProcessIdentity& identity);

}  // namespace puppet_master::transport::shm::detail
