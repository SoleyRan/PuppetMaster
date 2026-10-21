// puppet_master_config: checks and summarizes JSON project configurations
// without building the application, for use in CI and code review.
//
//     puppet_master_config validate config/*.json
//     puppet_master_config describe config/vehicle.json
//
// Component types are not checked here because factories live in the
// application binary; run the application with --validate for that.

#include <iostream>
#include <string>

#include <puppet_master/configuration/application.h>
#include <puppet_master/configuration/json_loader.h>

namespace config = puppet_master::configuration;

namespace {

int Usage(std::ostream& out)
{
    out << "usage: puppet_master_config validate <config.json>...\n"
        << "       puppet_master_config describe <config.json>\n";
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

    if (command == "describe") {
        if (argc != 3) {
            return Usage(std::cerr);
        }
        auto project = config::LoadJsonConfig(argv[2]);
        if (!project.ok()) {
            std::cerr << argv[2] << ": " << project.status().ToString() << '\n';
            return 1;
        }
        std::cout << config::DescribeProject(project.value());
        return 0;
    }

    if (command == "validate") {
        int failures = 0;
        for (int index = 2; index < argc; ++index) {
            const std::string path = argv[index];
            auto project = config::LoadJsonConfig(path);
            if (!project.ok()) {
                std::cerr << path << ": " << project.status().ToString() << '\n';
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
        }
        return failures == 0 ? 0 : 1;
    }

    return Usage(std::cerr);
}
