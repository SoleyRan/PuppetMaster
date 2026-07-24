#include <puppet_master/logging/logger.h>

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <mutex>

namespace puppet_master::logging {

namespace {

std::tm LocalTime(std::time_t time)
{
    std::tm value {};
#if defined(_WIN32)
    localtime_s(&value, &time);
#else
    localtime_r(&time, &value);
#endif
    return value;
}

void WriteToConsole(const observability::LogRecord& record)
{
    const auto now = std::chrono::system_clock::now();
    const auto seconds = std::chrono::system_clock::to_time_t(now);
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % std::chrono::seconds(1);
    const auto local_time = LocalTime(seconds);

    static auto* output_mutex = new std::mutex;
    std::lock_guard<std::mutex> lock(*output_mutex);
    std::clog
        << '[' << std::put_time(&local_time, "%Y-%m-%d %H:%M:%S")
        << '.' << std::setfill('0') << std::setw(3) << milliseconds.count()
        << "]<" << observability::ToString(record.level) << '>'
        << observability::FormatLogRecord(record)
        << '\n';
    std::clog.flush();
}

struct SinkState {
    std::mutex mutex;
    Sink sink {WriteToConsole};
};

SinkState& GlobalSink()
{
    static auto* state = new SinkState;
    return *state;
}

std::string SourceLocation(const char* file, int line)
{
    std::string path = file == nullptr ? "unknown" : file;
    const auto separator = path.find_last_of("/\\");
    if (separator != std::string::npos) {
        path.erase(0, separator + 1);
    }
    return path + ':' + std::to_string(line);
}

}  // namespace

void SetSink(Sink sink)
{
    auto& state = GlobalSink();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.sink = sink ? std::move(sink) : Sink {WriteToConsole};
}

void ResetSink()
{
    SetSink({});
}

void Write(const observability::LogRecord& record) noexcept
{
    try {
        Sink sink;
        auto& state = GlobalSink();
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            sink = state.sink;
        }

        if (sink) {
            sink(record);
        }
    } catch (...) {
        // Logging must not change middleware control flow.
    }
}

std::string HexDump(const void* data, std::size_t size)
{
    if (data == nullptr && size != 0) {
        return "<null>";
    }

    const auto* bytes = static_cast<const unsigned char*>(data);
    std::ostringstream stream;
    stream << std::hex << std::setfill('0');
    for (std::size_t index = 0; index < size; ++index) {
        if (index != 0) {
            stream << ' ';
        }
        stream << std::setw(2) << static_cast<unsigned int>(bytes[index]);
    }
    return stream.str();
}

LogStream::LogStream(
    observability::LogLevel level,
    const char* file,
    int line,
    std::string component)
    : source_(SourceLocation(file, line))
{
    record_.level = level;
    record_.component = std::move(component);
    record_.event = "log";
}

LogStream::~LogStream() noexcept
{
    try {
        record_.message = stream_.str();
        record_.fields.push_back({"source", source_});
        Write(record_);
    } catch (...) {
        // Destructors used by logging expressions must never throw.
    }
}

LogStream& LogStream::operator<<(std::ostream& (*manipulator)(std::ostream&))
{
    manipulator(stream_);
    return *this;
}

LogStream& LogStream::operator<<(std::ios_base& (*manipulator)(std::ios_base&))
{
    manipulator(stream_);
    return *this;
}

}  // namespace puppet_master::logging
