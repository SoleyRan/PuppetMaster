#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <puppet_master/core/result.h>
#include <puppet_master/core/status.h>
#include <puppet_master/core/types.h>
#include <puppet_master/export.h>
#include <puppet_master/transport/message.h>

// Recordings: messages of several topics in one file, in arrival order, for
// replaying them later or reading them from a program. Written by
// `puppet_master_topic record`. The format is documented in
// docs/recording.md; it does not depend on a transport.
namespace puppet_master::transport {

// One topic of a recording.
struct RecordedTopic {
    std::uint32_t id {0};  // assigned by RecordingWriter::AddTopic
    std::string name;
    MessageDescriptor descriptor;
    // Free-form settings, such as the transport the topic was recorded from
    // and its shared-memory ring size, so a replay can recreate it.
    std::map<std::string, std::string> parameters;
};

struct RecordedMessage {
    std::uint32_t topic {0};       // RecordedTopic::id
    core::Nanoseconds time {0};    // since the recording started
    core::SequenceNumber sequence {0};  // as the writer numbered it
    ByteBuffer payload;
};

// Writes a recording. Not thread-safe.
class PUPPET_MASTER_API RecordingWriter final {
public:
    // Creates `path`, replacing an existing file.
    static core::Result<std::unique_ptr<RecordingWriter>> Create(const std::string& path);

    ~RecordingWriter();  // closes
    RecordingWriter(const RecordingWriter&) = delete;
    RecordingWriter& operator=(const RecordingWriter&) = delete;

    // Declares a topic and returns its id; the id in `topic` is ignored.
    core::Result<std::uint32_t> AddTopic(const RecordedTopic& topic);

    core::Status Write(std::uint32_t topic, core::Nanoseconds time, core::SequenceNumber sequence, ByteView payload);

    // Pushes buffered records to the file, so a reader (or a crash) sees
    // everything written so far.
    core::Status Flush();
    core::Status Close();

    std::uint64_t message_count() const noexcept;

private:
    struct Impl;
    explicit RecordingWriter(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

// Reads a recording front to back. Not thread-safe.
class PUPPET_MASTER_API RecordingReader final {
public:
    static core::Result<std::unique_ptr<RecordingReader>> Open(const std::string& path);

    ~RecordingReader();
    RecordingReader(const RecordingReader&) = delete;
    RecordingReader& operator=(const RecordingReader&) = delete;

    // The next message, or none at the end. Topics are declared before their
    // first message, and topics() grows as they are read. A recording that
    // ends inside a record, as one cut off by a crash does, ends there:
    // truncated() tells. Records of unknown kinds are skipped.
    core::Result<std::optional<RecordedMessage>> Next();

    const std::vector<RecordedTopic>& topics() const noexcept;
    // The topic with this id, or nullptr.
    const RecordedTopic* FindTopic(std::uint32_t id) const noexcept;
    bool truncated() const noexcept;

private:
    struct Impl;
    explicit RecordingReader(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace puppet_master::transport
