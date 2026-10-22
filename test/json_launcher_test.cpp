#include <atomic>
#include <cassert>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <puppet_master/configuration/json_launcher.h>
#include <puppet_master/puppet_master.h>

namespace config = puppet_master::configuration;
namespace core = puppet_master::core;
namespace runtime = puppet_master::runtime;

namespace {

std::atomic<int> g_started {0};
std::atomic<int> g_shut_down {0};

// Starting raises SIGTERM, so a launched application stops as soon as it is
// up. The launcher installs its handler before starting components.
class SelfStoppingComponent final : public runtime::Component {
public:
    explicit SelfStoppingComponent(runtime::ComponentSpec spec) : spec_(std::move(spec)) {}

    runtime::ComponentSpec Describe() const override { return spec_; }

    core::Status Start(runtime::ComponentContext&) override
    {
        ++g_started;
        std::raise(SIGTERM);
        return core::Status::Ok();
    }

    core::Status Shutdown(runtime::ComponentContext&) override
    {
        ++g_shut_down;
        return core::Status::Ok();
    }

private:
    runtime::ComponentSpec spec_;
};

config::ComponentFactoryRegistry Factories()
{
    config::ComponentFactoryRegistry factories;
    assert(factories.Register("self_stopping", [](const runtime::ComponentSpec& spec) {
        return core::Result<runtime::ComponentPtr>(runtime::ComponentPtr(std::make_shared<SelfStoppingComponent>(spec)));
    }).ok());
    return factories;
}

std::string WriteConfig(const std::string& name, const std::string& json)
{
    const auto path = std::filesystem::temp_directory_path()
        / ("pm_launcher_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" + name);
    std::ofstream(path) << json;
    return path.string();
}

const char* kConfig = R"json({
    "topics": [{"id": "speed", "name": "/vehicle/speed", "message_type": "demo.Speed"}],
    "components": [{
        "name": "stopper",
        "type": "self_stopping",
        "readers": ["speed"],
        "triggers": [{"kind": "manual"}]
    }]
})json";

struct Captured {
    int code;
    std::string out;
    std::string err;
};

Captured Launch(std::vector<std::string> arguments)
{
    arguments.insert(arguments.begin(), "launcher_test");
    std::vector<char*> argv;
    for (auto& argument : arguments) {
        argv.push_back(argument.data());
    }
    std::ostringstream out;
    std::ostringstream err;
    auto* old_out = std::cout.rdbuf(out.rdbuf());
    auto* old_err = std::cerr.rdbuf(err.rdbuf());
    const int code = config::RunJsonApplication(static_cast<int>(argv.size()), argv.data(), Factories());
    std::cout.rdbuf(old_out);
    std::cerr.rdbuf(old_err);
    return Captured {code, out.str(), err.str()};
}

}  // namespace

int main()
{
    const auto good = WriteConfig("good.json", kConfig);
    std::string unknown_type = kConfig;
    unknown_type.replace(unknown_type.find("self_stopping"), 13, "not_registered");
    const auto bad_type = WriteConfig("bad_type.json", unknown_type);
    const auto malformed = WriteConfig("malformed.json", "{\"components\": [");

    // Usage errors.
    assert(Launch({}).code == 2);
    assert(Launch({"--validate", "--describe", good}).code == 2);
    assert(Launch({"--bogus", good}).code == 2);
    assert(Launch({"--help"}).code == 0);

    // --describe and --validate do not start anything.
    auto described = Launch({"--describe", good});
    assert(described.code == 0);
    assert(described.out.find("stopper (self_stopping)") != std::string::npos);
    auto validated = Launch({"--validate", good});
    assert(validated.code == 0 && validated.out.find(": ok (1 topics, 1 components)") != std::string::npos);
    assert(g_started == 0);

    // Configuration errors name the file and the problem.
    auto missing_type = Launch({"--validate", bad_type});
    assert(missing_type.code == 1);
    assert(missing_type.err.find("not_registered") != std::string::npos);
    assert(Launch({"--validate", malformed}).code == 1);
    assert(Launch({"--validate", good + ".does_not_exist"}).code == 1);
    assert(Launch({bad_type}).code == 1);
    assert(g_started == 0);

    // Imports come from --config-path; a deployment applies with --deployment.
    const auto share = std::filesystem::temp_directory_path()
        / ("pm_launcher_share_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(share / "speed_interfaces");
    std::ofstream(share / "speed_interfaces" / "topics.json")
        << R"json({"topics": [{"id": "speed", "name": "/vehicle/speed", "message_type": "demo.Speed"}]})json";
    const auto importing = WriteConfig("importing.json", R"json({
        "imports": ["speed_interfaces/topics.json"],
        "components": [{"name": "stopper", "type": "self_stopping", "readers": ["speed"],
                        "triggers": [{"kind": "manual"}]}]})json");
    const auto deployment = WriteConfig("deployment.json", R"json({"topics": [
        {"name": "/vehicle/speed", "transport_parameters": {"note": "from deployment"}}]})json");
    auto not_found = Launch({"--validate", importing});
    assert(not_found.code == 1 && not_found.err.find("import not found") != std::string::npos);
    assert(not_found.err.find("importing.json") != std::string::npos);
    assert(Launch({"--validate", "--config-path", share.string(), importing}).code == 0);
    auto deployed = Launch({"--describe", "--config-path", share.string(), "--deployment", deployment, importing});
    assert(deployed.code == 0 && deployed.out.find("note=from deployment") != std::string::npos);
    assert(Launch({"--validate", importing, "--config-path"}).code == 2);
    assert(Launch({"--deployment", deployment, "--deployment", deployment, importing}).code == 2);

    // A full run starts, stops on the signal, and shuts components down.
    auto ran = Launch({good});
    assert(ran.code == 0);
    assert(g_started == 1 && g_shut_down == 1);

    for (const auto& path : {good, bad_type, malformed, importing, deployment}) {
        std::filesystem::remove(path);
    }
    std::filesystem::remove_all(share);
    return 0;
}
