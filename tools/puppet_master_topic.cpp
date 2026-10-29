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
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <puppet_master/configuration/configuration.h>
#include <puppet_master/configuration/system_manifest.h>
#include <puppet_master/transport/recording.h>
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
        << "  --duration-ms N hz, record: stop after N ms\n"
        << "       puppet_master_topic record [<topic>...] -o FILE [--system FILE] [--domain N]\n"
        << "                                  [--count N] [--duration-ms N]\n"
        << "       puppet_master_topic replay FILE [<topic>...] [--domain N] [--rate X]\n"
        << "       puppet_master_topic info FILE\n"
        << "  record          write the topics' messages to FILE; with no topic, every\n"
        << "                  shm topic found (or the system's, with --system)\n"
        << "  -o FILE         record: the recording to write, replacing it\n"
        << "  --count N       record: stop after N messages in total\n"
        << "  replay          publish a recording's messages again (all, or the topics\n"
        << "                  named) at their recorded pace\n"
        << "  --rate X        replay: X times the recorded pace; 0 publishes at once\n"
        << "  info            the topics, message counts, and length of a recording\n"
        << "Ctrl+C stops echo, hz, record, and replay.\n";
    return 2;
}

struct Arguments {
    std::string command;
    std::vector<std::string> topics;
    std::string file;  // record: -o; replay, info: the first operand
    std::string system;
    std::optional<core::DomainId> domain;
    std::uint64_t count {0};
    milliseconds timeout {0};
    milliseconds window {1000};
    milliseconds duration {0};
    std::uint64_t bytes {32};
    bool text {false};
    double rate {1.0};
};

bool ParseRate(const std::string& text, double& value)
{
    if (text.empty() || text.size() > 12 || text.find_first_not_of("0123456789.") != std::string::npos ||
        text.find('.') != text.rfind('.') || text == ".") {
        return false;
    }
    value = std::stod(text);
    return value <= 1000.0;
}

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
    const std::set<std::string> commands {"list", "echo", "hz", "record", "replay", "info"};
    if (commands.count(args.command) == 0) {
        return Usage(std::cerr);
    }
    const bool echo = args.command == "echo";
    const bool record = args.command == "record";
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
        } else if (argument == "--count" && (echo || record)) {
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
        } else if (argument == "--duration-ms" && (args.command == "hz" || record)) {
            if (!numeric("--duration-ms")) {
                return 2;
            }
            args.duration = milliseconds(number);
        } else if ((argument == "-o" || argument == "--output") && record && has_value) {
            args.file = argv[++index];
        } else if (argument == "--rate" && args.command == "replay") {
            if (!has_value || !ParseRate(argv[index + 1], args.rate)) {
                std::cerr << "--rate needs a number from 0 to 1000\n";
                return 2;
            }
            ++index;
        } else if (!argument.empty() && argument[0] != '-' && args.command != "list") {
            const bool takes_file = args.command == "replay" || args.command == "info";
            if (takes_file && args.file.empty()) {
                args.file = argument;
            } else if (args.command == "info") {
                return Usage(std::cerr);
            } else {
                args.topics.push_back(argument);
            }
        } else {
            return Usage(std::cerr);
        }
    }
    const bool one_topic = echo || args.command == "hz";
    if ((one_topic && args.topics.size() != 1) ||
        ((record || args.command == "replay" || args.command == "info") && args.file.empty())) {
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

Topics LoadTopics(const Arguments& args)
{
    return args.system.empty() ? TopicsFromSegments(args.domain) : TopicsFromSystem(args.system, args.domain);
}

// The one shared-memory topic called `name` among `topics`.
core::Result<TopicInfo> FindTopic(const Arguments& args, const std::vector<TopicInfo>& topics, const std::string& name)
{
    using ResultType = core::Result<TopicInfo>;
    std::vector<const TopicInfo*> matches;
    for (const auto& topic : topics) {
        if (topic.name == name) {
            matches.push_back(&topic);
        }
    }
    if (matches.empty()) {
        return ResultType::FromStatus(core::Status::NotFound(
            args.system.empty() ? "no shm segment for topic " + name +
                                      " on this host; is its writer running? (--system FILE finds it from the "
                                      "manifest instead)"
                                : "no node of " + args.system + " declares topic " + name));
    }
    if (matches.size() > 1) {
        std::string domains;
        for (const auto* match : matches) {
            domains += (domains.empty() ? "" : ", ") + std::to_string(match->domain);
        }
        return ResultType::FromStatus(core::Status::InvalidArgument(
            "topic " + name + " exists in domains " + domains + "; pick one with --domain"));
    }
    if (matches.front()->transport != core::TransportKind::kIpc) {
        return ResultType::FromStatus(core::Status::Unsupported(
            "topic " + name + " uses the " + core::ToString(matches.front()->transport) +
            " transport; puppet_master_topic reads shared-memory topics only"));
    }
    return ResultType(*matches.front());
}

core::Result<TopicInfo> FindTopic(const Arguments& args, const std::string& name)
{
    auto topics = LoadTopics(args);
    if (!topics.ok()) {
        return core::Result<TopicInfo>::FromStatus(topics.status());
    }
    return FindTopic(args, topics.value(), name);
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
    auto topic = FindTopic(args, args.topics.front());
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
    auto topic = FindTopic(args, args.topics.front());
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

// --- record -------------------------------------------------------------

// Wakes the recording loop when any of its readers has data.
struct Wakeup {
    std::mutex mutex;
    std::condition_variable cv;
    bool pending {false};

    void Notify()
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            pending = true;
        }
        cv.notify_one();
    }

    void Wait(milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait_for(lock, timeout, [this] { return pending; });
        pending = false;
    }
};

