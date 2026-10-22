// Multi-file configuration: imports through a search path, deployment files,
// per-reader policy, and system validation across processes. The temporary
// layout mimics separately installed interface packages:
//
//     <root>/share/sensor_interfaces/topics.json      (sensor team)
//     <root>/share/estimation_interfaces/topics.json  (imports sensor topics)
//     <root>/nodes/*.json                             (one per process)
//     <root>/deploy.json                              (system integrator)

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <puppet_master/configuration/application.h>
#include <puppet_master/configuration/json_loader.h>
#include <puppet_master/transport/shm/shm_transport.h>

namespace config = puppet_master::configuration;
namespace core = puppet_master::core;
namespace fs = std::filesystem;
namespace shm = puppet_master::transport::shm;

namespace {

class TempTree {
public:
    TempTree()
        : root_(fs::temp_directory_path()
                / ("pm_imports_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())))
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

    std::string Path(const std::string& relative) const { return (root_ / relative).string(); }

private:
    fs::path root_;
};

const char* kSensorTopics = R"json({"topics": [
    {"id": "wheel", "name": "/vehicle/wheel_speed", "message_type": "vehicle.WheelSpeed", "message_version": 2,
     "encoding": "application/x-puppetmaster-trivial"},
    {"id": "imu", "name": "/vehicle/imu", "message_type": "vehicle.ImuSample", "message_version": 1,
     "encoding": "application/x-puppetmaster-trivial"}
]})json";

const char* kEstimationTopics = R"json({
    "imports": ["sensor_interfaces/topics.json"],
    "topics": [
        {"id": "state", "name": "/vehicle/state", "message_type": "vehicle.VehicleState", "message_version": 1,
         "encoding": "application/x-puppetmaster-trivial"}
    ]})json";

const char* kSensorsNode = R"json({
    "imports": ["sensor_interfaces/topics.json"],
    "components": [
        {"name": "wheel", "writers": ["wheel"], "triggers": [{"kind": "manual"}]},
        {"name": "imu", "writers": ["imu"], "triggers": [{"kind": "manual"}]}
    ]})json";

const char* kEstimatorNode = R"json({
    "imports": ["estimation_interfaces/topics.json"],
    "components": [{
        "name": "estimator",
        "readers": [{"topic": "wheel", "freshness": "queued", "queue_depth": 8}, "imu"],
        "writers": ["state"],
        "triggers": [{"kind": "data", "dependency_policy": "any", "data_topics": ["wheel", "imu"]}]
    }]})json";

const char* kMonitorNode = R"json({
    "imports": ["estimation_interfaces/topics.json"],
    "components": [{"name": "monitor", "readers": ["state"], "triggers": [{"kind": "data", "data_topics": ["state"]}]}]
})json";

config::JsonLoadOptions Options(const TempTree& tree, std::string deployment = {})
{
    config::JsonLoadOptions options;
    options.search_paths = {tree.Path("share")};
    options.deployment_path = std::move(deployment);
    return options;
}

TempTree MakeSystem()
{
    TempTree tree;
    tree.Write("share/sensor_interfaces/topics.json", kSensorTopics);
    tree.Write("share/estimation_interfaces/topics.json", kEstimationTopics);
    tree.Write("nodes/sensors.json", kSensorsNode);
    tree.Write("nodes/estimator.json", kEstimatorNode);
    tree.Write("nodes/monitor.json", kMonitorNode);
    return tree;
}

bool Contains(const std::string& text, const std::string& part)
{
    return text.find(part) != std::string::npos;
}

