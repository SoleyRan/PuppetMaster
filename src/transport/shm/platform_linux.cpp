#include "platform.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstring>
#include <ctime>
#include <fstream>
#include <new>
#include <sstream>
#include <thread>

#include <dirent.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace puppet_master::transport::shm::detail {

namespace {

static_assert(sizeof(pthread_mutex_t) <= WriterLock::kStorageSize, "WriterLock storage too small");
static_assert(alignof(pthread_mutex_t) <= WriterLock::kStorageAlignment, "WriterLock storage misaligned");
static_assert(std::atomic<std::uint32_t>::is_always_lock_free, "futex word must be lock-free");
static_assert(sizeof(std::atomic<std::uint32_t>) == sizeof(std::uint32_t), "futex word must be 32 bits");

std::string ErrnoMessage(const char* what, int error)
{
    return std::string(what) + ": " + std::strerror(error);
}

struct ProcStat {
    bool found {false};
    char state {'?'};
    std::uint64_t start_time {0};
};

// Fields 3 (state) and 22 (start time in clock ticks since boot) of
// /proc/<pid>/stat.
ProcStat ReadProcStat(std::uint32_t pid)
{
    ProcStat result;
    std::ifstream stat("/proc/" + std::to_string(pid) + "/stat");
    std::string line;
    if (!std::getline(stat, line)) {
        return result;
    }
    // The command name (field 2) may contain spaces; fields resume after ')'.
    const auto close = line.rfind(')');
    if (close == std::string::npos) {
        return result;
    }
    std::istringstream fields(line.substr(close + 1));
    std::string field;
    for (int index = 3; index <= 22; ++index) {
        if (!(fields >> field)) {
            return result;
        }
        if (index == 3) {
            result.state = field.empty() ? '?' : field.front();
        }
    }
    try {
        result.start_time = std::stoull(field);
    } catch (...) {
        return result;
    }
    result.found = true;
    return result;
}

long Futex(std::atomic<std::uint32_t>* word, int op, std::uint32_t value, const timespec* timeout)
{
    // Shared (non-private) futex ops so waiters in other processes match.
    return syscall(SYS_futex, reinterpret_cast<std::uint32_t*>(word), op, value, timeout, nullptr, 0);
}

}  // namespace

