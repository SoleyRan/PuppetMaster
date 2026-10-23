#include "platform.h"

#include <utility>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace puppet_master::transport::shm::detail {

namespace {

// Object names are ASCII by construction (see ObjectName).
std::wstring Widen(const std::string& text)
{
    return std::wstring(text.begin(), text.end());
}

std::string LastErrorMessage(const char* what)
{
    return std::string(what) + " failed with error " + std::to_string(GetLastError());
}

DWORD ToMilliseconds(core::Nanoseconds timeout)
{
    // Round up so a short positive timeout still waits.
    const auto count = (timeout.count() + 999999) / 1000000;
    return count >= static_cast<long long>(INFINITE) ? INFINITE - 1 : static_cast<DWORD>(count);
}

std::uint64_t CreationTime(HANDLE process)
{
    FILETIME creation {};
    FILETIME exit {};
    FILETIME kernel {};
    FILETIME user {};
    if (!GetProcessTimes(process, &creation, &exit, &kernel, &user)) {
        return 0;
    }
    return (static_cast<std::uint64_t>(creation.dwHighDateTime) << 32) | creation.dwLowDateTime;
}

std::string EventName(const std::string& base, std::uint32_t index)
{
    return base + ".evt." + std::to_string(index);
}

}  // namespace

core::Result<std::unique_ptr<SharedRegion>> SharedRegion::CreateOrOpen(
    const std::string& name, std::size_t size, core::Nanoseconds /*timeout*/)
{
    using ResultType = core::Result<std::unique_ptr<SharedRegion>>;
    if (size == 0) {
        return ResultType::FromStatus(core::Status::InvalidArgument("shared region size must be positive"));
    }

    const auto size64 = static_cast<std::uint64_t>(size);
    // Page-file backed mappings are created and sized atomically and start
    // zero-filled, so there is no creation race to wait out.
    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                        static_cast<DWORD>(size64 >> 32), static_cast<DWORD>(size64),
                                        Widen(name).c_str());
    if (mapping == nullptr) {
        return ResultType::FromStatus(core::Status::Unavailable(LastErrorMessage("CreateFileMappingW")));
    }
    const bool created = GetLastError() != ERROR_ALREADY_EXISTS;

    void* data = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (data == nullptr) {
        auto status = core::Status::Unavailable(LastErrorMessage("MapViewOfFile"));
        CloseHandle(mapping);
        return ResultType::FromStatus(std::move(status));
    }

    if (!created) {
        // An existing mapping keeps its size; report the mapped view, which is
        // rounded up to whole pages.
        MEMORY_BASIC_INFORMATION info {};
        if (VirtualQuery(data, &info, sizeof(info)) == 0) {
            auto status = core::Status::Unavailable(LastErrorMessage("VirtualQuery"));
            UnmapViewOfFile(data);
            CloseHandle(mapping);
            return ResultType::FromStatus(std::move(status));
        }
        size = info.RegionSize;
    }
    return ResultType(std::unique_ptr<SharedRegion>(new SharedRegion(data, size, created, mapping)));
}

core::Status SharedRegion::Remove(const std::string& /*name*/)
{
    return core::Status::Ok();
}

core::Result<std::unique_ptr<SharedRegion>> SharedRegion::OpenReadOnly(const std::string& /*name*/)
{
    return core::Result<std::unique_ptr<SharedRegion>>::FromStatus(
        core::Status::Unsupported("shm segment inspection is not supported on Windows"));
}

core::Result<std::vector<std::string>> SharedRegion::List(std::optional<core::DomainId> /*domain*/)
{
    return core::Result<std::vector<std::string>>::FromStatus(core::Status::Unsupported(
        "shm segments are not listed on Windows; they are freed when their last process exits"));
}

SharedRegion::~SharedRegion()
{
    UnmapViewOfFile(data_);
    CloseHandle(static_cast<HANDLE>(native_));
}

core::Status WriterLock::InitializeStorage(void* /*storage*/)
{
    return core::Status::Ok();
}

core::Result<std::unique_ptr<WriterLock>> WriterLock::Open(void* storage, const std::string& name)
{
    HANDLE mutex = CreateMutexW(nullptr, FALSE, Widen(name + ".lock").c_str());
    if (mutex == nullptr) {
        return core::Result<std::unique_ptr<WriterLock>>::FromStatus(
            core::Status::Unavailable(LastErrorMessage("CreateMutexW")));
    }
    return core::Result<std::unique_ptr<WriterLock>>(std::unique_ptr<WriterLock>(new WriterLock(storage, mutex)));
}

