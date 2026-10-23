// puppet_master_config: checks and summarizes JSON project configurations
// without building the application, for use in CI and code review.
//
//     puppet_master_config validate config/*.json
//     puppet_master_config validate --system sensors.json estimator.json monitor.json
//     puppet_master_config describe config/vehicle.json
//     puppet_master_config system validate system.json
//     puppet_master_config shm list
//
// Component types are not checked here because factories live in the
// application binary; run the application with --validate for that.

#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include <puppet_master/configuration/application.h>
#include <puppet_master/configuration/json_loader.h>
#include <puppet_master/configuration/system_manifest.h>

#ifdef PUPPETMASTER_CONFIG_WITH_SHM
#include <puppet_master/transport/shm/shm_transport.h>
#endif

namespace config = puppet_master::configuration;
#ifdef PUPPETMASTER_CONFIG_WITH_SHM
namespace shm = puppet_master::transport::shm;
#endif

namespace {

int Usage(std::ostream& out)
{
    out << "usage: puppet_master_config validate [--system] [options] <config.json>...\n"
        << "       puppet_master_config describe [options] <config.json>\n"
        << "       puppet_master_config system validate|describe|commands <system.json>\n"
        << "  --system           also check the files together as one system: matching\n"
        << "                     topic and service declarations, a writer for every read\n"
        << "                     topic, one writer per topic, one server per service\n"
        << "  --config-path DIR  also look for imports in DIR (repeatable; searched after\n"
        << "                     PUPPETMASTER_CONFIG_PATH)\n"
        << "  --deployment FILE  apply a deployment file to every configuration\n"
        << "  system validate    check every node of a system manifest, one by one and\n"
        << "                     together\n"
        << "  system describe    print every node's configuration\n"
        << "  system commands    print each node's start command, one per line:\n"
        << "                     name<TAB>word<TAB>...<TAB>#delay_ms=N\n"
        << "       puppet_master_config shm list [--domain N]\n"
        << "       puppet_master_config shm clean [--domain N] [--dry-run]\n"
        << "  shm list           list this host's shared-memory segments and who uses them\n"
        << "  shm clean          remove the segments no running process uses (Linux)\n";
    return 2;
}

void PrintReport(const config::SystemReport& report)
{
    for (const auto& warning : report.warnings) {
        std::cerr << "system: warning: " << warning << '\n';
    }
    for (const auto& error : report.errors) {
        std::cerr << "system: " << error << '\n';
    }
}

// `system <action> <manifest.json>`: the manifest lists every node, so no
// file list or search options are repeated on the command line.
int RunSystem(const std::string& action, const std::string& path)
{
    auto manifest = config::LoadSystemManifest(path);
    if (!manifest.ok()) {
        std::cerr << manifest.status().ToString() << '\n';
        return 1;
    }
    const auto& system = manifest.value();

    if (action == "commands") {
        // One node per line: name, then the command's words, tab-separated,
        // for launch scripts (`while IFS=$'\t' read -r name rest`).
        for (const auto& node : system.nodes) {
            std::cout << node.name;
            for (const auto& word : system.CommandFor(node)) {
                std::cout << '\t' << word;
            }
            std::cout << '\t' << "#delay_ms=" << node.start_delay.count() << '\n';
        }
        return 0;
    }

    auto nodes = config::LoadSystemNodes(system);
    if (!nodes.ok()) {
        std::cerr << nodes.status().ToString() << '\n';
        return 1;
    }

    if (action == "describe") {
        std::cout << "system " << (system.name.empty() ? system.path : system.name) << ": " << system.nodes.size()
                  << " nodes\n";
        for (std::size_t index = 0; index < system.nodes.size(); ++index) {
            const auto& node = system.nodes[index];
            std::cout << "\nnode " << node.name << " (" << node.executable << ", " << node.config << ")\n";
            std::cout << config::DescribeProject(nodes.value()[index].project);
        }
        return 0;
    }

    int failures = 0;
    std::vector<config::NamedProject> named;
    for (const auto& node : nodes.value()) {
        const auto report = config::ValidateProject(node.project);
        for (const auto& error : report.errors) {
            std::cerr << node.name << ": " << error << '\n';
        }
        failures += report.ok() ? 0 : 1;
        named.push_back(config::NamedProject {node.name, &node.project});
    }
    const auto report = config::ValidateSystem(named);
    PrintReport(report);
    if (failures != 0 || !report.ok()) {
        return 1;
    }
    std::cout << "system " << (system.name.empty() ? system.path : system.name) << ": ok (" << system.nodes.size()
              << " nodes)\n";
    return 0;
}

// `shm list|clean [--domain N] [--dry-run]`: shared-memory segments left in
// /dev/shm by this host's processes.
int RunShm(int argc, char** argv)
{
#ifdef PUPPETMASTER_CONFIG_WITH_SHM
    const std::string action = argv[2];
    if (action != "list" && action != "clean") {
        return Usage(std::cerr);
    }
    std::optional<puppet_master::core::DomainId> domain;
    bool dry_run = false;
    for (int index = 3; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--domain" && index + 1 < argc) {
            const std::string value = argv[++index];
            if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos || value.size() > 9) {
                std::cerr << "--domain must be a number\n";
                return 2;
            }
            domain = static_cast<puppet_master::core::DomainId>(std::stoul(value));
        } else if (argument == "--dry-run" && action == "clean") {
            dry_run = true;
        } else {
            return Usage(std::cerr);
        }
    }

    // A dry run lists what clean would remove: everything not in use.
    auto segments = action == "list" || dry_run ? shm::ListSegments(domain) : shm::RemoveUnusedSegments(domain);
    if (!segments.ok()) {
        std::cerr << segments.status().ToString() << '\n';
        return 1;
    }
    std::size_t shown = 0;
    for (const auto& segment : segments.value()) {
        if (action == "clean" && segment.state == shm::SegmentInfo::State::kInUse) {
            continue;
        }
        ++shown;
        std::cout << (action == "list" ? "" : dry_run ? "would remove " : "removed ") << segment.object_name << "  "
                  << shm::ToString(segment.state);
        if (!segment.topic.empty()) {
            std::cout << "  " << segment.topic << "  " << segment.descriptor.ToString() << "  "
                      << segment.geometry.slot_payload_size << " B x " << segment.geometry.slot_count << " slots, "
                      << segment.messages_written << " written";
        } else if (!segment.problem.empty()) {
            std::cout << "  (" << segment.problem << ")";
        }
        if (!segment.live_pids.empty()) {
            std::cout << "  pids";
            for (const auto pid : segment.live_pids) {
                std::cout << ' ' << pid;
            }
        }
        if (segment.dead_participants != 0) {
            std::cout << "  " << segment.dead_participants << " exited";
        }
        std::cout << '\n';
    }
    if (action == "clean") {
        std::cout << (dry_run ? "would remove " : "removed ") << shown << " segment(s)\n";
    } else if (shown == 0) {
        std::cout << "no segments\n";
    }
    return 0;
