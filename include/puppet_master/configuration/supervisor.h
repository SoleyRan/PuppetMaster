#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <puppet_master/configuration/system_manifest.h>
#include <puppet_master/core/result.h>
#include <puppet_master/core/status.h>
#include <puppet_master/export.h>

// Supervises the nodes of a system manifest: starts them in order, restarts
// them by policy, and stops them in reverse order. Process handling is behind
// ProcessLauncher, so the policy can be tested without real processes.
namespace puppet_master::configuration {

// How a process ended.
struct ProcessExit {
    // Exit code, or the signal number when `signaled`.
    int code {0};
    bool signaled {false};

    bool success() const noexcept { return !signaled && code == 0; }
};

PUPPET_MASTER_API std::string ToString(const ProcessExit& exit);

class PUPPET_MASTER_API ChildProcess {
public:
    virtual ~ChildProcess() = default;

    virtual std::int64_t id() const noexcept = 0;
    // The exit, once the process has ended; does not block.
    virtual std::optional<ProcessExit> Poll() = 0;
    // Asks the process to stop: SIGTERM on POSIX, CTRL_BREAK to its process
    // group on Windows.
    virtual void RequestStop() = 0;
    // Ends it at once: SIGKILL, TerminateProcess.
    virtual void Kill() = 0;
};

class PUPPET_MASTER_API ProcessLauncher {
public:
    virtual ~ProcessLauncher() = default;

    // Starts `command` (program, then arguments; the program is looked up on
    // PATH unless it has a directory part) in a process group of its own, so
    // stop requests reach only it. With `log_path`, its stdout and stderr are
    // appended to that file; otherwise it shares the caller's.
    virtual core::Result<std::unique_ptr<ChildProcess>> Start(
        const std::vector<std::string>& command, const std::string& log_path) = 0;
};

// Real processes on this platform.
PUPPET_MASTER_API std::unique_ptr<ProcessLauncher> MakeSystemProcessLauncher();

// What happened to a node, for logs and tests.
struct SupervisorEvent {
    enum class Kind : std::uint8_t {
        kStarted,
        kStartFailed,
        kExited,
        kRestarting,
        kGaveUp,       // its restart policy wants a restart, but none is left
        kStopping,
        kKilled,       // did not stop within stop_timeout
        kStopped,
    };
    Kind kind;
    std::string node;
    std::string detail;
};

PUPPET_MASTER_API const char* ToString(SupervisorEvent::Kind kind) noexcept;

struct SupervisorOptions {
    std::unique_ptr<ProcessLauncher> launcher;  // MakeSystemProcessLauncher() if empty
    // Called on the thread running Run(), which it blocks while it runs.
    std::function<void(const SupervisorEvent&)> on_event;
    // With a directory, each node's output goes to <directory>/<node>.log,
    // appended across restarts; the directory is created if needed.
    std::string log_directory;
    // How often child processes are checked.
    std::chrono::milliseconds poll_interval {std::chrono::milliseconds(50)};
    // Restart delays double for each restart that follows a run shorter than
    // this, up to max_restart_delay; a longer run resets the delay and the
    // restart count.
    std::chrono::milliseconds stable_run {std::chrono::milliseconds(10000)};
    std::chrono::milliseconds max_restart_delay {std::chrono::milliseconds(30000)};
};

// The outcome of a run.
struct SupervisorResult {
    // Why the system stopped: a stop request, a required node that ended
    // for good (an exit or a failed start), or every node done.
    std::string reason;
    // 1 when a required node ended with a failure, every node ended and one
    // failed, or the log directory could not be created; 0 otherwise.
    int exit_code {0};
};

class PUPPET_MASTER_API Supervisor final {
public:
    Supervisor(SystemManifest manifest, SupervisorOptions options = {});
    ~Supervisor();
    Supervisor(const Supervisor&) = delete;
    Supervisor& operator=(const Supervisor&) = delete;

    // Starts the nodes and supervises them until RequestStop(), a required
    // node ends for good, or every node has ended; then stops the ones still
    // running in reverse start order. Call once.
    SupervisorResult Run();

    // Thread-safe and async-signal-unsafe: call it from a signal watcher
    // thread, not a signal handler.
    void RequestStop() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace puppet_master::configuration
