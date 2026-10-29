// puppet_master_launch: starts the nodes of a system manifest, restarts them
// by their policy, and stops them in reverse order on SIGINT/SIGTERM (Ctrl+C
// or Ctrl+Break on Windows).
//
//     puppet_master_launch system.json
//     puppet_master_launch --log-dir logs system.json
//
// The manifest is checked as one system first, as by
// `puppet_master_config system validate`; --no-validate skips that.

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <string>
#include <thread>

#include <puppet_master/configuration/application.h>
#include <puppet_master/configuration/supervisor.h>
#include <puppet_master/configuration/system_manifest.h>

namespace config = puppet_master::configuration;

namespace {

std::atomic<bool> g_stop_signal {false};

void HandleStopSignal(int)
{
    g_stop_signal.store(true);
}

int Usage(std::ostream& out)
{
    out << "usage: puppet_master_launch [--log-dir DIR] [--no-validate] <system.json>\n"
        << "  --log-dir DIR   write each node's output to DIR/<node>.log\n"
        << "  --no-validate   start without checking the nodes' configurations together\n";
    return 2;
}

bool Validate(const config::SystemManifest& system)
{
    auto report = config::ValidateSystemManifest(system);
    if (!report.ok()) {
        std::cerr << "launch: " << report.status().ToString() << '\n';
        return false;
    }
    for (const auto& warning : report.value().warnings) {
        std::cerr << "launch: warning: " << warning << '\n';
    }
    for (const auto& error : report.value().errors) {
        std::cerr << "launch: " << error << '\n';
    }
    return report.value().ok();
}

}  // namespace

int main(int argc, char** argv)
{
    std::string log_directory;
    std::string path;
    bool validate = true;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--help" || argument == "-h") {
            Usage(std::cout);
            return 0;
        }
        if (argument == "--log-dir" && index + 1 < argc) {
            log_directory = argv[++index];
        } else if (argument == "--no-validate") {
            validate = false;
        } else if (!argument.empty() && argument[0] != '-' && path.empty()) {
            path = argument;
        } else {
            return Usage(std::cerr);
        }
    }
    if (path.empty()) {
        return Usage(std::cerr);
    }

    auto manifest = config::LoadSystemManifest(path);
    if (!manifest.ok()) {
        std::cerr << "launch: " << manifest.status().ToString() << '\n';
        return 1;
    }
    if (validate && !Validate(manifest.value())) {
        return 1;
    }
    const std::string name = manifest.value().name.empty() ? path : manifest.value().name;

    config::SupervisorOptions options;
    options.log_directory = log_directory;
    options.on_event = [](const config::SupervisorEvent& event) {
        std::cerr << "launch: " << event.node << ": " << config::ToString(event.kind);
        if (!event.detail.empty()) {
            std::cerr << " (" << event.detail << ')';
        }
        std::cerr << std::endl;
    };
    config::Supervisor supervisor(std::move(manifest).value(), std::move(options));

    // The nodes run in process groups of their own, so a Ctrl+C reaches only
    // this process, which stops them in order.
    std::signal(SIGINT, HandleStopSignal);
    std::signal(SIGTERM, HandleStopSignal);
#if defined(SIGBREAK)
    std::signal(SIGBREAK, HandleStopSignal);
#endif
    std::atomic<bool> done {false};
    std::thread watcher([&] {
        while (!done.load()) {
            if (g_stop_signal.exchange(false)) {
                std::cerr << "launch: stopping " << name << std::endl;
                supervisor.RequestStop();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    });

    std::cerr << "launch: starting " << name << std::endl;
    const auto result = supervisor.Run();
    done.store(true);
    watcher.join();

    std::cerr << "launch: " << name << " stopped: " << result.reason << std::endl;
    return result.exit_code;
}
