#include <puppet_master/configuration/json_launcher.h>

#include <iostream>
#include <string>
#include <utility>

#include <puppet_master/configuration/json_loader.h>

namespace puppet_master::configuration {

namespace {

void PrintUsage(std::ostream& out, const char* program)
{
    out << "usage: " << program << " [--validate | --describe] [options] <config.json>\n"
        << "  (no flag)            run until SIGINT or SIGTERM\n"
        << "  --validate           check the configuration and component types, then exit\n"
        << "  --describe           print the topics, components, and triggers, then exit\n"
        << "  --config-path DIR    also look for imports in DIR (repeatable; searched after\n"
        << "                       PUPPETMASTER_CONFIG_PATH)\n"
        << "  --deployment FILE    apply a deployment file: per-topic transport and settings\n";
}

}  // namespace

int RunJsonApplication(int argc, char** argv, const ComponentFactoryRegistry& factories,
                       ApplicationOptions options)
{
    const char* program = argc > 0 ? argv[0] : "puppet_master_app";
    std::string mode;
    std::string path;
    JsonLoadOptions load_options;
    load_options.search_paths = SearchPathsFromEnvironment();
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--help" || argument == "-h") {
            PrintUsage(std::cout, program);
            return 0;
        }
        if (argument == "--validate" || argument == "--describe") {
            if (!mode.empty()) {
                PrintUsage(std::cerr, program);
                return 2;
            }
            mode = argument;
        } else if (argument == "--config-path" || argument == "--deployment") {
            if (index + 1 >= argc) {
                PrintUsage(std::cerr, program);
                return 2;
            }
            const std::string value = argv[++index];
            if (argument == "--config-path") {
                load_options.search_paths.push_back(value);
            } else if (load_options.deployment_path.empty()) {
                load_options.deployment_path = value;
            } else {
                PrintUsage(std::cerr, program);
                return 2;
            }
        } else if (path.empty() && !argument.empty() && argument.front() != '-') {
            path = argument;
        } else {
            PrintUsage(std::cerr, program);
            return 2;
        }
    }
    if (path.empty()) {
        PrintUsage(std::cerr, program);
        return 2;
    }

    // Errors from LoadJsonConfig name the file they come from.
    auto project = LoadJsonConfig(path, load_options);
    if (!project.ok()) {
        std::cerr << project.status().ToString() << '\n';
        return 1;
    }

    if (mode == "--describe") {
        std::cout << DescribeProject(project.value());
        return 0;
    }

    const auto report = ValidateProject(project.value(), &factories);
    if (!report.ok()) {
        for (const auto& error : report.errors) {
            std::cerr << path << ": " << error << '\n';
        }
        return 1;
    }
    if (mode == "--validate") {
        std::cout << path << ": ok (" << project.value().topics.size() << " topics, "
                  << project.value().components.size() << " components)\n";
        return 0;
    }

    auto application = Application::Create(std::move(project).value(), factories, std::move(options));
    if (!application.ok()) {
        std::cerr << application.status().ToString() << '\n';
        return 1;
    }
    auto status = application.value()->InstallSignalHandlers();
    if (!status.ok()) {
        std::cerr << status.ToString() << '\n';
        return 1;
    }
    status = application.value()->Run();
    if (!status.ok()) {
        std::cerr << status.ToString() << '\n';
        return 1;
    }
    return 0;
}

}  // namespace puppet_master::configuration
