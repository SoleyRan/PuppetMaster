// puppet_master_topic against a live FastDDS topic: this process writes and
// reads through its own participant, the tool runs as a child process and
// its output is checked.
//
//     puppet_master_topic_fastdds_test <path to puppet_master_topic>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <puppet_master/configuration/supervisor.h>
#include <puppet_master/runtime/context.h>
#include <puppet_master/service/service.h>
#include <puppet_master/transport/fastdds/fastdds_transport.h>

namespace config = puppet_master::configuration;
namespace core = puppet_master::core;
namespace fastdds = puppet_master::transport::fastdds;
namespace fs = std::filesystem;
namespace runtime = puppet_master::runtime;
namespace service = puppet_master::service;
namespace transport = puppet_master::transport;

using namespace std::chrono_literals;

namespace {

constexpr core::DomainId kDomain = 79;

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

// A one-node system whose producer writes a reliable FastDDS `topic`, plus a
// participant in this process on the same domain.
class Fixture {
public:
    Fixture()
        : stamp_(std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())),
          root_(fs::temp_directory_path() / ("pm_topic_fastdds_" + stamp_)),
          topic_("/pm_topic_test/dds_" + stamp_)
    {
        WriteFile(root_ / "share/test_interfaces/topics.json",
            R"({"topics": [{"id": "text", "name": ")" + topic_ +
                R"(", "message_type": "test.Text", "transport": "fastdds",
                    "message_policy": {"delivery": "reliable"}}],
                "services": [{"id": "echo", "name": ")" + topic_ + R"(/echo", "transport": "fastdds",
                              "request_type": "test.EchoRequest", "reply_type": "test.EchoReply"}]})");
        WriteFile(root_ / "share/producer/producer.json", R"({
            "imports": ["test_interfaces/topics.json"],
            "components": [{"name": "producer", "writers": ["text"], "servers": ["echo"],
                            "triggers": [{"kind": "manual"}]}]})");
        WriteFile(root_ / "system.json", R"({
            "name": "topic_fastdds_test", "config_paths": ["share"],
            "nodes": [{"name": "producer", "executable": "producer_node", "config": "producer/producer.json"}]})");

        fastdds::Options options;
        options.domain_id = kDomain;
        options.participant_name = "topic_tool_test";
        transport_ = std::make_unique<fastdds::FastDdsTransport>(
            core::TransportName::Create("test").value(), options);
        assert(transport_->Open().ok());
        auto writer = transport_->CreateWriter(Endpoint());
        assert(writer.ok());
        writer_ = writer.value();
    }

    transport::EndpointConfig Endpoint() const
    {
        core::MessagePolicy policy;
        policy.delivery = core::DeliveryGuarantee::kReliable;
        policy.freshness = core::FreshnessPolicy::kQueued;
        policy.queue_depth = 16;
        return transport::EndpointConfig {
            core::TopicSpec {core::TopicName::Create(topic_).value(), core::TransportKind::kFastDds, policy},
            transport::MessageDescriptor {"test.Text", "application/octet-stream", 0}};
    }

    transport::ReaderPtr Subscribe()
    {
        auto reader = transport_->CreateReader(Endpoint());
        assert(reader.ok());
        return reader.value();
    }

    ~Fixture()
    {
        writer_.reset();
        transport_.reset();
        std::error_code ignored;
        fs::remove_all(root_, ignored);
    }

    const std::string& topic() const noexcept { return topic_; }
    std::string system() const { return (root_ / "system.json").string(); }
    std::string File(const std::string& name) const { return (root_ / name).string(); }

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
    // it exits. The first messages may go out before the tool's reader has
    // matched; the tests only count what arrives.
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
    std::unique_ptr<fastdds::FastDdsTransport> transport_;
    transport::WriterPtr writer_;
    int runs_ {0};
};

void TestList(Fixture& fixture, const std::string& tool)
{
    const auto run = fixture.Run({tool, "list", "--system", fixture.system(), "--domain", "79"}, 0ms);
    assert(run.exit.success());
    assert(Contains(run.output, fixture.topic() + "  test.Text (application/octet-stream)  fastdds domain 79, reliable"));
    assert(Contains(run.output, "writers: producer/producer"));
}

void TestEcho(Fixture& fixture, const std::string& tool)
{
    const auto run = fixture.Run({tool, "echo", fixture.topic(), "--system", fixture.system(), "--domain", "79",
                                  "--count", "3", "--timeout-ms", "20000", "--text"},
        10ms);
    assert(run.exit.success());
    assert(Contains(run.output, "echo " + fixture.topic() + ": test.Text (application/octet-stream)"));
    assert(Contains(run.output, "  hello "));
    // No writer sequence numbers or publish times on FastDDS.
    assert(!Contains(run.output, "latency"));
    assert(Contains(run.output, "echo: 3 received (losses are not visible on fastdds)"));
}

