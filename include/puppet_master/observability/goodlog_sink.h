#pragma once

#include <cstddef>
#include <string>

#include <puppet_master/core/status.h>
#include <puppet_master/export.h>
#include <puppet_master/observability/observability.h>

namespace puppet_master::observability::goodlog {

enum class CompressionMode {
    kNone = 0,
    kGzip,
};

enum class EncryptionMode {
    kNone = 0,
    kAes256Gcm,
};

struct SinkOptions {
    std::string directory {"/tmp/puppet_master/"};
    LogLevel console_level {LogLevel::kInfo};
    LogLevel file_level {LogLevel::kDebug};
    std::size_t max_file_size_mb {10};
    std::size_t max_files {10};
    CompressionMode compression {CompressionMode::kNone};
    EncryptionMode encryption {EncryptionMode::kNone};
    std::string encryption_key_hex;
};

// Installs GoodLog as the Observer's structured log callback. GoodLog owns
// process-wide Boost.Log sinks, so the backend is initialized once and later
// calls attach additional observers to the same sink configuration.
PUPPET_MASTER_API core::Status InstallSink(
    const ObserverPtr& observer,
    const SinkOptions& options = {});

}  // namespace puppet_master::observability::goodlog
