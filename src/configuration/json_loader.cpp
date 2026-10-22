#include <puppet_master/configuration/json_loader.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace puppet_master::configuration {
namespace {

using Json = nlohmann::json;

void CheckObject(const Json& value, const std::string& path,
                 std::initializer_list<std::string_view> fields)
{
    if (!value.is_object()) {
        throw std::invalid_argument(path + " must be an object");
    }
    for (auto it = value.begin(); it != value.end(); ++it) {
        if (std::find(fields.begin(), fields.end(), it.key()) == fields.end()) {
            throw std::invalid_argument(path + "." + it.key() + " is unknown");
        }
    }
}

template <typename T>
void Read(const Json& object, const char* key, T& output, const std::string& path)
{
    auto found = object.find(key);
    if (found == object.end()) {
        return;
    }
    const auto field = path + "." + key;
    if constexpr (std::is_same_v<T, std::string>) {
        if (!found->is_string()) {
            throw std::invalid_argument(field + " must be a string");
        }
    } else if constexpr (std::is_same_v<T, bool>) {
        if (!found->is_boolean()) {
            throw std::invalid_argument(field + " must be a boolean");
        }
    } else if constexpr (std::is_integral_v<T>) {
        if (!found->is_number_integer()) {
            throw std::invalid_argument(field + " must be an integer");
        }
        if (found->is_number_unsigned()) {
            if constexpr (std::is_signed_v<T>) {
                if (found->get<std::uint64_t>() > static_cast<std::uint64_t>(std::numeric_limits<T>::max())) {
                    throw std::invalid_argument(field + " is out of range");
                }
            } else if (found->get<std::uint64_t>() > std::numeric_limits<T>::max()) {
                throw std::invalid_argument(field + " is out of range");
            }
        } else {
            const auto number = found->get<std::int64_t>();
            if constexpr (std::is_unsigned_v<T>) {
                if (number < 0 || static_cast<std::uint64_t>(number) > std::numeric_limits<T>::max()) {
                    throw std::invalid_argument(field + " is out of range");
                }
            } else if (number < std::numeric_limits<T>::min() || number > std::numeric_limits<T>::max()) {
                throw std::invalid_argument(field + " is out of range");
            }
        }
    } else {
        if (!found->is_array() || !std::all_of(found->begin(), found->end(),
                [](const Json& item) { return item.is_string(); })) {
            throw std::invalid_argument(field + " must be an array of strings");
        }
    }
    output = found->get<T>();
}

template <typename T>
void ReadEnum(const Json& object, const char* key, T& output,
              core::Result<T> (*parse)(std::string_view), const std::string& path)
{
    if (!object.contains(key)) {
        return;
    }
    std::string text;
    Read(object, key, text, path);
    auto result = parse(text);
    if (!result.ok()) {
        throw std::invalid_argument(path + "." + key + ": " + result.status().message());
    }
    output = result.value();
}

const Json& Array(const Json& object, const char* key, const std::string& path)
{
    static const Json empty = Json::array();
    const auto found = object.find(key);
    if (found == object.end()) {
        return empty;
    }
    if (!found->is_array()) {
        throw std::invalid_argument(path + "." + key + " must be an array");
    }
    return *found;
}

// An object of scalar settings, stored as text; see runtime/parameters.h.
runtime::ParameterMap ReadParameters(const Json& object, const std::string& path, const char* key = "parameters")
{
    runtime::ParameterMap parameters;
    const auto found = object.find(key);
    if (found == object.end()) {
        return parameters;
    }
    const auto field = path + "." + key;
    if (!found->is_object()) {
        throw std::invalid_argument(field + " must be an object");
    }
    for (auto it = found->begin(); it != found->end(); ++it) {
        if (it->is_string()) {
            parameters.emplace(it.key(), it->get<std::string>());
        } else if (it->is_number() || it->is_boolean()) {
            parameters.emplace(it.key(), it->dump());
        } else {
            throw std::invalid_argument(field + "." + it.key() + " must be a string, number, or boolean");
        }
    }
    return parameters;
}

// Fields of a message_policy object; `policy` keeps the values of fields that
// are absent.
void ReadPolicy(const Json& value, core::MessagePolicy& policy, const std::string& path)
{
    CheckObject(value, path, {"delivery", "retention", "freshness", "overflow", "queue_depth"});
    ReadEnum(value, "delivery", policy.delivery, ParseDeliveryGuarantee, path);
    ReadEnum(value, "retention", policy.retention, ParseRetentionPolicy, path);
    ReadEnum(value, "freshness", policy.freshness, ParseFreshnessPolicy, path);
    ReadEnum(value, "overflow", policy.overflow, ParseQueueOverflowPolicy, path);
    Read(value, "queue_depth", policy.queue_depth, path);
}

TopicConfig DecodeTopic(const Json& value, const std::string& path)
{
    CheckObject(value, path, {"id", "name", "transport", "message_type", "message_version", "encoding",
                              "message_policy"});
    TopicConfig topic;
    Read(value, "id", topic.id, path);
    Read(value, "name", topic.name, path);
    Read(value, "message_type", topic.message_type, path);
    Read(value, "message_version", topic.message_version, path);
    Read(value, "encoding", topic.encoding, path);
    ReadEnum(value, "transport", topic.transport, ParseTransportKind, path);
    if (value.contains("message_policy")) {
        ReadPolicy(value.at("message_policy"), topic.message_policy, path + ".message_policy");
    }
    return topic;
}

// A reader is a topic id, or an object naming the topic with policy fields
// that apply to this reader only, starting from the topic's own policy.
void DecodeReaders(const Json& component, const std::string& path, const ProjectConfig& project,
                   ComponentConfig& output)
{
    std::size_t index = 0;
    for (const auto& entry : Array(component, "readers", path)) {
        const auto entry_path = path + ".readers[" + std::to_string(index++) + "]";
        if (entry.is_string()) {
            output.readers.push_back(entry.get<std::string>());
            continue;
        }
        if (!entry.is_object()) {
            throw std::invalid_argument(entry_path + " must be a topic id or an object");
        }
        CheckObject(entry, entry_path, {"topic", "delivery", "retention", "freshness", "overflow", "queue_depth"});
        std::string topic_id;
        Read(entry, "topic", topic_id, entry_path);
        if (topic_id.empty()) {
            throw std::invalid_argument(entry_path + ".topic is required");
        }
        auto topic = project.FindTopic(topic_id);
        if (!topic.ok()) {
            throw std::invalid_argument(entry_path + ": " + topic.status().message());
        }
        auto policy = topic.value().message_policy;
        Json fields = entry;
        fields.erase("topic");
        ReadPolicy(fields, policy, entry_path);
        output.readers.push_back(topic_id);
        if (!output.reader_policies.emplace(topic_id, policy).second) {
            throw std::invalid_argument(entry_path + ": topic " + topic_id + " is read twice");
        }
    }
}

ComponentConfig DecodeComponent(const Json& value, const std::string& path, const ProjectConfig& project)
{
    CheckObject(value, path, {"name", "description", "readers", "writers", "triggers", "type", "parameters"});
    ComponentConfig component;
    Read(value, "name", component.name, path);
    Read(value, "type", component.type, path);
    Read(value, "description", component.description, path);
    DecodeReaders(value, path, project, component);
    Read(value, "writers", component.writers, path);
    component.parameters = ReadParameters(value, path);
    std::size_t trigger_index = 0;
    for (const auto& entry : Array(value, "triggers", path)) {
        const auto trigger_path = path + ".triggers[" + std::to_string(trigger_index++) + "]";
        CheckObject(entry, trigger_path, {"kind", "period_ns", "dependency_policy", "data_topics",
                                          "task_dependencies", "priority", "deadline_ns"});
        TriggerConfig trigger;
        ReadEnum(entry, "kind", trigger.kind, ParseTriggerKind, trigger_path);
        auto period = trigger.period.count();
        Read(entry, "period_ns", period, trigger_path);
        trigger.period = core::Nanoseconds(period);
        Read(entry, "priority", trigger.priority, trigger_path);
        auto deadline = trigger.deadline.count();
        Read(entry, "deadline_ns", deadline, trigger_path);
        trigger.deadline = core::Nanoseconds(deadline);
        ReadEnum(entry, "dependency_policy", trigger.dependency_policy, ParseDependencyPolicy, trigger_path);
        Read(entry, "data_topics", trigger.data_topics, trigger_path);
        Read(entry, "task_dependencies", trigger.task_dependencies, trigger_path);
        component.triggers.push_back(std::move(trigger));
    }
    return component;
}

// Errors carry the file they came from.
[[noreturn]] void Fail(const std::string& file, const std::string& message)
{
    throw std::invalid_argument(file.empty() ? message : file + ": " + message);
}

// A configuration file that cannot be opened or read; Unavailable to callers.
struct UnreadableFile : std::runtime_error {
    using std::runtime_error::runtime_error;
};

Json ReadJsonFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        throw UnreadableFile("cannot open configuration file: " + path.string());
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    if (input.bad()) {
        throw UnreadableFile("cannot read configuration file: " + path.string());
    }
    try {
        return Json::parse(buffer.str());
    } catch (const Json::exception& error) {
        Fail(path.string(), error.what());
    }
}

