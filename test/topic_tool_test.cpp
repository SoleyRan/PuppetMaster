// puppet_master_topic against a live shared-memory topic: this process
// writes, the tool runs as a child process and its output is checked.
//
//     puppet_master_topic_tool_test <path to puppet_master_topic>

#include <cassert>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <puppet_master/configuration/supervisor.h>
#include <puppet_master/runtime/context.h>
#include <puppet_master/service/service.h>
#include <puppet_master/transport/shm/shm_transport.h>

namespace config = puppet_master::configuration;
namespace core = puppet_master::core;
namespace fs = std::filesystem;
namespace runtime = puppet_master::runtime;
namespace service = puppet_master::service;
namespace shm = puppet_master::transport::shm;
namespace transport = puppet_master::transport;

using namespace std::chrono_literals;

namespace {

constexpr core::DomainId kDomain = 77;

bool Contains(const std::string& text, const std::string& part)
{
    return text.find(part) != std::string::npos;
}

std::string ReadFile(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void WriteFile(const fs::path& path, const std::string& text)
{
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

// A one-node system whose producer writes `topic` over shm, plus a writer on
// the same topic in this process.
class Fixture {
public:
    Fixture()
        : stamp_(std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())),
          root_(fs::temp_directory_path() / ("pm_topic_tool_" + stamp_)),
          topic_("/pm_topic_test/text_" + stamp_),
          service_("/pm_topic_test/echo_" + stamp_)
    {
        WriteFile(root_ / "share/test_interfaces/topics.json",
            R"({"topics": [{"id": "text", "name": ")" + topic_ + R"(", "message_type": "test.Text"}],
                "services": [{"id": "echo", "name": ")" + service_ +
                R"(", "request_type": "test.EchoRequest", "reply_type": "test.EchoReply"}]})");
        WriteFile(root_ / "share/producer/producer.json", R"({
            "imports": ["test_interfaces/topics.json"],
            "components": [{"name": "producer", "writers": ["text"], "servers": ["echo"],
                            "triggers": [{"kind": "manual"}]}]})");
        WriteFile(root_ / "share/consumer/consumer.json", R"({
            "imports": ["test_interfaces/topics.json"],
            "components": [{"name": "consumer", "readers": ["text"], "clients": ["echo"],
                            "triggers": [{"kind": "data", "data_topics": ["text"]}]}]})");
        WriteFile(root_ / "deploy.json",
            R"({"defaults": {"transport": "shm", "transport_parameters": {"domain_id": 77}},
                "topics": [{"name": ")" + topic_ +
                R"(", "transport_parameters": {"slot_payload_size": 256, "slot_count": 8}}]})");
        WriteFile(root_ / "system.json", R"({
            "name": "topic_test", "config_paths": ["share"], "deployment": "deploy.json",
            "nodes": [
                {"name": "producer", "executable": "producer_node", "config": "producer/producer.json"},
                {"name": "consumer", "executable": "consumer_node", "config": "consumer/consumer.json"}]})");

        shm::Options options;
        options.domain_id = kDomain;
        options.topics[topic_] = shm::TopicOptions {256, 8};
        transport_ = std::make_unique<shm::ShmTransport>(core::TransportName::Create("test").value(), options);
        assert(transport_->Open().ok());
        auto writer = transport_->CreateWriter(Endpoint({}));
        assert(writer.ok());
        writer_ = writer.value();
    }

    transport::EndpointConfig Endpoint(core::MessagePolicy policy) const
    {
        return transport::EndpointConfig {
            core::TopicSpec {core::TopicName::Create(topic_).value(), core::TransportKind::kIpc, policy},
            transport::MessageDescriptor {"test.Text", "application/octet-stream", 0}};
    }

    // A reader in this process that queues a whole ring.
    transport::ReaderPtr Subscribe()
    {
        core::MessagePolicy policy;
        policy.freshness = core::FreshnessPolicy::kQueued;
        policy.queue_depth = 8;
        auto reader = transport_->CreateReader(Endpoint(policy));
        assert(reader.ok());
        return reader.value();
    }

    std::string File(const std::string& name) const { return (root_ / name).string(); }

    // The echo service as the system declares it.
    service::ServiceSpec Service() const
    {
        service::ServiceSpec spec(core::TopicName::Create(service_).value());
        spec.transport = core::TransportKind::kIpc;
        spec.request = transport::MessageDescriptor {"test.EchoRequest", "application/octet-stream", 0};
        spec.reply = transport::MessageDescriptor {"test.EchoReply", "application/octet-stream", 0};
        return spec;
    }

    const std::string& service_name() const noexcept { return service_; }

    ~Fixture()
    {
        writer_.reset();
        transport_.reset();
        shm::RemoveSegment(kDomain, topic_);
        shm::RemoveSegment(kDomain, service_ + "/request");
        shm::RemoveSegment(kDomain, service_ + "/presence");
        std::error_code ignored;
        fs::remove_all(root_, ignored);
    }

    const std::string& topic() const noexcept { return topic_; }
    std::string system() const { return (root_ / "system.json").string(); }

    void Publish(const std::string& text)
    {
        const auto status = writer_->Write(transport::ByteView::From(text.data(), text.size()));
        assert(status.ok());
    }

    struct Outcome {
        config::ProcessExit exit;
        std::string output;
    };

    // Runs the tool, publishing "hello N" every `period` (if not zero) until
    // it exits.
    Outcome Run(const std::vector<std::string>& command, std::chrono::milliseconds period)
    {
        const auto log = root_ / ("run_" + std::to_string(runs_++) + ".log");
        auto launcher = config::MakeSystemProcessLauncher();
        auto child = launcher->Start(command, log.string());
        assert(child.ok());
        const auto deadline = std::chrono::steady_clock::now() + 30s;
        std::optional<config::ProcessExit> exit;
        int sent = 0;
        while (!(exit = child.value()->Poll())) {
            if (std::chrono::steady_clock::now() > deadline) {
                child.value()->Kill();
                std::cerr << "tool did not exit; output:\n" << ReadFile(log) << '\n';
                assert(false);
            }
            if (period.count() != 0) {
                Publish("hello " + std::to_string(sent++));
                std::this_thread::sleep_for(period);
            } else {
                std::this_thread::sleep_for(5ms);
            }
        }
        Outcome outcome {*exit, ReadFile(log)};
        std::cout << "$";
        for (const auto& word : command) {
            std::cout << ' ' << word;
        }
        std::cout << "  -> " << config::ToString(*exit) << '\n' << outcome.output;
        return outcome;
    }

