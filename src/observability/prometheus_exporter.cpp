#include <puppet_master/observability/prometheus_exporter.h>

#include <chrono>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <string>
#include <vector>

namespace puppet_master::observability {
namespace {

std::string EscapeLabel(const std::string& value)
{
    std::string escaped;
    for (char character : value) {
        switch (character) {
        case '\\': escaped += "\\\\"; break;
        case '"': escaped += "\\\""; break;
        case '\n': escaped += "\\n"; break;
        default: escaped += character; break;
        }
    }
    return escaped;
}

template <typename Metrics, typename Value>
void AppendFamily(std::ostringstream& output, const char* name, const char* help,
                  const char* type, const std::vector<Metrics>& metrics,
                  const char* label_name, const std::string Metrics::*label, Value value)
{
    output << "# HELP " << name << ' ' << help << '\n'
           << "# TYPE " << name << ' ' << type << '\n';
    for (const auto& entry : metrics) {
        output << name << '{' << label_name << "=\""
               << EscapeLabel(entry.*label) << "\"} " << value(entry) << '\n';
    }
}

double Seconds(core::Nanoseconds duration)
{
    return std::chrono::duration<double>(duration).count();
}

}  // namespace

std::string PrometheusExporter::Render(const MetricsSnapshot& snapshot)
{
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::setprecision(std::numeric_limits<double>::max_digits10);
    output << "# HELP puppet_master_uptime_seconds Time since observer creation in seconds.\n"
           << "# TYPE puppet_master_uptime_seconds gauge\n"
           << "puppet_master_uptime_seconds " << Seconds(snapshot.uptime) << '\n';

#define TOPIC(name, help, type, field) \
    AppendFamily(output, "puppet_master_topic_" name, help, type, snapshot.topics, "topic", \
                 &TopicMetrics::topic_name, [](const TopicMetrics& m) { return m.field; })
#define TOPIC_SECONDS(name, help, field) \
    AppendFamily(output, "puppet_master_topic_" name "_seconds", help, "gauge", snapshot.topics, "topic", \
                 &TopicMetrics::topic_name, [](const TopicMetrics& m) { return Seconds(m.field); })
#define TASK(name, help, type, field) \
    AppendFamily(output, "puppet_master_task_" name, help, type, snapshot.tasks, "task", \
                 &TaskMetrics::task_name, [](const TaskMetrics& m) { return m.field; })
#define TASK_SECONDS(name, help, field) \
    AppendFamily(output, "puppet_master_task_" name "_seconds", help, "gauge", snapshot.tasks, "task", \
                 &TaskMetrics::task_name, [](const TaskMetrics& m) { return Seconds(m.field); })

    TOPIC("messages_published_total", "Published messages.", "counter", messages_published);
    TOPIC("bytes_published_total", "Published bytes.", "counter", bytes_published);
    TOPIC("messages_received_total", "Received messages.", "counter", messages_received);
    TOPIC("bytes_received_total", "Received bytes.", "counter", bytes_received);
    TOPIC("latency_samples_total", "Received messages with latency samples.", "counter", latency_samples);
    TOPIC("queue_depth", "Current queue depth.", "gauge", queue_depth);
    TOPIC("max_queue_depth", "Maximum observed queue depth.", "gauge", max_queue_depth);
    TOPIC_SECONDS("last_latency", "Last observed message latency.", last_latency);
    TOPIC_SECONDS("average_latency", "Average observed message latency.", average_latency);
    TOPIC_SECONDS("max_latency", "Maximum observed message latency.", max_latency);
    TOPIC("publish_messages_per_second", "Average published messages per second since observer creation.", "gauge", publish_messages_per_second);
    TOPIC("publish_bytes_per_second", "Average published bytes per second since observer creation.", "gauge", publish_bytes_per_second);
    TOPIC("receive_messages_per_second", "Average received messages per second since observer creation.", "gauge", receive_messages_per_second);
    TOPIC("receive_bytes_per_second", "Average received bytes per second since observer creation.", "gauge", receive_bytes_per_second);

    TASK("executions_total", "Task executions.", "counter", executions);
    TASK("failures_total", "Failed task executions.", "counter", failures);
    TASK("deadline_misses_total", "Task deadline misses.", "counter", deadline_misses);
    TASK_SECONDS("last_execution_time", "Last task execution time.", last_execution_time);
    TASK_SECONDS("average_execution_time", "Average task execution time.", average_execution_time);
    TASK_SECONDS("max_execution_time", "Maximum task execution time.", max_execution_time);
    TASK_SECONDS("last_queue_wait", "Last task queue wait.", last_queue_wait);
    TASK_SECONDS("average_queue_wait", "Average task queue wait.", average_queue_wait);
    TASK_SECONDS("max_queue_wait", "Maximum task queue wait.", max_queue_wait);
    TASK_SECONDS("last_response_time", "Last task response time.", last_response_time);
    TASK_SECONDS("average_response_time", "Average task response time.", average_response_time);
    TASK_SECONDS("max_response_time", "Maximum task response time.", max_response_time);

#undef TOPIC
#undef TOPIC_SECONDS
#undef TASK
#undef TASK_SECONDS
    return output.str();
}

}  // namespace puppet_master::observability
