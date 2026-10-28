#include <puppet_master/configuration/supervisor.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <system_error>
#include <thread>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
#endif

namespace puppet_master::configuration {

std::string ToString(const ProcessExit& exit)
{
    return (exit.signaled ? "signal " : "exit code ") + std::to_string(exit.code);
}

const char* ToString(SupervisorEvent::Kind kind) noexcept
{
    switch (kind) {
    case SupervisorEvent::Kind::kStarted:
        return "started";
    case SupervisorEvent::Kind::kStartFailed:
        return "start-failed";
    case SupervisorEvent::Kind::kExited:
        return "exited";
    case SupervisorEvent::Kind::kRestarting:
        return "restarting";
    case SupervisorEvent::Kind::kGaveUp:
        return "gave-up";
    case SupervisorEvent::Kind::kStopping:
        return "stopping";
    case SupervisorEvent::Kind::kKilled:
        return "killed";
    case SupervisorEvent::Kind::kStopped:
        return "stopped";
    }
    return "unknown";
}

namespace {

std::unique_ptr<ChildProcess> AsChild(ChildProcess* child)
{
    return std::unique_ptr<ChildProcess>(child);
}

#if !defined(_WIN32)

class PosixChild final : public ChildProcess {
public:
    explicit PosixChild(pid_t pid) : pid_(pid) {}

    ~PosixChild() override
    {
        // Never leave a zombie or an orphan behind.
        if (!exit_) {
            Kill();
            int status = 0;
            while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
            }
        }
    }

    std::int64_t id() const noexcept override { return pid_; }

    std::optional<ProcessExit> Poll() override
    {
        if (exit_) {
            return exit_;
        }
        int status = 0;
        pid_t result = 0;
        do {
            result = ::waitpid(pid_, &status, WNOHANG);
        } while (result < 0 && errno == EINTR);
        if (result == pid_) {
            ProcessExit exit;
            if (WIFSIGNALED(status)) {
                exit.signaled = true;
                exit.code = WTERMSIG(status);
            } else {
                exit.code = WEXITSTATUS(status);
            }
            exit_ = exit;
        } else if (result < 0) {
            // Reaped by someone else; the exit status is lost.
            exit_ = ProcessExit {-1, false};
        }
        return exit_;
    }

    void RequestStop() override { Signal(SIGTERM); }
    void Kill() override { Signal(SIGKILL); }

private:
    // The child leads its own process group; signalling the group reaches
    // what it started too.
    void Signal(int signal_number)
    {
        if (exit_) {
            return;
        }
        if (::kill(-pid_, signal_number) != 0) {
            ::kill(pid_, signal_number);
        }
    }

    pid_t pid_;
    std::optional<ProcessExit> exit_;
};

class PosixLauncher final : public ProcessLauncher {
public:
    core::Result<std::unique_ptr<ChildProcess>> Start(
        const std::vector<std::string>& command, const std::string& log_path) override
    {
        if (command.empty()) {
            return core::Result<std::unique_ptr<ChildProcess>>(core::Status::InvalidArgument("empty command"));
        }
        std::vector<char*> argv;
        argv.reserve(command.size() + 1);
        for (const auto& word : command) {
            argv.push_back(const_cast<char*>(word.c_str()));
        }
        argv.push_back(nullptr);

        posix_spawn_file_actions_t actions;
        posix_spawnattr_t attributes;
        posix_spawn_file_actions_init(&actions);
        posix_spawnattr_init(&attributes);

        int error = 0;
        if (!log_path.empty()) {
            error = posix_spawn_file_actions_addopen(
                &actions, STDOUT_FILENO, log_path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (error == 0) {
                error = posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
            }
        }
        // Our own stop handlers must not leak into the child as blocked or
        // ignored signals.
        sigset_t defaults;
        sigemptyset(&defaults);
        sigaddset(&defaults, SIGINT);
        sigaddset(&defaults, SIGTERM);
        sigaddset(&defaults, SIGPIPE);
        sigset_t mask;
        sigemptyset(&mask);
        if (error == 0) {
            error = posix_spawnattr_setflags(
                &attributes, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK);
        }
        if (error == 0) {
            error = posix_spawnattr_setpgroup(&attributes, 0);
        }
        if (error == 0) {
            error = posix_spawnattr_setsigdefault(&attributes, &defaults);
        }
        if (error == 0) {
            error = posix_spawnattr_setsigmask(&attributes, &mask);
        }
        pid_t pid = 0;
        if (error == 0) {
            error = ::posix_spawnp(&pid, argv[0], &actions, &attributes, argv.data(), environ);
        }
        posix_spawnattr_destroy(&attributes);
        posix_spawn_file_actions_destroy(&actions);

        if (error != 0) {
            return core::Result<std::unique_ptr<ChildProcess>>(core::Status::Unavailable(
                "starting " + command.front() + ": " + std::generic_category().message(error)));
        }
        return AsChild(new PosixChild(pid));
    }
};

std::unique_ptr<ProcessLauncher> MakePlatformLauncher()
{
    return std::make_unique<PosixLauncher>();
}

#else

// Quotes one argument the way the C runtime's command-line parser splits
// it back: backslashes are literal unless they precede a quote.
void AppendQuoted(std::string& line, const std::string& word)
{
    if (!word.empty() && word.find_first_of(" \t\n\v\"") == std::string::npos) {
        line += word;
        return;
    }
    line += '"';
    std::size_t backslashes = 0;
    for (const char c : word) {
        if (c == '\\') {
            ++backslashes;
            continue;
        }
        if (c == '"') {
            line.append(backslashes * 2 + 1, '\\');
        } else {
            line.append(backslashes, '\\');
        }
        backslashes = 0;
        line += c;
    }
    line.append(backslashes * 2, '\\');
    line += '"';
}

std::string LastError(const std::string& what)
{
    return what + ": " + std::system_category().message(static_cast<int>(::GetLastError()));
}

class WindowsChild final : public ChildProcess {
public:
    WindowsChild(HANDLE process, DWORD id) : process_(process), id_(id) {}

