#pragma once

#include <memory>

#include <puppet_master/transport/fastdds/options.h>
#include <puppet_master/transport/transport.h>

namespace puppet_master::transport::fastdds {

class FastDdsTransport final : public ::puppet_master::transport::Transport {
public:
    explicit FastDdsTransport(core::TransportName name, Options options = {});
    ~FastDdsTransport() override;

    FastDdsTransport(const FastDdsTransport&) = delete;
    FastDdsTransport& operator=(const FastDdsTransport&) = delete;
    FastDdsTransport(FastDdsTransport&&) = delete;
    FastDdsTransport& operator=(FastDdsTransport&&) = delete;

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
    std::unique_ptr<Impl> impl_;
};

}  // namespace puppet_master::transport::fastdds