core::Result<std::unique_ptr<SharedRegion>> SharedRegion::CreateOrOpen(
    const std::string& name, std::size_t size, core::Nanoseconds timeout)
{
    using ResultType = core::Result<std::unique_ptr<SharedRegion>>;
    if (size == 0) {
        return ResultType::FromStatus(core::Status::InvalidArgument("shared region size must be positive"));
    }

    bool created = true;
    int fd = shm_open(name.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd >= 0) {
        if (ftruncate(fd, static_cast<off_t>(size)) != 0) {
            const int error = errno;
            close(fd);
            shm_unlink(name.c_str());
            return ResultType::FromStatus(core::Status::ResourceExhausted(ErrnoMessage("ftruncate", error)));
        }
    } else if (errno == EEXIST) {
        created = false;
        fd = shm_open(name.c_str(), O_RDWR, 0600);
        if (fd < 0) {
            return ResultType::FromStatus(core::Status::Unavailable(ErrnoMessage("shm_open", errno)));
        }
        // The creator sizes the region right after creating it.
        const auto deadline = core::SteadyClock::now() + timeout;
        struct stat info {};
        while (true) {
            if (fstat(fd, &info) != 0) {
                const int error = errno;
                close(fd);
                return ResultType::FromStatus(core::Status::Unavailable(ErrnoMessage("fstat", error)));
            }
            if (info.st_size > 0) {
                break;
            }
            if (core::SteadyClock::now() >= deadline) {
                close(fd);
                return ResultType::FromStatus(
                    core::Status::DeadlineExceeded("shared region was never sized: " + name));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        size = static_cast<std::size_t>(info.st_size);
    } else {
        return ResultType::FromStatus(core::Status::Unavailable(ErrnoMessage("shm_open", errno)));
    }

    void* data = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    const int map_error = errno;
    close(fd);
    if (data == MAP_FAILED) {
        return ResultType::FromStatus(core::Status::Unavailable(ErrnoMessage("mmap", map_error)));
    }
    return ResultType(std::unique_ptr<SharedRegion>(new SharedRegion(data, size, created, nullptr)));
}

core::Status SharedRegion::Remove(const std::string& name)
{
    if (shm_unlink(name.c_str()) != 0 && errno != ENOENT) {
        return core::Status::Unavailable(ErrnoMessage("shm_unlink", errno));
    }
    return core::Status::Ok();
}

core::Result<std::unique_ptr<SharedRegion>> SharedRegion::OpenReadOnly(const std::string& name)
{
    using ResultType = core::Result<std::unique_ptr<SharedRegion>>;
    const int fd = shm_open(name.c_str(), O_RDONLY, 0);
    if (fd < 0) {
        const int error = errno;
        return ResultType::FromStatus(error == ENOENT ? core::Status::NotFound("no shared region " + name)
                                                      : core::Status::Unavailable(ErrnoMessage("shm_open", error)));
    }
    struct stat info {};
    if (fstat(fd, &info) != 0) {
        const int error = errno;
        close(fd);
        return ResultType::FromStatus(core::Status::Unavailable(ErrnoMessage("fstat", error)));
    }
    const auto size = static_cast<std::size_t>(info.st_size);
    if (size == 0) {
        close(fd);
        // Created but never sized: a creator crashed between the two steps.
        return ResultType(std::unique_ptr<SharedRegion>(new SharedRegion(nullptr, 0, false, nullptr)));
    }
    void* data = mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
    const int map_error = errno;
    close(fd);
    if (data == MAP_FAILED) {
        return ResultType::FromStatus(core::Status::Unavailable(ErrnoMessage("mmap", map_error)));
    }
    return ResultType(std::unique_ptr<SharedRegion>(new SharedRegion(data, size, false, nullptr)));
}

core::Result<std::vector<std::string>> SharedRegion::List(std::optional<core::DomainId> domain)
{
    // POSIX shared memory objects appear in /dev/shm on Linux, named as
    // passed to shm_open without the leading '/'. Only exact
    // "pm.<domain>.<16 hex digits>" names are ours; companion objects carry
    // a further suffix and other programs' files are left alone.
    DIR* directory = opendir("/dev/shm");
    if (directory == nullptr) {
        return core::Result<std::vector<std::string>>::FromStatus(
            core::Status::Unavailable(ErrnoMessage("opendir /dev/shm", errno)));
    }
    const std::string wanted_domain = domain ? std::to_string(*domain) : std::string();
    std::vector<std::string> names;
    while (const dirent* entry = readdir(directory)) {
        const std::string file = entry->d_name;
        if (file.rfind("pm.", 0) != 0) {
            continue;
        }
        const auto dot = file.find('.', 3);
        if (dot == std::string::npos || dot == 3) {
            continue;
        }
        const auto domain_text = file.substr(3, dot - 3);
        const auto hash = file.substr(dot + 1);
        const bool digits = domain_text.find_first_not_of("0123456789") == std::string::npos;
        const bool hex = hash.size() == 16 && hash.find_first_not_of("0123456789abcdef") == std::string::npos;
        if (!digits || !hex || (domain && domain_text != wanted_domain)) {
            continue;
        }
        names.push_back("/" + file);
    }
    closedir(directory);
    std::sort(names.begin(), names.end());
    return core::Result<std::vector<std::string>>(std::move(names));
}

SharedRegion::~SharedRegion()
{
    if (data_ != nullptr) {
        munmap(data_, size_);
    }
}

core::Status WriterLock::InitializeStorage(void* storage)
{
    pthread_mutexattr_t attributes;
    if (pthread_mutexattr_init(&attributes) != 0) {
        return core::Status::Internal("pthread_mutexattr_init failed");
    }
    int error = pthread_mutexattr_setpshared(&attributes, PTHREAD_PROCESS_SHARED);
    if (error == 0) {
        error = pthread_mutexattr_setrobust(&attributes, PTHREAD_MUTEX_ROBUST);
    }
    if (error == 0) {
        error = pthread_mutex_init(new (storage) pthread_mutex_t, &attributes);
    }
    pthread_mutexattr_destroy(&attributes);
    if (error != 0) {
        return core::Status::Internal(ErrnoMessage("robust mutex init", error));
    }
    return core::Status::Ok();
}

core::Result<std::unique_ptr<WriterLock>> WriterLock::Open(void* storage, const std::string& /*name*/)
{
    return core::Result<std::unique_ptr<WriterLock>>(std::unique_ptr<WriterLock>(new WriterLock(storage, nullptr)));
}

WriterLock::~WriterLock() = default;

core::Result<bool> WriterLock::Lock()
{
    auto* mutex = static_cast<pthread_mutex_t*>(storage_);
    const int error = pthread_mutex_lock(mutex);
    if (error == 0) {
        return core::Result<bool>(false);
    }
    if (error == EOWNERDEAD) {
        if (pthread_mutex_consistent(mutex) != 0) {
            pthread_mutex_unlock(mutex);
            return core::Result<bool>::FromStatus(core::Status::Internal("pthread_mutex_consistent failed"));
        }
        return core::Result<bool>(true);
    }
    return core::Result<bool>::FromStatus(core::Status::Unavailable(ErrnoMessage("pthread_mutex_lock", error)));
}

void WriterLock::Unlock() noexcept
{
    pthread_mutex_unlock(static_cast<pthread_mutex_t*>(storage_));
}

core::Result<std::unique_ptr<Notifier>> Notifier::Open(
    std::atomic<std::uint32_t>* word, const std::string& name, std::uint32_t self_index)
{
    if (self_index >= kMaxParticipants) {
        return core::Result<std::unique_ptr<Notifier>>::FromStatus(
            core::Status::OutOfRange("notifier participant index out of range"));
    }
    return core::Result<std::unique_ptr<Notifier>>(std::unique_ptr<Notifier>(new Notifier(word, name, nullptr)));
}

Notifier::~Notifier() = default;

std::uint32_t Notifier::Load() const noexcept
{
    return word_->load(std::memory_order_acquire);
}

void Notifier::NotifyAll(const std::vector<std::uint32_t>& /*indices*/) noexcept
{
    word_->fetch_add(1, std::memory_order_acq_rel);
    Futex(word_, FUTEX_WAKE, INT_MAX, nullptr);
}

Notifier::WaitResult Notifier::Wait(std::uint32_t observed, core::Nanoseconds timeout) noexcept
{
    if (Load() != observed) {
        return WaitResult::kNotified;
    }
    if (timeout <= core::Nanoseconds::zero()) {
        return WaitResult::kTimeout;
    }
    timespec relative {};
    relative.tv_sec = static_cast<time_t>(timeout.count() / 1000000000);
    relative.tv_nsec = static_cast<long>(timeout.count() % 1000000000);
    // EAGAIN (word already changed) and EINTR both count as a wakeup.
    if (Futex(word_, FUTEX_WAIT, observed, &relative) != 0 && errno == ETIMEDOUT) {
        return WaitResult::kTimeout;
    }
    return WaitResult::kNotified;
}

ProcessIdentity CurrentProcess()
{
    const auto pid = static_cast<std::uint32_t>(getpid());
    return ProcessIdentity {pid, ReadProcStat(pid).start_time};
}

bool IsProcessAlive(const ProcessIdentity& identity)
{
    // Identities come from shared memory. Reject values that kill() would
    // treat as a process group instead of one process.
    if (identity.pid == 0 || identity.pid > static_cast<std::uint32_t>(INT_MAX)) {
        return false;
    }
    if (kill(static_cast<pid_t>(identity.pid), 0) != 0 && errno == ESRCH) {
        return false;
    }
    const auto stat = ReadProcStat(identity.pid);
    if (!stat.found) {
        return true;
    }
    // Zombie or dead: exited but not yet reaped by its parent.
    if (stat.state == 'Z' || stat.state == 'X') {
        return false;
    }
    return identity.start_time == 0 || stat.start_time == identity.start_time;
}

}  // namespace puppet_master::transport::shm::detail
