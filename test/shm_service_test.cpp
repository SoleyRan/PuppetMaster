// Services across processes over shared memory: a child process serves, the
// parent calls, then the child is killed to check that clients notice.

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <puppet_master/runtime/context.h>
#include <puppet_master/service/service.h>
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
namespace service = puppet_master::service;
namespace shm = puppet_master::transport::shm;
namespace transport = puppet_master::transport;

using namespace std::chrono_literals;

namespace {

constexpr core::DomainId kDomain = 61;

std::shared_ptr<runtime::RuntimeContext> MakeRuntime()
{
    auto context = runtime::RuntimeContext::Create();
    assert(context.ok());
    shm::Options options;
    options.domain_id = kDomain;
    options.default_topic.slot_payload_size = 1024;
    assert(shm::RegisterTransport(*context.value(), core::TransportName::Unsafe("shm"), options).ok());
    return context.value();
}

service::ServiceSpec Spec(const std::string& name)
{
    service::ServiceSpec spec(core::TopicName::Unsafe(name));
    spec.transport = core::TransportKind::kIpc;
    spec.request = transport::MessageDescriptor {"test.Text", "text/plain"};
    spec.reply = transport::MessageDescriptor {"test.Text", "text/plain"};
    spec.heartbeat_interval = 20ms;
    return spec;
}

void RemoveSegments(const std::string& name)
{
    for (const char* suffix : {"/request", "/reply", "/presence"}) {
        shm::RemoveSegment(kDomain, name + suffix);
    }
}

// Child: serve until killed, or until `seconds` pass.
int RunServer(const std::string& name)
{
    auto context = MakeRuntime();
    auto server = service::Server::Create(*context, Spec(name), [](const service::Request& request) {
        std::string text(request.payload.begin(), request.payload.end());
        return core::Result<transport::ByteBuffer>(transport::ByteBuffer(text.rbegin(), text.rend()));
    });
    if (!server.ok()) {
        std::cerr << server.status().ToString() << '\n';
        return 2;
    }
    std::this_thread::sleep_for(30s);
    return 0;
}

// Child: call once, then wait to be killed, leaving its reply topic behind
// as a crashed client would.
int RunClient(const std::string& name)
{
    auto context = MakeRuntime();
    auto client = service::Client::Create(*context, Spec(name));
    if (!client.ok() || !client.value()->Call(transport::ByteView::From("hi", 2), 5s).ok()) {
        return 2;
    }
    std::this_thread::sleep_for(30s);
    return 0;
}

#ifdef _WIN32
using ChildHandle = HANDLE;

ChildHandle StartChild(const std::string& executable, const char* mode, const std::string& name)
{
    std::string command = "\"" + executable + "\" " + mode + " " + name;
    STARTUPINFOA startup {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process {};
    const BOOL started =
        CreateProcessA(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process);
    assert(started);
    CloseHandle(process.hThread);
    return process.hProcess;
}

void Kill(ChildHandle child)
{
    TerminateProcess(child, 9);
    WaitForSingleObject(child, INFINITE);
    CloseHandle(child);
}
#else
using ChildHandle = pid_t;

ChildHandle StartChild(const std::string& executable, const char* mode, const std::string& name)
{
    const pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        execl(executable.c_str(), executable.c_str(), mode, name.c_str(), nullptr);
        std::_Exit(127);
    }
    return pid;
}

void Kill(ChildHandle child)
{
    kill(child, SIGKILL);
    int status = 0;
    waitpid(child, &status, 0);
}
#endif

ChildHandle StartServer(const std::string& executable, const std::string& name)
{
    return StartChild(executable, "--server", name);
}

#ifndef _WIN32
// Reply segments of `service` on this host, by topic name.
std::vector<std::string> ReplySegments(const std::string& service)
{
    auto segments = shm::ListSegments(kDomain);
    assert(segments.ok());
    std::vector<std::string> topics;
    for (const auto& segment : segments.value()) {
        if (segment.topic.rfind(service + "/reply/", 0) == 0) {
            topics.push_back(segment.topic);
        }
    }
    return topics;
}

void WaitUntilNoReplySegments(const std::string& service)
{
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!ReplySegments(service).empty()) {
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(10ms);
    }
}

// A crashed client cannot remove its reply topic. The server closes its
// writer for that client after reply_idle_timeout and, being the last one
// attached, removes the segment, so nothing is left in /dev/shm.
void CrashedClientLeavesNoSegment(const std::string& executable)
{
    const auto name = "/pm/service-crash/" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    RemoveSegments(name);
    auto context = MakeRuntime();
    service::ServerOptions options;
    options.reply_idle_timeout = 100ms;
    auto server = service::Server::Create(*context, Spec(name), [](const service::Request& request) {
        return core::Result<transport::ByteBuffer>(transport::ByteBuffer(request.payload.begin(), request.payload.end()));
    }, options);
    assert(server.ok());

    auto child = StartChild(executable, "--client", name);
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (server.value()->handled_count() == 0) {
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(5ms);
    }
    assert(ReplySegments(name).size() == 1);
    Kill(child);
    WaitUntilNoReplySegments(name);
    server.value()->Close();
    RemoveSegments(name);
}
#endif

void CrossProcessCallsAndCrash(const std::string& executable)
{
    const auto name = "/pm/service-test/" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    RemoveSegments(name);
    auto context = MakeRuntime();
    auto client = service::Client::Create(*context, Spec(name));
    assert(client.ok());
    auto& c = *client.value();
    assert(c.Call(transport::ByteView::From("x", 1), 1s).status().code() == core::StatusCode::kUnavailable);

    auto child = StartServer(executable, name);
    auto info = c.WaitForService(10s);
    assert(info.ok());
    for (int index = 0; index < 200; ++index) {
        const auto text = "request " + std::to_string(index);
        auto reply = c.Call(transport::ByteView::From(text.data(), text.size()), 2s);
        assert(reply.ok());
        assert(std::string(reply.value().begin(), reply.value().end()) == std::string(text.rbegin(), text.rend()));
    }

    // A crashed server process: the client reports Unavailable within a few
    // heartbeats instead of hanging until each call's timeout.
    Kill(child);
    const auto killed_at = std::chrono::steady_clock::now();
    while (c.Server().ok()) {
        assert(std::chrono::steady_clock::now() - killed_at < 2s);
        std::this_thread::sleep_for(5ms);
    }
    auto after = c.Call(transport::ByteView::From("x", 1), 5s);
    assert(after.status().code() == core::StatusCode::kUnavailable);

    // A new server process takes over the same service.
    auto restarted = StartServer(executable, name);
    auto again = c.WaitForService(10s);
    assert(again.ok() && again.value().instance_id != info.value().instance_id);
    auto reply = c.Call(transport::ByteView::From("ok", 2), 2s);
    assert(reply.ok() && std::string(reply.value().begin(), reply.value().end()) == "ko");
    Kill(restarted);
#ifndef _WIN32
    // The client's own reply topic exists while it is open, and goes with it.
    assert(ReplySegments(name).size() == 1);
#endif
    c.Close();
#ifndef _WIN32
    WaitUntilNoReplySegments(name);
#endif
    RemoveSegments(name);
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc == 3 && std::string(argv[1]) == "--server") {
        return RunServer(argv[2]);
    }
    if (argc == 3 && std::string(argv[1]) == "--client") {
        return RunClient(argv[2]);
    }
    CrossProcessCallsAndCrash(argv[0]);
#ifndef _WIN32
    CrashedClientLeavesNoSegment(argv[0]);
#endif
    std::cout << "shm service tests passed\n";
    return 0;
}
