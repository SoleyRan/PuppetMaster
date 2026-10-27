// Services through the scheduler: a route planner asks a map component for
// the cost of each road segment, all on one scheduler worker.
//
//     puppet_master_scheduled_service_demo
//
// The map serves requests in its Execute(), run by a service trigger, so its
// handler follows the scheduler's priorities instead of a thread of its own.
// The planner calls it with CallAsync from its own Execute(): a blocking Call
// there would hold the only worker, and the map could never answer. Part 2
// shows exactly that.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <puppet_master/puppet_master.h>
#include <puppet_master/service/service.h>
#include <puppet_master/service/typed_service.h>

namespace core = puppet_master::core;
namespace runtime = puppet_master::runtime;
namespace scheduler = puppet_master::scheduler;
namespace service = puppet_master::service;
namespace transport = puppet_master::transport;

using namespace std::chrono_literals;

struct SegmentQuery {
    std::uint32_t segment;
};

struct SegmentCost {
    std::uint32_t segment;
    double cost;
};

template <>
struct puppet_master::transport::MessageTypeName<SegmentQuery> {
    static constexpr const char* value = "demo.SegmentQuery";
    static constexpr std::uint32_t version = 1;
};

template <>
struct puppet_master::transport::MessageTypeName<SegmentCost> {
    static constexpr const char* value = "demo.SegmentCost";
    static constexpr std::uint32_t version = 1;
};

namespace {

service::ServiceSpec CostService()
{
    auto spec = service::TypedServiceSpec<SegmentQuery, SegmentCost>(
        core::TopicName::Unsafe("/demo/segment_cost"), core::TransportKind::kInMemory);
    spec.heartbeat_interval = 50ms;
    return spec;
}

core::TriggerSpec Trigger(core::TriggerKind kind, std::int32_t priority = 0)
{
    core::TriggerSpec trigger;
    trigger.kind = kind;
    trigger.priority = priority;
    if (kind == core::TriggerKind::kService) {
        trigger.service_dependencies = {CostService().name};
    }
    return trigger;
}

// Answers segment costs. A service trigger runs Execute() when queries
// arrive; ServePending() handles them on the scheduler's worker.
class MapComponent final : public runtime::Component {
public:
    runtime::ComponentSpec Describe() const override
    {
        runtime::ComponentSpec spec {core::ComponentName::Unsafe("map"), "answers segment costs", {}, {},
                                     {Trigger(core::TriggerKind::kService, 10)}};
        spec.servers = {CostService()};
        return spec;
    }

    core::Status Initialize(runtime::ComponentContext& context) override
    {
        service::ServerOptions options;
        options.mode = service::ServeMode::kCaller;  // no handler thread of its own
        auto server = service::MakeTypedServer<SegmentQuery, SegmentCost>(
            context.runtime(), CostService(),
            [](const SegmentQuery& query) {
                if (query.segment == 0) {
                    return core::Result<SegmentCost>::FromStatus(core::Status::NotFound("no segment 0"));
                }
                return core::Result<SegmentCost>(SegmentCost {query.segment, 1.5 * query.segment});
            },
            options);
        if (!server.ok()) {
            return server.status();
        }
        server_ = std::move(server).value();
        return core::Status::Ok();
    }

    core::Status Execute(runtime::ComponentContext&) override
    {
        auto served = server_->ServePending();
        if (!served.ok()) {
            return served.status();
        }
        std::cout << "  map: served " << served.value() << " quer" << (served.value() == 1 ? "y" : "ies")
                  << " on worker " << std::this_thread::get_id() << '\n';
        return core::Status::Ok();
    }

    core::Status Shutdown(runtime::ComponentContext&) override
    {
        server_.reset();
        return core::Status::Ok();
    }

private:
    std::unique_ptr<service::Server> server_;
};

// Sums the cost of a route, one segment per call. Each Execute() either
// sends the next query or consumes the reply that triggered it.
class PlannerComponent final : public runtime::Component {
public:
    PlannerComponent(std::vector<std::uint32_t> route, bool blocking)
        : route_(std::move(route)), blocking_(blocking)
    {
    }

    runtime::ComponentSpec Describe() const override
    {
        runtime::ComponentSpec spec {core::ComponentName::Unsafe("planner"), "plans a route", {}, {},
                                     {Trigger(core::TriggerKind::kManual)}};
        spec.clients = {CostService()};
        return spec;
    }

    core::Status Initialize(runtime::ComponentContext& context) override
    {
        auto client = service::MakeTypedClient<SegmentQuery, SegmentCost>(context.runtime(), CostService());
        if (!client.ok()) {
            return client.status();
        }
        client_.emplace(std::move(client).value());
        return core::Status::Ok();
    }