    ~WindowsChild() override
    {
        if (!exit_) {
            Kill();
            ::WaitForSingleObject(process_, INFINITE);
        }
        ::CloseHandle(process_);
    }

    std::int64_t id() const noexcept override { return id_; }

    std::optional<ProcessExit> Poll() override
    {
        if (exit_) {
            return exit_;
        }
        if (::WaitForSingleObject(process_, 0) != WAIT_OBJECT_0) {
            return std::nullopt;
        }
        DWORD code = 0;
        ::GetExitCodeProcess(process_, &code);
        exit_ = ProcessExit {static_cast<int>(code), false};
        return exit_;
    }

    // The child leads a process group of its own, so the break reaches only
    // it (and what it started on the same console).
    void RequestStop() override
    {
        if (!exit_) {
            ::GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, id_);
        }
    }

    void Kill() override
    {
        if (!exit_) {
            ::TerminateProcess(process_, 1);
        }
    }

private:
    HANDLE process_;
    DWORD id_;
    std::optional<ProcessExit> exit_;
};

class WindowsLauncher final : public ProcessLauncher {
public:
    core::Result<std::unique_ptr<ChildProcess>> Start(
        const std::vector<std::string>& command, const std::string& log_path) override
    {
        if (command.empty()) {
            return core::Result<std::unique_ptr<ChildProcess>>(core::Status::InvalidArgument("empty command"));
        }
        std::string line;
        for (const auto& word : command) {
            if (!line.empty()) {
                line += ' ';
            }
            AppendQuoted(line, word);
        }

        STARTUPINFOA startup {};
        startup.cb = sizeof(startup);
        HANDLE log = INVALID_HANDLE_VALUE;
        if (!log_path.empty()) {
            SECURITY_ATTRIBUTES inherit {};
            inherit.nLength = sizeof(inherit);
            inherit.bInheritHandle = TRUE;
            log = ::CreateFileA(log_path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit,
                OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (log == INVALID_HANDLE_VALUE) {
                return core::Result<std::unique_ptr<ChildProcess>>(
                    core::Status::Unavailable(LastError("opening " + log_path)));
            }
            startup.dwFlags = STARTF_USESTDHANDLES;
            startup.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
            startup.hStdOutput = log;
            startup.hStdError = log;
        }

        PROCESS_INFORMATION info {};
        // A null application name makes CreateProcess search PATH for the
        // first word, adding .exe.
        const BOOL created = ::CreateProcessA(nullptr, line.data(), nullptr, nullptr, TRUE,
            CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &startup, &info);
        const std::string error = created ? std::string() : LastError("starting " + command.front());
        if (log != INVALID_HANDLE_VALUE) {
            ::CloseHandle(log);
        }
        if (!created) {
            return core::Result<std::unique_ptr<ChildProcess>>(core::Status::Unavailable(error));
        }
        ::CloseHandle(info.hThread);
        return AsChild(new WindowsChild(info.hProcess, info.dwProcessId));
    }
};

std::unique_ptr<ProcessLauncher> MakePlatformLauncher()
{
    return std::make_unique<WindowsLauncher>();
}

#endif

using Clock = std::chrono::steady_clock;
using Kind = SupervisorEvent::Kind;

std::string Milliseconds(std::chrono::milliseconds value)
{
    return std::to_string(value.count()) + " ms";
}

}  // namespace

std::unique_ptr<ProcessLauncher> MakeSystemProcessLauncher()
{
    return MakePlatformLauncher();
}

struct Supervisor::Impl {
    struct Node {
        enum class Phase : std::uint8_t { kPending, kRunning, kBackoff, kDone };

