#pragma once

#include <memory>

#include <puppet_master/transport/transport.h>

namespace puppet_master::transport::inmemory {

// InMemoryTransport is the reference local transport used by tests, demos, and
// future scheduler work. It keeps all data inside the current process while
// preserving the same Reader/Writer contract as external middleware adapters.
class InMemoryTransport final : public ::puppet_master::transport::Transport {
public:
    explicit InMemoryTransport(core::TransportName name);
    ~InMemoryTransport() override;

    InMemoryTransport(const InMemoryTransport&) = delete;
    InMemoryTransport& operator=(const InMemoryTransport&) = delete;
    InMemoryTransport(InMemoryTransport&&) = delete;
    InMemoryTransport& operator=(InMemoryTransport&&) = delete;

    const core::TransportName& name() const noexcept override;
    core::TransportKind kind() const noexcept override;
    TransportCapabilities capabilities() const noexcept override;

    core::Status Open() override;
    core::Status Close() noexcept override;
    bool is_open() const noexcept override;

    core::Status ValidateEndpoint(const EndpointConfig& endpoint) const override;
    core::Result<ReaderPtr> CreateReader(const EndpointConfig& endpoint) override;
    core::Result<WriterPtr> CreateWriter(const EndpointConfig& endpoint) override;

private:
    struct State;

    core::TransportName name_;
    std::shared_ptr<State> state_;
};

}  // namespace puppet_master::transport::inmemory