// Resolves and decodes imports depth-first, so topics land in declaration
// order. Tracks the files on the current chain to reject cycles.
class ImportResolver {
public:
    explicit ImportResolver(const JsonLoadOptions& options) : options_(options) {}

    // Appends the topics of every file `root` (read from `file`) imports.
    void ImportInto(const Json& root, const std::filesystem::path& file, ProjectConfig& project,
                    std::map<std::string, std::string>& origins)
    {
        std::vector<std::string> imports;
        Read(root, "imports", imports, "$");
        for (const auto& name : imports) {
            const auto resolved = Resolve(name, file);
            const auto key = std::filesystem::weakly_canonical(resolved).string();
            if (std::find(chain_.begin(), chain_.end(), key) != chain_.end()) {
                Fail(file.string(), "import cycle through " + resolved.string());
            }
            if (!loaded_.insert(key).second) {
                continue;  // reached through another import already
            }
            const auto imported = ReadJsonFile(resolved);
            try {
                CheckObject(imported, "$", {"imports", "topics"});
            } catch (const std::invalid_argument& error) {
                Fail(resolved.string(), std::string(error.what()) + " (an imported file may only declare topics)");
            }
            chain_.push_back(key);
            ImportInto(imported, resolved, project, origins);
            AppendTopics(imported, resolved.string(), project, origins);
            chain_.pop_back();
        }
    }

