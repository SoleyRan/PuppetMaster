// Transport latency and throughput. Not a unit test: it measures, and fails
// only if a measurement could not be taken.
//
//     puppet_master_transport_bench [--quick] [--json FILE]
//
// Cases:
//   pingpong   round trip between two threads over two topics, which wakes a
//              blocked reader each way: what a request/reply or a data-trigger
//              chain pays per hop. Reported as one-way latency (half the round
//              trip): median, p99, max.
//   throughput one writer, one queued reader, with at most half a queue in
//              flight: the sustained rate a reader that keeps up receives,
//              messages and bytes per second. Any loss fails the run.
//
// shm runs in one process here, so it measures the ring, the futex/event wake,
// and copies, not cross-process scheduling noise. Numbers depend heavily on
// the machine; compare runs on the same host only.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <puppet_master/puppet_master.h>

#ifdef PUPPETMASTER_BENCH_WITH_SHM
#include <puppet_master/transport/shm/shm_transport.h>
#endif

namespace core = puppet_master::core;
namespace inmemory = puppet_master::transport::inmemory;
namespace transport = puppet_master::transport;

using Clock = std::chrono::steady_clock;

namespace {

struct Settings {
    std::size_t pingpong_rounds {20000};
    std::size_t throughput_messages {200000};
    std::vector<std::size_t> sizes {64, 4096, 65536};
};

struct Result {
    std::string transport;
    std::string bench;
    std::size_t payload {0};
    // pingpong, one-way nanoseconds
    double median_ns {0};
    double p99_ns {0};
    double max_ns {0};
    // throughput
    double messages_per_second {0};
    double megabytes_per_second {0};
    std::size_t delivered {0};
    std::size_t sent {0};
};

using MakeTransport = std::function<std::shared_ptr<transport::Transport>(std::size_t max_payload)>;

std::string UniqueTopic(const char* tag)
{
    static int counter = 0;
    return std::string("/pm/bench/") + tag + "/" + std::to_string(Clock::now().time_since_epoch().count()) + "/"
        + std::to_string(++counter);
}

transport::EndpointConfig Endpoint(core::TransportKind kind, const std::string& topic, std::size_t depth)
{
    core::MessagePolicy policy;
    policy.delivery = core::DeliveryGuarantee::kReliable;
    policy.freshness = core::FreshnessPolicy::kQueued;
    policy.queue_depth = depth;
    return transport::EndpointConfig {core::TopicSpec {core::TopicName::Unsafe(topic), kind, policy},
                                      transport::MessageDescriptor {"bench.Bytes", "application/octet-stream"}};
}

template <typename T>
T Require(core::Result<T> result, const char* what)
{
    if (!result.ok()) {
        std::cerr << what << ": " << result.status().ToString() << '\n';
        std::exit(1);
    }
    return std::move(result).value();
}

void Require(const core::Status& status, const char* what)
{
    if (!status.ok()) {
        std::cerr << what << ": " << status.ToString() << '\n';
        std::exit(1);
    }
}

double Percentile(std::vector<std::int64_t>& samples, double fraction)
{
    const auto index = static_cast<std::size_t>(fraction * static_cast<double>(samples.size() - 1));
    std::nth_element(samples.begin(), samples.begin() + static_cast<std::ptrdiff_t>(index), samples.end());
    return static_cast<double>(samples[index]);
}

Result PingPong(const std::string& name, core::TransportKind kind, const MakeTransport& make, std::size_t payload,
                std::size_t rounds)
{
    auto backend = make(payload);
    Require(backend->Open(), "open");
    const auto ping_topic = UniqueTopic("ping");
    const auto pong_topic = UniqueTopic("pong");
    auto ping_writer = Require(backend->CreateWriter(Endpoint(kind, ping_topic, 4)), "ping writer");
    auto ping_reader = Require(backend->CreateReader(Endpoint(kind, ping_topic, 4)), "ping reader");
    auto pong_writer = Require(backend->CreateWriter(Endpoint(kind, pong_topic, 4)), "pong writer");
    auto pong_reader = Require(backend->CreateReader(Endpoint(kind, pong_topic, 4)), "pong reader");

    transport::ReadOptions wait;
    wait.wait = true;
    wait.timeout = std::chrono::seconds(5);
    const transport::ByteBuffer message(payload, 0x5a);
    const std::size_t warmup = std::max<std::size_t>(rounds / 10, 100);

    std::thread echo([&] {
        for (std::size_t index = 0; index < warmup + rounds; ++index) {
            auto received = Require(ping_reader->Read(wait), "echo read");
            Require(pong_writer->Write(transport::ByteView(received.payload)), "echo write");
        }
    });
    std::vector<std::int64_t> samples;
    samples.reserve(rounds);
    for (std::size_t index = 0; index < warmup + rounds; ++index) {
        const auto start = Clock::now();
        Require(ping_writer->Write(transport::ByteView(message)), "ping write");
        Require(pong_reader->Read(wait), "pong read");
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
        if (index >= warmup) {
            samples.push_back(elapsed / 2);  // one way
        }
    }
    echo.join();
    Require(backend->Close(), "close");

    Result result;
    result.transport = name;
    result.bench = "pingpong";
    result.payload = payload;
    result.median_ns = Percentile(samples, 0.5);
    result.p99_ns = Percentile(samples, 0.99);
    result.max_ns = static_cast<double>(*std::max_element(samples.begin(), samples.end()));
    return result;
}

Result Throughput(const std::string& name, core::TransportKind kind, const MakeTransport& make, std::size_t payload,
                  std::size_t messages)
{
    auto backend = make(payload);
    Require(backend->Open(), "open");
    const auto topic = UniqueTopic("throughput");
    constexpr std::size_t kDepth = 1024;
    auto writer = Require(backend->CreateWriter(Endpoint(kind, topic, kDepth)), "writer");
    auto reader = Require(backend->CreateReader(Endpoint(kind, topic, kDepth)), "reader");

    transport::ReadOptions wait;
    wait.wait = true;
    wait.timeout = std::chrono::milliseconds(200);
    const transport::ByteBuffer message(payload, 0xa5);
    // Credit-based flow control: the writer keeps at most half a queue in
    // flight, so drop-oldest never discards and the rate is what a reader
    // that keeps up actually receives. Without it the writer only measures
    // how fast it can overwrite the queue.
    constexpr std::size_t kWindow = kDepth / 2;
    std::atomic<std::size_t> delivered {0};

    std::thread consumer([&] {
        std::size_t idle = 0;
        while (delivered.load(std::memory_order_relaxed) < messages && idle < 25) {
            auto received = reader->Read(wait);
            if (received.ok()) {
                delivered.fetch_add(1, std::memory_order_release);
                idle = 0;
            } else {
                ++idle;  // 25 x 200 ms with nothing: messages were lost
            }
        }
    });
    const auto start = Clock::now();
    for (std::size_t index = 0; index < messages; ++index) {
        while (index - delivered.load(std::memory_order_acquire) >= kWindow) {
            std::this_thread::yield();
        }
        Require(writer->Write(transport::ByteView(message)), "write");
    }
    consumer.join();
    const auto seconds = std::chrono::duration<double>(Clock::now() - start).count();
    Require(backend->Close(), "close");

    Result result;
    result.transport = name;
    result.bench = "throughput";
    result.payload = payload;
    result.sent = messages;
    result.delivered = delivered.load();
    if (result.delivered != result.sent) {
        std::cerr << name << " throughput " << payload << " B: delivered " << result.delivered << " of "
                  << result.sent << " within the flow-control window\n";
        std::exit(1);
    }
    result.messages_per_second = static_cast<double>(result.delivered) / seconds;
    result.megabytes_per_second = result.messages_per_second * static_cast<double>(payload) / (1024.0 * 1024.0);
    return result;
}

std::string Human(double nanoseconds)
{
    std::ostringstream text;
    text << std::fixed << std::setprecision(1);
    if (nanoseconds >= 1e6) {
        text << nanoseconds / 1e6 << " ms";
    } else if (nanoseconds >= 1e3) {
        text << nanoseconds / 1e3 << " us";
    } else {
        text << nanoseconds << " ns";
    }
    return text.str();
}

void Print(const Result& result)
{
    std::cout << std::left << std::setw(10) << result.transport << std::setw(11) << result.bench << std::right
              << std::setw(7) << result.payload << " B  ";
    if (result.bench == "pingpong") {
        std::cout << "median " << std::setw(9) << Human(result.median_ns) << "  p99 " << std::setw(9)
                  << Human(result.p99_ns) << "  max " << Human(result.max_ns) << '\n';
    } else {
        std::cout << std::fixed << std::setprecision(0) << std::setw(10) << result.messages_per_second << " msg/s  "
                  << std::setprecision(1) << std::setw(8) << result.megabytes_per_second << " MiB/s  delivered "
                  << result.delivered << "/" << result.sent << '\n';
    }
}

void WriteJson(const std::string& path, const std::vector<Result>& results)
{
    std::ofstream out(path);
    out << "{\n  \"results\": [\n";
    for (std::size_t index = 0; index < results.size(); ++index) {
        const auto& result = results[index];
        out << "    {\"transport\": \"" << result.transport << "\", \"bench\": \"" << result.bench
            << "\", \"payload\": " << result.payload;
        if (result.bench == "pingpong") {
            out << ", \"median_ns\": " << result.median_ns << ", \"p99_ns\": " << result.p99_ns
                << ", \"max_ns\": " << result.max_ns;
        } else {
            out << ", \"messages_per_second\": " << result.messages_per_second
                << ", \"megabytes_per_second\": " << result.megabytes_per_second << ", \"delivered\": "
                << result.delivered << ", \"sent\": " << result.sent;
        }
        out << "}" << (index + 1 < results.size() ? "," : "") << '\n';
    }
    out << "  ]\n}\n";
}

}  // namespace

