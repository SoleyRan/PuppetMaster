// System manifests: one file listing every node, read by the system check
// and by launch scripts.

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <puppet_master/configuration/system_manifest.h>

namespace config = puppet_master::configuration;
namespace core = puppet_master::core;
namespace fs = std::filesystem;

namespace {

class TempTree {
public:
    TempTree()
        : root_(fs::temp_directory_path()
                / ("pm_manifest_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())))
    {
        fs::create_directories(root_);
    }
    ~TempTree() { fs::remove_all(root_); }

    std::string Write(const std::string& relative, const std::string& text) const
    {
        const auto path = root_ / relative;
        fs::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary) << text;
        return path.string();
    }

    std::string Path(const std::string& relative) const { return (root_ / relative).lexically_normal().string(); }

private:
    fs::path root_;
};

bool Contains(const std::string& text, const std::string& part)
{
    return text.find(part) != std::string::npos;
}

void SetEnvironment(const char* name, const std::string& value)
{
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

void UnsetEnvironment(const char* name)
{
#ifdef _WIN32
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
}

// An installed layout: interface and node configs under share/, binaries
// under bin/, the manifest and deployment under etc/.
void WriteSystem(const TempTree& tree)
{
    tree.Write("share/demo_interfaces/topics.json", R"json({
        "topics": [{"id": "speed", "name": "/demo/speed", "message_type": "demo.Speed"}],
        "services": [{"id": "reset", "name": "/demo/reset", "request_type": "demo.Reset", "reply_type": "demo.Ack"}]
    })json");
    tree.Write("share/producer/producer.json", R"json({
        "imports": ["demo_interfaces/topics.json"],
        "components": [{"name": "producer", "writers": ["speed"], "servers": ["reset"],
                        "triggers": [{"kind": "manual"}]}]})json");
    tree.Write("share/consumer/consumer.json", R"json({
        "imports": ["demo_interfaces/topics.json"],
        "components": [{"name": "consumer", "readers": ["speed"], "clients": ["reset"],
                        "triggers": [{"kind": "data", "data_topics": ["speed"]}]}]})json");
    tree.Write("bin/producer_node", "");
    tree.Write("etc/deploy.json", R"json({"defaults": {"transport": "shm"}})json");
}

void LoadsAndResolves()
{
    TempTree tree;
    WriteSystem(tree);
    SetEnvironment("PM_TEST_PREFIX", tree.Path(""));
    const auto manifest_path = tree.Write("etc/system.json", R"json({
        "name": "demo",
        "config_paths": ["${PM_TEST_PREFIX}/share"],
        "bin_paths": ["../bin"],
        "deployment": "deploy.json",
        "nodes": [
            {"name": "consumer", "executable": "consumer_node", "config": "consumer/consumer.json",
             "start_delay_ms": 200},
            {"name": "producer", "executable": "producer_node", "config": "producer/producer.json",
             "arguments": ["--describe"]}
        ]})json");
    auto manifest = config::LoadSystemManifest(manifest_path);
    if (!manifest.ok()) {
        std::fprintf(stderr, "%s\n", manifest.status().ToString().c_str());
    }
    assert(manifest.ok());
    const auto& system = manifest.value();
    assert(system.name == "demo" && system.nodes.size() == 2);
    assert(system.config_paths.front() == tree.Path("share"));
    assert(system.deployment == tree.Path("etc/deploy.json"));

    // Nodes keep start order; configs resolve through config_paths, and a
    // binary found in bin_paths gets its full path, while one that is not
    // stays a bare name for PATH lookup.
    const auto& consumer = system.nodes[0];
    assert(consumer.name == "consumer" && consumer.config == tree.Path("share/consumer/consumer.json"));
    assert(consumer.executable == "consumer_node" && consumer.start_delay == std::chrono::milliseconds(200));
    const auto& producer = system.nodes[1];
    assert(producer.executable == tree.Path("bin/producer_node"));

    const auto command = system.CommandFor(producer);
    const std::vector<std::string> expected {tree.Path("bin/producer_node"), "--config-path", tree.Path("share"),
                                             "--deployment", tree.Path("etc/deploy.json"), "--describe",
                                             tree.Path("share/producer/producer.json")};
    assert(command == expected);

    // Node files load with the manifest's search path and deployment.
    auto nodes = config::LoadSystemNodes(system);
    assert(nodes.ok() && nodes.value().size() == 2);
    assert(nodes.value()[1].name == "producer");
    assert(nodes.value()[1].project.topics.front().transport == core::TransportKind::kIpc);

    auto report = config::ValidateSystemManifest(system);
    assert(report.ok() && report.value().ok() && report.value().warnings.empty());
    UnsetEnvironment("PM_TEST_PREFIX");
}

