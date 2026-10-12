#include <cassert>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include <puppet_master/core/types.h>
#include <puppet_master/transport/fastdds/fastdds_transport.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace core = puppet_master::core;
namespace transport = puppet_master::transport;
namespace fastdds = puppet_master::transport::fastdds;

namespace {

transport::EndpointConfig Endpoint(const std::string& topic, const std::string& type)
{
    auto name = core::TopicName::Create(topic);
    assert(name.ok());
    core::MessagePolicy policy;
    policy.delivery = core::DeliveryGuarantee::kReliable;
    policy.queue_depth = 8;
    return transport::EndpointConfig {
        core::TopicSpec {name.value(), core::TransportKind::kFastDds, policy},
        transport::MessageDescriptor {type, "application/octet-stream"}
    };
}

std::unique_ptr<fastdds::FastDdsTransport> MakeTransport(const std::string& participant)
{
    auto name = core::TransportName::Create(participant);
    assert(name.ok());
    fastdds::Options options;
    options.domain_id = 190;
    options.participant_name = participant;
    options.transport_mode = fastdds::TransportMode::kUdp;
    return std::make_unique<fastdds::FastDdsTransport>(name.value(), options);
}

int RunChild(const std::string& role, const std::string& topic)
{
    auto transport_instance = MakeTransport("PuppetMasterCrossProcess_" + role);
    assert(transport_instance->Open().ok());

    const auto data = Endpoint(topic, "puppet_master.cross_process.Bytes");
    const auto acknowledgement = Endpoint(topic + "/ack", "puppet_master.cross_process.Ack");
    auto reader = transport_instance->CreateReader(role == "subscriber" ? data : acknowledgement);
    auto writer = transport_instance->CreateWriter(role == "subscriber" ? acknowledgement : data);
    assert(reader.ok());
    assert(writer.ok());

    transport::ReadOptions timeout_options;
    timeout_options.wait = true;
    timeout_options.timeout = std::chrono::milliseconds(20);
    auto empty_reader = transport_instance->CreateReader(
        Endpoint(topic + "/empty/" + role, "puppet_master.cross_process.Empty"));
    assert(empty_reader.ok());
    auto timeout_result = empty_reader.value()->Read(timeout_options);
    assert(!timeout_result.ok());
    assert(timeout_result.status().code() == core::StatusCode::kDeadlineExceeded);

    std::string payload("cross-process\0payload\xff", 22);
    payload.append(512, 'x');
    const std::string acknowledgement_payload = "received";
    transport::ReadOptions read_options;
    read_options.wait = true;
    read_options.timeout = std::chrono::milliseconds(100);

    bool received = false;
    for (int attempt = 0; attempt < 120 && !received; ++attempt) {
        if (role == "publisher") {
            assert(writer.value()->Write(
                transport::ByteView::From(payload.data(), payload.size())).ok());
        }
        auto message = reader.value()->Read(read_options);
        if (message.ok()) {
            const auto& bytes = message.value().payload;
            const std::string actual(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            if (role == "subscriber") {
                assert(actual == payload);
                // Keep the acknowledgement writer alive while discovery and delivery complete.
                for (int retry = 0; retry < 20; ++retry) {
                    assert(writer.value()->Write(transport::ByteView::From(
                        acknowledgement_payload.data(), acknowledgement_payload.size())).ok());
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            } else {
                assert(actual == acknowledgement_payload);
            }
            received = true;
        } else {
            assert(message.status().code() == core::StatusCode::kDeadlineExceeded);
        }
    }
    assert(received);
    assert(transport_instance->Close().ok());
    return 0;
}

#ifdef _WIN32
struct Child {
    PROCESS_INFORMATION process {};
};

Child StartChild(const std::string& executable, const std::string& role, const std::string& topic)
{
    std::string command = "\"" + executable + "\" --child " + role + " " + topic;
    STARTUPINFOA startup {};
    startup.cb = sizeof(startup);
    Child child;
    std::string mutable_command = command;
    if (CreateProcessA(nullptr, mutable_command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                       &startup, &child.process) == 0) {
        std::cerr << "Failed to start " << role << ": " << GetLastError() << '\n';
    }
    return child;
}

int WaitChild(Child& child, std::chrono::milliseconds timeout)
{
    if (child.process.hProcess == nullptr) {
        return -1;
    }
    const DWORD result = WaitForSingleObject(child.process.hProcess, static_cast<DWORD>(timeout.count()));
    if (result != WAIT_OBJECT_0) {
        TerminateProcess(child.process.hProcess, 1);
        WaitForSingleObject(child.process.hProcess, INFINITE);
        CloseHandle(child.process.hThread);
        CloseHandle(child.process.hProcess);
        return -1;
    }
    DWORD code = 1;
    GetExitCodeProcess(child.process.hProcess, &code);
    CloseHandle(child.process.hThread);
    CloseHandle(child.process.hProcess);
    return static_cast<int>(code);
}
#else
pid_t StartChild(const std::string& executable, const std::string& role, const std::string& topic)
{
    const pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        execl(executable.c_str(), executable.c_str(), "--child", role.c_str(), topic.c_str(), nullptr);
        std::_Exit(127);
    }
    return pid;
}

int WaitChild(pid_t pid, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    int status = 1;
    while (std::chrono::steady_clock::now() < deadline) {
        const pid_t result = waitpid(pid, &status, WNOHANG);
        if (result == pid) {
            return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
    return -1;
}
#endif

}  // namespace

int main(int argc, char** argv)
{
    if (argc == 4 && std::string(argv[1]) == "--child") {
        return RunChild(argv[2], argv[3]);
    }
    assert(argc == 1);

    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::string topic = "/puppet_master/cross_process/" + std::to_string(suffix);
#ifdef _WIN32
    Child subscriber = StartChild(argv[0], "subscriber", topic);
    Child publisher = StartChild(argv[0], "publisher", topic);
    const int subscriber_result = WaitChild(subscriber, std::chrono::seconds(15));
    const int publisher_result = WaitChild(publisher, std::chrono::seconds(15));
    if (subscriber_result != 0 || publisher_result != 0) {
        std::cerr << "FastDDS cross-process smoke failed: subscriber=" << subscriber_result
                  << ", publisher=" << publisher_result << '\n';
        return 1;
    }
#else
    const pid_t subscriber = StartChild(argv[0], "subscriber", topic);
    const pid_t publisher = StartChild(argv[0], "publisher", topic);
    const int subscriber_result = WaitChild(subscriber, std::chrono::seconds(15));
    const int publisher_result = WaitChild(publisher, std::chrono::seconds(15));
    if (subscriber_result != 0 || publisher_result != 0) {
        std::cerr << "FastDDS cross-process smoke failed: subscriber=" << subscriber_result
                  << ", publisher=" << publisher_result << '\n';
        return 1;
    }
#endif
    return 0;
}