private:
    std::string stamp_;
    fs::path root_;
    std::string topic_;
    std::string service_;
    std::unique_ptr<shm::ShmTransport> transport_;
    transport::WriterPtr writer_;
    int runs_ {0};
};

void TestListFromSystem(Fixture& fixture, const std::string& tool)
{
    const auto run = fixture.Run({tool, "list", "--system", fixture.system()}, 0ms);
    assert(run.exit.success());
    assert(Contains(run.output, fixture.topic() + "  test.Text (application/octet-stream)  domain 77, 256 B x 8 slots"));
    assert(Contains(run.output, "writers: producer/producer"));
    assert(Contains(run.output, "readers: consumer/consumer"));
}

void TestEcho(Fixture& fixture, const std::string& tool, bool with_system)
{
    std::vector<std::string> command {tool, "echo", fixture.topic(), "--count", "3", "--timeout-ms", "20000", "--text"};
    if (with_system) {
        command.insert(command.end(), {"--system", fixture.system()});
    }
    const auto run = fixture.Run(command, 10ms);
    assert(run.exit.success());
    assert(Contains(run.output, "echo " + fixture.topic() + ": test.Text (application/octet-stream)"));
    assert(Contains(run.output, "  hello "));
    assert(Contains(run.output, "echo: 3 received"));
}

