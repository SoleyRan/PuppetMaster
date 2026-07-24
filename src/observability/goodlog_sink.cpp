#include <puppet_master/observability/goodlog_sink.h>

#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>

#include <log.hpp>
#include <puppet_master/logging/logger.h>

namespace puppet_master::observability::goodlog {

namespace {

core::Status ValidateOptions(const SinkOptions& options)
{
    if (options.directory.empty()) {
        return core::Status::InvalidArgument("GoodLog directory must not be empty");
    }

    const auto max_int = static_cast<std::size_t>(std::numeric_limits<int>::max());
    if (options.max_file_size_mb == 0 || options.max_file_size_mb > max_int) {
        return core::Status::InvalidArgument(
            "GoodLog max file size must fit a positive int");
    }

    if (options.max_files == 0 || options.max_files > max_int) {
        return core::Status::InvalidArgument(
            "GoodLog max file count must fit a positive int");
    }

    if (options.encryption == EncryptionMode::kAes256Gcm
        && options.encryption_key_hex.empty()) {
        return core::Status::InvalidArgument(
            "GoodLog AES-256-GCM encryption requires a hexadecimal key");
    }

    return core::Status::Ok();
}

std::string NormalizeDirectory(std::string directory)
{
    const auto last = directory.back();
    if (last != '/' && last != '\\') {
        directory.push_back('/');
    }
    return directory;
}

::goodlog::LogOptions MakeBackendOptions(const SinkOptions& options)
{
    ::goodlog::LogOptions backend_options;
    backend_options.compression =
        options.compression == CompressionMode::kGzip
        ? ::goodlog::CompressionMode::Gzip
        : ::goodlog::CompressionMode::None;
    backend_options.encryption =
        options.encryption == EncryptionMode::kAes256Gcm
        ? ::goodlog::EncryptionMode::Aes256Gcm
        : ::goodlog::EncryptionMode::None;
    backend_options.encryption_key_hex = options.encryption_key_hex;
    return backend_options;
}

void WriteRecord(const LogRecord& record)
{
    const auto message = FormatLogRecord(record);
    switch (record.level) {
        case LogLevel::kTrace:
            LOG_Trace() << message;
            break;
        case LogLevel::kDebug:
            LOG_Debug() << message;
            break;
        case LogLevel::kInfo:
            LOG_Info() << message;
            break;
        case LogLevel::kWarning:
            LOG_Warn() << message;
            break;
        case LogLevel::kError:
            LOG_Error() << message;
            break;
        case LogLevel::kFatal:
            LOG_Fatal() << message;
            break;
    }
}

}  // namespace

core::Status InstallSink(
    const ObserverPtr& observer,
    const SinkOptions& options)
{
    if (!observer) {
        return core::Status::InvalidArgument("observer must not be null");
    }

    auto status = ValidateOptions(options);
    if (!status.ok()) {
        return status;
    }

    static std::mutex install_mutex;
    static bool initialized = false;

    std::lock_guard<std::mutex> lock(install_mutex);
    if (!initialized) {
        try {
            ::goodlog::logInit(
                NormalizeDirectory(options.directory),
                static_cast<int>(options.console_level),
                static_cast<int>(options.file_level),
                static_cast<int>(options.max_file_size_mb),
                static_cast<int>(options.max_files),
                MakeBackendOptions(options));
            initialized = true;
        } catch (const std::exception& exception) {
            return core::Status::Internal(
                std::string("failed to initialize GoodLog: ") + exception.what());
        } catch (...) {
            return core::Status::Internal(
                "failed to initialize GoodLog with an unknown error");
        }
    }

    ::puppet_master::logging::SetSink(WriteRecord);
    observer->SetLogCallback(::puppet_master::logging::Write);
    return core::Status::Ok();
}

}  // namespace puppet_master::observability::goodlog
