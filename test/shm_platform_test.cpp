#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "platform.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace core = puppet_master::core;
namespace detail = puppet_master::transport::shm::detail;

namespace {

using namespace std::chrono_literals;

// Shared test layout: lock storage, notify word, and a counter the lock guards.
struct TestBlock {
    alignas(detail::WriterLock::kStorageAlignment) unsigned char lock[detail::WriterLock::kStorageSize];
    std::atomic<std::uint32_t> word;
    std::uint64_t guarded;
    std::atomic<std::uint32_t> child_ready;
};

std::string UniqueName(const char* tag)
{
    const auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    return detail::ObjectName(0, std::string("/pm/test/") + tag + "/" + suffix);
}

struct Opened {
    std::unique_ptr<detail::SharedRegion> region;
    TestBlock* block {nullptr};
};

Opened OpenBlock(const std::string& name)
{
    auto region = detail::SharedRegion::CreateOrOpen(name, sizeof(TestBlock), 1s);
    assert(region.ok());
    Opened opened {std::move(region).value(), nullptr};
    assert(opened.region->size() >= sizeof(TestBlock));
    opened.block = static_cast<TestBlock*>(opened.region->data());
    if (opened.region->created()) {
        assert(detail::WriterLock::InitializeStorage(opened.block->lock).ok());
    }
    return opened;
}

#ifdef _WIN32
using ChildHandle = HANDLE;

ChildHandle StartChild(const std::string& executable, const std::string& mode, const std::string& name)
{
    std::string command = "\"" + executable + "\" --child " + mode + " " + name;
    STARTUPINFOA startup {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process {};
    const BOOL started = CreateProcessA(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                                        &startup, &process);
    assert(started);
    CloseHandle(process.hThread);
    return process.hProcess;
}

int WaitChild(ChildHandle child)
{
    const DWORD result = WaitForSingleObject(child, 20000);
    if (result != WAIT_OBJECT_0) {
        TerminateProcess(child, 1);
        WaitForSingleObject(child, INFINITE);
    }
    DWORD code = 1;
    GetExitCodeProcess(child, &code);
    CloseHandle(child);
    return result == WAIT_OBJECT_0 ? static_cast<int>(code) : -1;
}

[[noreturn]] void ExitWithoutCleanup()
{
    // Skips destructors, so held kernel mutexes are abandoned.
    TerminateProcess(GetCurrentProcess(), 0);
    std::abort();
}
#else
using ChildHandle = pid_t;

ChildHandle StartChild(const std::string& executable, const std::string& mode, const std::string& name)
{
    const pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        execl(executable.c_str(), executable.c_str(), "--child", mode.c_str(), name.c_str(), nullptr);
        std::_Exit(127);
    }
    return pid;
}

int WaitChild(ChildHandle child)
{
    int status = 0;
    const auto deadline = std::chrono::steady_clock::now() + 20s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (waitpid(child, &status, WNOHANG) == child) {
            return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        }
        std::this_thread::sleep_for(10ms);
    }
    kill(child, SIGKILL);
    waitpid(child, &status, 0);
    return -1;
}

[[noreturn]] void ExitWithoutCleanup()
{
    std::_Exit(0);
}
#endif

// Child: take the lock, then exit while still holding it.
int RunAbandonLock(const std::string& name)
{
    auto opened = OpenBlock(name);
    auto lock = detail::WriterLock::Open(opened.block->lock, name);
    assert(lock.ok());
    assert(lock.value()->Lock().ok());
    opened.block->guarded = 7;
    ExitWithoutCleanup();
}

// Child: wait for one notification, then report through the guarded field.
int RunWaitNotify(const std::string& name)
{
    auto opened = OpenBlock(name);
    auto notifier = detail::Notifier::Open(&opened.block->word, name, 1);
    assert(notifier.ok());
    const auto observed = notifier.value()->Load();
    opened.block->child_ready.store(1, std::memory_order_release);
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (notifier.value()->Load() == observed) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return 2;
        }
        notifier.value()->Wait(observed, 200ms);
    }
    return 0;
}

void NamesAreStableAndDistinct()
{
    assert(detail::Fnv1a64("") == 0xcbf29ce484222325ULL);
    assert(detail::Fnv1a64("a") == 0xaf63dc4c8601ec8cULL);
    const auto lidar = detail::ObjectName(3, "/lidar/points");
    assert(lidar == detail::ObjectName(3, "/lidar/points"));
    assert(lidar != detail::ObjectName(4, "/lidar/points"));
    assert(lidar != detail::ObjectName(3, "/lidar/points2"));
    assert(detail::ObjectName(3, "/lidar/points", "lock") == lidar + ".lock");
#ifdef _WIN32
    assert(lidar.rfind("Local\\pm.3.", 0) == 0);
#else
    assert(lidar.rfind("/pm.3.", 0) == 0);
    // POSIX shm names allow exactly one leading slash.
    assert(lidar.find('/', 1) == std::string::npos);
#endif
}

void RegionIsSharedAndZeroFilled()
{
    const auto name = UniqueName("region");
    auto first = detail::SharedRegion::CreateOrOpen(name, 4096, 1s);
    assert(first.ok());
    assert(first.value()->created());
    assert(first.value()->size() >= 4096);
    auto* bytes = static_cast<unsigned char*>(first.value()->data());
    for (std::size_t index = 0; index < 4096; ++index) {
        assert(bytes[index] == 0);
    }

    // A second open sees the same memory and keeps the original size.
    auto second = detail::SharedRegion::CreateOrOpen(name, 64, 1s);
    assert(second.ok());
    assert(!second.value()->created());
    assert(second.value()->size() >= 4096);
    bytes[100] = 0x5a;
    assert(static_cast<unsigned char*>(second.value()->data())[100] == 0x5a);

    assert(detail::SharedRegion::Remove(name).ok());
    assert(detail::SharedRegion::Remove(name).ok());
    assert(!detail::SharedRegion::CreateOrOpen(name, 0, 1s).ok());
}