    core::Status Execute(runtime::ComponentContext&) override
    {
        if (blocking_) {
            // Holds the only worker while waiting: the map cannot run.
            const auto started = std::chrono::steady_clock::now();
            auto reply = client_->Call(SegmentQuery {route_.front()}, 300ms);
            const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started);
            std::cout << "  planner: blocking Call -> " << reply.status().ToString() << " after " << waited.count()
                      << " ms\n";
            blocking_result_ = reply.status().code();
            finished_ = true;
            return core::Status::Ok();
        }

        std::optional<core::Result<SegmentCost>> reply;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            reply.swap(reply_);
        }
        if (reply) {
            if (!reply->ok()) {
                std::cout << "  planner: query failed: " << reply->status().ToString() << '\n';
                finished_ = true;
                return reply->status();
            }
            total_ += reply->value().cost;
            std::cout << "  planner: segment " << reply->value().segment << " costs " << reply->value().cost
                      << " (route so far " << total_ << ")\n";
            ++next_;
        }
        if (next_ == route_.size()) {
            finished_ = true;
            return core::Status::Ok();
        }
        // Returns at once; the reply arrives on the client's dispatcher,
        // which only stores it and asks the scheduler to run us again.
        return client_->CallAsync(SegmentQuery {route_[next_]}, 1s, [this](core::Result<SegmentCost> result) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                reply_ = std::move(result);
            }
            scheduler_->Trigger(core::ComponentName::Unsafe("planner"));
        });
    }

    core::Status Shutdown(runtime::ComponentContext&) override
    {
        client_.reset();
        return core::Status::Ok();
    }

    void set_scheduler(scheduler::Scheduler& scheduler) { scheduler_ = &scheduler; }
    bool finished() const { return finished_.load(); }
    double total() const { return total_; }
    core::StatusCode blocking_result() const { return blocking_result_; }

private:
    std::vector<std::uint32_t> route_;
    bool blocking_;
    std::optional<service::TypedClient<SegmentQuery, SegmentCost>> client_;
    scheduler::Scheduler* scheduler_ {nullptr};
    std::mutex mutex_;
    std::optional<core::Result<SegmentCost>> reply_;
    std::size_t next_ {0};
    double total_ {0};  // only touched in Execute(), which runs serially
    std::atomic<bool> finished_ {false};
    core::StatusCode blocking_result_ {core::StatusCode::kOk};
};

// Runs one planner against the map on a one-worker scheduler, until the
// planner finishes or `timeout` passes.
bool RunPlanner(PlannerComponent& planner_ref, const std::shared_ptr<PlannerComponent>& planner,
                std::chrono::milliseconds timeout)
{
    auto created = runtime::RuntimeContext::Create();
    if (!created.ok()) {
        std::cerr << created.status().ToString() << '\n';
        return false;
    }
    auto& context = *created.value();
    auto map = std::make_shared<MapComponent>();
    const auto names = {core::ComponentName::Unsafe("map"), core::ComponentName::Unsafe("planner")};
    if (!context.RegisterComponent(map).ok() || !context.RegisterComponent(planner).ok()) {
        return false;
    }
    for (const auto& name : names) {
        for (auto status : {context.ConfigureComponent(name), context.InitializeComponent(name),
                            context.StartComponent(name)}) {
            if (!status.ok()) {
                std::cerr << name.str() << ": " << status.ToString() << '\n';
                return false;
            }
        }
    }

    scheduler::Scheduler sched(context);  // one worker
    planner_ref.set_scheduler(sched);
    if (!sched.RegisterAllComponents().ok() || !sched.Start().ok()) {
        return false;
    }
    sched.Trigger(core::ComponentName::Unsafe("planner"));
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!planner_ref.finished() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(5ms);
    }
    sched.WaitIdle(1s);
    sched.Stop();
    for (const auto& name : {core::ComponentName::Unsafe("planner"), core::ComponentName::Unsafe("map")}) {
        context.StopComponent(name);
        context.ShutdownComponent(name);
    }
    return planner_ref.finished();
}

}  // namespace

int main()
{
    std::cout << "1. CallAsync from Execute(), one worker\n";
    auto planner = std::make_shared<PlannerComponent>(std::vector<std::uint32_t> {3, 1, 4, 2}, false);
    const bool async_ok = RunPlanner(*planner, planner, 3s) && planner->total() == 15.0;
    std::cout << "  route cost: " << planner->total() << '\n';

    std::cout << "2. blocking Call from Execute(), one worker\n";
    auto blocking = std::make_shared<PlannerComponent>(std::vector<std::uint32_t> {3}, true);
    const bool blocking_ok = RunPlanner(*blocking, blocking, 3s)
        && blocking->blocking_result() == core::StatusCode::kDeadlineExceeded;
    std::cout << "  the map only got the worker after the planner had given up: use CallAsync\n";

    const bool ok = async_ok && blocking_ok;
    std::cout << (ok ? "scheduled service demo finished\n" : "scheduled service demo FAILED\n");
    return ok ? 0 : 1;
}
