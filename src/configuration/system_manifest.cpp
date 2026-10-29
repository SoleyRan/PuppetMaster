#include <puppet_master/configuration/system_manifest.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

namespace puppet_master::configuration {

namespace {

using Json = nlohmann::json;
namespace fs = std::filesystem;

[[noreturn]] void Fail(const std::string& message)
{
    throw std::invalid_argument(message);
}

void CheckFields(const Json& value, const std::string& path, std::initializer_list<std::string_view> fields)
{
    if (!value.is_object()) {
        Fail(path + " must be an object");
    }
    for (auto it = value.begin(); it != value.end(); ++it) {
        if (std::find(fields.begin(), fields.end(), it.key()) == fields.end()) {
            Fail(path + "." + it.key() + " is unknown");
        }
    }
}

// A non-negative millisecond count, at most a day; leaves `output` alone if
// the field is absent.
void ReadMilliseconds(const Json& object, const char* key, const std::string& path,
                      std::chrono::milliseconds& output)
{
    if (!object.contains(key)) {
        return;
    }
    const auto& value = object.at(key);
    constexpr std::int64_t kDay = 24LL * 60 * 60 * 1000;
    if (!value.is_number_integer() || value.get<std::int64_t>() < 0 || value.get<std::int64_t>() > kDay) {
        Fail(path + "." + key + " must be a non-negative integer of milliseconds, at most a day");
    }
    output = std::chrono::milliseconds(value.get<std::int64_t>());
}

std::string GetEnvironment(const std::string& name, bool& found)
{
#ifdef _WIN32
    char* value = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&value, &length, name.c_str()) != 0 || value == nullptr) {
        found = false;
        return {};
    }
    std::string text(value);
    std::free(value);
    found = true;
    return text;
#else
    const char* value = std::getenv(name.c_str());
    found = value != nullptr;
    return found ? std::string(value) : std::string();
#endif
}

// Replaces ${NAME} with the environment variable NAME, so one manifest can
// point at an install prefix chosen when the system is deployed. An unset
// variable is an error rather than an empty string, which would silently
// turn "${PREFIX}/bin" into "/bin".
std::string Expand(const std::string& text, const std::string& path)
{
    std::string result;
    std::size_t position = 0;
    while (true) {
        const auto start = text.find("${", position);
        if (start == std::string::npos) {
            result.append(text, position, std::string::npos);
            return result;
        }
        const auto end = text.find('}', start + 2);
        if (end == std::string::npos) {
            Fail(path + ": unterminated ${ in \"" + text + "\"");
        }
        const auto name = text.substr(start + 2, end - start - 2);
        if (name.empty()) {
            Fail(path + ": empty variable name in \"" + text + "\"");
        }
        bool found = false;
        const auto value = GetEnvironment(name, found);
        if (!found) {
            Fail(path + ": environment variable " + name + " is not set");
        }
        result.append(text, position, start - position);
        result += value;
        position = end + 1;
    }
}

std::string String(const Json& object, const char* key, const std::string& path, bool required)
{
    const auto found = object.find(key);
    if (found == object.end()) {
        if (required) {
            Fail(path + "." + key + " is required");
        }
        return {};
    }
    if (!found->is_string()) {
        Fail(path + "." + key + " must be a string");
    }
    return Expand(found->get<std::string>(), path + "." + key);
}

std::vector<std::string> Strings(const Json& object, const char* key, const std::string& path)
{
    std::vector<std::string> values;
    const auto found = object.find(key);
    if (found == object.end()) {
        return values;
    }
    if (!found->is_array()) {
        Fail(path + "." + key + " must be an array of strings");
    }
    std::size_t index = 0;
    for (const auto& item : *found) {
        const auto item_path = path + "." + key + "[" + std::to_string(index++) + "]";
        if (!item.is_string()) {
            Fail(item_path + " must be a string");
        }
        values.push_back(Expand(item.get<std::string>(), item_path));
    }
    return values;
}

std::string Absolute(const fs::path& base, const std::string& value)
{
    const fs::path path(value);
    return (path.is_absolute() ? path : base / path).lexically_normal().string();
}

bool IsFile(const fs::path& path)
{
    std::error_code error;
    return fs::is_regular_file(path, error);
}

