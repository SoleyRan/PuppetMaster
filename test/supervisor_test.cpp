// Supervisor: start order, restart policy, and stopping, first with scripted
// fake processes, then with real ones (this binary started in child modes).

#include <atomic>
#include <cassert>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <puppet_master/configuration/supervisor.h>

namespace config = puppet_master::configuration;
namespace core = puppet_master::core;
namespace fs = std::filesystem;

using Kind = config::SupervisorEvent::Kind;
using Restart = config::NodeManifest::Restart;
using namespace std::chrono_literals;

namespace {

// One scripted run of a fake process.
struct Run {
    int exit_after_polls {-1};  // -1: runs until stopped
    config::ProcessExit exit {};
    bool ignores_stop {false};
};

// What the fake processes did, in order: "start a", "stop a", "kill a".
struct World {
    std::map<std::string, std::deque<Run>> scripts;  // by executable
    std::set<std::string> fail_start;
    std::vector<std::string> log;
    std::int64_t next_id {100};

    std::size_t Count(const std::string& entry) const
    {
        std::size_t count = 0;
        for (const auto& line : log) {
            count += line == entry ? 1 : 0;
        }
        return count;
    }
};

class FakeChild final : public config::ChildProcess {
public:
    FakeChild(World& world, std::string name, Run run, std::int64_t id)
        : world_(world), name_(std::move(name)), run_(run), id_(id)
    {
    }

    std::int64_t id() const noexcept override { return id_; }

    std::optional<config::ProcessExit> Poll() override
    {
        if (!exit_ && run_.exit_after_polls >= 0 && polls_++ >= run_.exit_after_polls) {
            exit_ = run_.exit;
        }
        return exit_;
    }

    void RequestStop() override
    {
        world_.log.push_back("stop " + name_);
        if (!exit_ && !run_.ignores_stop) {
            exit_ = config::ProcessExit {0, false};
        }
    }

    void Kill() override
    {
        world_.log.push_back("kill " + name_);
        if (!exit_) {
            exit_ = config::ProcessExit {9, true};
        }
    }

private:
    World& world_;
    std::string name_;
    Run run_;
    std::int64_t id_;
    int polls_ {0};
    std::optional<config::ProcessExit> exit_;
};

class FakeLauncher final : public config::ProcessLauncher {
public:
    explicit FakeLauncher(World& world) : world_(world) {}

    core::Result<std::unique_ptr<config::ChildProcess>> Start(
        const std::vector<std::string>& command, const std::string&) override
    {
        const std::string& name = command.front();
        if (world_.fail_start.count(name) != 0) {
            return core::Result<std::unique_ptr<config::ChildProcess>>(core::Status::Unavailable("no " + name));
        }
        world_.log.push_back("start " + name);
        Run run;
        auto& script = world_.scripts[name];
        if (!script.empty()) {
            run = script.front();
            script.pop_front();
        }
        return std::unique_ptr<config::ChildProcess>(new FakeChild(world_, name, run, world_.next_id++));
    }

private:
    World& world_;
};

config::NodeManifest Node(const std::string& name)
{
    config::NodeManifest node;
    node.name = name;
    node.executable = name;
    node.config = name + ".json";
    node.restart_delay = 1ms;
    node.stop_timeout = 50ms;
    return node;
}

config::SystemManifest System(std::vector<config::NodeManifest> nodes)
{
    config::SystemManifest system;
    system.name = "test";
    system.nodes = std::move(nodes);
    return system;
}

Run ExitsWith(int code, int after_polls = 0)
{
    Run run;
    run.exit_after_polls = after_polls;
    run.exit = config::ProcessExit {code, false};
    return run;
}

// A supervisor over fake processes that records its events and can stop
// itself from them: `stop_on(event)` returns true to request a stop.
struct Harness {
    World world;
    std::vector<config::SupervisorEvent> events;
    std::function<bool(const config::SupervisorEvent&)> stop_on;

    config::SupervisorResult Run(config::SystemManifest system, std::chrono::milliseconds stable_run = 10s,
        std::chrono::milliseconds max_restart_delay = 30s)
    {
        config::Supervisor* self = nullptr;
        config::SupervisorOptions options;
        options.launcher = std::make_unique<FakeLauncher>(world);
        options.poll_interval = 1ms;
        options.stable_run = stable_run;
        options.max_restart_delay = max_restart_delay;
        options.on_event = [this, &self](const config::SupervisorEvent& event) {
            events.push_back(event);
            if (stop_on && stop_on(event)) {
                self->RequestStop();
            }
        };
        config::Supervisor supervisor(std::move(system), std::move(options));
        self = &supervisor;
        return supervisor.Run();
    }

