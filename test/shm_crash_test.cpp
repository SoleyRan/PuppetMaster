// Crash recovery for the shared-memory ring: a writer process dies while it
// holds the writer lock with a half-written slot, and the topic must keep
// working for the surviving processes.

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include "segment.h"

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
namespace transport = puppet_master::transport;

namespace {

using namespace std::chrono_literals;

constexpr int kCrashExitCode = 42;
constexpr std::size_t kPayload = 64;
constexpr std::size_t kSlots = 4;

detail::Segment::Identity Identity(const std::string& topic)
{
    return detail::Segment::Identity {topic, transport::MessageDescriptor {"test.Crash", "application/octet-stream"}};
}

std::shared_ptr<detail::Segment> OpenSegment(const std::string& topic)
{
    auto segment = detail::Segment::Open(0, Identity(topic), detail::SegmentGeometry {kPayload, kSlots}, 1s);
    assert(segment.ok());
    return segment.value();
}

void Publish(detail::Segment& segment, const std::string& text)
{
    auto sequence = segment.Publish(transport::ByteView::From(text.data(), text.size()), {});
    assert(sequence.ok());
}

[[noreturn]] void DieNow()
{
#ifdef _WIN32
    // Skips all cleanup, so the named writer mutex is abandoned.
    TerminateProcess(GetCurrentProcess(), kCrashExitCode);
#endif
    std::_Exit(kCrashExitCode);
}

// Child: join the topic, publish one good message, then die inside the next
// publish with the lock held and the slot half-written.
int RunCrashingWriter(const std::string& topic)
{
    auto segment = OpenSegment(topic);
    auto participant = segment->ClaimParticipant();
    if (!participant.ok()) {
        return 2;
    }
    Publish(*segment, "before crash");
    segment->SetCrashHookForTesting(&DieNow);
    Publish(*segment, "never committed");
    return 3;  // unreachable: the hook exits
}

#ifdef _WIN32
int RunChild(const std::string& executable, const std::string& topic)
{
    std::string command = "\"" + executable + "\" --crash-writer " + topic;
    STARTUPINFOA startup {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process {};
    const BOOL started =
        CreateProcessA(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process);
    assert(started);
    CloseHandle(process.hThread);
    const DWORD result = WaitForSingleObject(process.hProcess, 20000);
    DWORD code = 1;
    if (result == WAIT_OBJECT_0) {
        GetExitCodeProcess(process.hProcess, &code);
    } else {
        TerminateProcess(process.hProcess, 1);
    }
    CloseHandle(process.hProcess);
    return result == WAIT_OBJECT_0 ? static_cast<int>(code) : -1;
}
#else
int RunChild(const std::string& executable, const std::string& topic)
{
    const pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        execl(executable.c_str(), executable.c_str(), "--crash-writer", topic.c_str(), nullptr);
        std::_Exit(127);
    }
    int status = 0;
    const auto deadline = std::chrono::steady_clock::now() + 20s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (waitpid(pid, &status, WNOHANG) == pid) {
            return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        }
        std::this_thread::sleep_for(10ms);
    }
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
    return -1;
}
#endif

void WriterCrashInsideLock(const std::string& executable)
{
    const std::string topic =
        "/pm/crash-test/" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    auto segment = OpenSegment(topic);
    auto self = segment->ClaimParticipant();
    assert(self.ok());

    // A reader waiting on exactly the slot the child will die in.
    std::uint64_t cursor = 1;
    transport::Message message;

    assert(RunChild(executable, topic) == kCrashExitCode);

    // The committed message is intact; the interrupted one never appears.
    assert(segment->WriteSequence() == 1);
    auto outcome = segment->TryRead(cursor, kSlots, message);
    assert(outcome.has_message);
    assert(std::string(message.payload.begin(), message.payload.end()) == "before crash");
    assert(!segment->TryRead(cursor, kSlots, message).has_message);

    // The dead child's participant entry is reclaimed when someone joins.
    const auto active_before = segment->ActiveParticipants().size();
    assert(active_before == 2);
    auto rejoin = segment->ClaimParticipant();
    assert(rejoin.ok());
    assert(segment->ActiveParticipants().size() == 2);
    segment->ReleaseParticipant(rejoin.value());

    // The abandoned lock is taken over, and the half-written slot is reused:
    // a full lap of new messages all read back correctly.
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    for (int index = 0; index < static_cast<int>(kSlots) * 2; ++index) {
        Publish(*segment, "after " + std::to_string(index));
        assert(std::chrono::steady_clock::now() < deadline);
    }
    assert(segment->WriteSequence() == 1 + kSlots * 2);
    for (int index = static_cast<int>(kSlots); index < static_cast<int>(kSlots) * 2; ++index) {
        outcome = segment->TryRead(cursor, kSlots, message);
        assert(outcome.has_message);
        assert(std::string(message.payload.begin(), message.payload.end()) == "after " + std::to_string(index));
    }

    segment->ReleaseParticipant(self.value());
    segment.reset();
    assert(detail::SharedRegion::Remove(detail::ObjectName(0, topic)).ok());
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc == 3 && std::string(argv[1]) == "--crash-writer") {
        return RunCrashingWriter(argv[2]);
    }
    assert(argc == 1);
    WriterCrashInsideLock(argv[0]);
    std::cout << "shm crash tests passed\n";
    return 0;
}