void SystemProblemsNameNodes()
{
    TempTree tree;
    WriteSystem(tree);
    // Without the producer, the consumer's topic and service have no source.
    const auto path = tree.Write("system.json", R"json({
        "config_paths": ["share"],
        "nodes": [{"name": "consumer", "executable": "consumer_node", "config": "consumer/consumer.json"}]})json");
    auto manifest = config::LoadSystemManifest(path);
    assert(manifest.ok());
    auto report = config::ValidateSystemManifest(manifest.value());
    assert(report.ok() && !report.value().ok());
    bool topic = false;
    bool service = false;
    for (const auto& error : report.value().errors) {
        topic = topic || Contains(error, "topic /demo/speed is read by consumer but written by no process");
        service = service || Contains(error, "service /demo/reset is called by consumer but served by no process");
    }
    assert(topic && service);

    // A node whose file does not load is named.
    tree.Write("share/broken/broken.json", R"json({"components": [{"name": "x", "readers": ["missing"]}]})json");
    const auto broken_path = tree.Write("broken_system.json", R"json({
        "config_paths": ["share"],
        "nodes": [{"name": "broken", "executable": "x", "config": "broken/broken.json"}]})json");
    auto broken = config::LoadSystemManifest(broken_path);
    assert(broken.ok());
    auto failed = config::ValidateSystemManifest(broken.value());
    assert(!failed.ok() && Contains(failed.status().message(), "node broken: "));
}

void RejectsInvalidManifests()
{
    TempTree tree;
    WriteSystem(tree);
    const auto check = [&tree](const std::string& json, const std::string& expected) {
        const auto path = tree.Write("bad.json", json);
        auto manifest = config::LoadSystemManifest(path);
        assert(manifest.status().code() == core::StatusCode::kInvalidArgument);
        if (!Contains(manifest.status().message(), expected)) {
            std::fprintf(stderr, "expected \"%s\" in: %s\n", expected.c_str(), manifest.status().message().c_str());
            assert(false);
        }
    };
    const std::string node = R"json({"name": "a", "executable": "a", "config": "share/producer/producer.json"})json";
    check(R"json({"nodes": []})json", "$.nodes must be a non-empty array");
    check(R"json({"nodes": [)json" + node + "," + node + "]}", "node name a is duplicated");
    check(R"json({"nodes": [{"name": "a", "config": "x.json"}]})json", "$.nodes[0].executable is required");
    check(R"json({"nodes": [{"name": "a", "executable": "a", "config": "missing.json"}]})json",
          "config missing.json of node a not found");
    check(R"json({"unknown": 1, "nodes": [)json" + node + "]}", "$.unknown is unknown");
    check(R"json({"deployment": "nope.json", "nodes": [)json" + node + "]}", "nope.json not found");
    check(R"json({"config_paths": ["${PM_TEST_UNSET_VARIABLE}/share"], "nodes": [)json" + node + "]}",
          "environment variable PM_TEST_UNSET_VARIABLE is not set");
    check(R"json({"nodes": [{"name": "a", "executable": "a", "config": "share/producer/producer.json",
                             "start_delay_ms": -1}]})json",
          "start_delay_ms must be a non-negative integer");
    check(R"json({"nodes": [{"name": "a", "executable": "a", "config": "share/producer/producer.json",
                             "arguments": ["a\tb"]}]})json",
          "must not contain tabs");
    check("{not json", "parse");

    assert(config::LoadSystemManifest(tree.Path("absent.json")).status().code() == core::StatusCode::kUnavailable);
}

}  // namespace

int main()
{
    LoadsAndResolves();
    SystemProblemsNameNodes();
    RejectsInvalidManifests();
    return 0;
}