    void Enter(const std::filesystem::path& file)
    {
        const auto key = std::filesystem::weakly_canonical(file).string();
        chain_.push_back(key);
        loaded_.insert(key);
    }

    static void AppendTopics(const Json& root, const std::string& file, ProjectConfig& project,
                             std::map<std::string, std::string>& origins)
    {
        std::size_t index = 0;
        for (const auto& value : Array(root, "topics", "$")) {
            const auto path = "$.topics[" + std::to_string(index++) + "]";
            TopicConfig topic;
            try {
                topic = DecodeTopic(value, path);
            } catch (const std::invalid_argument& error) {
                Fail(file, error.what());
            }
            const auto inserted = origins.emplace(topic.id, file);
            if (!inserted.second) {
                const auto& first = inserted.first->second;
                Fail(file, "topic id " + topic.id + " is already declared"
                               + (first.empty() || first == file ? std::string() : " in " + first));
            }
            project.topics.push_back(std::move(topic));
        }
    }

private:
    std::filesystem::path Resolve(const std::string& name, const std::filesystem::path& from) const
    {
        const std::filesystem::path relative(name);
        if (relative.is_absolute()) {
            if (std::filesystem::exists(relative)) {
                return relative;
            }
            Fail(from.string(), "import not found: " + name);
        }
        std::vector<std::filesystem::path> candidates {from.parent_path() / relative};
        for (const auto& directory : options_.search_paths) {
            candidates.push_back(std::filesystem::path(directory) / relative);
        }
        for (const auto& candidate : candidates) {
            if (std::filesystem::is_regular_file(candidate)) {
                return candidate;
            }
        }
        std::string searched;
        for (const auto& candidate : candidates) {
            searched += (searched.empty() ? "" : ", ") + candidate.parent_path().string();
        }
        Fail(from.string(), "import not found: " + name + " (searched " + searched
                                + "; add directories with PUPPETMASTER_CONFIG_PATH or --config-path)");
    }