struct RecordedChannel {
    TopicInfo topic;
    std::uint32_t id {0};
    std::optional<core::SequenceNumber> last;
    std::uint64_t received {0};
    std::uint64_t lost {0};
    Subscription subscription;  // last: its reader's callback uses the wakeup
};

std::map<std::string, std::string> RecordedParameters(const TopicInfo& topic)
{
    return {
        {"transport", "shm"},
        {"domain_id", std::to_string(topic.domain)},
        {"slot_payload_size", std::to_string(topic.geometry.slot_payload_size)},
        {"slot_count", std::to_string(topic.geometry.slot_count)},
    };
}

core::Result<std::vector<TopicInfo>> TopicsToRecord(const Arguments& args)
{
    using ResultType = core::Result<std::vector<TopicInfo>>;
    auto topics = LoadTopics(args);
    if (!topics.ok()) {
        return ResultType::FromStatus(topics.status());
    }
    std::vector<TopicInfo> chosen;
    if (args.topics.empty()) {
        for (const auto& topic : topics.value()) {
            if (topic.transport == core::TransportKind::kIpc) {
                chosen.push_back(topic);
            }
        }
        if (chosen.empty()) {
            return ResultType::FromStatus(core::Status::NotFound("no shm topics to record"));
        }
        return ResultType(std::move(chosen));
    }
    for (const auto& name : args.topics) {
        auto topic = FindTopic(args, topics.value(), name);
        if (!topic.ok()) {
            return ResultType::FromStatus(topic.status());
        }
        chosen.push_back(std::move(topic).value());
    }
    return ResultType(std::move(chosen));
}

