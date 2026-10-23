#pragma once

#include <chrono>
#include <cstddef>
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
#include <puppet_master/transport/transport.h>

namespace puppet_master::runtime {
class RuntimeContext;
}

namespace puppet_master::configuration {
struct ProjectConfig;
}

namespace puppet_master::transport::shm {

// Ring geometry of one topic. Every process on a topic must use the same
// values; a mismatch is reported when the topic is first opened.
struct PUPPET_MASTER_API TopicOptions {
    std::size_t slot_payload_size {64 * 1024};  // max bytes per message
    std::size_t slot_count {16};                // ring capacity

    core::Status Validate() const;
};

struct PUPPET_MASTER_API Options {
    core::DomainId domain_id {0};
    TopicOptions default_topic;
    std::map<std::string, TopicOptions> topics;  // per-topic override by full name
    // Upper bound for one notification wait, so a lost wakeup delays delivery
    // by at most this interval.
    core::Nanoseconds wait_poll_interval {std::chrono::milliseconds(50)};

    core::Status Validate() const;
    const TopicOptions& ForTopic(const std::string& topic) const;
};

PUPPET_MASTER_API core::Status RegisterTransport(
    runtime::RuntimeContext& runtime,
    core::TransportName name,
    Options options = {});

// Starts from `base` and applies the shm keys of every ipc topic's
// transport_parameters, as set by a deployment file:
//   slot_payload_size, slot_count   ring geometry of that topic
//   domain_id                       must be the same on every topic that sets it
// Unknown keys and invalid values are InvalidArgument naming the topic.
PUPPET_MASTER_API core::Result<Options> OptionsFromProject(
    const configuration::ProjectConfig& project, Options base = {});

// Deletes a topic's segment so the next opener starts a fresh one, for example
// after changing its TopicOptions. Processes that still have it mapped keep
// working on the old segment. No-op on Windows, which frees a segment when the
// last process closes it.
PUPPET_MASTER_API core::Status RemoveSegment(core::DomainId domain_id, const std::string& topic);

// One segment found on this host, as reported by its header.
struct SegmentInfo {
    enum class State : std::uint8_t {
        kInUse = 0,       // a live process is attached
        kUnused,          // valid, but no live process is attached
        kUninitialized,   // its creator never finished: always stale
        kIncompatible,    // another layout version, or truncated
    };

    std::string object_name;  // the OS name, e.g. "/pm.0.3f2a..." on Linux
    core::DomainId domain_id {0};
    State state {State::kUnused};
    std::string problem;  // for kUninitialized and kIncompatible
    std::size_t size_bytes {0};
    std::string topic;
    MessageDescriptor descriptor;
    TopicOptions geometry;
    std::uint64_t messages_written {0};
    std::vector<std::uint32_t> live_pids;   // attached processes still running
    std::size_t dead_participants {0};      // entries of processes that exited
};

PUPPET_MASTER_API const char* ToString(SegmentInfo::State state) noexcept;

// Every PuppetMaster segment on this host, or in one domain. Read-only: it
// does not attach, so it changes nothing for running processes. Unsupported
// on Windows, which frees segments with their last process.
PUPPET_MASTER_API core::Result<std::vector<SegmentInfo>> ListSegments(
    std::optional<core::DomainId> domain_id = std::nullopt);

// Removes the listed segments that no live process uses (kUnused,
// kUninitialized, kIncompatible) and returns them. Segments in use are never
// removed. A process that attaches between the check and the removal keeps
// working on the removed segment, but no longer shares it with later
// processes, so run it while the system is stopped. Unsupported on Windows.
PUPPET_MASTER_API core::Result<std::vector<SegmentInfo>> RemoveUnusedSegments(
    std::optional<core::DomainId> domain_id = std::nullopt);

// Same-host, cross-process transport over shared memory (Linux and Windows).
// See docs/shm-transport.md for the supported policies and limits.
class PUPPET_MASTER_API ShmTransport final : public ::puppet_master::transport::Transport {
public:
    explicit ShmTransport(core::TransportName name, Options options = {});
    ~ShmTransport() override;

    ShmTransport(const ShmTransport&) = delete;
    ShmTransport& operator=(const ShmTransport&) = delete;
    ShmTransport(ShmTransport&&) = delete;
    ShmTransport& operator=(ShmTransport&&) = delete;

    const core::TransportName& name() const noexcept override;
    core::TransportKind kind() const noexcept override;
    TransportCapabilities capabilities() const noexcept override;

    core::Status Open() override;
    core::Status Close() noexcept override;
    bool is_open() const noexcept override;

    core::Status ValidateEndpoint(const EndpointConfig& endpoint) const override;
    core::Result<ReaderPtr> CreateReader(const EndpointConfig& endpoint) override;
    core::Result<WriterPtr> CreateWriter(const EndpointConfig& endpoint) override;

    const Options& options() const noexcept;

private:
    struct Impl;

    core::TransportName name_;
    Options options_;
    std::shared_ptr<Impl> impl_;
};

}  // namespace puppet_master::transport::shm
