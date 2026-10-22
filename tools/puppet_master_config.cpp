// puppet_master_config: checks and summarizes JSON project configurations
// without building the application, for use in CI and code review.
//
//     puppet_master_config validate config/*.json
//     puppet_master_config validate --system sensors.json estimator.json monitor.json
//     puppet_master_config describe config/vehicle.json
//
// Component types are not checked here because factories live in the
// application binary; run the application with --validate for that.

#include <iostream>
#include <string>
#include <vector>

#include <puppet_master/configuration/application.h>
#include <puppet_master/configuration/json_loader.h>

namespace config = puppet_master::configuration;

namespace {

int Usage(std::ostream& out)
{
    out << "usage: puppet_master_config validate [--system] [options] <config.json>...\n"
        << "       puppet_master_config describe [options] <config.json>\n"
        << "  --system           also check the files together as one system: matching\n"
        << "                     topic declarations, a writer for every read topic,\n"
        << "                     one writer per topic\n"
        << "  --config-path DIR  also look for imports in DIR (repeatable; searched after\n"
        << "                     PUPPETMASTER_CONFIG_PATH)\n"
        << "  --deployment FILE  apply a deployment file to every configuration\n";
    return 2;
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
        for (const auto& warning : report.warnings) {
            std::cerr << "system: warning: " << warning << '\n';
        }
        for (const auto& error : report.errors) {
            std::cerr << "system: " << error << '\n';
        }
        if (!report.ok()) {
            return 1;
        }
        std::cout << "system: ok (" << projects.size() << " processes)\n";
    }
    return failures == 0 ? 0 : 1;
}