int Record(const Arguments& args)
{
    auto topics = TopicsToRecord(args);
    if (!topics.ok()) {
        std::cerr << topics.status().ToString() << '\n';
        return 1;
    }
    auto recording = transport::RecordingWriter::Create(args.file);
    if (!recording.ok()) {
        std::cerr << recording.status().ToString() << '\n';
        return 1;
    }
    auto& writer = *recording.value();

    Wakeup wakeup;
    std::vector<std::unique_ptr<RecordedChannel>> channels;
    for (auto& topic : topics.value()) {
        auto channel = std::make_unique<RecordedChannel>();
        transport::RecordedTopic recorded;
        recorded.name = topic.name;
        recorded.descriptor = topic.descriptor;
        recorded.parameters = RecordedParameters(topic);
        auto id = writer.AddTopic(recorded);
        if (!id.ok()) {
            std::cerr << id.status().ToString() << '\n';
            return 1;
        }
        channel->id = id.value();
        auto subscription = Subscribe(topic);
        if (!subscription.ok()) {
            std::cerr << subscription.status().ToString() << '\n';
            return 1;
        }
        channel->subscription = std::move(subscription).value();
        auto status = channel->subscription.reader->SetDataAvailableCallback([&wakeup] { wakeup.Notify(); });
        if (!status.ok()) {
            std::cerr << status.ToString() << '\n';
            return 1;
        }
        channel->topic = std::move(topic);
        std::cerr << "record " << channel->topic.name << ": " << channel->topic.descriptor.ToString() << std::endl;
        channels.push_back(std::move(channel));
    }

    // Times are publish times relative to the start, so the replay keeps the
    // writers' pace rather than this reader's.
    const auto started = core::TimePoint(std::chrono::duration_cast<core::Nanoseconds>(
        core::SteadyClock::now().time_since_epoch()));
    const auto deadline = args.duration.count() == 0 ? Clock::time_point::max() : Clock::now() + args.duration;
    auto next_flush = Clock::now() + milliseconds(200);
    std::uint64_t total = 0;
    const auto done = [&] { return g_stop.load() || (args.count != 0 && total >= args.count); };
    while (!done() && Clock::now() < deadline) {
        bool any = false;
        for (auto& channel : channels) {
            while (!done()) {
                auto pending = channel->subscription.reader->PendingMessageCount();
                if (!pending.ok()) {
                    std::cerr << channel->topic.name << ": " << pending.status().ToString() << '\n';
                    return 1;
                }
                if (pending.value() == 0) {
                    break;
                }
                auto message = channel->subscription.reader->Read();
                if (!message.ok()) {
                    break;  // raced with the ring: try again next round
                }
                any = true;
                const auto& metadata = message.value().metadata;
                channel->lost += Gap(channel->last, metadata.sequence);
                channel->last = metadata.sequence;
                ++channel->received;
                ++total;
                const auto time = std::max(core::Nanoseconds(0), metadata.source_timestamp - started);
                auto status = writer.Write(channel->id, time, metadata.sequence, transport::ByteView(message.value().payload));
                if (!status.ok()) {
                    std::cerr << status.ToString() << '\n';
                    return 1;
                }
            }
        }
        if (Clock::now() >= next_flush) {
            auto status = writer.Flush();
            if (!status.ok()) {
                std::cerr << status.ToString() << '\n';
                return 1;
            }
            next_flush = Clock::now() + milliseconds(200);
        }
        if (!any) {
            const auto left = std::max(deadline - Clock::now(), Clock::duration::zero());
            wakeup.Wait(left < kReadSlice ? std::chrono::duration_cast<milliseconds>(left) : kReadSlice);
        }
    }
    for (const auto& channel : channels) {
        std::cerr << "record " << channel->topic.name << ": " << channel->received << " messages, "
                  << channel->lost << " lost" << std::endl;
    }
    channels.clear();  // stop the callbacks before the wakeup goes away
    auto status = writer.Close();
    if (!status.ok()) {
        std::cerr << status.ToString() << '\n';
        return 1;
    }
    std::cerr << "record: " << total << " messages to " << args.file << std::endl;
    return 0;
}

// --- replay and info ----------------------------------------------------

std::uint64_t ParameterOr(const transport::RecordedTopic& topic, const std::string& key, std::uint64_t fallback)
{
    const auto found = topic.parameters.find(key);
    std::uint64_t value = 0;
    return found != topic.parameters.end() && ParseNumber(found->second, value) ? value : fallback;
}

struct ReplayChannel {
    std::unique_ptr<shm::ShmTransport> shm_transport;
    transport::WriterPtr writer;  // declared last: released first
    std::uint64_t sent {0};
};

