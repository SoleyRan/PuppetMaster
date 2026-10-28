#include <puppet_master/transport/recording.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>
#include <utility>

// Layout (docs/recording.md), little-endian throughout:
//   file header  "PMREC001", u32 format version, u32 reserved
//   record       u32 kind, u32 body size, body
//   topic body   u32 id, str name, str type, str encoding, u32 version,
//                u32 parameter count, (str key, str value)...
//   message body u32 topic, i64 time ns, u64 sequence, payload (the rest)
//   str          u32 size, bytes
namespace puppet_master::transport {

namespace {

constexpr char kMagic[8] = {'P', 'M', 'R', 'E', 'C', '0', '0', '1'};
constexpr std::uint32_t kFormatVersion = 1;
constexpr std::size_t kFileHeaderSize = 16;
constexpr std::size_t kRecordHeaderSize = 8;
constexpr std::uint32_t kTopicRecord = 1;
constexpr std::uint32_t kMessageRecord = 2;
constexpr std::size_t kMessageFixedSize = 4 + 8 + 8;
constexpr std::size_t kMaxRecordSize = std::numeric_limits<std::uint32_t>::max();

void PutU32(ByteBuffer& out, std::uint32_t value)
{
    for (int shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<core::Byte>(value >> shift));
    }
}

void PutU64(ByteBuffer& out, std::uint64_t value)
{
    for (int shift = 0; shift < 64; shift += 8) {
        out.push_back(static_cast<core::Byte>(value >> shift));
    }
}

void PutString(ByteBuffer& out, const std::string& text)
{
    PutU32(out, static_cast<std::uint32_t>(text.size()));
    out.insert(out.end(), text.begin(), text.end());
}

std::uint32_t GetU32(const core::Byte* data)
{
    std::uint32_t value = 0;
    for (int index = 3; index >= 0; --index) {
        value = (value << 8) | data[index];
    }
    return value;
}

std::uint64_t GetU64(const core::Byte* data)
{
    std::uint64_t value = 0;
    for (int index = 7; index >= 0; --index) {
        value = (value << 8) | data[index];
    }
    return value;
}

// Bounds-checked reads from one record body.
class BodyReader {
public:
    explicit BodyReader(const ByteBuffer& body) : body_(body) {}

    bool U32(std::uint32_t& value)
    {
        if (left() < 4) {
            return false;
        }
        value = GetU32(body_.data() + offset_);
        offset_ += 4;
        return true;
    }

    bool String(std::string& text)
    {
        std::uint32_t size = 0;
        if (!U32(size) || left() < size) {
            return false;
        }
        text.assign(reinterpret_cast<const char*>(body_.data() + offset_), size);
        offset_ += size;
        return true;
    }

private:
    std::size_t left() const noexcept { return body_.size() - offset_; }

    const ByteBuffer& body_;
    std::size_t offset_ {0};
};

bool TooLong(const std::string& text)
{
    return text.size() > std::numeric_limits<std::uint32_t>::max();
}

}  // namespace

// --- writer -------------------------------------------------------------

struct RecordingWriter::Impl {
    std::string path;
    std::ofstream out;
    std::vector<std::uint32_t> topics;  // declared ids, in order
    std::uint64_t messages {0};
    ByteBuffer scratch;
    bool closed {false};

    core::Status Failed(const std::string& what) const
    {
        return core::Status::Unavailable(what + " " + path + " failed");
    }

    core::Status Emit(std::uint32_t kind, const ByteBuffer& body, ByteView tail = {})
    {
        if (closed) {
            return core::Status::FailedPrecondition("recording " + path + " is closed");
        }
        const std::size_t size = body.size() + tail.size();
        if (size > kMaxRecordSize) {
            return core::Status::InvalidArgument("record of " + std::to_string(size) + " bytes is too large");
        }
        ByteBuffer header;
        PutU32(header, kind);
        PutU32(header, static_cast<std::uint32_t>(size));
        out.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));
        out.write(reinterpret_cast<const char*>(body.data()), static_cast<std::streamsize>(body.size()));
        if (tail.size() != 0) {
            out.write(reinterpret_cast<const char*>(tail.data()), static_cast<std::streamsize>(tail.size()));
        }
        return out ? core::Status::Ok() : Failed("writing");
    }
};