    std::vector<std::string> Details(Kind kind, const std::string& node) const
    {
        std::vector<std::string> details;
        for (const auto& event : events) {
            if (event.kind == kind && event.node == node) {
                details.push_back(event.detail);
            }
        }
        return details;
    }
};

bool Contains(const std::string& text, const std::string& part)
{
    return text.find(part) != std::string::npos;
}

// --- fake processes ---------------------------------------------------------

void TestStartsInOrderAndStopsInReverse()
{
    Harness harness;
    harness.stop_on = [](const config::SupervisorEvent& event) {
        return event.kind == Kind::kStarted && event.node == "c";
    };
    const auto result = harness.Run(System({Node("a"), Node("b"), Node("c")}));
    assert(result.exit_code == 0);
    assert(result.reason == "stop requested");
    const std::vector<std::string> expected {
        "start a", "start b", "start c", "stop c", "stop b", "stop a"};
    assert(harness.world.log == expected);
    assert(harness.Details(Kind::kStopped, "a") == std::vector<std::string> {"exit code 0"});
}

void TestRestartsOnFailureWithGrowingDelay()
{
    Harness harness;
    harness.world.scripts["a"] = {ExitsWith(1), ExitsWith(1, 2)};
    auto a = Node("a");
    a.restart = Restart::kOnFailure;
    int starts = 0;
    harness.stop_on = [&starts](const config::SupervisorEvent& event) {
        return event.kind == Kind::kStarted && ++starts == 3;
    };
    const auto result = harness.Run(System({a}));
    assert(result.exit_code == 0);
    assert(harness.world.Count("start a") == 3);
    const std::vector<std::string> expected {"in 1 ms, restart 1 of 5", "in 2 ms, restart 2 of 5"};
    assert(harness.Details(Kind::kRestarting, "a") == expected);
    assert(harness.Details(Kind::kExited, "a") == std::vector<std::string>(2, "exit code 1"));
}

void TestOnFailureDoesNotRestartASuccess()
{
    Harness harness;
    harness.world.scripts["a"] = {ExitsWith(0)};
    auto a = Node("a");
    a.restart = Restart::kOnFailure;
    a.required = false;
    const auto result = harness.Run(System({a}));
    assert(result.reason == "every node ended");
    assert(result.exit_code == 0);
    assert(harness.world.Count("start a") == 1);
    assert(harness.Details(Kind::kGaveUp, "a").empty());
}

void TestRequiredNodeThatGivesUpStopsTheSystem()
{
    Harness harness;
    harness.world.scripts["a"] = {ExitsWith(1, 3), ExitsWith(1, 3), ExitsWith(1, 3)};
    auto a = Node("a");
    a.restart = Restart::kAlways;
    a.max_restarts = 2;
    const auto result = harness.Run(System({Node("b"), a}));
    assert(result.exit_code == 1);
    assert(result.reason == "required node a failed");
    assert(harness.world.Count("start a") == 3);
    assert(harness.Details(Kind::kGaveUp, "a") == std::vector<std::string> {"after 2 restarts"});
    // b keeps running until the system stops it.
    assert(harness.world.log.back() == "stop b");
}

void TestRequiredNodeThatEndsWellStopsTheSystemCleanly()
{
    Harness harness;
    harness.world.scripts["batch"] = {ExitsWith(0, 2)};
    const auto result = harness.Run(System({Node("server"), Node("batch")}));
    assert(result.exit_code == 0);
    assert(result.reason == "required node batch ended");
    assert(harness.world.log.back() == "stop server");
}

void TestOptionalNodeIsLeftStopped()
{
    Harness harness;
    harness.world.scripts["tool"] = {ExitsWith(4)};
    auto tool = Node("tool");
    tool.required = false;
    // Stop from another thread once the optional node has ended.
    config::SupervisorOptions options;
    options.launcher = std::make_unique<FakeLauncher>(harness.world);
    options.poll_interval = 1ms;
    std::atomic<bool> tool_ended {false};
    options.on_event = [&](const config::SupervisorEvent& event) {
        harness.events.push_back(event);
        if (event.node == "tool" && event.kind == Kind::kExited) {
            tool_ended.store(true);
        }
    };
    config::Supervisor supervisor(System({tool, Node("server")}), std::move(options));
    std::thread stopper([&] {
        while (!tool_ended.load()) {
            std::this_thread::sleep_for(1ms);
        }
        std::this_thread::sleep_for(20ms);  // the system keeps running
        supervisor.RequestStop();
    });
    const auto result = supervisor.Run();
    stopper.join();
    assert(result.reason == "stop requested");
    assert(result.exit_code == 0);
    assert(harness.world.Count("start tool") == 1);
    assert(harness.world.Count("stop tool") == 0);
    assert(harness.world.log.back() == "stop server");
}

void TestEveryNodeEnded()
{
    Harness harness;
    harness.world.scripts["a"] = {ExitsWith(0)};
    harness.world.scripts["b"] = {ExitsWith(2, 1)};
    auto a = Node("a");
    auto b = Node("b");
    a.required = false;
    b.required = false;
    const auto result = harness.Run(System({a, b}));
    assert(result.reason == "every node ended");
    assert(result.exit_code == 1);  // b failed
}

void TestDelayIsCappedAndResetByAStableRun()
{
    {
        Harness harness;
        harness.world.scripts["a"] = {ExitsWith(1), ExitsWith(1), ExitsWith(1), ExitsWith(1), ExitsWith(1)};
        auto a = Node("a");
        a.restart = Restart::kAlways;
        a.max_restarts = 4;
        harness.Run(System({a}), 10s, 3ms);
        const std::vector<std::string> expected {
            "in 1 ms, restart 1 of 4", "in 2 ms, restart 2 of 4", "in 3 ms, restart 3 of 4", "in 3 ms, restart 4 of 4"};
        assert(harness.Details(Kind::kRestarting, "a") == expected);
    }
    {
        // Every run counts as stable: the delay and the count start over.
        Harness harness;
        harness.world.scripts["a"] = {ExitsWith(1), ExitsWith(1), ExitsWith(1)};
        auto a = Node("a");
        a.restart = Restart::kOnFailure;
        a.max_restarts = 1;
        int starts = 0;
        harness.stop_on = [&starts](const config::SupervisorEvent& event) {
            return event.kind == Kind::kStarted && ++starts == 4;
        };
        harness.Run(System({a}), 0ms);
        assert(harness.Details(Kind::kRestarting, "a") == std::vector<std::string>(3, "in 1 ms, restart 1 of 1"));
        assert(harness.Details(Kind::kGaveUp, "a").empty());
    }
}

void TestFailedStart()
{
    {
        Harness harness;
        harness.world.fail_start = {"b"};
        const auto result = harness.Run(System({Node("a"), Node("b"), Node("c")}));
        assert(result.exit_code == 1);
        assert(result.reason == "required node b failed");
        assert(harness.Details(Kind::kStartFailed, "b") == std::vector<std::string> {"no b"});
        // c is never started; a is stopped.
        const std::vector<std::string> expected {"start a", "stop a"};
        assert(harness.world.log == expected);
    }
    {
        // A restart policy covers failed starts too.
        Harness harness;
        harness.world.fail_start = {"b"};
        auto b = Node("b");
        b.restart = Restart::kOnFailure;
        b.max_restarts = 2;
        b.required = false;
        int failures = 0;
        harness.stop_on = [&failures](const config::SupervisorEvent& event) {
            failures += event.kind == Kind::kStartFailed ? 1 : 0;
            return false;
        };
        harness.world.scripts["a"] = {ExitsWith(0, 50)};
        auto a = Node("a");
        a.required = false;
        harness.Run(System({b, a}));
        assert(failures == 3);
        assert(harness.Details(Kind::kGaveUp, "b") == std::vector<std::string> {"after 2 restarts"});
    }
}

void TestKillsANodeThatDoesNotStop()
{
    Harness harness;
    Run stubborn;
    stubborn.ignores_stop = true;
    harness.world.scripts["a"] = {stubborn};
    harness.stop_on = [](const config::SupervisorEvent& event) { return event.kind == Kind::kStarted; };
    const auto started = std::chrono::steady_clock::now();
    const auto result = harness.Run(System({Node("a")}));
    const auto elapsed = std::chrono::steady_clock::now() - started;
    assert(result.exit_code == 0);
    const std::vector<std::string> expected {"start a", "stop a", "kill a"};
    assert(harness.world.log == expected);
    assert(harness.Details(Kind::kKilled, "a") == std::vector<std::string> {"still running after 50 ms"});
    assert(harness.Details(Kind::kStopped, "a") == std::vector<std::string> {"signal 9"});
    assert(elapsed >= 50ms);
}

void TestStopDuringStartDelay()
{
    Harness harness;
    auto a = Node("a");
    a.start_delay = 10s;
    harness.stop_on = [](const config::SupervisorEvent& event) { return event.kind == Kind::kStarted; };
    const auto started = std::chrono::steady_clock::now();
    harness.Run(System({a, Node("b")}));
    assert(std::chrono::steady_clock::now() - started < 5s);
    assert(harness.world.Count("start b") == 0);
}

void TestRunOnce()
{
    World world;
    config::SupervisorOptions options;
    options.launcher = std::make_unique<FakeLauncher>(world);
    world.scripts["a"] = {ExitsWith(0)};
    config::Supervisor supervisor(System({Node("a")}), std::move(options));
    assert(supervisor.Run().exit_code == 0);
    const auto again = supervisor.Run();
    assert(again.exit_code == 1);
    assert(Contains(again.reason, "already"));
}

// --- real processes ---------------------------------------------------------

std::atomic<bool> g_child_stop {false};

void HandleChildStop(int)
{
    g_child_stop.store(true);
}

// This binary as a node: `--child exit N`, `--child serve`, `--child stubborn`,
// followed by the config argument the supervisor adds.
int RunChild(const std::string& mode, const std::string& argument)
{
    if (mode == "exit") {
        std::cout << "child " << mode << " started" << std::endl;
        return std::stoi(argument);
    }
    // Handlers first: "started" tells the test a stop request is handled.
    if (mode == "serve") {
        std::signal(SIGINT, HandleChildStop);
        std::signal(SIGTERM, HandleChildStop);
#if defined(SIGBREAK)
        std::signal(SIGBREAK, HandleChildStop);
#endif
    } else {
        std::signal(SIGTERM, SIG_IGN);
#if defined(SIGBREAK)
        std::signal(SIGBREAK, SIG_IGN);
#endif
    }
    std::cout << "child " << mode << " started" << std::endl;
    // Bounded, so a broken test never leaves a process behind for long.
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    while (!g_child_stop.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(5ms);
    }
    std::cout << "child " << mode << " stopping" << std::endl;
    return 0;
}

config::NodeManifest ChildNode(const std::string& self, const std::string& name, std::vector<std::string> arguments)
{
    config::NodeManifest node;
    node.name = name;
    node.executable = self;
    node.arguments = {"--child"};
    node.arguments.insert(node.arguments.end(), arguments.begin(), arguments.end());
    node.config = "unused.json";
    node.restart_delay = 10ms;
    node.stop_timeout = 3s;
    return node;
}

std::string ReadFile(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::size_t Occurrences(const std::string& text, const std::string& part)
{
    std::size_t count = 0;
    for (auto at = text.find(part); at != std::string::npos; at = text.find(part, at + 1)) {
        ++count;
    }
    return count;
}

// Waits until a node's log has `text`; used before stopping a node, so the
// stop finds its signal handlers installed.
void WaitForLog(const fs::path& path, const std::string& text)
{
    const auto deadline = std::chrono::steady_clock::now() + 20s;
    while (!Contains(ReadFile(path), text)) {
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(5ms);
    }
}

struct RealRun {
    config::SupervisorResult result;
    std::vector<config::SupervisorEvent> events;

    std::vector<std::string> Details(Kind kind, const std::string& node) const
    {
        std::vector<std::string> details;
        for (const auto& event : events) {
            if (event.kind == kind && event.node == node) {
                details.push_back(event.detail);
            }
        }
        return details;
    }
};

// Runs real processes; `stop_on` is checked for every event.
RealRun RunReal(config::SystemManifest system, const std::string& log_directory,
    std::function<bool(const config::SupervisorEvent&)> stop_on)
{
    RealRun run;
    config::Supervisor* self = nullptr;
    config::SupervisorOptions options;
    options.log_directory = log_directory;
    options.poll_interval = 5ms;
    options.on_event = [&](const config::SupervisorEvent& event) {
        std::cout << "  " << event.node << ": " << config::ToString(event.kind) << " (" << event.detail << ")\n";
        run.events.push_back(event);
        if (stop_on && stop_on(event)) {
            self->RequestStop();
        }
    };
    config::Supervisor supervisor(std::move(system), std::move(options));
    self = &supervisor;
    run.result = supervisor.Run();
    return run;
}

fs::path TempDirectory(const std::string& name)
{
    const auto path = fs::temp_directory_path() /
                      (name + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::remove_all(path);
    return path;
}

void TestRealProcessesRestartLogAndStop(const std::string& self)
{
    const auto logs = TempDirectory("pm_supervisor_logs_");
    auto worker = ChildNode(self, "worker", {"exit", "3"});
    worker.restart = Restart::kOnFailure;
    worker.max_restarts = 2;
    worker.required = false;
    auto server = ChildNode(self, "server", {"serve"});

    const auto run = RunReal(System({server, worker}), logs.string(), [&](const config::SupervisorEvent& event) {
        if (event.kind != Kind::kGaveUp || event.node != "worker") {
            return false;
        }
        WaitForLog(logs / "server.log", "child serve started");
        return true;
    });
    assert(run.result.reason == "stop requested");
    assert(run.result.exit_code == 0);
    assert(run.Details(Kind::kExited, "worker") == std::vector<std::string>(3, "exit code 3"));
    assert(run.Details(Kind::kStopped, "server").size() == 1);
    assert(run.Details(Kind::kStopped, "worker").empty());

    // Output is appended across restarts, one file per node.
    const auto worker_log = ReadFile(logs / "worker.log");
    assert(Occurrences(worker_log, "child exit started") == 3);
    const auto server_log = ReadFile(logs / "server.log");
    assert(Contains(server_log, "child serve started"));
#if !defined(_WIN32)
    // The stop request reached the node's handler, which ended it cleanly.
    // (Windows CI runners may have no console to deliver Ctrl+Break.)
    assert(run.Details(Kind::kStopped, "server") == std::vector<std::string> {"exit code 0"});
    assert(Contains(server_log, "child serve stopping"));
#endif
    fs::remove_all(logs);
}

void TestRealProcessThatIgnoresStopIsKilled(const std::string& self)
{
    auto stubborn = ChildNode(self, "stubborn", {"stubborn"});
    stubborn.stop_timeout = 200ms;
    const auto logs = TempDirectory("pm_supervisor_kill_");
    // Stop once it ignores the stop request.
    const auto run = RunReal(System({stubborn}), logs.string(), [&](const config::SupervisorEvent& event) {
        if (event.kind != Kind::kStarted) {
            return false;
        }
        WaitForLog(logs / "stubborn.log", "child stubborn started");
        return true;
    });
    assert(run.result.exit_code == 0);
    assert(run.Details(Kind::kKilled, "stubborn") == std::vector<std::string> {"still running after 200 ms"});
#if !defined(_WIN32)
    assert(run.Details(Kind::kStopped, "stubborn") == std::vector<std::string> {"signal 9"});
#endif
    fs::remove_all(logs);
}

void TestRealStartFailure()
{
    config::NodeManifest missing;
    missing.name = "missing";
    missing.executable = "puppet_master_no_such_program_for_supervisor_test";
    missing.config = "unused.json";
    const auto run = RunReal(System({missing}), "", nullptr);
    assert(run.result.exit_code == 1);
    assert(run.result.reason == "required node missing failed");
    const auto failures = run.Details(Kind::kStartFailed, "missing");
    assert(failures.size() == 1);
    assert(Contains(failures.front(), "starting puppet_master_no_such_program_for_supervisor_test"));
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc >= 3 && std::string(argv[1]) == "--child") {
        return RunChild(argv[2], argc >= 4 ? argv[3] : "");
    }

    TestStartsInOrderAndStopsInReverse();
    TestRestartsOnFailureWithGrowingDelay();
    TestOnFailureDoesNotRestartASuccess();
    TestRequiredNodeThatGivesUpStopsTheSystem();
    TestRequiredNodeThatEndsWellStopsTheSystemCleanly();
    TestOptionalNodeIsLeftStopped();
    TestEveryNodeEnded();
    TestDelayIsCappedAndResetByAStableRun();
    TestFailedStart();
    TestKillsANodeThatDoesNotStop();
    TestStopDuringStartDelay();
    TestRunOnce();

    const std::string self = fs::absolute(argv[0]).string();
    TestRealProcessesRestartLogAndStop(self);
    TestRealProcessThatIgnoresStopIsKilled(self);
    TestRealStartFailure();

    std::cout << "supervisor tests passed\n";
    return 0;
}