void ImportsResolveThroughSearchPath()
{
    const auto tree = MakeSystem();
    auto estimator = config::LoadJsonConfig(tree.Path("nodes/estimator.json"), Options(tree));
    assert(estimator.ok());
    // Transitive imports in declaration order: sensor topics, then state.
    const auto& topics = estimator.value().topics;
    assert(topics.size() == 3);
    assert(topics[0].id == "wheel" && topics[1].id == "imu" && topics[2].id == "state");
    assert(topics[0].message_version == 2);

    // Without the search path the package is not found, and the error says
    // where it looked and how to fix it.
    auto missing = config::LoadJsonConfig(tree.Path("nodes/estimator.json"), config::JsonLoadOptions {});
    assert(missing.status().code() == core::StatusCode::kInvalidArgument);
    assert(Contains(missing.status().message(), "import not found: estimation_interfaces/topics.json"));
    assert(Contains(missing.status().message(), "PUPPETMASTER_CONFIG_PATH"));
    assert(Contains(missing.status().message(), "estimator.json"));

    // A file next to the importer wins over the search path.
    tree.Write("nodes/sensor_interfaces/topics.json", R"json({"topics": [
        {"id": "wheel", "name": "/local/wheel", "message_type": "vehicle.WheelSpeed", "message_version": 2,
         "encoding": "application/x-puppetmaster-trivial"},
        {"id": "imu", "name": "/local/imu", "message_type": "vehicle.ImuSample", "message_version": 1,
         "encoding": "application/x-puppetmaster-trivial"}]})json");
    auto local = config::LoadJsonConfig(tree.Path("nodes/sensors.json"), Options(tree));
    assert(local.ok() && local.value().topics[0].name == "/local/wheel");

    // Text has no file to resolve imports against.
    auto text = config::ParseJsonConfig(kSensorsNode);
    assert(text.status().code() == core::StatusCode::kInvalidArgument);
    assert(Contains(text.status().message(), "LoadJsonConfig"));
}

void ImportErrorsNameTheirFile()
{
    TempTree tree;
    tree.Write("share/a/topics.json", R"json({"imports": ["b/topics.json"], "topics": []})json");
    tree.Write("share/b/topics.json", R"json({"imports": ["a/topics.json"], "topics": []})json");
    tree.Write("cycle.json", R"json({"imports": ["a/topics.json"]})json");
    auto cycle = config::LoadJsonConfig(tree.Path("cycle.json"), Options(tree));
    assert(cycle.status().code() == core::StatusCode::kInvalidArgument);
    assert(Contains(cycle.status().message(), "import cycle"));

    // An interface file may only declare topics.
    tree.Write("share/bad/topics.json", R"json({"topics": [], "components": []})json");
    tree.Write("bad.json", R"json({"imports": ["bad/topics.json"]})json");
    auto bad = config::LoadJsonConfig(tree.Path("bad.json"), Options(tree));
    assert(Contains(bad.status().message(), "bad/topics.json") || Contains(bad.status().message(), "bad\\topics.json"));
    assert(Contains(bad.status().message(), "may only declare topics"));

    // A topic id declared twice names the file of the first declaration.
    tree.Write("share/dup/topics.json", R"json({"topics": [
        {"id": "speed", "name": "/a", "message_type": "t"}]})json");
    tree.Write("dup.json", R"json({"imports": ["dup/topics.json"], "topics": [
        {"id": "speed", "name": "/b", "message_type": "t"}]})json");
    auto duplicate = config::LoadJsonConfig(tree.Path("dup.json"), Options(tree));
    assert(Contains(duplicate.status().message(), "topic id speed is already declared in"));

    // Errors inside an imported file point at it.
    tree.Write("share/broken/topics.json", R"json({"topics": [{"id": "x", "name": "/x", "unknown": 1}]})json");
    tree.Write("broken.json", R"json({"imports": ["broken/topics.json"]})json");
    auto broken = config::LoadJsonConfig(tree.Path("broken.json"), Options(tree));
    assert(Contains(broken.status().message(), "topics.json: $.topics[0].unknown is unknown"));

    // Importing the same package along two paths is fine (diamond).
    tree.Write("share/base/topics.json", R"json({"topics": [{"id": "base", "name": "/base", "message_type": "t"}]})json");
    tree.Write("share/left/topics.json", R"json({"imports": ["base/topics.json"], "topics": []})json");
    tree.Write("share/right/topics.json", R"json({"imports": ["base/topics.json"], "topics": []})json");
    tree.Write("diamond.json", R"json({"imports": ["left/topics.json", "right/topics.json"]})json");
    auto diamond = config::LoadJsonConfig(tree.Path("diamond.json"), Options(tree));
    assert(diamond.ok() && diamond.value().topics.size() == 1);

    auto unreadable = config::LoadJsonConfig(tree.Path("absent.json"), Options(tree));
    assert(unreadable.status().code() == core::StatusCode::kUnavailable);
}