RecordingWriter::RecordingWriter(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

RecordingWriter::~RecordingWriter()
{
    Close();
}

core::Result<std::unique_ptr<RecordingWriter>> RecordingWriter::Create(const std::string& path)
{
    using ResultType = core::Result<std::unique_ptr<RecordingWriter>>;
    auto impl = std::make_unique<Impl>();
    impl->path = path;
    impl->out.open(path, std::ios::binary | std::ios::trunc);
    if (!impl->out) {
        return ResultType::FromStatus(core::Status::Unavailable("cannot create recording " + path));
    }
    ByteBuffer header(kMagic, kMagic + sizeof(kMagic));
    PutU32(header, kFormatVersion);
    PutU32(header, 0);
    impl->out.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));
    if (!impl->out) {
        return ResultType::FromStatus(impl->Failed("writing"));
    }
    return ResultType(std::unique_ptr<RecordingWriter>(new RecordingWriter(std::move(impl))));
}

core::Result<std::uint32_t> RecordingWriter::AddTopic(const RecordedTopic& topic)
{
    using ResultType = core::Result<std::uint32_t>;
    auto status = core::ValidateName("topic name", topic.name);
    if (status.ok()) {
        status = topic.descriptor.Validate();
    }
    if (!status.ok()) {
        return ResultType::FromStatus(std::move(status));
    }
    const auto id = static_cast<std::uint32_t>(impl_->topics.size() + 1);
    ByteBuffer body;
    PutU32(body, id);
    PutString(body, topic.name);
    PutString(body, topic.descriptor.type_name);
    PutString(body, topic.descriptor.encoding);
    PutU32(body, topic.descriptor.version);
    PutU32(body, static_cast<std::uint32_t>(topic.parameters.size()));
    for (const auto& entry : topic.parameters) {
        if (TooLong(entry.first) || TooLong(entry.second)) {
            return ResultType::FromStatus(core::Status::InvalidArgument("topic parameter is too long"));
        }
        PutString(body, entry.first);
        PutString(body, entry.second);
    }
    status = impl_->Emit(kTopicRecord, body);
    if (!status.ok()) {
        return ResultType::FromStatus(std::move(status));
    }
    impl_->topics.push_back(id);
    return ResultType(id);
}

core::Status RecordingWriter::Write(
    std::uint32_t topic, core::Nanoseconds time, core::SequenceNumber sequence, ByteView payload)
{
    if (topic == 0 || topic > impl_->topics.size()) {
        return core::Status::InvalidArgument("topic id " + std::to_string(topic) + " was not added");
    }
    if (time.count() < 0) {
        return core::Status::InvalidArgument("message time must not be negative");
    }
    auto& body = impl_->scratch;
    body.clear();
    PutU32(body, topic);
    PutU64(body, static_cast<std::uint64_t>(time.count()));
    PutU64(body, sequence);
    auto status = impl_->Emit(kMessageRecord, body, payload);
    if (status.ok()) {
        ++impl_->messages;
    }
    return status;
}

core::Status RecordingWriter::Flush()
{
    if (impl_->closed) {
        return core::Status::FailedPrecondition("recording " + impl_->path + " is closed");
    }
    impl_->out.flush();
    return impl_->out ? core::Status::Ok() : impl_->Failed("flushing");
}

core::Status RecordingWriter::Close()
{
    if (impl_->closed) {
        return core::Status::Ok();
    }
    impl_->closed = true;
    impl_->out.close();
    return impl_->out ? core::Status::Ok() : impl_->Failed("closing");
}

std::uint64_t RecordingWriter::message_count() const noexcept
{
    return impl_->messages;
}

// --- reader -------------------------------------------------------------

struct RecordingReader::Impl {
    std::string path;
    std::ifstream in;
    std::vector<RecordedTopic> topics;
    ByteBuffer body;
    std::uint64_t file_size {0};
    std::uint64_t position {kFileHeaderSize};  // of the next record
    bool truncated {false};
    bool ended {false};

    core::Status Corrupt(const std::string& what) const
    {
        return core::Status::DataLoss("recording " + path + ": " + what);
    }

    // Reads exactly `size` bytes; false at the end of the file.
    bool ReadExactly(core::Byte* data, std::size_t size)
    {
        in.read(reinterpret_cast<char*>(data), static_cast<std::streamsize>(size));
        return static_cast<std::size_t>(in.gcount()) == size;
    }

    core::Status DecodeTopic()
    {
        BodyReader reader(body);
        RecordedTopic topic;
        std::uint32_t count = 0;
        if (!reader.U32(topic.id) || !reader.String(topic.name) || !reader.String(topic.descriptor.type_name) ||
            !reader.String(topic.descriptor.encoding) || !reader.U32(topic.descriptor.version) ||
            !reader.U32(count)) {
            return Corrupt("malformed topic record");
        }
        for (std::uint32_t index = 0; index < count; ++index) {
            std::string key;
            std::string value;
            if (!reader.String(key) || !reader.String(value)) {
                return Corrupt("malformed topic record");
            }
            topic.parameters[std::move(key)] = std::move(value);
        }
        if (topic.id == 0 || FindTopic(topic.id) != nullptr) {
            return Corrupt("topic id " + std::to_string(topic.id) + " is invalid or declared twice");
        }
        topics.push_back(std::move(topic));
        return core::Status::Ok();
    }