int main(int argc, char** argv)
{
    Settings settings;
    std::string json_path;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--quick") {
            // For CI: proves every case runs, not a measurement.
            settings.pingpong_rounds = 500;
            settings.throughput_messages = 5000;
        } else if (argument == "--json" && index + 1 < argc) {
            json_path = argv[++index];
        } else {
            std::cerr << "usage: puppet_master_transport_bench [--quick] [--json FILE]\n";
            return 2;
        }
    }

    std::vector<std::pair<std::string, std::pair<core::TransportKind, MakeTransport>>> backends;
    backends.push_back({"inmemory",
                        {core::TransportKind::kInMemory, [](std::size_t) -> std::shared_ptr<transport::Transport> {
                             return std::make_shared<inmemory::InMemoryTransport>(core::TransportName::Unsafe("mem"));
                         }}});
#ifdef PUPPETMASTER_BENCH_WITH_SHM
    namespace shm = puppet_master::transport::shm;
    backends.push_back({"shm", {core::TransportKind::kIpc, [](std::size_t payload) -> std::shared_ptr<transport::Transport> {
                                    shm::Options options;
                                    options.domain_id = 197;
                                    options.default_topic = shm::TopicOptions {payload, 1024};
                                    return std::make_shared<shm::ShmTransport>(core::TransportName::Unsafe("shm"),
                                                                               options);
                                }}});
#endif

    std::vector<Result> results;
    for (const auto& [name, backend] : backends) {
        for (const auto size : settings.sizes) {
            results.push_back(PingPong(name, backend.first, backend.second, size, settings.pingpong_rounds));
            Print(results.back());
        }
        for (const auto size : settings.sizes) {
            results.push_back(Throughput(name, backend.first, backend.second, size, settings.throughput_messages));
            Print(results.back());
        }
    }
#ifdef PUPPETMASTER_BENCH_WITH_SHM
    // Topics are unique per run; leave nothing behind in /dev/shm.
    puppet_master::transport::shm::RemoveUnusedSegments(197);
#endif
    if (!json_path.empty()) {
        WriteJson(json_path, results);
    }
    return 0;
}