    const JsonLoadOptions& options_;
    std::vector<std::string> chain_;
    std::set<std::string> loaded_;
};

void DecodeProjectSettings(const Json& root, ProjectConfig& project)
{
    if (root.contains("runtime")) {
        const auto& options = root.at("runtime");
        CheckObject(options, "$.runtime", {"default_transport_name", "register_inmemory_transport",
                                         "open_registered_transports"});
        std::string name = project.runtime_options.default_transport_name.str();
        Read(options, "default_transport_name", name, "$.runtime");
        auto parsed_name = core::TransportName::Create(std::move(name));
        if (!parsed_name.ok()) {
            throw std::invalid_argument("$.runtime.default_transport_name: " + parsed_name.status().message());
        }
        project.runtime_options.default_transport_name = std::move(parsed_name).value();
        Read(options, "register_inmemory_transport", project.runtime_options.register_inmemory_transport, "$.runtime");
        Read(options, "open_registered_transports", project.runtime_options.open_registered_transports, "$.runtime");
    }
    if (root.contains("scheduler")) {
        const auto& options = root.at("scheduler");
        CheckObject(options, "$.scheduler", {"worker_count", "priority_aging_interval_ns"});
        Read(options, "worker_count", project.scheduler_options.worker_count, "$.scheduler");
        auto aging = project.scheduler_options.priority_aging_interval.count();
        Read(options, "priority_aging_interval_ns", aging, "$.scheduler");
        project.scheduler_options.priority_aging_interval = core::Nanoseconds(aging);
    }
}

// Decodes one project document. `resolver` is null for text without a file,
// which cannot have imports.
ProjectConfig Decode(const Json& root, const std::filesystem::path& file, ImportResolver* resolver)
{
    CheckObject(root, "$", {"imports", "runtime", "scheduler", "topics", "components"});
    ProjectConfig project;
    DecodeProjectSettings(root, project);
    std::map<std::string, std::string> origins;
    if (root.contains("imports")) {
        if (resolver == nullptr) {
            throw std::invalid_argument("$.imports needs a configuration file to resolve against; use LoadJsonConfig");
        }
        resolver->ImportInto(root, file, project, origins);
    }
    ImportResolver::AppendTopics(root, file.string(), project, origins);
    std::size_t index = 0;
    for (const auto& value : Array(root, "components", "$")) {
        project.components.push_back(DecodeComponent(value, "$.components[" + std::to_string(index++) + "]", project));
    }
    return project;
}

// A deployment file decides where topics run: their transport and transport
// parameters. It cannot change what a topic is, so a deployment that names a
// different type or version is an error, not an override.
void ApplyDeployment(const std::filesystem::path& file, ProjectConfig& project)
{
    const auto root = ReadJsonFile(file);
    try {
        CheckObject(root, "$", {"topics", "defaults"});
        if (root.contains("defaults")) {
            const auto& defaults = root.at("defaults");
            CheckObject(defaults, "$.defaults", {"transport", "transport_parameters"});
            for (auto& topic : project.topics) {
                ReadEnum(defaults, "transport", topic.transport, ParseTransportKind, "$.defaults");
                for (const auto& [key, value] : ReadParameters(defaults, "$.defaults", "transport_parameters")) {
                    topic.transport_parameters[key] = value;
                }
            }
        }
        std::size_t index = 0;
        for (const auto& value : Array(root, "topics", "$")) {
            const auto path = "$.topics[" + std::to_string(index++) + "]";
            CheckObject(value, path, {"name", "transport", "transport_parameters"});
            std::string name;
            Read(value, "name", name, path);
            if (name.empty()) {
                throw std::invalid_argument(path + ".name is required");
            }
            // Matched by wire name, which is what the interface package
            // publishes; topic ids are local to each project file. Topics this
            // project does not use are ignored, so one deployment serves every
            // process.
            for (auto& topic : project.topics) {
                if (topic.name != name) {
                    continue;
                }
                ReadEnum(value, "transport", topic.transport, ParseTransportKind, path);
                for (const auto& [key, parameter] : ReadParameters(value, path, "transport_parameters")) {
                    topic.transport_parameters[key] = parameter;
                }
            }
        }
    } catch (const std::invalid_argument& error) {
        Fail(file.string(), error.what());
    }
}