std::string ResolveExecutable(const SystemManifest& manifest, const fs::path& base, const std::string& value)
{
    const fs::path path(value);
    if (path.is_absolute()) {
        return value;
    }
    if (path.has_parent_path()) {
        return Absolute(base, value);  // "bin/node" means relative to the manifest
    }
    for (const auto& directory : manifest.bin_paths) {
        for (const char* suffix : {"", ".exe"}) {
            const auto candidate = fs::path(directory) / (value + suffix);
            if (IsFile(candidate)) {
                return candidate.lexically_normal().string();
            }
        }
    }
    return value;  // left to PATH when started
}

std::string ResolveConfig(const SystemManifest& manifest, const fs::path& base, const NodeManifest& node,
                          const std::string& path)
{
    const fs::path config(node.config);
    std::vector<fs::path> candidates;
    if (config.is_absolute()) {
        candidates.push_back(config);
    } else {
        candidates.push_back(base / config);
        for (const auto& directory : manifest.config_paths) {
            candidates.push_back(fs::path(directory) / config);
        }
    }
    for (const auto& candidate : candidates) {
        if (IsFile(candidate)) {
            return candidate.lexically_normal().string();
        }
    }
    std::string searched;
    for (const auto& candidate : candidates) {
        searched += (searched.empty() ? "" : ", ") + candidate.parent_path().string();
    }
    Fail(path + ": config " + node.config + " of node " + node.name + " not found (searched " + searched + ")");
}

SystemManifest Decode(const Json& root, const fs::path& file)
{
    CheckFields(root, "$", {"name", "config_paths", "bin_paths", "deployment", "nodes"});
    const auto base = file.parent_path();
    SystemManifest manifest;
    manifest.path = file.lexically_normal().string();
    manifest.name = String(root, "name", "$", false);
    for (const auto& entry : Strings(root, "config_paths", "$")) {
        manifest.config_paths.push_back(Absolute(base, entry));
    }
    for (const auto& entry : Strings(root, "bin_paths", "$")) {
        manifest.bin_paths.push_back(Absolute(base, entry));
    }
    const auto deployment = String(root, "deployment", "$", false);
    if (!deployment.empty()) {
        manifest.deployment = Absolute(base, deployment);
        if (!IsFile(manifest.deployment)) {
            Fail("$.deployment: " + manifest.deployment + " not found");
        }
    }

    const auto nodes = root.find("nodes");
    if (nodes == root.end() || !nodes->is_array() || nodes->empty()) {
        Fail("$.nodes must be a non-empty array");
    }
    std::set<std::string> names;
    std::size_t index = 0;
    for (const auto& value : *nodes) {
        const auto path = "$.nodes[" + std::to_string(index++) + "]";
        CheckFields(value, path, {"name", "executable", "config", "arguments", "start_delay_ms", "restart",
                                  "max_restarts", "restart_delay_ms", "required", "stop_timeout_ms"});
        NodeManifest node;
        node.name = String(value, "name", path, true);
        auto status = core::ValidateName("node name", node.name);
        if (!status.ok()) {
            Fail(path + ".name: " + status.message());
        }
        if (!names.insert(node.name).second) {
            Fail(path + ": node name " + node.name + " is duplicated");
        }
        node.executable = String(value, "executable", path, true);
        node.config = String(value, "config", path, true);
        node.arguments = Strings(value, "arguments", path);
        for (const auto& argument : node.arguments) {
            // Tools print commands tab-separated, one node per line.
            if (argument.find_first_of("\t\n\r") != std::string::npos) {
                Fail(path + ".arguments: arguments must not contain tabs or line breaks");
            }
        }
        ReadMilliseconds(value, "start_delay_ms", path, node.start_delay);
        ReadMilliseconds(value, "restart_delay_ms", path, node.restart_delay);
        ReadMilliseconds(value, "stop_timeout_ms", path, node.stop_timeout);
        if (value.contains("restart")) {
            const auto& restart = value.at("restart");
            const std::string text = restart.is_string() ? restart.get<std::string>() : std::string();
            if (text == "never") {
                node.restart = NodeManifest::Restart::kNever;
            } else if (text == "on_failure") {
                node.restart = NodeManifest::Restart::kOnFailure;
            } else if (text == "always") {
                node.restart = NodeManifest::Restart::kAlways;
            } else {
                Fail(path + ".restart must be \"never\", \"on_failure\", or \"always\"");
            }
        }
        if (value.contains("max_restarts")) {
            const auto& count = value.at("max_restarts");
            if (!count.is_number_integer() || count.get<std::int64_t>() < 0
                || count.get<std::int64_t>() > 1000000) {
                Fail(path + ".max_restarts must be a non-negative integer");
            }
            node.max_restarts = static_cast<std::uint32_t>(count.get<std::int64_t>());
        }
        if (value.contains("required")) {
            if (!value.at("required").is_boolean()) {
                Fail(path + ".required must be true or false");
            }
            node.required = value.at("required").get<bool>();
        }
        node.executable = ResolveExecutable(manifest, base, node.executable);
        node.config = ResolveConfig(manifest, base, node, path);
        manifest.nodes.push_back(std::move(node));
    }
    return manifest;
}

}  // namespace