void LockExcludesThreads()
{
    const auto name = UniqueName("lock");
    auto opened = OpenBlock(name);
    auto lock = detail::WriterLock::Open(opened.block->lock, name);
    assert(lock.ok());

    // Each thread opens its own handle, as separate processes would.
    std::vector<std::thread> threads;
    for (int thread = 0; thread < 4; ++thread) {
        threads.emplace_back([&] {
            auto own = detail::WriterLock::Open(opened.block->lock, name);
            assert(own.ok());
            for (int iteration = 0; iteration < 2000; ++iteration) {
                auto acquired = own.value()->Lock();
                assert(acquired.ok() && !acquired.value());
                const auto value = opened.block->guarded;
                opened.block->guarded = value + 1;
                own.value()->Unlock();
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    assert(opened.block->guarded == 8000);
    assert(detail::SharedRegion::Remove(name).ok());
}

void LockRecoversFromDeadOwner(const std::string& executable)
{
    const auto name = UniqueName("abandon");
    auto opened = OpenBlock(name);
    auto lock = detail::WriterLock::Open(opened.block->lock, name);
    assert(lock.ok());

    assert(WaitChild(StartChild(executable, "abandon-lock", name)) == 0);
    assert(opened.block->guarded == 7);

    auto first = lock.value()->Lock();
    assert(first.ok());
    assert(first.value());  // previous owner died holding it
    lock.value()->Unlock();

    auto second = lock.value()->Lock();
    assert(second.ok());
    assert(!second.value());  // recovered state is clean again
    lock.value()->Unlock();
    assert(detail::SharedRegion::Remove(name).ok());
}

void NotifierTimesOutAndWakes()
{
    const auto name = UniqueName("notify");
    auto opened = OpenBlock(name);
    auto waiter = detail::Notifier::Open(&opened.block->word, name, 0);
    auto sender = detail::Notifier::Open(&opened.block->word, name, 1);
    assert(waiter.ok());
    assert(sender.ok());
    assert(!detail::Notifier::Open(&opened.block->word, name, detail::kMaxParticipants).ok());

    const auto observed = waiter.value()->Load();
    const auto started = std::chrono::steady_clock::now();
    assert(waiter.value()->Wait(observed, 30ms) == detail::Notifier::WaitResult::kTimeout);
    assert(std::chrono::steady_clock::now() - started >= 25ms);
    assert(waiter.value()->Wait(observed, core::Nanoseconds::zero()) == detail::Notifier::WaitResult::kTimeout);

    std::thread notify([&] {
        std::this_thread::sleep_for(20ms);
        sender.value()->NotifyAll({0});
    });
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (waiter.value()->Load() == observed && std::chrono::steady_clock::now() < deadline) {
        waiter.value()->Wait(observed, 1s);
    }
    notify.join();
    assert(waiter.value()->Load() == observed + 1);
    // A changed counter returns immediately.
    assert(waiter.value()->Wait(observed, 1s) == detail::Notifier::WaitResult::kNotified);
    assert(detail::SharedRegion::Remove(name).ok());
}

void NotifierWakesOtherProcess(const std::string& executable)
{
    const auto name = UniqueName("notify-process");
    auto opened = OpenBlock(name);
    auto sender = detail::Notifier::Open(&opened.block->word, name, 0);
    assert(sender.ok());

    auto child = StartChild(executable, "wait-notify", name);
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (opened.block->child_ready.load(std::memory_order_acquire) == 0) {
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(1ms);
    }
    sender.value()->NotifyAll({1});
    assert(WaitChild(child) == 0);
    assert(detail::SharedRegion::Remove(name).ok());
}

void ProcessLiveness(const std::string& executable)
{
    const auto self = detail::CurrentProcess();
    assert(self.pid != 0);
    assert(self.start_time != 0);
    assert(detail::IsProcessAlive(self));
    assert(!detail::IsProcessAlive(detail::ProcessIdentity {}));
    // Same pid, different start time: the pid was reused.
    assert(!detail::IsProcessAlive(detail::ProcessIdentity {self.pid, self.start_time + 1}));

    // A child that exited is reported dead once reaped.
    const auto name = UniqueName("liveness");
    auto opened = OpenBlock(name);
    auto child = StartChild(executable, "abandon-lock", name);
#ifdef _WIN32
    const detail::ProcessIdentity identity {GetProcessId(child), 0};
#else
    const detail::ProcessIdentity identity {static_cast<std::uint32_t>(child), 0};
#endif
    assert(WaitChild(child) == 0);
    assert(!detail::IsProcessAlive(identity));
    assert(detail::SharedRegion::Remove(name).ok());
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc == 4 && std::string(argv[1]) == "--child") {
        const std::string mode = argv[2];
        if (mode == "abandon-lock") {
            return RunAbandonLock(argv[3]);
        }
        if (mode == "wait-notify") {
            return RunWaitNotify(argv[3]);
        }
        return 3;
    }
    assert(argc == 1);
    const std::string executable = argv[0];

    NamesAreStableAndDistinct();
    RegionIsSharedAndZeroFilled();
    LockExcludesThreads();
    LockRecoversFromDeadOwner(executable);
    NotifierTimesOutAndWakes();
    NotifierWakesOtherProcess(executable);
    ProcessLiveness(executable);
    std::cout << "shm platform tests passed\n";
    return 0;
}