core::Result<ProjectConfig> Validated(ProjectConfig project, const std::string& file)
{
    auto status = project.Validate();
    if (!status.ok()) {
        return core::Result<ProjectConfig>::FromStatus(
            file.empty() ? std::move(status) : core::Status::Error(status.code(), file + ": " + status.message()));
    }
    return project;
}

}  // namespace

std::vector<std::string> SearchPathsFromEnvironment()
{
#ifdef _WIN32
    constexpr char kSeparator = ';';
    char* value = nullptr;
    std::size_t length = 0;
    std::string text;
    if (_dupenv_s(&value, &length, "PUPPETMASTER_CONFIG_PATH") == 0 && value != nullptr) {
        text = value;
        std::free(value);
    }
#else
    constexpr char kSeparator = ':';
    const char* value = std::getenv("PUPPETMASTER_CONFIG_PATH");
    const std::string text = value == nullptr ? std::string() : std::string(value);
#endif
    std::vector<std::string> paths;
    std::size_t start = 0;
    while (start <= text.size()) {
        const auto end = std::min(text.find(kSeparator, start), text.size());
        if (end > start) {
            paths.push_back(text.substr(start, end - start));
        }
        start = end + 1;
    }
    return paths;
}

core::Result<ProjectConfig> ParseJsonConfig(std::string_view json)
{
    try {
        return Validated(Decode(Json::parse(json.begin(), json.end()), {}, nullptr), {});
    } catch (const Json::exception& error) {
        return core::Result<ProjectConfig>::FromStatus(core::Status::InvalidArgument(error.what()));
    } catch (const std::invalid_argument& error) {
        return core::Result<ProjectConfig>::FromStatus(core::Status::InvalidArgument(error.what()));
    }
}

core::Result<ProjectConfig> LoadJsonConfig(const std::string& path)
{
    JsonLoadOptions options;
    options.search_paths = SearchPathsFromEnvironment();
    return LoadJsonConfig(path, options);
}

core::Result<ProjectConfig> LoadJsonConfig(const std::string& path, const JsonLoadOptions& options)
{
    try {
        const std::filesystem::path file(path);
        const auto root = ReadJsonFile(file);
        ImportResolver resolver(options);
        resolver.Enter(file);
        ProjectConfig project;
        try {
            project = Decode(root, file, &resolver);
        } catch (const std::invalid_argument& error) {
            // Errors from imported files already name their file.
            const std::string message = error.what();
            if (message.rfind("$", 0) == 0) {
                Fail(path, message);
            }
            throw;
        }
        if (!options.deployment_path.empty()) {
            ApplyDeployment(options.deployment_path, project);
        }
        return Validated(std::move(project), path);
    } catch (const UnreadableFile& error) {
        return core::Result<ProjectConfig>::FromStatus(core::Status::Unavailable(error.what()));
    } catch (const std::invalid_argument& error) {
        return core::Result<ProjectConfig>::FromStatus(core::Status::InvalidArgument(error.what()));
    } catch (const Json::exception& error) {
        return core::Result<ProjectConfig>::FromStatus(core::Status::InvalidArgument(error.what()));
    } catch (const std::filesystem::filesystem_error& error) {
        return core::Result<ProjectConfig>::FromStatus(core::Status::Unavailable(error.what()));
    }
}

}  // namespace puppet_master::configuration
