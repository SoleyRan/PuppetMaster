// puppet_master_topic: looks at the shared-memory topics of a running system
// without changing it.
//
//     puppet_master_topic list
//     puppet_master_topic echo /vehicle/state --count 5
//     puppet_master_topic hz /vehicle/state
//     puppet_master_topic echo --system system.json /vehicle/state
//
// echo and hz subscribe like any other reader: they never slow a writer
// down, and what they miss when they fall behind is overwritten in the ring
// and reported as lost. A topic's type and ring size come from its segment
// (Linux only), or with --system from the manifest's nodes and deployment,
// which also works on Windows and before the writer starts.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <puppet_master/configuration/configuration.h>
#include <puppet_master/configuration/system_manifest.h>
#include <puppet_master/transport/shm/shm_transport.h>

namespace config = puppet_master::configuration;
namespace core = puppet_master::core;
namespace shm = puppet_master::transport::shm;
namespace transport = puppet_master::transport;

namespace {

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

// Reads wait this long at most, so a stop or a deadline is noticed.
constexpr milliseconds kReadSlice {100};

std::atomic<bool> g_stop {false};

void HandleStop(int)
{
    g_stop.store(true);
}

int Usage(std::ostream& out)
{
    out << "usage: puppet_master_topic list [--system FILE] [--domain N]\n"
        << "       puppet_master_topic echo <topic> [--system FILE] [--domain N] [--count N]\n"
        << "                                [--timeout-ms N] [--bytes N] [--text]\n"
        << "       puppet_master_topic hz <topic> [--system FILE] [--domain N] [--window-ms N]\n"
        << "                              [--duration-ms N]\n"
        << "  list            the topics with a shared-memory segment on this host (Linux);\n"
        << "                  with --system, the system's topics and who writes and reads them\n"
        << "  echo            print each message: sequence, size, latency, and its bytes\n"
        << "  hz              print the message rate, intervals, and losses every window\n"
        << "  --system FILE   take topics from a system manifest instead of segment headers\n"
        << "  --domain N      the shared-memory domain (default: any, or the deployment's)\n"
        << "  --count N       echo: stop after N messages\n"
        << "  --timeout-ms N  echo: stop after N ms; fails if --count was not reached\n"
        << "  --bytes N       echo: show at most N bytes of each message in hex (default 32)\n"
        << "  --text          echo: print the payload as text instead\n"
        << "  --window-ms N   hz: report every N ms (default 1000)\n"
        << "  --duration-ms N hz: stop after N ms\n"
        << "Ctrl+C stops echo and hz.\n";
    return 2;
}

struct Arguments {
    std::string command;
    std::string topic;
    std::string system;
    std::optional<core::DomainId> domain;
    std::uint64_t count {0};
    milliseconds timeout {0};
    milliseconds window {1000};
    milliseconds duration {0};
    std::uint64_t bytes {32};
    bool text {false};
};

bool ParseNumber(const std::string& text, std::uint64_t& value)
{
    if (text.empty() || text.size() > 12 || text.find_first_not_of("0123456789") != std::string::npos) {
        return false;
    }
    value = std::stoull(text);
    return true;
}

// Returns 0 when the arguments are good, else the exit code.
int Parse(int argc, char** argv, Arguments& args)
{
    if (argc < 2) {
        return Usage(std::cerr);
    }
    args.command = argv[1];
    if (args.command == "--help" || args.command == "-h") {
        Usage(std::cout);
        return -1;
    }
    if (args.command != "list" && args.command != "echo" && args.command != "hz") {
        return Usage(std::cerr);
    }
    for (int index = 2; index < argc; ++index) {
        const std::string argument = argv[index];
        const bool has_value = index + 1 < argc;
        std::uint64_t number = 0;
        const auto numeric = [&](const char* name) {
            if (!has_value || !ParseNumber(argv[index + 1], number)) {
                std::cerr << name << " needs a number\n";
                return false;
            }
            ++index;
            return true;
        };
        if (argument == "--system" && has_value) {
            args.system = argv[++index];
        } else if (argument == "--domain") {
            if (!numeric("--domain") || number > 0xffffffffULL) {
                return 2;
            }
            args.domain = static_cast<core::DomainId>(number);
        } else if (argument == "--count" && args.command == "echo") {
            if (!numeric("--count")) {
                return 2;
            }
            args.count = number;
        } else if (argument == "--timeout-ms" && args.command == "echo") {
            if (!numeric("--timeout-ms")) {
                return 2;
            }
            args.timeout = milliseconds(number);
        } else if (argument == "--bytes" && args.command == "echo") {
            if (!numeric("--bytes")) {
                return 2;
            }
            args.bytes = number;
        } else if (argument == "--text" && args.command == "echo") {
            args.text = true;
        } else if (argument == "--window-ms" && args.command == "hz") {
            if (!numeric("--window-ms") || number == 0) {
                return 2;
            }
            args.window = milliseconds(number);
        } else if (argument == "--duration-ms" && args.command == "hz") {
            if (!numeric("--duration-ms")) {
                return 2;
            }
            args.duration = milliseconds(number);
        } else if (!argument.empty() && argument[0] != '-' && args.topic.empty() && args.command != "list") {
            args.topic = argument;
        } else {
            return Usage(std::cerr);
        }
    }
    if (args.command != "list" && args.topic.empty()) {
        return Usage(std::cerr);
    }
    return 0;
}

// --- finding topics -----------------------------------------------------

struct TopicInfo {
    std::string name;
    core::TransportKind transport {core::TransportKind::kIpc};
    transport::MessageDescriptor descriptor;
    shm::TopicOptions geometry;
    core::DomainId domain {0};
    // From a segment header.
    bool from_segment {false};
    std::string state;
    std::uint64_t written {0};
    std::vector<std::uint32_t> pids;
    // From a manifest: "node/component".
    std::set<std::string> writers;
    std::set<std::string> readers;
};

using Topics = core::Result<std::vector<TopicInfo>>;

Topics TopicsFromSegments(std::optional<core::DomainId> domain)
{
    auto segments = shm::ListSegments(domain);
    if (!segments.ok()) {
        std::string message = segments.status().message();
        if (segments.status().code() == core::StatusCode::kUnsupported) {
            message += "; pass --system FILE to take topics from a system manifest";
        }
        return Topics::FromStatus(core::Status::Error(segments.status().code(), message));
    }
    std::vector<TopicInfo> topics;
    for (const auto& segment : segments.value()) {
        if (segment.topic.empty()) {
            continue;  // never initialized or another layout: nothing to read
        }
        TopicInfo info;
        info.name = segment.topic;
        info.descriptor = segment.descriptor;
        info.geometry = segment.geometry;
        info.domain = segment.domain_id;
        info.from_segment = true;
        info.state = shm::ToString(segment.state);
        info.written = segment.messages_written;
        info.pids = segment.live_pids;
        topics.push_back(std::move(info));
    }
    std::sort(topics.begin(), topics.end(), [](const TopicInfo& lhs, const TopicInfo& rhs) {
        return lhs.name != rhs.name ? lhs.name < rhs.name : lhs.domain < rhs.domain;
    });
    return Topics(std::move(topics));
}

core::Status Differs(const std::string& topic, const std::string& node)
{
    return core::Status::InvalidArgument("topic " + topic + " is declared differently in node " + node
                                         + "; run puppet_master_config system validate");
}

// Every topic the manifest's nodes declare, with the deployment's transport
// settings, and which components write and read it.
Topics TopicsFromSystem(const std::string& path, std::optional<core::DomainId> domain)
{
    auto manifest = config::LoadSystemManifest(path);
    if (!manifest.ok()) {
        return Topics::FromStatus(manifest.status());
    }
    auto nodes = config::LoadSystemNodes(manifest.value());
    if (!nodes.ok()) {
        return Topics::FromStatus(nodes.status());
    }
    std::map<std::string, TopicInfo> by_name;
    for (const auto& node : nodes.value()) {
        auto options = shm::OptionsFromProject(node.project);
        if (!options.ok()) {
            return Topics::FromStatus(core::Status::Error(
                options.status().code(), "node " + node.name + ": " + options.status().message()));
        }
        std::map<std::string, std::string> names;  // topic id -> wire name
        for (const auto& topic : node.project.topics) {
            names[topic.id] = topic.name;
            TopicInfo info;
            info.name = topic.name;
            info.transport = topic.transport;
            info.descriptor = transport::MessageDescriptor {topic.message_type, topic.encoding, topic.message_version};
            info.geometry = options.value().ForTopic(topic.name);
            info.domain = domain ? *domain : options.value().domain_id;
            const auto found = by_name.find(topic.name);
            if (found == by_name.end()) {
                by_name.emplace(topic.name, std::move(info));
                continue;
            }
            const auto& known = found->second;
            const bool same_geometry = known.geometry.slot_payload_size == info.geometry.slot_payload_size &&
                                       known.geometry.slot_count == info.geometry.slot_count;
            if (known.descriptor != info.descriptor || known.transport != info.transport ||
                known.domain != info.domain ||
                (info.transport == core::TransportKind::kIpc && !same_geometry)) {
                return Topics::FromStatus(Differs(topic.name, node.name));
            }
        }
        const auto add = [&](const std::vector<std::string>& ids, const std::string& component, bool writes) {
            for (const auto& id : ids) {
                const auto name = names.find(id);
                if (name != names.end()) {
                    auto& info = by_name[name->second];
                    (writes ? info.writers : info.readers).insert(node.name + "/" + component);
                }
            }
        };
        for (const auto& component : node.project.components) {
            add(component.writers, component.name, true);
            add(component.readers, component.name, false);
        }
    }
    std::vector<TopicInfo> topics;
    for (auto& entry : by_name) {
        topics.push_back(std::move(entry.second));
    }
    return Topics(std::move(topics));
}

core::Result<TopicInfo> FindTopic(const Arguments& args)
{
    using ResultType = core::Result<TopicInfo>;
    auto topics = args.system.empty() ? TopicsFromSegments(args.domain) : TopicsFromSystem(args.system, args.domain);
    if (!topics.ok()) {
        return ResultType::FromStatus(topics.status());
    }
    std::vector<TopicInfo> matches;
    for (auto& topic : topics.value()) {
        if (topic.name == args.topic) {
            matches.push_back(std::move(topic));
        }
    }
    if (matches.empty()) {
        return ResultType::FromStatus(core::Status::NotFound(
            args.system.empty() ? "no shm segment for topic " + args.topic +
                                      " on this host; is its writer running? (--system FILE finds it from the "
                                      "manifest instead)"
                                : "no node of " + args.system + " declares topic " + args.topic));
    }
    if (matches.size() > 1) {
        std::string domains;
        for (const auto& match : matches) {
            domains += (domains.empty() ? "" : ", ") + std::to_string(match.domain);
        }
        return ResultType::FromStatus(core::Status::InvalidArgument(
            "topic " + args.topic + " exists in domains " + domains + "; pick one with --domain"));
    }
    if (matches.front().transport != core::TransportKind::kIpc) {
        return ResultType::FromStatus(core::Status::Unsupported(
            "topic " + args.topic + " uses the " + core::ToString(matches.front().transport) +
            " transport; puppet_master_topic reads shared-memory topics only"));
    }
    return ResultType(std::move(matches.front()));
}

// --- reading ------------------------------------------------------------

// A plain reader on the topic. It starts after the newest message and keeps
// up to a ring's worth queued; the ring overwrites what it falls behind on.
struct Subscription {
    std::unique_ptr<shm::ShmTransport> shm_transport;
    transport::ReaderPtr reader;  // declared last: released first
};

core::Result<Subscription> Subscribe(const TopicInfo& topic)
{
    using ResultType = core::Result<Subscription>;
    const auto failed = [&topic](const core::Status& status) {
        return ResultType::FromStatus(
            core::Status::Error(status.code(), "subscribing to " + topic.name + ": " + status.message()));
    };
    shm::Options options;
    options.domain_id = topic.domain;
    options.topics[topic.name] = topic.geometry;
    auto transport_name = core::TransportName::Create("puppet_master_topic");
    auto topic_name = core::TopicName::Create(topic.name);
    if (!topic_name.ok()) {
        return failed(topic_name.status());
    }

    Subscription subscription;
    subscription.shm_transport = std::make_unique<shm::ShmTransport>(transport_name.value(), options);
    auto status = subscription.shm_transport->Open();
    if (!status.ok()) {
        return failed(status);
    }
    core::MessagePolicy policy;
    policy.freshness = core::FreshnessPolicy::kQueued;
    policy.retention = core::RetentionPolicy::kKeepLast;
    policy.queue_depth = topic.geometry.slot_count;
    const transport::EndpointConfig endpoint {
        core::TopicSpec {topic_name.value(), core::TransportKind::kIpc, policy}, topic.descriptor};
    auto reader = subscription.shm_transport->CreateReader(endpoint);
    if (!reader.ok()) {
        return failed(reader.status());
    }
    subscription.reader = reader.value();
    return ResultType(std::move(subscription));
}

// The next message, or none after a short wait.
core::Result<std::optional<transport::Message>> Next(Subscription& subscription)
{
    using ResultType = core::Result<std::optional<transport::Message>>;
    auto message = subscription.reader->Read(transport::ReadOptions {true, kReadSlice});
    if (message.ok()) {
        return ResultType(std::optional<transport::Message>(std::move(message).value()));
    }
    if (message.status().code() == core::StatusCode::kDeadlineExceeded) {
        return ResultType(std::optional<transport::Message>());
    }
    return ResultType::FromStatus(message.status());
}

// Messages skipped before `sequence`, given the previous one seen.
std::uint64_t Gap(const std::optional<core::SequenceNumber>& last, core::SequenceNumber sequence)
{
    return last && sequence > *last + 1 ? sequence - *last - 1 : 0;
}

double Microseconds(core::Nanoseconds value)
{
    return static_cast<double>(value.count()) / 1000.0;
}

// --- commands -----------------------------------------------------------

std::string Join(const std::set<std::string>& names)
{
    std::string text;
    for (const auto& name : names) {
        text += (text.empty() ? "" : ", ") + name;
    }
    return text.empty() ? "-" : text;
}

int List(const Arguments& args)
{
    auto topics = args.system.empty() ? TopicsFromSegments(args.domain) : TopicsFromSystem(args.system, args.domain);
    if (!topics.ok()) {
        std::cerr << topics.status().ToString() << '\n';
        return 1;
    }
    if (topics.value().empty()) {
        std::cout << (args.system.empty() ? "no shm topics\n" : "no topics\n");
        return 0;
    }
    for (const auto& topic : topics.value()) {
        std::cout << topic.name << "  " << topic.descriptor.ToString();
        if (topic.transport != core::TransportKind::kIpc) {
            std::cout << "  " << core::ToString(topic.transport);
        } else {
            std::cout << "  domain " << topic.domain << ", " << topic.geometry.slot_payload_size << " B x "
                      << topic.geometry.slot_count << " slots";
        }
        if (topic.from_segment) {
            std::cout << "  " << topic.state << ", " << topic.written << " written";
            if (!topic.pids.empty()) {
                std::cout << ", pids";
                for (const auto pid : topic.pids) {
                    std::cout << ' ' << pid;
                }
            }
        } else {
            std::cout << "\n  writers: " << Join(topic.writers) << "\n  readers: " << Join(topic.readers);
        }
        std::cout << '\n';
    }
    return 0;
}

void PrintPayload(const transport::ByteBuffer& payload, const Arguments& args)
{
    if (args.text) {
        std::string text;
        for (const auto byte : payload) {
            const auto c = static_cast<char>(byte);
            text += (byte >= 0x20 && byte < 0x7f) || c == '\t' ? c : '.';
        }
        std::cout << "  " << text;
        return;
    }
    const auto shown = static_cast<std::size_t>(std::min<std::uint64_t>(payload.size(), args.bytes));
    if (shown == 0) {
        return;
    }
    std::ostringstream hex;
    hex << std::hex << std::setfill('0');
    for (std::size_t index = 0; index < shown; ++index) {
        hex << (index == 0 ? "" : " ") << std::setw(2) << static_cast<unsigned>(payload[index]);
    }
    std::cout << "  " << hex.str() << (shown < payload.size() ? " ..." : "");
}

int Echo(const Arguments& args)
{
    auto topic = FindTopic(args);
    if (!topic.ok()) {
        std::cerr << topic.status().ToString() << '\n';
        return 1;
    }
    auto subscription = Subscribe(topic.value());
    if (!subscription.ok()) {
        std::cerr << subscription.status().ToString() << '\n';
        return 1;
    }
    std::cerr << "echo " << topic.value().name << ": " << topic.value().descriptor.ToString() << std::endl;

    const auto deadline = args.timeout.count() == 0 ? Clock::time_point::max() : Clock::now() + args.timeout;
    std::uint64_t received = 0;
    std::uint64_t lost = 0;
    std::optional<core::SequenceNumber> last;
    while (!g_stop.load() && Clock::now() < deadline && (args.count == 0 || received < args.count)) {
        auto next = Next(subscription.value());
        if (!next.ok()) {
            std::cerr << next.status().ToString() << '\n';
            return 1;
        }
        if (!next.value()) {
            continue;
        }
        const auto& message = *next.value();
        const auto gap = Gap(last, message.metadata.sequence);
        if (gap != 0) {
            std::cout << "(" << gap << " lost)\n";
            lost += gap;
        }
        last = message.metadata.sequence;
        ++received;
        std::cout << "#" << message.metadata.sequence << "  " << message.payload.size() << " B  latency "
                  << std::fixed << std::setprecision(1)
                  << Microseconds(message.metadata.reception_timestamp - message.metadata.source_timestamp) << " us";
        std::cout.unsetf(std::ios::floatfield);
        PrintPayload(message.payload, args);
        std::cout << std::endl;
    }
    std::cerr << "echo: " << received << " received, " << lost << " lost" << std::endl;
    // With --count, ending early (timeout) is a failure, so scripts can wait
    // for traffic; Ctrl+C is not.
    return args.count != 0 && received < args.count && !g_stop.load() ? 1 : 0;
}

struct Window {
    std::uint64_t received {0};
    std::uint64_t lost {0};
    std::uint64_t bytes {0};
    core::Nanoseconds min_interval {core::Nanoseconds::max()};
    core::Nanoseconds max_interval {0};
};

void Report(const Window& window, Clock::duration elapsed)
{
    const double seconds = std::chrono::duration<double>(elapsed).count();
    std::cout << std::fixed << std::setprecision(2) << "rate " << (seconds > 0 ? window.received / seconds : 0.0)
              << " Hz  " << window.received << " msgs  " << (seconds > 0 ? window.bytes / seconds : 0.0) << " B/s";
    if (window.received >= 2) {
        std::cout << std::setprecision(3) << "  interval min " << Microseconds(window.min_interval) / 1000.0
                  << " ms, max " << Microseconds(window.max_interval) / 1000.0 << " ms";
    }
    std::cout << "  lost " << window.lost << std::endl;
    std::cout.unsetf(std::ios::floatfield);
}

int Hz(const Arguments& args)
{
    auto topic = FindTopic(args);
    if (!topic.ok()) {
        std::cerr << topic.status().ToString() << '\n';
        return 1;
    }
    auto subscription = Subscribe(topic.value());
    if (!subscription.ok()) {
        std::cerr << subscription.status().ToString() << '\n';
        return 1;
    }
    std::cerr << "hz " << topic.value().name << ": " << topic.value().descriptor.ToString() << std::endl;

    const auto started = Clock::now();
    const auto deadline = args.duration.count() == 0 ? Clock::time_point::max() : started + args.duration;
    auto window_start = started;
    Window window;
    std::optional<core::SequenceNumber> last;
    std::optional<core::TimePoint> last_time;
    while (!g_stop.load()) {
        auto next = Next(subscription.value());
        if (!next.ok()) {
            std::cerr << next.status().ToString() << '\n';
            return 1;
        }
        if (next.value()) {
            const auto& message = *next.value();
            window.lost += Gap(last, message.metadata.sequence);
            last = message.metadata.sequence;
            ++window.received;
            window.bytes += message.payload.size();
            // Intervals by publish time, so the reader's own delays do not
            // show up as jitter.
            const auto stamp = message.metadata.source_timestamp;
            if (last_time) {
                const auto interval = stamp - *last_time;
                window.min_interval = std::min(window.min_interval, interval);
                window.max_interval = std::max(window.max_interval, interval);
            }
            last_time = stamp;
        }
        const auto now = Clock::now();
        if (now - window_start >= args.window || now >= deadline) {
            Report(window, now - window_start);
            window = Window {};
            window_start = now;
        }
        if (now >= deadline) {
            break;
        }
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv)
{
    Arguments args;
    const int parsed = Parse(argc, argv, args);
    if (parsed != 0) {
        return parsed < 0 ? 0 : parsed;
    }
    std::signal(SIGINT, HandleStop);
    std::signal(SIGTERM, HandleStop);
#if defined(SIGBREAK)
    std::signal(SIGBREAK, HandleStop);
#endif
    if (args.command == "list") {
        return List(args);
    }
    return args.command == "echo" ? Echo(args) : Hz(args);
}