void ReaderPoliciesApplyPerReader()
{
    const auto tree = MakeSystem();
    auto estimator = config::LoadJsonConfig(tree.Path("nodes/estimator.json"), Options(tree));
    assert(estimator.ok());
    auto spec = estimator.value().BuildComponentSpec("estimator");
    assert(spec.ok());
    const auto& wheel = spec.value().readers[0].topic.message_policy;
    assert(wheel.freshness == core::FreshnessPolicy::kQueued && wheel.queue_depth == 8);
    // The plain reader keeps the topic's own policy.
    assert(spec.value().readers[1].topic.message_policy.freshness == core::FreshnessPolicy::kLatest);
    // The topic itself is unchanged for other readers and writers.
    assert(estimator.value().FindTopic("wheel").value().message_policy.freshness == core::FreshnessPolicy::kLatest);

    for (const auto* json : {
             R"json({"topics":[{"id":"a","name":"/a","message_type":"t"}],"components":[{"name":"c","readers":[{"queue_depth":2}]}]})json",
             R"json({"topics":[{"id":"a","name":"/a","message_type":"t"}],"components":[{"name":"c","readers":[{"topic":"missing"}]}]})json",
             R"json({"topics":[{"id":"a","name":"/a","message_type":"t"}],"components":[{"name":"c","readers":[{"topic":"a","depth":2}]}]})json",
             R"json({"topics":[{"id":"a","name":"/a","message_type":"t"}],"components":[{"name":"c","readers":[{"topic":"a"},{"topic":"a"}]}]})json",
             R"json({"topics":[{"id":"a","name":"/a","message_type":"t"}],"components":[{"name":"c","readers":[{"topic":"a","freshness":"queued","queue_depth":0}]}]})json",
             R"json({"topics":[{"id":"a","name":"/a","message_type":"t"}],"components":[{"name":"c","readers":[7]}]})json",
         }) {
        auto invalid = config::ParseJsonConfig(json);
        assert(invalid.status().code() == core::StatusCode::kInvalidArgument);
    }

    // C++ model: a policy for a topic the component does not read is rejected.
    config::ProjectConfig project;
    assert(project.AddTopic(config::TopicConfig {"a", "/a", core::TransportKind::kInMemory, "t", "e", {}}).ok());
    config::ComponentConfig component {"c", "", {}, {}, {}};
    component.reader_policies["a"] = core::MessagePolicy {};
    assert(project.AddComponent(component).ok());
    assert(Contains(project.Validate().message(), "which it does not read"));
}