        const NodeManifest* manifest {nullptr};
        Phase phase {Phase::kPending};
        std::unique_ptr<ChildProcess> child;
        Clock::time_point started_at {};
        Clock::time_point restart_at {};
        std::chrono::milliseconds delay {0};  // before the next restart
        std::uint32_t restarts {0};           // since the last stable run
        bool failed {false};                  // the final exit was a failure
    };

    SystemManifest manifest;
    SupervisorOptions options;
    std::vector<Node> nodes;

    std::mutex mutex;
    std::condition_variable wake;
    bool stop_requested {false};
    bool ran {false};

    // Set once the system has to stop for a reason other than a request.
    std::optional<SupervisorResult> ending;

    Impl(SystemManifest system, SupervisorOptions supervisor_options)
        : manifest(std::move(system)), options(std::move(supervisor_options))
    {
        if (!options.launcher) {
            options.launcher = MakeSystemProcessLauncher();
        }
        nodes.resize(manifest.nodes.size());
        for (std::size_t index = 0; index < nodes.size(); ++index) {
            nodes[index].manifest = &manifest.nodes[index];
            nodes[index].delay = manifest.nodes[index].restart_delay;
        }
    }

    void Emit(Kind kind, const Node& node, std::string detail)
    {
        if (options.on_event) {
            options.on_event(SupervisorEvent {kind, node.manifest->name, std::move(detail)});
        }
    }

    bool StopRequested()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return stop_requested;
    }

    // Sleeps until `deadline` or a stop request.
    void SleepUntil(Clock::time_point deadline)
    {
        std::unique_lock<std::mutex> lock(mutex);
        wake.wait_until(lock, deadline, [this] { return stop_requested; });
    }

    std::string LogPath(const Node& node) const
    {
        if (options.log_directory.empty()) {
            return {};
        }
        return (std::filesystem::path(options.log_directory) / (node.manifest->name + ".log")).string();
    }

    void Start(Node& node, Clock::time_point now)
    {
        auto child = options.launcher->Start(manifest.CommandFor(*node.manifest), LogPath(node));
        if (!child.ok()) {
            Emit(Kind::kStartFailed, node, child.status().message());
            Ended(node, false, now, now);
            return;
        }
        node.child = std::move(child).value();
        node.phase = Node::Phase::kRunning;
        node.started_at = now;
        Emit(Kind::kStarted, node, "pid " + std::to_string(node.child->id()));
    }

    // Applies the restart policy to a node whose process ended (or did not
    // start) after running since `started_at`.
    void Ended(Node& node, bool success, Clock::time_point started_at, Clock::time_point now)
    {
        const NodeManifest& spec = *node.manifest;
        node.child.reset();
        if (now - started_at >= options.stable_run) {
            node.restarts = 0;
            node.delay = spec.restart_delay;
        }

        const bool wanted = spec.restart == NodeManifest::Restart::kAlways ||
                            (spec.restart == NodeManifest::Restart::kOnFailure && !success);
        const bool allowed = spec.max_restarts == 0 || node.restarts < spec.max_restarts;
        if (wanted && allowed) {
            ++node.restarts;
            node.phase = Node::Phase::kBackoff;
            node.restart_at = now + node.delay;
            std::string detail = "in " + Milliseconds(node.delay) + ", restart " + std::to_string(node.restarts);
            if (spec.max_restarts != 0) {
                detail += " of " + std::to_string(spec.max_restarts);
            }
            Emit(Kind::kRestarting, node, std::move(detail));
            const auto cap = std::max(options.max_restart_delay, spec.restart_delay);
            node.delay = std::min(node.delay * 2, cap);
            return;
        }

        node.phase = Node::Phase::kDone;
        node.failed = !success;
        if (wanted) {
            Emit(Kind::kGaveUp, node, "after " + std::to_string(node.restarts) + " restarts");
        }
        if (spec.required && !ending) {
            ending = SupervisorResult {
                "required node " + spec.name + (success ? " ended" : " failed"), success ? 0 : 1};
        }
    }

