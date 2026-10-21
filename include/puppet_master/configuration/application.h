#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <puppet_master/configuration/configuration.h>
#include <puppet_master/core/result.h>
#include <puppet_master/core/status.h>
#include <puppet_master/runtime/context.h>
#include <puppet_master/scheduler/scheduler.h>

// Launch tooling: runs a ProjectConfig end to end, so an application only
// supplies component factories. See docs/launcher.md.
namespace puppet_master::configuration {

// Problems found by ValidateProject. `ok()` is true when there are none.
struct ValidationReport {
    std::vector<std::string> errors;

    bool ok() const noexcept { return errors.empty(); }
};

// Checks everything that can be checked without starting anything: the
// project's own rules, and with `factories` that every component type has a
// registered factory. Reports every type problem, not only the first.
ValidationReport ValidateProject(const ProjectConfig& project, const ComponentFactoryRegistry* factories = nullptr);

// Human-readable summary of topics, components, and triggers.
std::string DescribeProject(const ProjectConfig& project);

struct ApplicationOptions {
    // Additional transports to register before the runtime opens, for
    // example FastDDS or shared memory. Called once with the new runtime.
    std::function<core::Status(runtime::RuntimeContext&)> register_transports;
};

// Owns the runtime and scheduler for one project and drives the component
// lifecycle: create, configure, initialize, start, then the reverse on stop.
// A failure at any step rolls back what already started.
class Application final {
public:
    static core::Result<std::unique_ptr<Application>> Create(
        ProjectConfig project, const ComponentFactoryRegistry& factories, ApplicationOptions options = {});

    ~Application();
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    // Brings every component up in declaration order and starts the scheduler.
    core::Status Start();

    // Stops the scheduler, then stops and shuts down components in reverse
    // order. Safe to call more than once; returns the first error seen.
    core::Status Stop();

    // Starts, then blocks until RequestStop() (or a signal when installed),
    // then stops. Returns the first error from either phase.
    core::Status Run();

    // Wakes Run(). Safe from any thread.
    void RequestStop() noexcept;

    // Makes SIGINT and SIGTERM call RequestStop() on this application until it
    // is destroyed. Only one application can own the handlers at a time.
    core::Status InstallSignalHandlers();

    bool running() const noexcept;
    const ProjectConfig& project() const noexcept;
    runtime::RuntimeContext& runtime() noexcept;
    scheduler::Scheduler& scheduler() noexcept;

private:
    struct Impl;
    explicit Application(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

}  // namespace puppet_master::configuration