void DeploymentSetsTransportNotIdentity()
{
    const auto tree = MakeSystem();
    const auto deployment = tree.Write("deploy.json", R"json({
        "defaults": {"transport": "shm", "transport_parameters": {"domain_id": 7}},
        "topics": [
            {"name": "/vehicle/wheel_speed", "transport_parameters": {"slot_payload_size": 4096, "slot_count": 32}},
            {"name": "/not/used/here", "transport": "fastdds"}
        ]})json");
    auto sensors = config::LoadJsonConfig(tree.Path("nodes/sensors.json"), Options(tree, deployment));
    assert(sensors.ok());
    const auto& wheel = sensors.value().topics[0];
    assert(wheel.transport == core::TransportKind::kIpc);
    assert(wheel.transport_parameters.at("slot_count") == "32");
    assert(wheel.transport_parameters.at("domain_id") == "7");
    assert(sensors.value().topics[1].transport == core::TransportKind::kIpc);
    assert(Contains(config::DescribeProject(sensors.value()), "slot_count=32"));

    // The shm adapter turns the parameters into its options.
    auto options = shm::OptionsFromProject(sensors.value());
    assert(options.ok());
    assert(options.value().domain_id == 7);
    assert(options.value().ForTopic("/vehicle/wheel_speed").slot_count == 32);
    assert(options.value().ForTopic("/vehicle/wheel_speed").slot_payload_size == 4096);
    assert(options.value().ForTopic("/vehicle/imu").slot_count == shm::TopicOptions {}.slot_count);

    // A deployment cannot redefine what a topic carries.
    const auto identity = tree.Write("identity.json", R"json({"topics": [
        {"name": "/vehicle/wheel_speed", "message_type": "other"}]})json");
    auto rejected = config::LoadJsonConfig(tree.Path("nodes/sensors.json"), Options(tree, identity));
    assert(rejected.status().code() == core::StatusCode::kInvalidArgument);
    assert(Contains(rejected.status().message(), "identity.json"));
    assert(Contains(rejected.status().message(), "message_type is unknown"));

    // Bad shm settings are reported with the topic.
    for (const auto* settings : {R"json({"slot_count": 0})json", R"json({"slots": 4})json",
                                 R"json({"slot_count": "many"})json"}) {
        const auto bad_shm = tree.Write("bad_shm.json", std::string(R"json({"topics": [
            {"name": "/vehicle/imu", "transport": "shm", "transport_parameters": )json") + settings + "}]}");
        auto bad = config::LoadJsonConfig(tree.Path("nodes/sensors.json"), Options(tree, bad_shm));
        assert(bad.ok());
        auto bad_options = shm::OptionsFromProject(bad.value());
        assert(bad_options.status().code() == core::StatusCode::kInvalidArgument);
        assert(Contains(bad_options.status().message(), "topic /vehicle/imu"));
    }

    // Two topics may not ask for different domains.
    const auto domains = tree.Write("domains.json", R"json({"defaults": {"transport": "shm"}, "topics": [
        {"name": "/vehicle/imu", "transport_parameters": {"domain_id": 1}},
        {"name": "/vehicle/wheel_speed", "transport_parameters": {"domain_id": 2}}]})json");
    auto split = config::LoadJsonConfig(tree.Path("nodes/sensors.json"), Options(tree, domains));
    assert(split.ok());
    assert(Contains(shm::OptionsFromProject(split.value()).status().message(), "one transport has one domain"));
}

void SystemValidationFindsCrossProcessProblems()
{
    const auto tree = MakeSystem();
    std::vector<config::ProjectConfig> projects;
    for (const char* node : {"nodes/sensors.json", "nodes/estimator.json", "nodes/monitor.json"}) {
        auto project = config::LoadJsonConfig(tree.Path(node), Options(tree));
        assert(project.ok());
        projects.push_back(std::move(project).value());
    }
    const auto check = [&projects](std::vector<std::size_t> indices) {
        std::vector<config::NamedProject> named;
        const char* labels[] = {"sensors", "estimator", "monitor", "extra"};
        for (const auto index : indices) {
            named.push_back(config::NamedProject {labels[index], &projects[index]});
        }
        return config::ValidateSystem(named);
    };

    const auto whole = check({0, 1, 2});
    assert(whole.ok() && whole.warnings.empty());

    // Without the sensor process, the estimator waits for data that never comes.
    const auto no_sensors = check({1, 2});
    assert(!no_sensors.ok());
    assert(Contains(no_sensors.errors.front(), "/vehicle/imu is read by estimator but written by no process"));

    // Without the monitor, the state topic is written for nobody.
    const auto no_monitor = check({0, 1});
    assert(no_monitor.ok());
    assert(no_monitor.warnings.size() == 1 && Contains(no_monitor.warnings.front(), "/vehicle/state"));

    // A process built from an older interface release disagrees on a version.
    auto stale = projects[0];
    stale.topics[0].message_version = 1;
    projects.push_back(stale);
    const auto mismatch = check({3, 1, 2});
    assert(!mismatch.ok());
    assert(Contains(mismatch.errors.front(), "vehicle.WheelSpeed v1"));
    assert(Contains(mismatch.errors.front(), "vehicle.WheelSpeed v2"));
    assert(Contains(mismatch.errors.front(), "extra"));

    // Two processes writing one topic.
    projects[3] = projects[0];
    const auto two_writers = check({0, 1, 2, 3});
    assert(!two_writers.ok());
    assert(Contains(two_writers.errors.front(), "written by several processes: extra, sensors"));
}

}  // namespace

int main()
{
    ImportsResolveThroughSearchPath();
    ImportErrorsNameTheirFile();
    ReaderPoliciesApplyPerReader();
    DeploymentSetsTransportNotIdentity();
    SystemValidationFindsCrossProcessProblems();
    return 0;
}
