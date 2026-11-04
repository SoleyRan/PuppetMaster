// puppet_master_topic: looks at the topics of a running system without
// changing it, on shared memory and FastDDS.
//
//     puppet_master_topic list
//     puppet_master_topic echo /vehicle/state --count 5
//     puppet_master_topic hz /vehicle/state
//     puppet_master_topic echo --system system.json /vehicle/state
//
// echo and hz subscribe like any other reader. On shared memory they never
// slow a writer down, and what they miss when they fall behind is
// overwritten in the ring and reported as lost. A shared-memory topic's type
// and ring size come from its segment (Linux only), or with --system from
// the manifest's nodes and deployment, which also works on Windows and
// before the writer starts. FastDDS topics come from --system only, and
// their domain from --domain (default 0); FastDDS carries no writer sequence
// or publish time, so losses and latency are not shown for them, and rates
// and intervals are by arrival.
//
//     puppet_master_topic service list --system system.json
//     puppet_master_topic service call /map/query --system system.json --data "a b"
//
// service list reads each service's presence topic only, so it changes
// nothing. service call sends one real request through a service::Client, so
// the server handles it like any other.

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
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
#include <puppet_master/runtime/context.h>
#include <puppet_master/service/service.h>
#include <puppet_master/transport/recording.h>
#include <puppet_master/transport/shm/shm_transport.h>
#ifdef PUPPETMASTER_TOPIC_WITH_FASTDDS
#include <puppet_master/transport/fastdds/fastdds_transport.h>
#endif

namespace config = puppet_master::configuration;
namespace core = puppet_master::core;
namespace runtime = puppet_master::runtime;
namespace service = puppet_master::service;
namespace shm = puppet_master::transport::shm;
namespace transport = puppet_master::transport;
#ifdef PUPPETMASTER_TOPIC_WITH_FASTDDS
namespace fastdds = puppet_master::transport::fastdds;
#endif

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
        << "  --system FILE   take topics from a system manifest instead of segment headers;\n"
        << "                  needed for FastDDS topics\n"
        << "  --domain N      the domain (shm default: any, or the deployment's; FastDDS: 0)\n"
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
        << "                  shm topic found (or the system's shm and FastDDS ones, with --system)\n"
        << "  -o FILE         record: the recording to write, replacing it\n"
        << "  --count N       record: stop after N messages in total\n"
        << "  replay          publish a recording's messages again (all, or the topics\n"
        << "                  named) at their recorded pace\n"
        << "  --rate X        replay: X times the recorded pace; 0 publishes at once\n"
        << "  info            the topics, message counts, and length of a recording\n"
        << "       puppet_master_topic service list --system FILE [--domain N] [--timeout-ms N]\n"
        << "       puppet_master_topic service call <service> --system FILE [--domain N]\n"
        << "                                        [--data TEXT | --hex HEX] [--timeout-ms N]\n"
        << "                                        [--bytes N] [--text]\n"
        << "  service list    the system's services, who serves and calls them, and whether a\n"
        << "                  server is up, from its heartbeats; listens --timeout-ms (default:\n"
        << "                  three heartbeat intervals, at least 1 s on FastDDS)\n"
        << "  service call    send one request, TEXT's bytes, HEX's (\"68 69\"), or none, and print\n"
        << "                  the reply; fails with the call's status (--timeout-ms, default 2000)\n"
        << "Ctrl+C stops echo, hz, record, and replay.\n";
    return 2;
}