// A writer for a recorded topic, with the ring size and domain it was
// recorded with (or --domain).
core::Result<std::unique_ptr<ReplayChannel>> OpenReplayChannel(
    const transport::RecordedTopic& topic, const Arguments& args)
{
    using ResultType = core::Result<std::unique_ptr<ReplayChannel>>;
    const auto failed = [&topic](const core::Status& status) {
        return ResultType::FromStatus(
            core::Status::Error(status.code(), "replaying to " + topic.name + ": " + status.message()));
    };
    const shm::TopicOptions defaults;
    const auto domain = ParameterOr(topic, "domain_id", 0);
    if (domain > 0xffffffffULL) {
        return failed(core::Status::InvalidArgument("recorded domain_id is out of range"));
    }
    shm::Options options;
    options.domain_id = args.domain ? *args.domain : static_cast<core::DomainId>(domain);
    options.topics[topic.name] = shm::TopicOptions {
        static_cast<std::size_t>(ParameterOr(topic, "slot_payload_size", defaults.slot_payload_size)),
        static_cast<std::size_t>(ParameterOr(topic, "slot_count", defaults.slot_count))};
    auto status = options.Validate();
    if (!status.ok()) {
        return failed(status);
    }
    auto topic_name = core::TopicName::Create(topic.name);
    if (!topic_name.ok()) {
        return failed(topic_name.status());
    }

    auto channel = std::make_unique<ReplayChannel>();
    channel->shm_transport = std::make_unique<shm::ShmTransport>(
        core::TransportName::Create("puppet_master_topic").value(), options);
    status = channel->shm_transport->Open();
    if (!status.ok()) {
        return failed(status);
    }
    const transport::EndpointConfig endpoint {
        core::TopicSpec {topic_name.value(), core::TransportKind::kIpc, core::MessagePolicy {}}, topic.descriptor};
    auto writer = channel->shm_transport->CreateWriter(endpoint);
    if (!writer.ok()) {
        return failed(writer.status());
    }
    channel->writer = writer.value();
    return ResultType(std::move(channel));
}

int Replay(const Arguments& args)
{
    auto opened = transport::RecordingReader::Open(args.file);
    if (!opened.ok()) {
        std::cerr << opened.status().ToString() << '\n';
        return 1;
    }
    auto& recording = *opened.value();
    const std::set<std::string> wanted(args.topics.begin(), args.topics.end());

    // By recorded topic id; null for a topic left out.
    std::map<std::uint32_t, std::unique_ptr<ReplayChannel>> channels;
    // Paced from the first replayed message, so a quiet start is skipped.
    std::optional<core::Nanoseconds> first_time;
    auto started = Clock::now();
    std::uint64_t total = 0;
    while (!g_stop.load()) {
        auto next = recording.Next();
        if (!next.ok()) {
            std::cerr << next.status().ToString() << '\n';
            return 1;
        }
        if (!next.value()) {
            break;
        }
        const auto& message = *next.value();
        auto found = channels.find(message.topic);
        if (found == channels.end()) {
            const auto* topic = recording.FindTopic(message.topic);
            std::unique_ptr<ReplayChannel> channel;
            if (wanted.empty() || wanted.count(topic->name) != 0) {
                auto created = OpenReplayChannel(*topic, args);
                if (!created.ok()) {
                    std::cerr << created.status().ToString() << '\n';
                    return 1;
                }
                channel = std::move(created).value();
                std::cerr << "replay " << topic->name << ": " << topic->descriptor.ToString() << std::endl;
            }
            found = channels.emplace(message.topic, std::move(channel)).first;
        }
        if (!found->second) {
            continue;
        }
        if (!first_time) {
            first_time = message.time;
            started = Clock::now();
        }
        if (args.rate > 0) {
            const auto offset = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double, std::nano>(
                static_cast<double>((message.time - *first_time).count()) / args.rate));
            const auto target = started + offset;
            // In slices, so Ctrl+C is noticed during a long gap.
            for (auto now = Clock::now(); now < target && !g_stop.load(); now = Clock::now()) {
                std::this_thread::sleep_for(std::min<Clock::duration>(target - now, kReadSlice));
            }
            if (g_stop.load()) {
                break;
            }
        }
        auto status = found->second->writer->Write(transport::ByteView(message.payload));
        if (!status.ok()) {
            std::cerr << "replaying to " << recording.FindTopic(message.topic)->name << ": " << status.ToString()
                      << '\n';
            return 1;
        }
        ++found->second->sent;
        ++total;
    }

    int exit_code = 0;
    for (const auto& name : wanted) {
        const auto& topics = recording.topics();
        const bool known = std::any_of(
            topics.begin(), topics.end(), [&name](const transport::RecordedTopic& topic) { return topic.name == name; });
        if (!known && !g_stop.load()) {
            std::cerr << "replay: topic " << name << " is not in " << args.file << '\n';
            exit_code = 1;
        }
    }
    std::cerr << "replay: " << total << " messages" << (recording.truncated() ? " (the recording was cut off)" : "")
              << std::endl;
    return exit_code;
}

