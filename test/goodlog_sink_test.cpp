#include <cassert>
#include <memory>

#include <puppet_master/observability/goodlog_sink.h>

namespace core = puppet_master::core;
namespace observability = puppet_master::observability;
namespace goodlog_sink = puppet_master::observability::goodlog;

namespace {

void InvalidConfigurationIsRejectedBeforeBackendInitialization()
{
    auto observer = std::make_shared<observability::Observer>();

    goodlog_sink::SinkOptions options;
    options.directory.clear();
    auto status = goodlog_sink::InstallSink(observer, options);
    assert(!status.ok());
    assert(status.code() == core::StatusCode::kInvalidArgument);

    options = {};
    options.encryption = goodlog_sink::EncryptionMode::kAes256Gcm;
    status = goodlog_sink::InstallSink(observer, options);
    assert(!status.ok());
    assert(status.code() == core::StatusCode::kInvalidArgument);

    status = goodlog_sink::InstallSink(nullptr);
    assert(!status.ok());
    assert(status.code() == core::StatusCode::kInvalidArgument);
}

}  // namespace

int main()
{
    InvalidConfigurationIsRejectedBeforeBackendInitialization();
    return 0;
}