void TestHz(Fixture& fixture, const std::string& tool)
{
    const auto run = fixture.Run({tool, "hz", fixture.topic(), "--system", fixture.system(), "--domain", "79",
                                  "--window-ms", "500", "--duration-ms", "2000"},
        10ms);
    assert(run.exit.success());
    assert(Contains(run.output, "rate "));
    assert(Contains(run.output, " ms (by arrival)"));
    assert(!Contains(run.output, "lost"));
}

// Up to `count` payloads, waiting a while for each.
std::vector<std::string> Drain(const transport::ReaderPtr& reader, std::size_t count)
{
    std::vector<std::string> payloads;
    while (payloads.size() < count) {
        auto message = reader->Read(transport::ReadOptions {true, std::chrono::seconds(5)});
        if (!message.ok()) {
            break;
        }
        payloads.emplace_back(message.value().payload.begin(), message.value().payload.end());
    }
    return payloads;
}

void TestRecordInfoReplay(Fixture& fixture, const std::string& tool)
{
    const auto file = fixture.File("capture.pmrec");
    auto run = fixture.Run({tool, "record", "-o", file, "--system", fixture.system(), "--domain", "79",
                            "--count", "5", "--duration-ms", "20000"},
        10ms);
    assert(run.exit.success());
    assert(Contains(run.output, "record " + fixture.topic() + ": 5 messages\n"));
    assert(Contains(run.output, "record: 5 messages to "));

    run = fixture.Run({tool, "info", file}, 0ms);
    assert(run.exit.success());
    assert(Contains(run.output, fixture.topic() + "  test.Text (application/octet-stream)  5 messages, "));
    assert(Contains(run.output, "fastdds domain 79, reliable"));

    // The replay waits for this reader to match and for it to acknowledge
    // every message before closing, so all five arrive, in order.
    auto reader = fixture.Subscribe();
    run = fixture.Run({tool, "replay", file, "--rate", "0"}, 0ms);
    assert(run.exit.success());
    assert(Contains(run.output, "replay: 5 messages"));
    const auto payloads = Drain(reader, 5);
    assert(payloads.size() == 5);
    const int first = std::stoi(payloads.front().substr(6));
    for (int index = 0; index < 5; ++index) {
        assert(payloads[static_cast<std::size_t>(index)] == "hello " + std::to_string(first + index));
    }
}

// A FastDDS service served from this process, seen and called by the tool.
void TestService(Fixture& fixture, const std::string& tool)
{
    const auto name = fixture.topic() + "/echo";
    runtime::RuntimeOptions options;
    options.register_inmemory_transport = false;
    auto context = runtime::RuntimeContext::Create(options);
    assert(context.ok());
    fastdds::Options dds;
    dds.domain_id = kDomain;
    dds.participant_name = "topic_tool_service_test";
    assert(fastdds::RegisterTransport(*context.value(), core::TransportName::Create("dds").value(), dds).ok());
    service::ServiceSpec spec(core::TopicName::Create(name).value());
    spec.transport = core::TransportKind::kFastDds;
    spec.request = transport::MessageDescriptor {"test.EchoRequest", "application/octet-stream", 0};
    spec.reply = transport::MessageDescriptor {"test.EchoReply", "application/octet-stream", 0};
    auto server = service::Server::Create(*context.value(), spec, [](const service::Request& request) {
        transport::ByteBuffer reply(request.payload.begin(), request.payload.end());
        std::reverse(reply.begin(), reply.end());
        return core::Result<transport::ByteBuffer>(std::move(reply));
    });
    assert(server.ok());

    auto run = fixture.Run({tool, "service", "list", "--system", fixture.system(), "--domain", "79"}, 0ms);
    assert(run.exit.success());
    assert(Contains(run.output, name + "  request test.EchoRequest (application/octet-stream), reply "
                                       "test.EchoReply (application/octet-stream)  fastdds domain 79"));
    assert(Contains(run.output, "  server: up, instance "));

    run = fixture.Run({tool, "service", "call", name, "--system", fixture.system(), "--domain", "79", "--data",
                       "abc", "--text", "--timeout-ms", "5000"},
        0ms);
    assert(run.exit.success());
    assert(Contains(run.output, " ms  cba"));
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::cerr << "usage: puppet_master_topic_fastdds_test <puppet_master_topic>\n";
        return 2;
    }
    const std::string tool = fs::absolute(argv[1]).string();
    Fixture fixture;

    TestList(fixture, tool);
    TestEcho(fixture, tool);
    TestHz(fixture, tool);
    TestRecordInfoReplay(fixture, tool);
    TestService(fixture, tool);

    std::cout << "topic tool FastDDS tests passed\n";
    return 0;
}