int Info(const Arguments& args)
{
    auto opened = transport::RecordingReader::Open(args.file);
    if (!opened.ok()) {
        std::cerr << opened.status().ToString() << '\n';
        return 1;
    }
    auto& recording = *opened.value();
    struct Stats {
        std::uint64_t messages {0};
        std::uint64_t bytes {0};
        core::Nanoseconds first {0};
        core::Nanoseconds last {0};
    };
    std::map<std::uint32_t, Stats> stats;
    std::uint64_t total = 0;
    std::optional<core::Nanoseconds> first;
    core::Nanoseconds last {0};
    while (true) {
        auto next = recording.Next();
        if (!next.ok()) {
            std::cerr << next.status().ToString() << '\n';
            return 1;
        }
        if (!next.value()) {
            break;
        }
        const auto& message = *next.value();
        auto& topic = stats[message.topic];
        if (topic.messages == 0) {
            topic.first = message.time;
        }
        ++topic.messages;
        topic.bytes += message.payload.size();
        topic.last = message.time;
        if (!first) {
            first = message.time;
        }
        last = std::max(last, message.time);
        ++total;
    }

    const auto seconds = [](core::Nanoseconds value) { return std::chrono::duration<double>(value).count(); };
    std::cout << std::fixed << std::setprecision(3) << args.file << ": " << recording.topics().size() << " topics, "
              << total << " messages, " << (first ? seconds(last - *first) : 0.0) << " s"
              << (recording.truncated() ? ", cut off at the end" : "") << '\n';
    for (const auto& topic : recording.topics()) {
        const auto& topic_stats = stats[topic.id];
        std::cout << "  " << topic.name << "  " << topic.descriptor.ToString() << "  " << topic_stats.messages
                  << " messages, " << topic_stats.bytes << " B";
        const auto span = seconds(topic_stats.last - topic_stats.first);
        if (topic_stats.messages >= 2 && span > 0) {
            std::cout << std::setprecision(2) << ", " << static_cast<double>(topic_stats.messages - 1) / span
                      << " Hz" << std::setprecision(3);
        }
        const auto transport_name = topic.parameters.find("transport");
        if (transport_name != topic.parameters.end() && transport_name->second == "shm") {
            std::cout << "  shm domain " << ParameterOr(topic, "domain_id", 0) << ", "
                      << ParameterOr(topic, "slot_payload_size", 0) << " B x " << ParameterOr(topic, "slot_count", 0)
                      << " slots";
        }
        std::cout << '\n';
    }
    std::cout.unsetf(std::ios::floatfield);
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
    if (args.command == "echo") {
        return Echo(args);
    }
    if (args.command == "hz") {
        return Hz(args);
    }
    if (args.command == "record") {
        return Record(args);
    }
    return args.command == "replay" ? Replay(args) : Info(args);
}
