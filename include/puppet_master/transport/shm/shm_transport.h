#pragma once

#include <chrono>
#include <cstddef>
#include <map>
#include <memory>
#include <string>

#include <puppet_master/core/status.h>
#include <puppet_master/core/types.h>
#include <puppet_master/export.h>
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
