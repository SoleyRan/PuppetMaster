#include <cassert>
#include <string>
#include <vector>

#include <puppet_master/logging/log.h>
#include <puppet_master/puppet_master.h>

namespace logging = puppet_master::logging;
namespace observability = puppet_master::observability;

namespace {

void StreamMacrosUseTheConfiguredSink()
{
    std::vector<observability::LogRecord> records;
    logging::SetSink([&records](const observability::LogRecord& record) {
        records.push_back(record);
    });

    LOG_Info() << "message=" << 42;

    assert(records.size() == 1);
    assert(records.front().level == observability::LogLevel::kInfo);
    assert(records.front().component == "application");
    assert(records.front().message == "message=42");
    assert(records.front().fields.size() == 1);
    assert(records.front().fields.front().key == "source");
    assert(records.front().fields.front().value.find("logging_test.cpp:")
        != std::string::npos);
}

void ObserverUsesTheProcessWideSinkByDefault()
{
    std::vector<observability::LogRecord> records;
    logging::SetSink([&records](const observability::LogRecord& record) {
        records.push_back(record);
    });

    observability::Observer observer;
    observer.Log(observability::LogRecord {
        observability::LogLevel::kWarning,
        "scheduler",
        "test_warning",
        "warning from observer",
        {},
    });

    assert(records.size() == 1);
    assert(records.front().event == "test_warning");
}

void ExplicitObserverCallbackOverridesTheDefaultSink()
{
    int global_count = 0;
    int observer_count = 0;
    logging::SetSink([&global_count](const observability::LogRecord&) {
        ++global_count;
    });

    observability::Options options;
    options.log_callback = [&observer_count](const observability::LogRecord&) {
        ++observer_count;
    };

    observability::Observer observer(options);
    observer.Log({});

    assert(global_count == 0);
    assert(observer_count == 1);
}

void DefaultObserverSinkCanBeDisabled()
{
    int global_count = 0;
    logging::SetSink([&global_count](const observability::LogRecord&) {
        ++global_count;
    });

    observability::Options options;
    options.use_default_log_sink = false;

    observability::Observer observer(options);
    observer.Log({});

    assert(global_count == 0);
}

void HexDumpUsesAStablePlainTextFormat()
{
    const unsigned char payload[] = {0x00, 0x12, 0xab, 0xff};
    assert(logging::HexDump(payload, sizeof(payload)) == "00 12 ab ff");
    assert(logging::HexDump(nullptr, 1) == "<null>");
}

}  // namespace

int main()
{
    StreamMacrosUseTheConfiguredSink();
    ObserverUsesTheProcessWideSinkByDefault();
    ExplicitObserverCallbackOverridesTheDefaultSink();
    DefaultObserverSinkCanBeDisabled();
    HexDumpUsesAStablePlainTextFormat();
    logging::ResetSink();
    return 0;
}
