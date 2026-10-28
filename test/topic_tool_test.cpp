// puppet_master_topic against a live shared-memory topic: this process
// writes, the tool runs as a child process and its output is checked.
//
//     puppet_master_topic_tool_test <path to puppet_master_topic>

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <puppet_master/configuration/supervisor.h>
#include <puppet_master/transport/shm/shm_transport.h>

namespace config = puppet_master::configuration;
namespace core = puppet_master::core;
namespace fs = std::filesystem;
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
          topic_("/pm_topic_test/text_" + stamp_)
    {
        WriteFile(root_ / "share/test_interfaces/topics.json",
            R"({"topics": [{"id": "text", "name": ")" + topic_ + R"(", "message_type": "test.Text"}]})");
        WriteFile(root_ / "share/producer/producer.json", R"({
            "imports": ["test_interfaces/topics.json"],
            "components": [{"name": "producer", "writers": ["text"], "triggers": [{"kind": "manual"}]}]})");
        WriteFile(root_ / "share/consumer/consumer.json", R"({
            "imports": ["test_interfaces/topics.json"],
            "components": [{"name": "consumer", "readers": ["text"],
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
        const transport::EndpointConfig endpoint {
            core::TopicSpec {core::TopicName::Create(topic_).value(), core::TransportKind::kIpc, {}},
            transport::MessageDescriptor {"test.Text", "application/octet-stream", 0}};
        auto writer = transport_->CreateWriter(endpoint);
        assert(writer.ok());
        writer_ = writer.value();
    }

    ~Fixture()
    {
        writer_.reset();
        transport_.reset();
        shm::RemoveSegment(kDomain, topic_);
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
#if !defined(_WIN32)
    // Segment headers can only be read on Linux.
    TestListFromSegments(fixture, tool);
    TestEcho(fixture, tool, false);
    TestMissingSegment(fixture, tool);
#endif

    std::cout << "topic tool tests passed\n";
    return 0;
}
