// Recordings: round trip, cut-off files, and malformed input.

#include <cassert>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <puppet_master/transport/recording.h>

namespace core = puppet_master::core;
namespace fs = std::filesystem;
namespace transport = puppet_master::transport;

using namespace std::chrono_literals;

namespace {

class TempFile {
public:
    explicit TempFile(const std::string& name)
        : path_(fs::temp_directory_path()
                / ("pm_recording_" + name + "_"
                   + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".pmrec"))
    {
    }
    ~TempFile()
    {
        std::error_code ignored;
        fs::remove(path_, ignored);
    }

    std::string str() const { return path_.string(); }

    std::vector<char> Bytes() const
    {
        std::ifstream in(path_, std::ios::binary);
        return std::vector<char>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

    void Replace(const std::vector<char>& bytes) const
    {
        std::ofstream(path_, std::ios::binary | std::ios::trunc).write(bytes.data(), bytes.size());
    }

private:
    fs::path path_;
};

transport::ByteView View(const std::string& text)
{
    return transport::ByteView::From(text.data(), text.size());
}

std::string Text(const transport::ByteBuffer& bytes)
{
    return std::string(bytes.begin(), bytes.end());
}

transport::RecordedTopic Topic(const std::string& name, const std::string& type)
{
    transport::RecordedTopic topic;
    topic.name = name;
    topic.descriptor = transport::MessageDescriptor {type, "application/x-test", 3};
    return topic;
}

// Writes two topics and three messages, the last one empty.
void WriteSample(const TempFile& file)
{
    auto writer = transport::RecordingWriter::Create(file.str());
    assert(writer.ok());
    auto speed = Topic("/demo/speed", "demo.Speed");
    speed.parameters = {{"transport", "shm"}, {"slot_count", "8"}};
    const auto speed_id = writer.value()->AddTopic(speed);
    assert(speed_id.ok() && speed_id.value() == 1);
    assert(writer.value()->Write(1, 0ns, 10, View("first")).ok());
    const auto text_id = writer.value()->AddTopic(Topic("/demo/text", "demo.Text"));
    assert(text_id.ok() && text_id.value() == 2);
    assert(writer.value()->Write(2, 1500us, 4, View("second")).ok());
    assert(writer.value()->Write(1, 3ms, 11, View("")).ok());
    assert(writer.value()->message_count() == 3);
    assert(writer.value()->Close().ok());
    assert(writer.value()->Close().ok());  // twice is fine
    assert(writer.value()->Write(1, 4ms, 12, View("late")).code() == core::StatusCode::kFailedPrecondition);
}

void TestRoundTrip()
{
    TempFile file("round_trip");
    WriteSample(file);

    auto reader = transport::RecordingReader::Open(file.str());
    assert(reader.ok());
    auto& recording = *reader.value();

    auto first = recording.Next();
    assert(first.ok() && first.value());
    assert(first.value()->topic == 1 && first.value()->time == 0ns && first.value()->sequence == 10);
    assert(Text(first.value()->payload) == "first");
    assert(recording.topics().size() == 1);  // the second is not read yet

    auto second = recording.Next();
    assert(second.ok() && second.value());
    assert(second.value()->topic == 2 && second.value()->time == 1500us && second.value()->sequence == 4);
    assert(Text(second.value()->payload) == "second");

    auto third = recording.Next();
    assert(third.ok() && third.value() && third.value()->payload.empty() && third.value()->time == 3ms);

    auto end = recording.Next();
    assert(end.ok() && !end.value());
    assert(!recording.truncated());
    assert(recording.Next().ok());  // stays at the end

    assert(recording.topics().size() == 2);
    const auto* speed = recording.FindTopic(1);
    assert(speed != nullptr && speed->name == "/demo/speed");
    assert(speed->descriptor == (transport::MessageDescriptor {"demo.Speed", "application/x-test", 3}));
    assert(speed->parameters.at("transport") == "shm" && speed->parameters.at("slot_count") == "8");
    assert(recording.FindTopic(3) == nullptr);
}

void TestCutOffRecording()
{
    TempFile file("cut");
    WriteSample(file);
    auto bytes = file.Bytes();
    // The last record is an 8-byte header and a 20-byte body (empty payload):
    // cut inside its body, then inside its header.
    for (const std::size_t cut : {std::size_t {10}, std::size_t {25}}) {
        file.Replace(std::vector<char>(bytes.begin(), bytes.end() - static_cast<std::ptrdiff_t>(cut)));
        auto reader = transport::RecordingReader::Open(file.str());
        assert(reader.ok());
        int messages = 0;
        while (true) {
            auto next = reader.value()->Next();
            assert(next.ok());
            if (!next.value()) {
                break;
            }
            ++messages;
        }
        assert(messages == 2);
        assert(reader.value()->truncated());
    }
}

void TestMalformed()
{
    TempFile file("bad");
    file.Replace({'n', 'o', 't', ' ', 'a', ' ', 'r', 'e', 'c', 'o', 'r', 'd', 'i', 'n', 'g', '!'});
    assert(transport::RecordingReader::Open(file.str()).status().code() == core::StatusCode::kInvalidArgument);

    WriteSample(file);
    auto bytes = file.Bytes();
    bytes[8] = 9;  // format version
    file.Replace(bytes);
    assert(transport::RecordingReader::Open(file.str()).status().code() == core::StatusCode::kUnsupported);

    // A garbage record size larger than the file ends the read as cut off,
    // without trying to allocate it.
    WriteSample(file);
    bytes = file.Bytes();
    bytes[16 + 4] = '\xff';
    bytes[16 + 5] = '\xff';
    bytes[16 + 6] = '\xff';
    bytes[16 + 7] = '\x7f';
    file.Replace(bytes);
    auto reader = transport::RecordingReader::Open(file.str());
    assert(reader.ok());
    auto next = reader.value()->Next();
    assert(next.ok() && !next.value() && reader.value()->truncated());

    // A message whose topic was never declared.
    {
        auto writer = transport::RecordingWriter::Create(file.str());
        assert(writer.ok());
        assert(writer.value()->AddTopic(Topic("/demo/a", "demo.A")).ok());
        assert(writer.value()->Write(1, 0ns, 1, View("x")).ok());
        assert(writer.value()->Close().ok());
    }
    bytes = file.Bytes();
    // First record is the topic; find the message record's topic id: the
    // last 1 + 20 bytes are its body.
    bytes[bytes.size() - 21] = 5;
    file.Replace(bytes);
    reader = transport::RecordingReader::Open(file.str());
    assert(reader.ok());
    next = reader.value()->Next();
    assert(next.status().code() == core::StatusCode::kDataLoss);

    assert(transport::RecordingReader::Open(file.str() + ".missing").status().code() == core::StatusCode::kNotFound);
}

void TestWriterChecks()
{
    TempFile file("checks");
    auto writer = transport::RecordingWriter::Create(file.str());
    assert(writer.ok());
    assert(writer.value()->Write(1, 0ns, 1, View("x")).code() == core::StatusCode::kInvalidArgument);
    assert(!writer.value()->AddTopic(Topic("", "demo.A")).ok());
    assert(!writer.value()->AddTopic(Topic("/demo/a", "")).ok());
    assert(writer.value()->AddTopic(Topic("/demo/a", "demo.A")).ok());
    assert(writer.value()->Write(1, -1ns, 1, View("x")).code() == core::StatusCode::kInvalidArgument);
    assert(writer.value()->Flush().ok());

    // Flushed records are readable while the writer is still open.
    auto reader = transport::RecordingReader::Open(file.str());
    assert(reader.ok());
    assert(reader.value()->Next().ok());
    assert(reader.value()->topics().size() == 1);

    assert(transport::RecordingWriter::Create((fs::path(file.str()) / "no" / "dir.pmrec").string()).status().code()
           == core::StatusCode::kUnavailable);
}

}  // namespace

int main()
{
    TestRoundTrip();
    TestCutOffRecording();
    TestMalformed();
    TestWriterChecks();
    return 0;
}