void TestEchoHex(Fixture& fixture, const std::string& tool)
{
    const auto run = fixture.Run(
        {tool, "echo", fixture.topic(), "--system", fixture.system(), "--count", "1", "--bytes", "4"}, 10ms);
    assert(run.exit.success());
    // "hell" in hex, then a mark for the bytes not shown.
    assert(Contains(run.output, "  68 65 6c 6c ..."));
}

void TestEchoTimesOut(Fixture& fixture, const std::string& tool)
{
    const auto run = fixture.Run(
        {tool, "echo", fixture.topic(), "--system", fixture.system(), "--count", "1", "--timeout-ms", "300"}, 0ms);
    assert(run.exit.code == 1 && !run.exit.signaled);
    assert(Contains(run.output, "echo: 0 received"));
}

void TestHz(Fixture& fixture, const std::string& tool)
{
    const auto run = fixture.Run(
        {tool, "hz", fixture.topic(), "--system", fixture.system(), "--window-ms", "300", "--duration-ms", "1000"},
        10ms);
    assert(run.exit.success());
    assert(Contains(run.output, "rate "));
    assert(Contains(run.output, " Hz  "));
    assert(Contains(run.output, "interval min "));
    assert(Contains(run.output, "lost 0"));
}

void TestErrors(Fixture& fixture, const std::string& tool)
{
    auto run = fixture.Run({tool, "echo", "/pm_topic_test/nobody", "--system", fixture.system()}, 0ms);
    assert(run.exit.code == 1);
    assert(Contains(run.output, "declares topic /pm_topic_test/nobody"));

    run = fixture.Run({tool, "hz"}, 0ms);
    assert(run.exit.code == 2);
    assert(Contains(run.output, "usage: puppet_master_topic"));

    run = fixture.Run({tool, "echo", fixture.topic(), "--count", "x"}, 0ms);
    assert(run.exit.code == 2);
}

std::vector<std::string> Drain(const transport::ReaderPtr& reader)
{
    std::vector<std::string> payloads;
    while (true) {
        auto message = reader->Read();
        if (!message.ok()) {
            return payloads;
        }
        payloads.emplace_back(message.value().payload.begin(), message.value().payload.end());
    }
}

void TestRecordInfoReplay(Fixture& fixture, const std::string& tool)
{
    const auto file = fixture.File("capture.pmrec");

    // With no topic named, every topic of the system is recorded.
    auto run = fixture.Run(
        {tool, "record", "-o", file, "--system", fixture.system(), "--count", "5", "--duration-ms", "20000"}, 10ms);
    assert(run.exit.success());
    assert(Contains(run.output, "record " + fixture.topic() + ": 5 messages, 0 lost"));
    assert(Contains(run.output, "record: 5 messages to "));

    run = fixture.Run({tool, "info", file}, 0ms);
    assert(run.exit.success());
    assert(Contains(run.output, ": 1 topics, 5 messages, "));
    assert(Contains(run.output, fixture.topic() + "  test.Text (application/octet-stream)  5 messages, "));
    assert(Contains(run.output, "shm domain 77, 256 B x 8 slots"));

    // As fast as possible: the payloads come back in order.
    {
        auto reader = fixture.Subscribe();
        run = fixture.Run({tool, "replay", file, "--rate", "0"}, 0ms);
        assert(run.exit.success());
        assert(Contains(run.output, "replay: 5 messages"));
        const auto payloads = Drain(reader);
        assert(payloads.size() == 5);
        const int first = std::stoi(payloads.front().substr(6));
        for (int index = 0; index < 5; ++index) {
            assert(payloads[static_cast<std::size_t>(index)] == "hello " + std::to_string(first + index));
        }
    }

    // At the recorded pace: about 10 ms between messages, so 40 ms or more.
    {
        auto reader = fixture.Subscribe();
        const auto started = std::chrono::steady_clock::now();
        run = fixture.Run({tool, "replay", file, fixture.topic()}, 0ms);
        assert(run.exit.success());
        assert(std::chrono::steady_clock::now() - started >= 35ms);
        assert(Drain(reader).size() == 5);
    }

    // Topics left out are not replayed; one not in the recording is an error.
    {
        auto reader = fixture.Subscribe();
        run = fixture.Run({tool, "replay", file, "/pm_topic_test/nobody", "--rate", "0"}, 0ms);
        assert(run.exit.code == 1);
        assert(Contains(run.output, "topic /pm_topic_test/nobody is not in"));
        assert(Contains(run.output, "replay: 0 messages"));
        assert(Drain(reader).empty());
    }

    run = fixture.Run({tool, "info", fixture.File("missing.pmrec")}, 0ms);
    assert(run.exit.code == 1);
    run = fixture.Run({tool, "record", fixture.topic()}, 0ms);  // no -o
    assert(run.exit.code == 2);
    run = fixture.Run({tool, "replay", file, "--rate", "fast"}, 0ms);
    assert(run.exit.code == 2);
}