    const RecordedTopic* FindTopic(std::uint32_t id) const noexcept
    {
        for (const auto& topic : topics) {
            if (topic.id == id) {
                return &topic;
            }
        }
        return nullptr;
    }
};

RecordingReader::RecordingReader(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

RecordingReader::~RecordingReader() = default;

core::Result<std::unique_ptr<RecordingReader>> RecordingReader::Open(const std::string& path)
{
    using ResultType = core::Result<std::unique_ptr<RecordingReader>>;
    auto impl = std::make_unique<Impl>();
    impl->path = path;
    impl->in.open(path, std::ios::binary | std::ios::ate);
    if (!impl->in) {
        return ResultType::FromStatus(core::Status::NotFound("cannot open recording " + path));
    }
    impl->file_size = static_cast<std::uint64_t>(impl->in.tellg());
    impl->in.seekg(0);
    core::Byte header[kFileHeaderSize] {};
    if (!impl->ReadExactly(header, sizeof(header)) || std::memcmp(header, kMagic, sizeof(kMagic)) != 0) {
        return ResultType::FromStatus(core::Status::InvalidArgument(path + " is not a PuppetMaster recording"));
    }
    const auto version = GetU32(header + 8);
    if (version != kFormatVersion) {
        return ResultType::FromStatus(core::Status::Unsupported(
            "recording " + path + " has format version " + std::to_string(version) + ", expected "
            + std::to_string(kFormatVersion)));
    }
    return ResultType(std::unique_ptr<RecordingReader>(new RecordingReader(std::move(impl))));
}

core::Result<std::optional<RecordedMessage>> RecordingReader::Next()
{
    using ResultType = core::Result<std::optional<RecordedMessage>>;
    auto& impl = *impl_;
    while (!impl.ended) {
        core::Byte header[kRecordHeaderSize] {};
        impl.in.read(reinterpret_cast<char*>(header), sizeof(header));
        const auto got = static_cast<std::size_t>(impl.in.gcount());
        if (got != sizeof(header)) {
            // A clean end falls between records; anything else was cut off.
            impl.truncated = got != 0;
            impl.ended = true;
            break;
        }
        const auto kind = GetU32(header);
        const auto size = GetU32(header + 4);
        impl.position += sizeof(header);
        // A size past the end of the file is a cut-off record; checking it
        // first keeps a garbage size from allocating gigabytes.
        if (size > impl.file_size - std::min(impl.file_size, impl.position)) {
            impl.truncated = true;
            impl.ended = true;
            break;
        }
        impl.body.resize(size);
        if (!impl.ReadExactly(impl.body.data(), size)) {
            impl.truncated = true;
            impl.ended = true;
            break;
        }
        impl.position += size;
        if (kind == kTopicRecord) {
            auto status = impl.DecodeTopic();
            if (!status.ok()) {
                return ResultType::FromStatus(std::move(status));
            }
            continue;
        }
        if (kind != kMessageRecord) {
            continue;  // a newer record kind: skip it
        }
        if (size < kMessageFixedSize) {
            return ResultType::FromStatus(impl.Corrupt("malformed message record"));
        }
        RecordedMessage message;
        message.topic = GetU32(impl.body.data());
        const auto time = GetU64(impl.body.data() + 4);
        if (time > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            return ResultType::FromStatus(impl.Corrupt("message time out of range"));
        }
        message.time = core::Nanoseconds(static_cast<std::int64_t>(time));
        message.sequence = GetU64(impl.body.data() + 12);
        if (impl.FindTopic(message.topic) == nullptr) {
            return ResultType::FromStatus(
                impl.Corrupt("message of undeclared topic id " + std::to_string(message.topic)));
        }
        message.payload.assign(impl.body.begin() + kMessageFixedSize, impl.body.end());
        return ResultType(std::optional<RecordedMessage>(std::move(message)));
    }
    return ResultType(std::optional<RecordedMessage>());
}

const std::vector<RecordedTopic>& RecordingReader::topics() const noexcept
{
    return impl_->topics;
}

const RecordedTopic* RecordingReader::FindTopic(std::uint32_t id) const noexcept
{
    return impl_->FindTopic(id);
}

bool RecordingReader::truncated() const noexcept
{
    return impl_->truncated;
}

}  // namespace puppet_master::transport
