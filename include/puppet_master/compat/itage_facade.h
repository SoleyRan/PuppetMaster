#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <puppet_master/core/message_policy.h>
#include <puppet_master/core/result.h>
#include <puppet_master/core/status.h>
#include <puppet_master/core/types.h>
#include <puppet_master/runtime/context.h>
#include <puppet_master/transport/transport.h>

namespace puppet_master::compat::itage {

// TopicOptions is the supported replacement for itage_engine's backend-specific
// void* attribute payload. The compatibility facade keeps the old call shape,
// but the data carried through that slot is now transport-neutral.
struct TopicOptions {
    core::TransportKind transport {core::TransportKind::kInMemory};
    std::string message_type {"puppet_master.compat.RawBytes"};
    std::string encoding {"application/octet-stream"};
    core::MessagePolicy message_policy;
    std::size_t fixed_payload_size {0};
    bool wait_on_read {false};
    core::Nanoseconds read_timeout {0};

    core::Status Validate() const;
};

struct NodeOptions {
    runtime::RuntimeOptions runtime_options;
    TopicOptions default_topic_options;

    core::Status Validate() const;
};

class WriterBase {
public:
    WriterBase(const WriterBase&) = delete;
    WriterBase& operator=(const WriterBase&) = delete;
    WriterBase(WriterBase&&) = delete;
    WriterBase& operator=(WriterBase&&) = delete;
    virtual ~WriterBase() = default;

    virtual int Write(void* data, std::size_t len = 0) = 0;
    virtual const core::Status& last_status() const noexcept = 0;
    virtual const transport::EndpointConfig& endpoint() const noexcept = 0;

protected:
    WriterBase() = default;
};

class ReaderBase {
public:
    ReaderBase(const ReaderBase&) = delete;
    ReaderBase& operator=(const ReaderBase&) = delete;
    ReaderBase(ReaderBase&&) = delete;
    ReaderBase& operator=(ReaderBase&&) = delete;
    virtual ~ReaderBase() = default;

    virtual void SetCallBack(const std::function<void()>& callback) = 0;
    virtual int Read(void* data, std::int64_t& time_diff_ms) = 0;
    virtual int Read(void* data, std::size_t capacity, std::int64_t& time_diff_ms) = 0;
    virtual const core::Status& last_status() const noexcept = 0;
    virtual const transport::EndpointConfig& endpoint() const noexcept = 0;

protected:
    ReaderBase() = default;
};

class NodeBase {
public:
    NodeBase(const NodeBase&) = delete;
    NodeBase& operator=(const NodeBase&) = delete;
    NodeBase(NodeBase&&) = delete;
    NodeBase& operator=(NodeBase&&) = delete;
    virtual ~NodeBase() = default;

    virtual std::shared_ptr<WriterBase> CreateWriter(
        std::string topic_name,
        void* data = nullptr,
        void* attribute = nullptr) = 0;

    virtual std::shared_ptr<ReaderBase> CreateReader(
        std::string topic_name,
        void* data = nullptr,
        void* attribute = nullptr) = 0;

    virtual const core::Status& last_status() const noexcept = 0;

protected:
    NodeBase() = default;
};

class Node final : public NodeBase {
public:
    explicit Node(std::string node_name, NodeOptions options = {});

    static core::Result<std::shared_ptr<Node>> Create(
        std::string node_name,
        NodeOptions options = {});

    const std::string& name() const noexcept;
    std::shared_ptr<runtime::RuntimeContext> context() const noexcept;
    const core::Status& last_status() const noexcept override;

    core::Result<transport::EndpointConfig> BuildEndpoint(
        std::string topic_name,
        const TopicOptions& options) const;

    std::shared_ptr<WriterBase> CreateWriter(
        std::string topic_name,
        const TopicOptions& options);

    std::shared_ptr<ReaderBase> CreateReader(
        std::string topic_name,
        const TopicOptions& options);

    std::shared_ptr<WriterBase> CreateWriter(
        std::string topic_name,
        void* data = nullptr,
        void* attribute = nullptr) override;

    std::shared_ptr<ReaderBase> CreateReader(
        std::string topic_name,
        void* data = nullptr,
        void* attribute = nullptr) override;

private:
    TopicOptions ResolveTopicOptions(const void* attribute) const;

    std::string node_name_;
    NodeOptions options_;
    std::shared_ptr<runtime::RuntimeContext> context_;
    core::Status last_status_;
};

using WriterPtr = std::shared_ptr<WriterBase>;
using ReaderPtr = std::shared_ptr<ReaderBase>;
using NodePtr = std::shared_ptr<Node>;

}  // namespace puppet_master::compat::itage
