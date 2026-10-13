#include <puppet_master/observability/json_log_exporter.h>

#include <sstream>

namespace puppet_master::observability {
namespace {

void WriteJsonString(std::ostream& output, const std::string& value)
{
    constexpr char hex[] = "0123456789abcdef";
    output << '"';
    for (unsigned char character : value) {
        switch (character) {
        case '"': output << "\\\""; break;
        case '\\': output << "\\\\"; break;
        case '\b': output << "\\b"; break;
        case '\f': output << "\\f"; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default:
            if (character < 0x20) {
                output << "\\u00" << hex[character >> 4] << hex[character & 0x0f];
            } else {
                output << static_cast<char>(character);
            }
            break;
        }
    }
    output << '"';
}

}  // namespace

std::string JsonLogExporter::Render(const LogRecord& record)
{
    std::ostringstream output;
    output << "{\"level\":";
    WriteJsonString(output, ToString(record.level));
    output << ",\"component\":";
    WriteJsonString(output, record.component);
    output << ",\"event\":";
    WriteJsonString(output, record.event);
    output << ",\"message\":";
    WriteJsonString(output, record.message);
    output << ",\"fields\":[";
    bool first = true;
    for (const auto& field : record.fields) {
        if (!first) {
            output << ',';
        }
        first = false;
        output << "{\"key\":";
        WriteJsonString(output, field.key);
        output << ",\"value\":";
        WriteJsonString(output, field.value);
        output << '}';
    }
    output << "]}";
    return output.str();
}

}  // namespace puppet_master::observability