std::string Hex16(std::uint64_t value)
{
    std::ostringstream text;
    text << std::hex << std::setw(16) << std::setfill('0') << value;
    return text.str();
}

void TestServices(Fixture& fixture, const std::string& tool)
{
    const auto& name = fixture.service_name();

    // No server yet: declared, but down.
    auto run = fixture.Run({tool, "service", "list", "--system", fixture.system()}, 0ms);
    assert(run.exit.success());
    assert(Contains(run.output, name + "  request test.EchoRequest (application/octet-stream), reply "
                                       "test.EchoReply (application/octet-stream)  shm domain 77"));
    assert(Contains(run.output, "  server: down (no heartbeat in 300 ms)"));
    assert(Contains(run.output, "  servers: producer/producer"));
    assert(Contains(run.output, "  clients: consumer/consumer"));

    // A server in this process: upper-cases the request, refuses "fail".
    runtime::RuntimeOptions options;
    options.register_inmemory_transport = false;
    auto context = runtime::RuntimeContext::Create(options);
    assert(context.ok());
    shm::Options shm_options;
    shm_options.domain_id = kDomain;
    assert(shm::RegisterTransport(*context.value(), core::TransportName::Create("shm").value(), shm_options).ok());
    auto server = service::Server::Create(*context.value(), fixture.Service(), [](const service::Request& request) {
        std::string text(request.payload.begin(), request.payload.end());
        if (text == "fail") {
            return core::Result<transport::ByteBuffer>::FromStatus(core::Status::InvalidArgument("no fail"));
        }
        for (auto& c : text) {
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
        return core::Result<transport::ByteBuffer>(transport::ByteBuffer(text.begin(), text.end()));
    });
    assert(server.ok());
    const auto instance = Hex16(server.value()->instance_id());

    run = fixture.Run({tool, "service", "list", "--system", fixture.system()}, 0ms);
    assert(run.exit.success());
    assert(Contains(run.output, "  server: up, instance " + instance + ", heartbeat "));

    run = fixture.Run({tool, "service", "call", name, "--system", fixture.system(), "--data", "hi there", "--text"},
        0ms);
    assert(run.exit.success());
    assert(Contains(run.output, "call " + name + ": server " + instance + ", request 8 B"));
    assert(Contains(run.output, "reply 8 B in "));
    assert(Contains(run.output, " ms  HI THERE"));

    run = fixture.Run({tool, "service", "call", name, "--system", fixture.system(), "--hex", "68:69"}, 0ms);
    assert(run.exit.success());
    assert(Contains(run.output, " ms  48 49"));

    // The handler's error, with its message.
    run = fixture.Run({tool, "service", "call", name, "--system", fixture.system(), "--data", "fail"}, 0ms);
    assert(run.exit.code == 1);
    assert(Contains(run.output, "invalid_argument: no fail"));

    run = fixture.Run({tool, "service", "call", "/pm_topic_test/nobody", "--system", fixture.system()}, 0ms);
    assert(run.exit.code == 1);
    assert(Contains(run.output, "declares service /pm_topic_test/nobody"));
    run = fixture.Run({tool, "service", "list"}, 0ms);
    assert(run.exit.code == 2);
    assert(Contains(run.output, "needs --system FILE"));
    run = fixture.Run({tool, "service", "call", name, "--system", fixture.system(), "--hex", "6"}, 0ms);
    assert(run.exit.code == 2);

    // Stopped in order before the tool started: its goodbye is still the
    // newest presence message, which the tool reads from the segment. A
    // presence writer kept open keeps the segment alive on Windows too.
    const auto endpoints = service::EndpointsFor(fixture.Service());
    assert(endpoints.ok());
    auto presence = context.value()->CreateWriter(endpoints.value().presence);
    assert(presence.ok());
    server.value()->Close();
    run = fixture.Run({tool, "service", "list", "--system", fixture.system()}, 0ms);
    assert(run.exit.success());
    assert(Contains(run.output, "  server: stopped (instance " + instance + " said goodbye "));
    run = fixture.Run(
        {tool, "service", "call", name, "--system", fixture.system(), "--data", "x", "--timeout-ms", "500"}, 0ms);
    assert(run.exit.code == 1);
    assert(Contains(run.output, "unavailable: "));

    // Gone without a goodbye, as after a crash: its last heartbeat, a second
    // old, is the newest presence message.
    unsigned char heartbeat[16] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x08, 5};
    transport::WriteOptions old;
    old.source_timestamp = std::chrono::time_point_cast<core::Nanoseconds>(core::SteadyClock::now() - 1s);
    assert(presence.value()->Write(transport::ByteView::From(heartbeat, sizeof(heartbeat)), old).ok());
    run = fixture.Run({tool, "service", "list", "--system", fixture.system()}, 0ms);
    assert(run.exit.success());
    assert(Contains(run.output,
        "  server: down (instance 0877665544332211 stopped without a goodbye; last heartbeat "));
}

