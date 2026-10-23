// Listing and cleaning up shared-memory segments: ListSegments reports what
// each segment's header says, and RemoveUnusedSegments never removes a
// segment that a live process uses.

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <puppet_master/transport/shm/shm_transport.h>

#ifndef _WIN32
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace core = puppet_master::core;
namespace shm = puppet_master::transport::shm;
namespace transport = puppet_master::transport;

namespace {

// A domain no other test uses, so listing it sees only this test's segments.
constexpr core::DomainId kDomain = 93;

std::string UniqueTopic(const char* tag)
{
    return std::string("/pm/segments-test/") + tag + "/"
        + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
}

transport::EndpointConfig Endpoint(const std::string& topic)
{
    core::MessagePolicy policy;
    policy.freshness = core::FreshnessPolicy::kQueued;
    policy.queue_depth = 4;
    return transport::EndpointConfig {
        core::TopicSpec {core::TopicName::Unsafe(topic), core::TransportKind::kIpc, policy},
        transport::MessageDescriptor {"test.Segment", "application/octet-stream", 3},
    };
}

std::unique_ptr<shm::ShmTransport> MakeTransport()
{
    shm::Options options;
    options.domain_id = kDomain;
    options.default_topic = shm::TopicOptions {128, 8};
    auto created = std::make_unique<shm::ShmTransport>(core::TransportName::Unsafe("shm"), options);
    assert(created->Open().ok());
    return created;
}

std::vector<shm::SegmentInfo> List()
{
    auto segments = shm::ListSegments(kDomain);
    assert(segments.ok());
    return segments.value();
}

const shm::SegmentInfo* Find(const std::vector<shm::SegmentInfo>& segments, const std::string& topic)
{
    for (const auto& segment : segments) {
        if (segment.topic == topic) {
            return &segment;
        }
    }
    return nullptr;
}

void CleanDomain()
{
    assert(shm::RemoveUnusedSegments(kDomain).ok());
}

#ifndef _WIN32

// A process that attaches and dies without detaching leaves a participant
// entry behind; the segment then counts as unused.
void LeaveDeadParticipant(const std::string& topic)
{
    const pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        auto child = MakeTransport();
        auto writer = child->CreateWriter(Endpoint(topic));
        std::_Exit(writer.ok() ? 0 : 1);  // no Close(): the entry stays claimed
    }
    int status = 0;
    waitpid(pid, &status, 0);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

void ReportsStates()
{
    CleanDomain();
    assert(List().empty());

    // In use: this process is attached, and the header reports what it carries.
    const auto live_topic = UniqueTopic("live");
    auto transport = MakeTransport();
    auto writer = transport->CreateWriter(Endpoint(live_topic));
    assert(writer.ok());
    for (int index = 0; index < 5; ++index) {
        assert(writer.value()->Write(transport::ByteView::From("x", 1)).ok());
    }
    auto segments = List();
    const auto* live = Find(segments, live_topic);
    assert(live != nullptr && live->state == shm::SegmentInfo::State::kInUse);
    assert(live->domain_id == kDomain && live->object_name.rfind("/pm.93.", 0) == 0);
    assert(live->descriptor.ToString() == "test.Segment v3 (application/octet-stream)");
    assert(live->geometry.slot_payload_size == 128 && live->geometry.slot_count == 8);
    assert(live->messages_written == 5 && live->size_bytes > 0);
    assert(live->live_pids.size() == 1 && live->live_pids.front() == static_cast<std::uint32_t>(getpid()));
    assert(std::string(shm::ToString(live->state)) == "in_use");

    // A dead process's entry does not keep a segment in use.
    const auto dead_topic = UniqueTopic("dead");
    LeaveDeadParticipant(dead_topic);
    segments = List();
    const auto* dead = Find(segments, dead_topic);
    assert(dead != nullptr && dead->state == shm::SegmentInfo::State::kUnused);
    assert(dead->live_pids.empty() && dead->dead_participants == 1);

    // Leftovers that are not valid segments: one never sized (its creator
    // crashed between create and size), one truncated. Names that are not
    // exactly pm.<domain>.<16 hex digits> belong to someone else.
    const std::string base = "/dev/shm/pm." + std::to_string(kDomain) + ".";
    std::ofstream(base + "00000000000000aa").close();
    std::ofstream(base + "00000000000000bb") << "short";
    std::ofstream(base + "00000000000000cc.lock") << "companion";
    std::ofstream(base + "not-a-hash") << "foreign";
    segments = List();
    std::size_t uninitialized = 0;
    std::size_t incompatible = 0;
    for (const auto& segment : segments) {
        uninitialized += segment.state == shm::SegmentInfo::State::kUninitialized ? 1 : 0;
        incompatible += segment.state == shm::SegmentInfo::State::kIncompatible ? 1 : 0;
        assert(segment.object_name.find("lock") == std::string::npos);
        assert(segment.object_name.find("not-a-hash") == std::string::npos);
    }
    assert(uninitialized == 1 && incompatible == 1 && segments.size() == 4);

    // Cleaning removes everything except the segment in use.
    auto removed = shm::RemoveUnusedSegments(kDomain);
    assert(removed.ok() && removed.value().size() == 3);
    segments = List();
    assert(segments.size() == 1 && segments.front().topic == live_topic);
    assert(writer.value()->Write(transport::ByteView::From("y", 1)).ok());  // still works

    // Once this process detaches, the segment is unused and can go too.
    writer.value().reset();
    assert(transport->Close().ok());
    segments = List();
    assert(segments.size() == 1 && segments.front().state == shm::SegmentInfo::State::kUnused);
    assert(segments.front().dead_participants == 0);
    removed = shm::RemoveUnusedSegments(kDomain);
    assert(removed.ok() && removed.value().size() == 1 && List().empty());

    std::remove((base + "00000000000000cc.lock").c_str());
    std::remove((base + "not-a-hash").c_str());
}

void FiltersByDomain()
{
    CleanDomain();
    shm::Options other_options;
    other_options.domain_id = kDomain + 1;
    shm::ShmTransport other(core::TransportName::Unsafe("shm"), other_options);
    assert(other.Open().ok());
    const auto topic = UniqueTopic("other-domain");
    auto writer = other.CreateWriter(Endpoint(topic));
    assert(writer.ok());

    assert(List().empty());
    auto everywhere = shm::ListSegments();
    assert(everywhere.ok() && Find(everywhere.value(), topic) != nullptr);
    writer.value().reset();
    assert(other.Close().ok());
    assert(shm::RemoveSegment(kDomain + 1, topic).ok());
}

#endif

}  // namespace

int main()
{
#ifdef _WIN32
    // Windows frees a segment with its last process, so there is nothing to
    // list or clean.
    assert(shm::ListSegments().status().code() == core::StatusCode::kUnsupported);
    assert(shm::RemoveUnusedSegments().status().code() == core::StatusCode::kUnsupported);
#else
    ReportsStates();
    FiltersByDomain();
#endif
    std::cout << "shm segment tests passed\n";
    return 0;
}