WriterLock::~WriterLock()
{
    CloseHandle(static_cast<HANDLE>(native_));
}

core::Result<bool> WriterLock::Lock()
{
    switch (WaitForSingleObject(static_cast<HANDLE>(native_), INFINITE)) {
        case WAIT_OBJECT_0:
            return core::Result<bool>(false);
        case WAIT_ABANDONED:
            return core::Result<bool>(true);
        default:
            return core::Result<bool>::FromStatus(core::Status::Unavailable(LastErrorMessage("WaitForSingleObject")));
    }
}

void WriterLock::Unlock() noexcept
{
    ReleaseMutex(static_cast<HANDLE>(native_));
}

core::Result<std::unique_ptr<Notifier>> Notifier::Open(
    std::atomic<std::uint32_t>* word, const std::string& name, std::uint32_t self_index)
{
    using ResultType = core::Result<std::unique_ptr<Notifier>>;
    if (self_index >= kMaxParticipants) {
        return ResultType::FromStatus(core::Status::OutOfRange("notifier participant index out of range"));
    }
    // Auto-reset: a signal sent before the wait starts is kept, so a notify
    // between Load() and Wait() is not lost.
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, Widen(EventName(name, self_index)).c_str());
    if (event == nullptr) {
        return ResultType::FromStatus(core::Status::Unavailable(LastErrorMessage("CreateEventW")));
    }
    return ResultType(std::unique_ptr<Notifier>(new Notifier(word, name, event)));
}

Notifier::~Notifier()
{
    for (void* peer : peers_) {
        if (peer != nullptr) {
            CloseHandle(static_cast<HANDLE>(peer));
        }
    }
    CloseHandle(static_cast<HANDLE>(own_event_));
}

std::uint32_t Notifier::Load() const noexcept
{
    return word_->load(std::memory_order_acquire);
}

void Notifier::NotifyAll(const std::vector<std::uint32_t>& indices) noexcept
{
    word_->fetch_add(1, std::memory_order_acq_rel);
    std::lock_guard<std::mutex> lock(peers_mutex_);
    for (const auto index : indices) {
        if (index >= kMaxParticipants) {
            continue;
        }
        if (peers_[index] == nullptr) {
            // A participant that has not created its event yet is skipped; it
            // checks the counter before its first wait.
            peers_[index] = OpenEventW(EVENT_MODIFY_STATE, FALSE, Widen(EventName(name_, index)).c_str());
        }
        if (peers_[index] != nullptr) {
            SetEvent(static_cast<HANDLE>(peers_[index]));
        }
    }
}

Notifier::WaitResult Notifier::Wait(std::uint32_t observed, core::Nanoseconds timeout) noexcept
{
    if (Load() != observed) {
        return WaitResult::kNotified;
    }
    if (timeout <= core::Nanoseconds::zero()) {
        return WaitResult::kTimeout;
    }
    // Kernel waits count scheduler ticks and can return up to one tick before
    // the steady-clock deadline, so wait again for any remainder.
    const auto deadline = core::SteadyClock::now() + timeout;
    while (true) {
        const auto remaining = deadline - core::SteadyClock::now();
        if (remaining <= core::SteadyClock::duration::zero()) {
            return WaitResult::kTimeout;
        }
        const DWORD result = WaitForSingleObject(
            static_cast<HANDLE>(own_event_),
            ToMilliseconds(std::chrono::duration_cast<core::Nanoseconds>(remaining)));
        if (result == WAIT_OBJECT_0) {
            return WaitResult::kNotified;
        }
        if (result != WAIT_TIMEOUT) {
            return WaitResult::kTimeout;
        }
    }
}

ProcessIdentity CurrentProcess()
{
    return ProcessIdentity {GetCurrentProcessId(), CreationTime(GetCurrentProcess())};
}

bool IsProcessAlive(const ProcessIdentity& identity)
{
    if (identity.pid == 0) {
        return false;
    }
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, identity.pid);
    if (process == nullptr) {
        // ERROR_INVALID_PARAMETER means no such pid; anything else (such as
        // access denied) means a process exists that we cannot inspect.
        return GetLastError() != ERROR_INVALID_PARAMETER;
    }
    bool alive = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    if (alive && identity.start_time != 0) {
        const auto start_time = CreationTime(process);
        alive = start_time == 0 || start_time == identity.start_time;
    }
    CloseHandle(process);
    return alive;
}

}  // namespace puppet_master::transport::shm::detail