#if !defined(_WIN32)
void TestListFromSegments(Fixture& fixture, const std::string& tool)
{
    fixture.Publish("first");
    const auto run = fixture.Run({tool, "list", "--domain", "77"}, 0ms);
    assert(run.exit.success());
    assert(Contains(run.output, fixture.topic() + "  test.Text (application/octet-stream)  domain 77, 256 B x 8 slots"));
    assert(Contains(run.output, "in_use"));
}

void TestMissingSegment(Fixture& fixture, const std::string& tool)
{
    const auto run = fixture.Run({tool, "echo", "/pm_topic_test/nobody", "--domain", "77"}, 0ms);
    assert(run.exit.code == 1);
    assert(Contains(run.output, "no shm segment for topic /pm_topic_test/nobody"));
}
#endif

}  // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::cerr << "usage: puppet_master_topic_tool_test <puppet_master_topic>\n";
        return 2;
    }
    const std::string tool = fs::absolute(argv[1]).string();
    Fixture fixture;

    TestListFromSystem(fixture, tool);
    TestEcho(fixture, tool, true);
    TestEchoHex(fixture, tool);
    TestEchoTimesOut(fixture, tool);
    TestHz(fixture, tool);
    TestErrors(fixture, tool);
    TestRecordInfoReplay(fixture, tool);
    TestServices(fixture, tool);
#if !defined(_WIN32)
    // Segment headers can only be read on Linux.
    TestListFromSegments(fixture, tool);
    TestEcho(fixture, tool, false);
    TestMissingSegment(fixture, tool);
#endif

    std::cout << "topic tool tests passed\n";
    return 0;
}
