#include <cassert>
#include <filesystem>
#include <memory>

#include <puppet_master/logging/log.h>
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

void InstalledSinkAcceptsObserverAndStreamLogs()
{
    auto observer = std::make_shared<observability::Observer>();

    goodlog_sink::SinkOptions options;
    options.directory =
        (std::filesystem::temp_directory_path() / "puppet_master_goodlog_test").string();
    auto status = goodlog_sink::InstallSink(observer, options);
    assert(status.ok());

    LOG_Info() << "stream-style GoodLog integration";
    observer->Log(observability::LogRecord {
        observability::LogLevel::kInfo,
        "goodlog_test",
        "observer_log",
        "observer GoodLog integration",
        {},
    });
}

}  // namespace

int main()
{
    InvalidConfigurationIsRejectedBeforeBackendInitialization();
    InstalledSinkAcceptsObserverAndStreamLogs();
    return 0;
}