    // One round of supervision over the nodes started so far.
    void Supervise()
    {
        for (auto& node : nodes) {
            const auto now = Clock::now();
            if (node.phase == Node::Phase::kRunning) {
                const auto exit = node.child->Poll();
                if (exit) {
                    Emit(Kind::kExited, node, ToString(*exit));
                    Ended(node, exit->success(), node.started_at, now);
                }
            } else if (node.phase == Node::Phase::kBackoff && now >= node.restart_at) {
                Start(node, now);
            }
        }
        if (ending) {
            return;
        }
        const bool all_done = std::all_of(
            nodes.begin(), nodes.end(), [](const Node& node) { return node.phase == Node::Phase::kDone; });
        if (all_done) {
            const bool any_failed =
                std::any_of(nodes.begin(), nodes.end(), [](const Node& node) { return node.failed; });
            ending = SupervisorResult {"every node ended", any_failed ? 1 : 0};
        }
    }

    // Supervises until `deadline`, a stop request, or a reason to end.
    void SuperviseUntil(Clock::time_point deadline)
    {
        while (!ending && !StopRequested()) {
            Supervise();
            const auto now = Clock::now();
            if (ending || now >= deadline) {
                return;
            }
            SleepUntil(std::min(deadline, now + options.poll_interval));
        }
    }

    // Stops the running nodes, last started first.
    void StopAll()
    {
        for (auto it = nodes.rbegin(); it != nodes.rend(); ++it) {
            Node& node = *it;
            if (node.phase != Node::Phase::kRunning) {
                node.phase = Node::Phase::kDone;
                continue;
            }
            Emit(Kind::kStopping, node, "pid " + std::to_string(node.child->id()));
            node.child->RequestStop();
            auto exit = WaitForExit(node, node.manifest->stop_timeout);
            if (!exit) {
                node.child->Kill();
                Emit(Kind::kKilled, node, "still running after " + Milliseconds(node.manifest->stop_timeout));
                exit = WaitForExit(node, std::chrono::milliseconds(5000));
            }
            Emit(Kind::kStopped, node, exit ? ToString(*exit) : "did not end after a kill");
            node.child.reset();  // reaps it, waiting if it has to
            node.phase = Node::Phase::kDone;
        }
    }

    std::optional<ProcessExit> WaitForExit(Node& node, std::chrono::milliseconds timeout)
    {
        const auto deadline = Clock::now() + timeout;
        while (true) {
            auto exit = node.child->Poll();
            const auto now = Clock::now();
            if (exit || now >= deadline) {
                return exit;
            }
            // Stop requests are already being served; sleep without them.
            std::this_thread::sleep_for(std::min<Clock::duration>(deadline - now, options.poll_interval));
        }
    }

    SupervisorResult Run()
    {
        if (!options.log_directory.empty()) {
            std::error_code error;
            std::filesystem::create_directories(options.log_directory, error);
            if (error) {
                return SupervisorResult {"creating " + options.log_directory + ": " + error.message(), 1};
            }
        }

        for (auto& node : nodes) {
            if (ending || StopRequested()) {
                break;
            }
            const auto now = Clock::now();
            Start(node, now);
            SuperviseUntil(now + node.manifest->start_delay);
        }
        SuperviseUntil(Clock::time_point::max());

        SupervisorResult result = ending ? *ending : SupervisorResult {"stop requested", 0};
        StopAll();
        return result;
    }
};

Supervisor::Supervisor(SystemManifest manifest, SupervisorOptions options)
    : impl_(std::make_unique<Impl>(std::move(manifest), std::move(options)))
{
}

Supervisor::~Supervisor() = default;

SupervisorResult Supervisor::Run()
{
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->ran) {
            return SupervisorResult {"Run() was already called", 1};
        }
        impl_->ran = true;
    }
    return impl_->Run();
}

void Supervisor::RequestStop() noexcept
{
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->stop_requested = true;
    }
    impl_->wake.notify_all();
}

}  // namespace puppet_master::configuration
