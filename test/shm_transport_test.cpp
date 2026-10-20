#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include <puppet_master/runtime/context.h>
#include <puppet_master/transport/shm/shm_transport.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace core = puppet_master::core;
namespace runtime = puppet_master::runtime;
namespace shm = puppet_master::transport::shm;
namespace transport = puppet_master::transport;

namespace {

using namespace std::chrono_literals;

constexpr core::DomainId kDomain = 7;

std::string UniqueTopic(const char* tag)
{
    static int counter = 0;
    return std::string("/pm/transport-test/") + tag + "/"
        + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "/"
        + std::to_string(++counter);
}

transport::EndpointConfig Endpoint(const std::string& topic, core::MessagePolicy policy = {},
                                   const std::string& type = "test.Bytes")
{
    auto name = core::TopicName::Create(topic);
    assert(name.ok());
    return transport::EndpointConfig {
        core::TopicSpec {name.value(), core::TransportKind::kIpc, policy},
        transport::MessageDescriptor {type, "application/octet-stream"},
    };
}

core::MessagePolicy Queued(std::size_t depth)
{
    core::MessagePolicy policy;
    policy.freshness = core::FreshnessPolicy::kQueued;
    policy.queue_depth = depth;
    return policy;
}

std::shared_ptr<shm::ShmTransport> MakeTransport(shm::Options options = {})
{
    options.domain_id = kDomain;
    auto name = core::TransportName::Create("shm");
    assert(name.ok());
    auto created = std::make_shared<shm::ShmTransport>(name.value(), options);
    assert(created->Open().ok());
    return created;
}

void Write(transport::Writer& writer, const std::string& text, transport::WriteOptions options = {})
{
    assert(writer.Write(transport::ByteView::From(text.data(), text.size()), options).ok());
}

std::string Text(const transport::Message& message)
{
    return std::string(reinterpret_cast<const char*>(message.payload.data()), message.payload.size());
}

core::Result<transport::Message> ReadWait(transport::Reader& reader, std::chrono::milliseconds timeout = 2s)
{
    transport::ReadOptions options;
    options.wait = true;
    options.timeout = timeout;
    return reader.Read(options);
}

struct TopicCleanup {
    std::string topic;
    ~TopicCleanup() { shm::RemoveSegment(kDomain, topic); }
};

void OptionsAndCapabilities()
{
    shm::Options options;
    assert(options.Validate().ok());
    options.default_topic.slot_count = 0;
    assert(options.Validate().code() == core::StatusCode::kInvalidArgument);
    options.default_topic.slot_count = 4;
    options.topics["/big"] = shm::TopicOptions {0, 4};
    assert(options.Validate().code() == core::StatusCode::kInvalidArgument);
    options.topics["/big"] = shm::TopicOptions {1 << 20, 4};
    assert(options.Validate().ok());
    assert(options.ForTopic("/big").slot_payload_size == 1u << 20);
    assert(options.ForTopic("/other").slot_count == 4);
    options.wait_poll_interval = core::Nanoseconds::zero();
    assert(options.Validate().code() == core::StatusCode::kInvalidArgument);

    auto shm_transport = MakeTransport();
    assert(shm_transport->kind() == core::TransportKind::kIpc);
    const auto caps = shm_transport->capabilities();
    assert(caps.supports_callbacks && caps.supports_blocking_read);
    assert(!caps.supports_keep_all && !caps.supports_zero_copy);
}

void EndpointValidation()
{
    shm::Options options;
    options.default_topic.slot_count = 4;
    auto shm_transport = MakeTransport(options);
    const auto topic = UniqueTopic("validate");

    auto wrong_kind = Endpoint(topic);
    wrong_kind.topic.transport = core::TransportKind::kInMemory;
    assert(shm_transport->ValidateEndpoint(wrong_kind).code() == core::StatusCode::kInvalidArgument);

    auto keep_all = Queued(2);
    keep_all.retention = core::RetentionPolicy::kKeepAll;
    assert(shm_transport->ValidateEndpoint(Endpoint(topic, keep_all)).code() == core::StatusCode::kUnsupported);

    for (auto overflow : {core::QueueOverflowPolicy::kDropNewest, core::QueueOverflowPolicy::kBlock,
                          core::QueueOverflowPolicy::kReject}) {
        auto policy = Queued(2);
        policy.overflow = overflow;
        assert(shm_transport->ValidateEndpoint(Endpoint(topic, policy)).code() == core::StatusCode::kUnsupported);
    }

    assert(shm_transport->ValidateEndpoint(Endpoint(topic, Queued(5))).code() == core::StatusCode::kInvalidArgument);
    assert(shm_transport->ValidateEndpoint(Endpoint(topic, Queued(4))).ok());

    auto closed = MakeTransport();
    assert(closed->Close().ok());
    assert(closed->CreateReader(Endpoint(topic)).status().code() == core::StatusCode::kFailedPrecondition);
}

void RoundTripPreservesMetadata()
{
    const TopicCleanup cleanup {UniqueTopic("roundtrip")};
    auto shm_transport = MakeTransport();
    auto reader = shm_transport->CreateReader(Endpoint(cleanup.topic, Queued(4)));
    auto writer = shm_transport->CreateWriter(Endpoint(cleanup.topic, Queued(4)));
    assert(reader.ok() && writer.ok());

    assert(reader.value()->Read().status().code() == core::StatusCode::kUnavailable);
    assert(reader.value()->PendingMessageCount().value() == 0);

    transport::WriteOptions explicit_time;
    explicit_time.source_timestamp = core::TimePoint(core::Nanoseconds(42));
    Write(*writer.value(), "first", explicit_time);
    const auto before = std::chrono::time_point_cast<core::Nanoseconds>(core::SteadyClock::now());
    Write(*writer.value(), "second");
    assert(reader.value()->PendingMessageCount().value() == 2);

    auto first = reader.value()->Read();
    assert(first.ok() && Text(first.value()) == "first");
    assert(first.value().metadata.sequence == 1);
    assert(first.value().metadata.source_timestamp == core::TimePoint(core::Nanoseconds(42)));
    assert(first.value().metadata.reception_timestamp != core::TimePoint {});

    auto second = reader.value()->Read();
    assert(second.ok() && Text(second.value()) == "second" && second.value().metadata.sequence == 2);
    assert(second.value().metadata.source_timestamp >= before);

    const std::string too_big(64 * 1024 + 1, 'x');
    assert(writer.value()->Write(transport::ByteView::From(too_big.data(), too_big.size())).code()
           == core::StatusCode::kOutOfRange);
}

void FanoutAndFreshness()
{
    const TopicCleanup cleanup {UniqueTopic("fanout")};
    shm::Options options;
    options.default_topic.slot_count = 8;
    auto shm_transport = MakeTransport(options);
    auto latest = shm_transport->CreateReader(Endpoint(cleanup.topic));
    auto queued = shm_transport->CreateReader(Endpoint(cleanup.topic, Queued(3)));
    auto writer = shm_transport->CreateWriter(Endpoint(cleanup.topic));
    assert(latest.ok() && queued.ok() && writer.ok());

    for (int index = 1; index <= 5; ++index) {
        Write(*writer.value(), std::to_string(index));
    }
    auto newest = latest.value()->Read();
    assert(newest.ok() && Text(newest.value()) == "5");
    assert(latest.value()->Read().status().code() == core::StatusCode::kUnavailable);

    assert(queued.value()->PendingMessageCount().value() == 3);
    for (const char* expected : {"3", "4", "5"}) {
        auto message = queued.value()->Read();
        assert(message.ok() && Text(message.value()) == expected);
    }

    // A reader created later is volatile: it does not see earlier messages.
    auto late = shm_transport->CreateReader(Endpoint(cleanup.topic, Queued(3)));
    assert(late.ok());
    assert(late.value()->Read().status().code() == core::StatusCode::kUnavailable);
    Write(*writer.value(), "6");
    auto six = late.value()->Read();
    assert(six.ok() && Text(six.value()) == "6");
}

void DescriptorConflicts()
{
    const TopicCleanup cleanup {UniqueTopic("descriptor")};
    auto first = MakeTransport();
    assert(first->CreateWriter(Endpoint(cleanup.topic)).ok());
    auto same_process = first->CreateReader(Endpoint(cleanup.topic, {}, "test.Other"));
    assert(same_process.status().code() == core::StatusCode::kInvalidArgument);

    // A second transport sees the conflict through the shared header.
    auto second = MakeTransport();
    auto other_transport = second->CreateReader(Endpoint(cleanup.topic, {}, "test.Other"));
    assert(other_transport.status().code() == core::StatusCode::kInvalidArgument);

    shm::Options other_geometry;
    other_geometry.default_topic.slot_count = 32;
    auto third = MakeTransport(other_geometry);
    assert(third->CreateReader(Endpoint(cleanup.topic)).status().code() == core::StatusCode::kFailedPrecondition);
}

void BlockingReadAndCallbacks()
{
    const TopicCleanup cleanup {UniqueTopic("blocking")};
    auto shm_transport = MakeTransport();
    auto reader = shm_transport->CreateReader(Endpoint(cleanup.topic, Queued(4)));
    auto writer = shm_transport->CreateWriter(Endpoint(cleanup.topic, Queued(4)));
    assert(reader.ok() && writer.ok());

    const auto started = std::chrono::steady_clock::now();
    assert(ReadWait(*reader.value(), 30ms).status().code() == core::StatusCode::kDeadlineExceeded);
    assert(std::chrono::steady_clock::now() - started >= 25ms);

    std::thread delayed([&] {
        std::this_thread::sleep_for(20ms);
        Write(*writer.value(), "wake");
    });
    auto woken = ReadWait(*reader.value());
    delayed.join();
    assert(woken.ok() && Text(woken.value()) == "wake");

    std::atomic<int> callbacks {0};
    assert(reader.value()->SetDataAvailableCallback([&] {
        ++callbacks;
        throw std::runtime_error("callbacks may throw");
    }).ok());
    Write(*writer.value(), "a");
    Write(*writer.value(), "b");
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (callbacks.load() < 2 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    assert(callbacks.load() == 2);
    assert(reader.value()->Read().ok() && reader.value()->Read().ok());
}

void CloseInvalidatesHandles()
{
    const TopicCleanup cleanup {UniqueTopic("close")};
    auto shm_transport = MakeTransport();
    auto reader = shm_transport->CreateReader(Endpoint(cleanup.topic, Queued(2)));
    auto writer = shm_transport->CreateWriter(Endpoint(cleanup.topic, Queued(2)));
    assert(reader.ok() && writer.ok());

    std::thread waiter([&] {
        transport::ReadOptions options;
        options.wait = true;  // timeout 0: wait until data or close
        assert(reader.value()->Read(options).status().code() == core::StatusCode::kUnavailable);
    });
    std::this_thread::sleep_for(20ms);
    assert(shm_transport->Close().ok());
    waiter.join();

    assert(!shm_transport->is_open());
    assert(reader.value()->Read().status().code() == core::StatusCode::kUnavailable);
    assert(reader.value()->PendingMessageCount().status().code() == core::StatusCode::kUnavailable);
    assert(reader.value()->SetDataAvailableCallback({}).code() == core::StatusCode::kUnavailable);
    assert(writer.value()->Write(transport::ByteView()).code() == core::StatusCode::kUnavailable);
    assert(shm_transport->Close().ok());
}

// Releasing the last reader and the transport from inside a callback runs
// the topic's teardown on the dispatcher thread itself; it must not deadlock
// or touch freed memory.
void ReleaseFromCallback()
{
    const TopicCleanup cleanup {UniqueTopic("release")};
    auto reader_transport = MakeTransport();
    auto reader = reader_transport->CreateReader(Endpoint(cleanup.topic));
    assert(reader.ok());

    std::atomic<bool> fired {false};
    struct Held {
        std::shared_ptr<shm::ShmTransport> transport;
        transport::ReaderPtr reader;
    };
    auto held = std::make_shared<Held>(Held {reader_transport, reader.value()});
    assert(reader.value()->SetDataAvailableCallback([held, &fired] {
        held->reader.reset();
        held->transport.reset();
        fired = true;
    }).ok());
    reader_transport.reset();
    reader = core::Result<transport::ReaderPtr>::FromStatus(core::Status::Cancelled("released"));

    auto writer_transport = MakeTransport();
    auto writer = writer_transport->CreateWriter(Endpoint(cleanup.topic));
    assert(writer.ok());
    Write(*writer.value(), "release");
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!fired.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    assert(fired.load());
    // Give the detached dispatcher time to unwind before the test exits.
    std::this_thread::sleep_for(50ms);
}

void RuntimeRegistration()
{
    const TopicCleanup cleanup {UniqueTopic("runtime")};
    auto context = runtime::RuntimeContext::Create();
    assert(context.ok());
    auto name = core::TransportName::Create("shm");
    assert(name.ok());

    shm::Options invalid;
    invalid.default_topic.slot_count = 0;
    assert(shm::RegisterTransport(*context.value(), name.value(), invalid).code()
           == core::StatusCode::kInvalidArgument);
    shm::Options options;
    options.domain_id = kDomain;
    assert(shm::RegisterTransport(*context.value(), name.value(), options).ok());

    auto reader = context.value()->CreateReader(Endpoint(cleanup.topic, Queued(2)));
    auto writer = context.value()->CreateWriter(Endpoint(cleanup.topic, Queued(2)));
    assert(reader.ok() && writer.ok());
    Write(*writer.value(), "through runtime");
    auto message = ReadWait(*reader.value());
    assert(message.ok() && Text(message.value()) == "through runtime");
    assert(context.value()->Close().ok());
}

// Cross-process: a child publisher, a parent subscriber, and an
// acknowledgement topic back to the child.

#ifdef _WIN32
using ChildHandle = HANDLE;

ChildHandle StartChild(const std::string& executable, const std::string& topic)
{
    std::string command = "\"" + executable + "\" --publisher " + topic;
    STARTUPINFOA startup {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process {};
    const BOOL started =
        CreateProcessA(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process);
    assert(started);
    CloseHandle(process.hThread);
    return process.hProcess;
}

int WaitChild(ChildHandle child)
{
    const DWORD result = WaitForSingleObject(child, 30000);
    if (result != WAIT_OBJECT_0) {
        TerminateProcess(child, 1);
        WaitForSingleObject(child, INFINITE);
    }
    DWORD code = 1;
    GetExitCodeProcess(child, &code);
    CloseHandle(child);
    return result == WAIT_OBJECT_0 ? static_cast<int>(code) : -1;
}
#else
using ChildHandle = pid_t;

ChildHandle StartChild(const std::string& executable, const std::string& topic)
{
    const pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        execl(executable.c_str(), executable.c_str(), "--publisher", topic.c_str(), nullptr);
        std::_Exit(127);
    }
    return pid;
}

int WaitChild(ChildHandle child)
{
    int status = 0;
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (waitpid(child, &status, WNOHANG) == child) {
            return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        }
        std::this_thread::sleep_for(10ms);
    }
    kill(child, SIGKILL);
    waitpid(child, &status, 0);
    return -1;
}
#endif

constexpr int kCrossProcessMessages = 200;

// Child: publish until the parent acknowledges every message.
int RunPublisher(const std::string& topic)
{
    auto shm_transport = MakeTransport();
    auto writer = shm_transport->CreateWriter(Endpoint(topic + "/data", Queued(16)));
    auto ack = shm_transport->CreateReader(Endpoint(topic + "/ack"));
    if (!writer.ok() || !ack.ok()) {
        return 2;
    }
    // Repeat "ready" until the parent's reader exists and answers.
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (true) {
        Write(*writer.value(), "ready");
        if (ReadWait(*ack.value(), 50ms).ok()) {
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return 3;
        }
    }
    for (int index = 0; index < kCrossProcessMessages; ++index) {
        Write(*writer.value(), "msg " + std::to_string(index));
        // Wait for each acknowledgement so the ring never overwrites. Late
        // answers to earlier "ready" messages are skipped.
        while (true) {
            auto acknowledged = ReadWait(*ack.value(), 5s);
            if (!acknowledged.ok()) {
                return 4;
            }
            const auto text = Text(acknowledged.value());
            if (text == std::to_string(index)) {
                break;
            }
            if (text != "ready") {
                return 5;
            }
        }
    }
    return 0;
}

void CrossProcessDelivery(const std::string& executable)
{
    const auto base = UniqueTopic("cross");
    const TopicCleanup data_topic {base + "/data"};
    const TopicCleanup ack_topic {base + "/ack"};
    auto shm_transport = MakeTransport();
    auto reader = shm_transport->CreateReader(Endpoint(data_topic.topic, Queued(16)));
    auto ack = shm_transport->CreateWriter(Endpoint(ack_topic.topic));
    assert(reader.ok() && ack.ok());

    auto child = StartChild(executable, base);
    int next = 0;
    bool started = false;
    while (next < kCrossProcessMessages) {
        auto message = ReadWait(*reader.value(), 10s);
        assert(message.ok());
        const auto text = Text(message.value());
        if (text == "ready") {
            // Answer every "ready": the child may miss an early answer if its
            // ack reader attached after this process wrote it.
            started = true;
            Write(*ack.value(), "ready");
            continue;
        }
        assert(started);
        assert(text == "msg " + std::to_string(next));
        // The child's steady clock is the same system-wide clock.
        const auto latency = core::SteadyClock::now() - message.value().metadata.source_timestamp;
        assert(latency >= core::SteadyClock::duration::zero() && latency < 10s);
        Write(*ack.value(), std::to_string(next));
        ++next;
    }
    assert(WaitChild(child) == 0);
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc == 3 && std::string(argv[1]) == "--publisher") {
        return RunPublisher(argv[2]);
    }
    assert(argc == 1);

    OptionsAndCapabilities();
    EndpointValidation();
    RoundTripPreservesMetadata();
    FanoutAndFreshness();
    DescriptorConflicts();
    BlockingReadAndCallbacks();
    CloseInvalidatesHandles();
    ReleaseFromCallback();
    RuntimeRegistration();
    CrossProcessDelivery(argv[0]);
    std::cout << "shm transport tests passed\n";
    return 0;
}