struct Arguments {
    std::string command;
    std::string subcommand;  // service: list or call
    std::vector<std::string> topics;  // service call: the service
    std::string file;  // record: -o; replay, info: the first operand
    std::string system;
    std::optional<core::DomainId> domain;
    std::uint64_t count {0};
    milliseconds timeout {0};
    bool timeout_set {false};
    transport::ByteBuffer data;  // service call: the request
    milliseconds window {1000};
    milliseconds duration {0};
    std::uint64_t bytes {32};
    bool bytes_set {false};
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

// "68 69", "6869", or "68:69": pairs of hex digits; spaces and colons are
// ignored.
bool ParseHex(const std::string& text, transport::ByteBuffer& bytes)
{
    std::string digits;
    for (const char c : text) {
        if (c == ' ' || c == ':') {
            continue;
        }
        if (!std::isxdigit(static_cast<unsigned char>(c))) {
            return false;
        }
        digits += c;
    }
    if (digits.size() % 2 != 0) {
        return false;
    }
    bytes.clear();
    for (std::size_t index = 0; index < digits.size(); index += 2) {
        bytes.push_back(static_cast<core::Byte>(std::stoul(digits.substr(index, 2), nullptr, 16)));
    }
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
    const std::set<std::string> commands {"list", "echo", "hz", "record", "replay", "info", "service"};
    if (commands.count(args.command) == 0) {
        return Usage(std::cerr);
    }
    int first = 2;
    if (args.command == "service") {
        if (argc < 3 || (std::string(argv[2]) != "list" && std::string(argv[2]) != "call")) {
            return Usage(std::cerr);
        }
        args.subcommand = argv[2];
        first = 3;
    }
    const bool echo = args.command == "echo";
    const bool record = args.command == "record";
    const bool call = args.subcommand == "call";
    bool has_data = false;
    for (int index = first; index < argc; ++index) {
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
        } else if (argument == "--timeout-ms" && (echo || args.command == "service")) {
            if (!numeric("--timeout-ms")) {
                return 2;
            }
            args.timeout = milliseconds(number);
            args.timeout_set = true;
        } else if (argument == "--bytes" && (echo || call)) {
            if (!numeric("--bytes")) {
                return 2;
            }
            args.bytes = number;
            args.bytes_set = true;
        } else if (argument == "--text" && (echo || call)) {
            args.text = true;
        } else if ((argument == "--data" || argument == "--hex") && call && has_value) {
            if (has_data) {
                std::cerr << "give --data or --hex once\n";
                return 2;
            }
            has_data = true;
            const std::string value = argv[++index];
            if (argument == "--data") {
                args.data.assign(value.begin(), value.end());
            } else if (!ParseHex(value, args.data)) {
                std::cerr << "--hex needs pairs of hex digits, such as \"68 69\"\n";
                return 2;
            }
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
        } else if (!argument.empty() && argument[0] != '-' && args.command != "list" &&
                   args.subcommand != "list") {
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
    if (args.command == "service") {
        if (call && args.topics.size() != 1) {
            return Usage(std::cerr);
        }
        if (args.system.empty()) {
            std::cerr << "service " << args.subcommand << " needs --system FILE\n";
            return 2;
        }
        if (call && !args.bytes_set) {
            args.bytes = std::numeric_limits<std::uint64_t>::max();  // a reply is shown whole
        }
        return 0;
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
    shm::TopicOptions geometry;  // shm only
    // FastDDS: the topic's delivery, which a reader must match to connect.
    core::DeliveryGuarantee delivery {core::DeliveryGuarantee::kBestEffort};
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
            info.delivery = topic.message_policy.delivery;
            // A FastDDS domain is chosen in each application's code, not in
            // configuration, so it comes from --domain (default 0).
            info.domain = domain ? *domain
                                 : topic.transport == core::TransportKind::kFastDds ? 0 : options.value().domain_id;
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
                (info.transport == core::TransportKind::kIpc && !same_geometry) ||
                (info.transport == core::TransportKind::kFastDds && known.delivery != info.delivery)) {
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

// Transports this build can subscribe and publish on.
bool Readable(core::TransportKind kind)
{
#ifdef PUPPETMASTER_TOPIC_WITH_FASTDDS
    if (kind == core::TransportKind::kFastDds) {
        return true;
    }
#endif
    return kind == core::TransportKind::kIpc;
}

#ifdef PUPPETMASTER_TOPIC_WITH_FASTDDS
constexpr const char* kReadableTransports = "puppet_master_topic reads shared-memory and FastDDS topics only";
#else
constexpr const char* kReadableTransports =
    "this puppet_master_topic was built without FastDDS and reads shared-memory topics only";
#endif

Topics LoadTopics(const Arguments& args)
{
    return args.system.empty() ? TopicsFromSegments(args.domain) : TopicsFromSystem(args.system, args.domain);
}

// The one readable topic called `name` among `topics`.
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
    if (!Readable(matches.front()->transport)) {
        return ResultType::FromStatus(core::Status::Unsupported(
            "topic " + name + " uses the " + core::ToString(matches.front()->transport) + " transport; " +
            kReadableTransports));
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

// How deep a FastDDS reader of the tool queues: there is no ring to size it
// by, so enough for a burst between two reads.
constexpr std::size_t kFastDdsQueueDepth = 256;
// How long replay waits for a FastDDS writer to reach a reader, before the
// first message and before closing.
constexpr auto kFastDdsMatchTimeout = std::chrono::seconds(2);

// The transport a topic is read and written through: a private shm
// transport per topic, sized to its ring, or one FastDDS participant per
// domain, shared by every topic in it.
class Transports {
public:
    core::Result<transport::TransportPtr> For(const TopicInfo& topic)
    {
        using ResultType = core::Result<transport::TransportPtr>;
        transport::TransportPtr created;
        if (topic.transport == core::TransportKind::kIpc) {
            shm::Options options;
            options.domain_id = topic.domain;
            options.topics[topic.name] = topic.geometry;
            created = std::make_shared<shm::ShmTransport>(Name(), options);
        }
#ifdef PUPPETMASTER_TOPIC_WITH_FASTDDS
        else if (topic.transport == core::TransportKind::kFastDds) {
            const auto found = fastdds_.find(topic.domain);
            if (found != fastdds_.end()) {
                return ResultType(found->second);
            }
            fastdds::Options options;
            options.domain_id = topic.domain;
            options.participant_name = "puppet_master_topic";
            created = std::make_shared<fastdds::FastDdsTransport>(Name(), options);
            fastdds_.emplace(topic.domain, created);
        }
#endif
        else {
            return ResultType::FromStatus(core::Status::Unsupported(kReadableTransports));
        }
        auto status = created->Open();
        if (!status.ok()) {
            return ResultType::FromStatus(status);
        }
        return ResultType(std::move(created));
    }

private:
    static core::TransportName Name() { return core::TransportName::Create("puppet_master_topic").value(); }

    std::map<core::DomainId, transport::TransportPtr> fastdds_;
};

// What a transport carries besides the payload.
struct Carried {
    // The writer's sequence numbers, so gaps are losses (shm). FastDDS
    // readers number what they receive, so gaps cannot be seen.
    bool sequence {false};
    // The publish time on a clock this process shares, so latency and
    // publish intervals can be measured (shm). Otherwise intervals are by
    // arrival.
    bool publish_time {false};
};

Carried CarriedBy(core::TransportKind kind)
{
    return kind == core::TransportKind::kIpc ? Carried {true, true} : Carried {false, false};
}

// A plain reader on the topic. On shm it starts after the newest message and
// keeps up to a ring's worth queued; the ring overwrites what it falls behind
// on. On FastDDS it asks for the topic's delivery, which a reader must match
// to connect to the writers.
struct Subscription {
    transport::TransportPtr owner;
    Carried carried;
    transport::ReaderPtr reader;  // declared last: released first
};

core::Result<Subscription> Subscribe(Transports& transports, const TopicInfo& topic)
{
    using ResultType = core::Result<Subscription>;
    const auto failed = [&topic](const core::Status& status) {
        return ResultType::FromStatus(
            core::Status::Error(status.code(), "subscribing to " + topic.name + ": " + status.message()));
    };
    auto topic_name = core::TopicName::Create(topic.name);
    if (!topic_name.ok()) {
        return failed(topic_name.status());
    }
    auto owner = transports.For(topic);
    if (!owner.ok()) {
        return failed(owner.status());
    }

    Subscription subscription;
    subscription.owner = owner.value();
    subscription.carried = CarriedBy(topic.transport);
    core::MessagePolicy policy;
    policy.freshness = core::FreshnessPolicy::kQueued;
    policy.retention = core::RetentionPolicy::kKeepLast;
    if (topic.transport == core::TransportKind::kIpc) {
        policy.queue_depth = topic.geometry.slot_count;
    } else {
        policy.queue_depth = kFastDdsQueueDepth;
        policy.delivery = topic.delivery;
    }
    const transport::EndpointConfig endpoint {core::TopicSpec {topic_name.value(), topic.transport, policy},
                                              topic.descriptor};
    auto reader = subscription.owner->CreateReader(endpoint);
    if (!reader.ok()) {
        return failed(reader.status());
    }
    subscription.reader = reader.value();
    return ResultType(std::move(subscription));
}

core::Result<Subscription> Subscribe(const TopicInfo& topic)
{
    Transports transports;  // the subscription keeps its transport
    return Subscribe(transports, topic);
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
        if (topic.transport == core::TransportKind::kFastDds) {
            std::cout << "  fastdds domain " << topic.domain << ", " << core::ToString(topic.delivery);
        } else if (topic.transport != core::TransportKind::kIpc) {
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
        const auto& carried = subscription.value().carried;
        if (carried.sequence) {
            const auto gap = Gap(last, message.metadata.sequence);
            if (gap != 0) {
                std::cout << "(" << gap << " lost)\n";
                lost += gap;
            }
            last = message.metadata.sequence;
        }
        ++received;
        std::cout << "#" << message.metadata.sequence << "  " << message.payload.size() << " B";
        if (carried.publish_time) {
            std::cout << "  latency " << std::fixed << std::setprecision(1)
                      << Microseconds(message.metadata.reception_timestamp - message.metadata.source_timestamp)
                      << " us";
            std::cout.unsetf(std::ios::floatfield);
        }
        PrintPayload(message.payload, args);
        std::cout << std::endl;
    }
    if (subscription.value().carried.sequence) {
        std::cerr << "echo: " << received << " received, " << lost << " lost" << std::endl;
    } else {
        std::cerr << "echo: " << received << " received (losses are not visible on "
                  << core::ToString(topic.value().transport) << ")" << std::endl;
    }
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

void Report(const Window& window, Clock::duration elapsed, const Carried& carried)
{
    const double seconds = std::chrono::duration<double>(elapsed).count();
    std::cout << std::fixed << std::setprecision(2) << "rate " << (seconds > 0 ? window.received / seconds : 0.0)
              << " Hz  " << window.received << " msgs  " << (seconds > 0 ? window.bytes / seconds : 0.0) << " B/s";
    if (window.received >= 2) {
        std::cout << std::setprecision(3) << "  interval min " << Microseconds(window.min_interval) / 1000.0
                  << " ms, max " << Microseconds(window.max_interval) / 1000.0 << " ms"
                  << (carried.publish_time ? "" : " (by arrival)");
    }
    if (carried.sequence) {
        std::cout << "  lost " << window.lost;
    }
    std::cout << std::endl;
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
            const auto& carried = subscription.value().carried;
            if (carried.sequence) {
                window.lost += Gap(last, message.metadata.sequence);
                last = message.metadata.sequence;
            }
            ++window.received;
            window.bytes += message.payload.size();
            // Intervals by publish time where the transport carries it, so
            // the reader's own delays do not show up as jitter.
            const auto stamp = carried.publish_time ? message.metadata.source_timestamp
                                                    : message.metadata.reception_timestamp;
            if (last_time) {
                const auto interval = stamp - *last_time;
                window.min_interval = std::min(window.min_interval, interval);
                window.max_interval = std::max(window.max_interval, interval);
            }
            last_time = stamp;
        }
        const auto now = Clock::now();
        if (now - window_start >= args.window || now >= deadline) {
            Report(window, now - window_start, subscription.value().carried);
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

// What a replay needs to recreate the topic: its transport and domain, plus
// the ring size on shm or the delivery on FastDDS.
std::map<std::string, std::string> RecordedParameters(const TopicInfo& topic)
{
    if (topic.transport == core::TransportKind::kFastDds) {
        return {
            {"transport", "fastdds"},
            {"domain_id", std::to_string(topic.domain)},
            {"delivery", core::ToString(topic.delivery)},
        };
    }
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
            if (Readable(topic.transport)) {
                chosen.push_back(topic);
            }
        }
        if (chosen.empty()) {
            return ResultType::FromStatus(core::Status::NotFound("no topics to record"));
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
    Transports transports;  // one FastDDS participant per domain for all topics
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
        auto subscription = Subscribe(transports, topic);
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
    // writers' pace rather than this reader's; arrival times on transports
    // that carry no publish time.
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
                const auto& carried = channel->subscription.carried;
                if (carried.sequence) {
                    channel->lost += Gap(channel->last, metadata.sequence);
                    channel->last = metadata.sequence;
                }
                ++channel->received;
                ++total;
                // Publish time where the transport carries it, else arrival.
                const auto stamp = carried.publish_time ? metadata.source_timestamp : metadata.reception_timestamp;
                const auto time = std::max(core::Nanoseconds(0), stamp - started);
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
        std::cerr << "record " << channel->topic.name << ": " << channel->received << " messages";
        if (channel->subscription.carried.sequence) {
            std::cerr << ", " << channel->lost << " lost";
        }
        std::cerr << std::endl;
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
    std::string name;
    core::TransportKind transport {core::TransportKind::kIpc};
    transport::TransportPtr owner;
    transport::WriterPtr writer;  // declared last: released first
    std::uint64_t sent {0};
};

// The recorded topic as the replay recreates it: its transport, the domain
// it was recorded in (or --domain), and its ring size or delivery.
core::Result<TopicInfo> ReplayedTopic(const transport::RecordedTopic& recorded, const Arguments& args)
{
    using ResultType = core::Result<TopicInfo>;
    TopicInfo topic;
    topic.name = recorded.name;
    topic.descriptor = recorded.descriptor;
    const auto domain = ParameterOr(recorded, "domain_id", 0);
    if (domain > 0xffffffffULL) {
        return ResultType::FromStatus(core::Status::InvalidArgument("recorded domain_id is out of range"));
    }
    topic.domain = args.domain ? *args.domain : static_cast<core::DomainId>(domain);
    const auto kind = recorded.parameters.find("transport");
    const std::string transport_name = kind == recorded.parameters.end() ? "shm" : kind->second;
    if (transport_name == "fastdds") {
        topic.transport = core::TransportKind::kFastDds;
        const auto delivery = recorded.parameters.find("delivery");
        topic.delivery = delivery != recorded.parameters.end() && delivery->second == "reliable"
                             ? core::DeliveryGuarantee::kReliable
                             : core::DeliveryGuarantee::kBestEffort;
    } else if (transport_name == "shm") {
        const shm::TopicOptions defaults;
        topic.transport = core::TransportKind::kIpc;
        topic.geometry = shm::TopicOptions {
            static_cast<std::size_t>(ParameterOr(recorded, "slot_payload_size", defaults.slot_payload_size)),
            static_cast<std::size_t>(ParameterOr(recorded, "slot_count", defaults.slot_count))};
        auto status = topic.geometry.Validate();
        if (!status.ok()) {
            return ResultType::FromStatus(status);
        }
    } else {
        return ResultType::FromStatus(
            core::Status::Unsupported("recorded from the " + transport_name + " transport; " + kReadableTransports));
    }
    if (!Readable(topic.transport)) {
        return ResultType::FromStatus(core::Status::Unsupported(kReadableTransports));
    }
    return ResultType(std::move(topic));
}

// A writer for a recorded topic. On FastDDS it waits until a reader is
// matched, so the first messages are not sent to nobody.
core::Result<std::unique_ptr<ReplayChannel>> OpenReplayChannel(
    Transports& transports, const transport::RecordedTopic& recorded, const Arguments& args)
{
    using ResultType = core::Result<std::unique_ptr<ReplayChannel>>;
    const auto failed = [&recorded](const core::Status& status) {
        return ResultType::FromStatus(
            core::Status::Error(status.code(), "replaying to " + recorded.name + ": " + status.message()));
    };
    auto topic = ReplayedTopic(recorded, args);
    if (!topic.ok()) {
        return failed(topic.status());
    }
    auto topic_name = core::TopicName::Create(recorded.name);
    if (!topic_name.ok()) {
        return failed(topic_name.status());
    }
    auto owner = transports.For(topic.value());
    if (!owner.ok()) {
        return failed(owner.status());
    }
    core::MessagePolicy policy;
    if (topic.value().transport == core::TransportKind::kFastDds) {
        policy.freshness = core::FreshnessPolicy::kQueued;
        policy.queue_depth = kFastDdsQueueDepth;
        policy.delivery = topic.value().delivery;
    }
    const transport::EndpointConfig endpoint {
        core::TopicSpec {topic_name.value(), topic.value().transport, policy}, recorded.descriptor};
    auto writer = owner.value()->CreateWriter(endpoint);
    if (!writer.ok()) {
        return failed(writer.status());
    }

    auto channel = std::make_unique<ReplayChannel>();
    channel->name = recorded.name;
    channel->transport = topic.value().transport;
    channel->owner = owner.value();
    channel->writer = writer.value();
    if (channel->transport == core::TransportKind::kFastDds) {
        const auto deadline = Clock::now() + kFastDdsMatchTimeout;
        while (!g_stop.load()) {
            auto matched = channel->writer->MatchedReaderCount();
            if (matched.ok() && matched.value() > 0) {
                break;
            }
            if (Clock::now() >= deadline) {
                std::cerr << "replay " << recorded.name << ": no reader matched within "
                          << std::chrono::duration_cast<milliseconds>(kFastDdsMatchTimeout).count()
                          << " ms; publishing anyway" << std::endl;
                break;
            }
            std::this_thread::sleep_for(milliseconds(10));
        }
    }
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

    Transports transports;  // one FastDDS participant per domain for all topics
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
                auto created = OpenReplayChannel(transports, *topic, args);
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

    // FastDDS sends asynchronously: let reliable readers acknowledge the last
    // messages before the writers close and drop them.
    for (const auto& entry : channels) {
        const auto& channel = entry.second;
        if (channel && channel->transport == core::TransportKind::kFastDds && channel->sent != 0) {
            auto delivered = channel->writer->WaitForDelivery(kFastDdsMatchTimeout);
            if (!delivered.ok()) {
                std::cerr << "replay " << channel->name << ": " << delivered.message() << std::endl;
            }
        }
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
        } else if (transport_name != topic.parameters.end() && transport_name->second == "fastdds") {
            const auto delivery = topic.parameters.find("delivery");
            std::cout << "  fastdds domain " << ParameterOr(topic, "domain_id", 0) << ", "
                      << (delivery == topic.parameters.end() ? "best_effort" : delivery->second);
        }
        std::cout << '\n';
    }
    std::cout.unsetf(std::ios::floatfield);
    return 0;
}

// --- services -----------------------------------------------------------

// A service the manifest's nodes declare, with what is needed to reach it.
struct ServiceInfo {
    explicit ServiceInfo(service::ServiceSpec spec_in) : spec(std::move(spec_in)) {}

    service::ServiceSpec spec;
    core::DomainId domain {0};
    // The ring sizes of its topics, from the first node that declares it.
    shm::Options shm_options;
    // "node/component".
    std::set<std::string> servers;
    std::set<std::string> clients;
};

using Services = core::Result<std::vector<ServiceInfo>>;

bool SameContract(const service::ServiceSpec& lhs, const service::ServiceSpec& rhs)
{
    return lhs.transport == rhs.transport && lhs.request == rhs.request && lhs.reply == rhs.reply &&
           lhs.queue_depth == rhs.queue_depth && lhs.heartbeat_interval == rhs.heartbeat_interval;
}

// Every service the manifest's nodes declare, with the deployment's
// transport settings, and which components serve and call it.
Services ServicesFromSystem(const Arguments& args)
{
    auto manifest = config::LoadSystemManifest(args.system);
    if (!manifest.ok()) {
        return Services::FromStatus(manifest.status());
    }
    auto nodes = config::LoadSystemNodes(manifest.value());
    if (!nodes.ok()) {
        return Services::FromStatus(nodes.status());
    }
    std::map<std::string, ServiceInfo> by_name;
    for (const auto& node : nodes.value()) {
        auto options = shm::OptionsFromProject(node.project);
        if (!options.ok()) {
            return Services::FromStatus(core::Status::Error(
                options.status().code(), "node " + node.name + ": " + options.status().message()));
        }
        std::map<std::string, std::string> names;  // service id -> wire name
        for (const auto& declared : node.project.services) {
            auto spec = declared.ToServiceSpec();
            if (!spec.ok()) {
                return Services::FromStatus(core::Status::Error(
                    spec.status().code(), "node " + node.name + ": " + spec.status().message()));
            }
            names[declared.id] = declared.name;
            // As for topics: a FastDDS domain is chosen in code, so it comes
            // from --domain (default 0).
            const core::DomainId domain = args.domain ? *args.domain
                                          : declared.transport == core::TransportKind::kFastDds
                                              ? 0
                                              : options.value().domain_id;
            const auto found = by_name.find(declared.name);
            if (found == by_name.end()) {
                ServiceInfo info(std::move(spec).value());
                info.domain = domain;
                info.shm_options = options.value();
                info.shm_options.domain_id = domain;
                by_name.emplace(declared.name, std::move(info));
            } else if (!SameContract(found->second.spec, spec.value()) || found->second.domain != domain) {
                return Services::FromStatus(core::Status::InvalidArgument(
                    "service " + declared.name + " is declared differently in node " + node.name +
                    "; run puppet_master_config system validate"));
            }
        }
        for (const auto& component : node.project.components) {
            for (const auto* ids : {&component.servers, &component.clients}) {
                for (const auto& id : *ids) {
                    const auto name = names.find(id);
                    if (name == names.end()) {
                        continue;
                    }
                    auto& info = by_name.find(name->second)->second;
                    (ids == &component.servers ? info.servers : info.clients)
                        .insert(node.name + "/" + component.name);
                }
            }
        }
    }
    std::vector<ServiceInfo> services;
    for (auto& entry : by_name) {
        services.push_back(std::move(entry.second));
    }
    return Services(std::move(services));
}

std::string TransportText(core::TransportKind kind, core::DomainId domain)
{
    if (kind == core::TransportKind::kIpc) {
        return "shm domain " + std::to_string(domain);
    }
    if (kind == core::TransportKind::kFastDds) {
        return "fastdds domain " + std::to_string(domain);
    }
    return core::ToString(kind);
}

std::string Hex16(std::uint64_t value)
{
    std::ostringstream text;
    text << std::hex << std::setw(16) << std::setfill('0') << value;
    return text.str();
}

// A reader of the service's presence topic only: no client, no reply topic.
core::Result<Subscription> WatchPresence(Transports& transports, const ServiceInfo& info)
{
    using ResultType = core::Result<Subscription>;
    if (!Readable(info.spec.transport)) {
        return ResultType::FromStatus(core::Status::Unsupported(
            std::string("it uses the ") + core::ToString(info.spec.transport) + " transport; " + kReadableTransports));
    }
    auto endpoints = service::EndpointsFor(info.spec);
    if (!endpoints.ok()) {
        return ResultType::FromStatus(endpoints.status());
    }
    const auto& presence = endpoints.value().presence;
    TopicInfo topic;
    topic.name = presence.topic.name.str();
    topic.transport = info.spec.transport;
    topic.descriptor = presence.message;
    topic.geometry = info.shm_options.ForTopic(topic.name);
    topic.domain = info.domain;
    auto owner = transports.For(topic);
    if (!owner.ok()) {
        return ResultType::FromStatus(owner.status());
    }
    auto reader = owner.value()->CreateReader(presence);
    if (!reader.ok()) {
        return ResultType::FromStatus(reader.status());
    }
    Subscription subscription;
    subscription.owner = owner.value();
    subscription.carried = CarriedBy(topic.transport);
    subscription.reader = reader.value();
    return ResultType(std::move(subscription));
}

// What the presence topic said while the tool listened.
struct Presence {
    std::optional<Subscription> subscription;
    std::string problem;  // why it could not be watched
    std::uint64_t instance {0};
    bool goodbye {false};
    std::optional<Clock::time_point> heard;  // newest heartbeat or goodbye
};

// Reads the heartbeats that arrived; each is {u64 instance, u64 sequence},
// little-endian, and sequence 0 is the server's goodbye.
void Poll(Presence& presence)
{
    for (auto message = presence.subscription->reader->Read(); message.ok();
         message = presence.subscription->reader->Read()) {
        const auto& payload = message.value().payload;
        if (payload.size() != 16) {
            continue;
        }
        std::uint64_t instance = 0;
        std::uint64_t sequence = 0;
        for (int index = 7; index >= 0; --index) {
            instance = (instance << 8) | payload[static_cast<std::size_t>(index)];
            sequence = (sequence << 8) | payload[static_cast<std::size_t>(8 + index)];
        }
        presence.instance = instance;
        presence.goodbye = sequence == 0;
        presence.heard = Clock::now();
    }
}

int ServiceList(const Arguments& args)
{
    auto services = ServicesFromSystem(args);
    if (!services.ok()) {
        std::cerr << services.status().ToString() << '\n';
        return 1;
    }
    if (services.value().empty()) {
        std::cout << "no services\n";
        return 0;
    }

    // Long enough to see a heartbeat of every live server, and on FastDDS
    // for discovery first.
    core::Nanoseconds listen {0};
    for (const auto& info : services.value()) {
        listen = std::max(listen, 3 * info.spec.heartbeat_interval);
        if (info.spec.transport == core::TransportKind::kFastDds) {
            listen = std::max<core::Nanoseconds>(listen, std::chrono::seconds(1));
        }
    }
    if (args.timeout_set) {
        listen = args.timeout;
    }

    Transports transports;
    std::vector<Presence> presences(services.value().size());
    for (std::size_t index = 0; index < presences.size(); ++index) {
        auto watched = WatchPresence(transports, services.value()[index]);
        if (watched.ok()) {
            presences[index].subscription = std::move(watched).value();
        } else {
            presences[index].problem = watched.status().message();
        }
    }
    const auto deadline = Clock::now() + std::chrono::duration_cast<Clock::duration>(listen);
    while (!g_stop.load() && Clock::now() < deadline) {
        bool all_heard = true;
        for (auto& presence : presences) {
            if (presence.subscription) {
                Poll(presence);
                all_heard = all_heard && presence.heard.has_value();
            }
        }
        if (all_heard) {
            break;
        }
        std::this_thread::sleep_for(milliseconds(10));
    }

    const auto now = Clock::now();
    for (std::size_t index = 0; index < presences.size(); ++index) {
        const auto& info = services.value()[index];
        const auto& presence = presences[index];
        std::cout << info.spec.name.str() << "  request " << info.spec.request.ToString() << ", reply "
                  << info.spec.reply.ToString() << "  " << TransportText(info.spec.transport, info.domain) << '\n';
        std::cout << "  server: ";
        if (!presence.subscription) {
            std::cout << "unknown (" << presence.problem << ")";
        } else if (!presence.heard) {
            std::cout << "down (no heartbeat in " << std::chrono::duration_cast<milliseconds>(listen).count()
                      << " ms)";
        } else if (presence.goodbye) {
            std::cout << "stopped (instance " << Hex16(presence.instance) << " said goodbye)";
        } else {
            std::cout << "up, instance " << Hex16(presence.instance) << ", heartbeat "
                      << std::chrono::duration_cast<milliseconds>(now - *presence.heard).count() << " ms ago";
        }
        std::cout << '\n';
        std::cout << "  servers: " << Join(info.servers) << '\n';
        std::cout << "  clients: " << Join(info.clients) << '\n';
    }
    return 0;
}

int ServiceCall(const Arguments& args)
{
    auto services = ServicesFromSystem(args);
    if (!services.ok()) {
        std::cerr << services.status().ToString() << '\n';
        return 1;
    }
    const auto& name = args.topics.front();
    const auto found = std::find_if(services.value().begin(), services.value().end(),
                                    [&name](const ServiceInfo& info) { return info.spec.name.str() == name; });
    if (found == services.value().end()) {
        std::cerr << "not_found: no node of " << args.system << " declares service " << name << '\n';
        return 1;
    }
    const auto& info = *found;
    if (!Readable(info.spec.transport)) {
        std::cerr << "unsupported: service " << name << " uses the " << core::ToString(info.spec.transport)
                  << " transport; " << kReadableTransports << '\n';
        return 1;
    }

    // A runtime of its own with the one transport the service runs on, so
    // the client below is an ordinary one.
    runtime::RuntimeOptions options;
    options.register_inmemory_transport = false;
    auto created = runtime::RuntimeContext::Create(options);
    if (!created.ok()) {
        std::cerr << created.status().ToString() << '\n';
        return 1;
    }
    auto& context = *created.value();
    const auto transport_name = core::TransportName::Create("puppet_master_topic").value();
    transport::TransportPtr backend;
    if (info.spec.transport == core::TransportKind::kIpc) {
        backend = std::make_shared<shm::ShmTransport>(transport_name, info.shm_options);
    }
#ifdef PUPPETMASTER_TOPIC_WITH_FASTDDS
    else {
        fastdds::Options dds;
        dds.domain_id = info.domain;
        dds.participant_name = "puppet_master_topic";
        backend = std::make_shared<fastdds::FastDdsTransport>(transport_name, dds);
    }
#endif
    auto status = context.RegisterTransport(backend);
    if (!status.ok()) {
        std::cerr << status.ToString() << '\n';
        return 1;
    }

    auto client = service::Client::Create(context, info.spec);
    if (!client.ok()) {
        std::cerr << client.status().ToString() << '\n';
        return 1;
    }
    const auto timeout = args.timeout_set ? args.timeout : milliseconds(2000);
    const auto started = Clock::now();
    auto server = client.value()->WaitForService(timeout);
    if (!server.ok()) {
        std::cerr << server.status().ToString() << '\n';
        return 1;
    }
    std::cerr << "call " << name << ": server " << Hex16(server.value().instance_id) << ", request "
              << args.data.size() << " B" << std::endl;
    const auto left = std::max<Clock::duration>(timeout - (Clock::now() - started), milliseconds(1));
    const auto sent = Clock::now();
    auto reply = client.value()->Call(transport::ByteView(args.data),
                                      std::chrono::duration_cast<core::Nanoseconds>(left));
    const auto took = Clock::now() - sent;
    if (!reply.ok()) {
        std::cerr << reply.status().ToString() << '\n';
        return 1;
    }
    std::cout << "reply " << reply.value().size() << " B in " << std::fixed << std::setprecision(1)
              << Microseconds(std::chrono::duration_cast<core::Nanoseconds>(took)) / 1000.0 << " ms";
    std::cout.unsetf(std::ios::floatfield);
    PrintPayload(reply.value(), args);
    std::cout << std::endl;
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
    if (args.command == "service") {
        return args.subcommand == "list" ? ServiceList(args) : ServiceCall(args);
    }
    return args.command == "replay" ? Replay(args) : Info(args);
}