#else
    (void)argc;
    (void)argv;
    std::cerr << "puppet_master_config was built without the shared-memory adapter\n";
    return 1;
#endif
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc >= 2 && (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h")) {
        Usage(std::cout);
        return 0;
    }
    if (argc < 3) {
        return Usage(std::cerr);
    }
    const std::string command = argv[1];
    if (command == "shm") {
        return RunShm(argc, argv);
    }
    if (command == "system") {
        if (argc != 4) {
            return Usage(std::cerr);
        }
        const std::string action = argv[2];
        if (action != "validate" && action != "describe" && action != "commands") {
            return Usage(std::cerr);
        }
        return RunSystem(action, argv[3]);
    }
    if (command != "validate" && command != "describe") {
        return Usage(std::cerr);
    }

    bool system = false;
    config::JsonLoadOptions options;
    options.search_paths = config::SearchPathsFromEnvironment();
    std::vector<std::string> files;
    for (int index = 2; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--system" && command == "validate") {
            system = true;
        } else if (argument == "--config-path" || argument == "--deployment") {
            if (index + 1 >= argc) {
                return Usage(std::cerr);
            }
            const std::string value = argv[++index];
            if (argument == "--config-path") {
                options.search_paths.push_back(value);
            } else {
                options.deployment_path = value;
            }
        } else if (!argument.empty() && argument.front() == '-') {
            return Usage(std::cerr);
        } else {
            files.push_back(argument);
        }
    }
    if (files.empty() || (command == "describe" && files.size() != 1)) {
        return Usage(std::cerr);
    }

    if (command == "describe") {
        auto project = config::LoadJsonConfig(files.front(), options);
        if (!project.ok()) {
            std::cerr << project.status().ToString() << '\n';
            return 1;
        }
        std::cout << config::DescribeProject(project.value());
        return 0;
    }

    int failures = 0;
    std::vector<config::ProjectConfig> projects;
    projects.reserve(files.size());
    std::vector<std::string> loaded_files;
    for (const auto& path : files) {
        auto project = config::LoadJsonConfig(path, options);
        if (!project.ok()) {
            std::cerr << project.status().ToString() << '\n';
            ++failures;
            continue;
        }
        const auto report = config::ValidateProject(project.value());
        if (!report.ok()) {
            for (const auto& error : report.errors) {
                std::cerr << path << ": " << error << '\n';
            }
            ++failures;
            continue;
        }
        std::cout << path << ": ok (" << project.value().topics.size() << " topics, "
                  << project.value().components.size() << " components)\n";
        projects.push_back(std::move(project).value());
        loaded_files.push_back(path);
    }

    if (system) {
        if (failures != 0) {
            std::cerr << "system: not checked, " << failures << " file(s) failed\n";
            return 1;
        }
        std::vector<config::NamedProject> named;
        for (std::size_t index = 0; index < projects.size(); ++index) {
            named.push_back(config::NamedProject {loaded_files[index], &projects[index]});
        }
        const auto report = config::ValidateSystem(named);
        PrintReport(report);
        if (!report.ok()) {
            return 1;
        }
        std::cout << "system: ok (" << projects.size() << " processes)\n";
    }
    return failures == 0 ? 0 : 1;
}
