#pragma once

#include <cstddef>
#include <functional>
#include <ios>
#include <ostream>
#include <sstream>
#include <string>
#include <utility>

#include <puppet_master/export.h>
#include <puppet_master/observability/observability.h>

namespace puppet_master::logging {

using Sink = std::function<void(const observability::LogRecord&)>;

// Replaces the process-wide log sink. Passing an empty sink restores the
// built-in console logger.
PUPPET_MASTER_API void SetSink(Sink sink);
PUPPET_MASTER_API void ResetSink();
PUPPET_MASTER_API void Write(const observability::LogRecord& record) noexcept;

PUPPET_MASTER_API std::string HexDump(const void* data, std::size_t size);

// LogStream preserves the familiar LOG_Info() << value API while keeping the
// backend replaceable. The completed record is emitted when the temporary
// stream leaves the expression.
class PUPPET_MASTER_API LogStream final {
public:
    LogStream(
        observability::LogLevel level,
        const char* file,
        int line,
        std::string component = "application");
    ~LogStream() noexcept;

    LogStream(const LogStream&) = delete;
    LogStream& operator=(const LogStream&) = delete;
    LogStream(LogStream&&) = delete;
    LogStream& operator=(LogStream&&) = delete;

    template <typename Value>
    LogStream& operator<<(Value&& value)
    {
        stream_ << std::forward<Value>(value);
        return *this;
    }

    LogStream& operator<<(std::ostream& (*manipulator)(std::ostream&));
    LogStream& operator<<(std::ios_base& (*manipulator)(std::ios_base&));

private:
    observability::LogRecord record_;
    std::ostringstream stream_;
    std::string source_;
};

}  // namespace puppet_master::logging