JsonLoadOptions SystemManifest::LoadOptions() const
{
    // The same order a launcher uses: the environment, then --config-path.
    JsonLoadOptions options;
    options.search_paths = SearchPathsFromEnvironment();
    options.search_paths.insert(options.search_paths.end(), config_paths.begin(), config_paths.end());
    options.deployment_path = deployment;
    return options;
}

std::vector<std::string> SystemManifest::CommandFor(const NodeManifest& node) const
{
    std::vector<std::string> command {node.executable};
    for (const auto& directory : config_paths) {
        command.push_back("--config-path");
        command.push_back(directory);
    }
    if (!deployment.empty()) {
        command.push_back("--deployment");
        command.push_back(deployment);
    }
    command.insert(command.end(), node.arguments.begin(), node.arguments.end());
    command.push_back(node.config);
    return command;
}

const char* ToString(NodeManifest::Restart restart) noexcept
{
    switch (restart) {
        case NodeManifest::Restart::kNever:
            return "never";
        case NodeManifest::Restart::kOnFailure:
            return "on_failure";
        case NodeManifest::Restart::kAlways:
            return "always";
    }
    return "unknown";
}

core::Result<SystemManifest> LoadSystemManifest(const std::string& path)
{
    using ResultType = core::Result<SystemManifest>;
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        return ResultType::FromStatus(core::Status::Unavailable("cannot open system manifest: " + path));
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    try {
        const auto file = fs::absolute(fs::path(path));
        return ResultType(Decode(Json::parse(buffer.str()), file));
    } catch (const Json::exception& error) {
        return ResultType::FromStatus(core::Status::InvalidArgument(path + ": " + error.what()));
    } catch (const std::invalid_argument& error) {
        return ResultType::FromStatus(core::Status::InvalidArgument(path + ": " + error.what()));
    } catch (const fs::filesystem_error& error) {
        return ResultType::FromStatus(core::Status::Unavailable(path + ": " + error.what()));
    }
}

core::Result<std::vector<LoadedNode>> LoadSystemNodes(const SystemManifest& manifest)
{
    using ResultType = core::Result<std::vector<LoadedNode>>;
    const auto options = manifest.LoadOptions();
    std::vector<LoadedNode> nodes;
    nodes.reserve(manifest.nodes.size());
    for (const auto& node : manifest.nodes) {
        auto project = LoadJsonConfig(node.config, options);
        if (!project.ok()) {
            return ResultType::FromStatus(core::Status::Error(
                project.status().code(), "node " + node.name + ": " + project.status().message()));
        }
        nodes.push_back(LoadedNode {node.name, std::move(project).value()});
    }
    return ResultType(std::move(nodes));
}

core::Result<SystemReport> ValidateSystemManifest(const SystemManifest& manifest)
{
    auto nodes = LoadSystemNodes(manifest);
    if (!nodes.ok()) {
        return core::Result<SystemReport>::FromStatus(nodes.status());
    }
    std::vector<NamedProject> named;
    named.reserve(nodes.value().size());
    for (const auto& node : nodes.value()) {
        named.push_back(NamedProject {node.name, &node.project});
    }
    return core::Result<SystemReport>(ValidateSystem(named));
}

}  // namespace puppet_master::configuration
