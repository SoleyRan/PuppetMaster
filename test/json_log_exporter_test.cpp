#include <cassert>
#include <string>

#include <puppet_master/observability/json_log_exporter.h>

namespace observability = puppet_master::observability;

namespace {

void RendersEmptyRecordAndLevels()
{
    observability::LogRecord record;
    assert(observability::JsonLogExporter::Render(record) ==
           "{\"level\":\"info\",\"component\":\"\",\"event\":\"\",\"message\":\"\",\"fields\":[]}");

    const observability::LogLevel levels[] = {
        observability::LogLevel::kTrace, observability::LogLevel::kDebug,
        observability::LogLevel::kInfo, observability::LogLevel::kWarning,
        observability::LogLevel::kError, observability::LogLevel::kFatal,
    };
    const char* names[] = {"trace", "debug", "info", "warning", "error", "fatal"};
    for (int index = 0; index < 6; ++index) {
        record.level = levels[index];
        assert(observability::JsonLogExporter::Render(record) ==
               std::string("{\"level\":\"") + names[index] +
                   "\",\"component\":\"\",\"event\":\"\",\"message\":\"\",\"fields\":[]}");
    }
}

void EscapesTextAndPreservesFields()
{
    observability::LogRecord record;
    record.level = observability::LogLevel::kWarning;
    record.component = "comp\"\\";
    record.event = "start\nnext";
    record.message = "line\tend\r";
    record.fields = {{"same", "first"}, {"same", "second"}, {"key\"", "value\\"}};

    assert(observability::JsonLogExporter::Render(record) ==
           "{\"level\":\"warning\",\"component\":\"comp\\\"\\\\\","
           "\"event\":\"start\\nnext\",\"message\":\"line\\tend\\r\","
           "\"fields\":[{\"key\":\"same\",\"value\":\"first\"},"
           "{\"key\":\"same\",\"value\":\"second\"},"
           "{\"key\":\"key\\\"\",\"value\":\"value\\\\\"}]}");
}

void EscapesControlCharacters()
{
    observability::LogRecord record;
    record.component = std::string(1, '\0') + '\b' + '\f' + '\n' + '\r' + '\t' +
                       static_cast<char>(0x1f);
    record.event = std::string(1, static_cast<char>(0x01));
    record.message = std::string(1, static_cast<char>(0x7f));
    record.fields = {{std::string(1, static_cast<char>(0x0b)),
                      std::string(1, static_cast<char>(0x1e))}};

    assert(observability::JsonLogExporter::Render(record) ==
           "{\"level\":\"info\",\"component\":\"\\u0000\\b\\f\\n\\r\\t\\u001f\","
           "\"event\":\"\\u0001\",\"message\":\"\x7f\","
           "\"fields\":[{\"key\":\"\\u000b\",\"value\":\"\\u001e\"}]}");
}

void IntegratesWithObserverLogCallback()
{
    observability::Options options;
    options.use_default_log_sink = false;
    observability::Observer observer(options);
    std::string latest;
    int count = 0;
    observer.SetLogCallback([&latest, &count](const observability::LogRecord& record) {
        latest = observability::JsonLogExporter::Render(record);
        ++count;
    });

    observability::LogRecord record;
    record.level = observability::LogLevel::kError;
    record.component = "runtime";
    record.event = "failed";
    record.message = "retry";
    record.fields = {{"attempt", "2"}};
    observer.Log(record);
    assert(count == 1);
    assert(latest == observability::JsonLogExporter::Render(record));
    assert(latest == "{\"level\":\"error\",\"component\":\"runtime\",\"event\":\"failed\","
                     "\"message\":\"retry\",\"fields\":[{\"key\":\"attempt\",\"value\":\"2\"}]}");
}

}  // namespace

int main()
{
    RendersEmptyRecordAndLevels();
    EscapesTextAndPreservesFields();
    EscapesControlCharacters();
    IntegratesWithObserverLogCallback();
    return 0;
}
