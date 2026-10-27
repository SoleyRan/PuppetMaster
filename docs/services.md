# Services

A service is a request/reply call between components, possibly in different
processes: a client sends a request and waits for the one reply to it. It
covers what pub/sub does not, such as registration, queries, and commands
that must be acknowledged.

Services are built from three ordinary topics, so they need no transport
changes and inherit message versions and topic metrics. They are tested on
the in-memory transport (one process), the shared-memory transport (across
processes, including a server crash), and FastDDS 2.x and 3.x (across
participants, including a stop and restart).

On FastDDS, discovery is asynchronous: a client can see the server's
heartbeat before its own request writer has matched the server's request
reader, and a request written then would be dropped. `Call` therefore also
waits, within its timeout, for `Writer::MatchedReaderCount()` to become
non-zero, and returns `Unavailable` if it does not. Requests and replies ask
for reliable delivery, so a lost UDP packet is retransmitted instead of
failing the call.

## Usage

```cpp
#include <puppet_master/service/service.h>

namespace service = puppet_master::service;

service::ServiceSpec spec(core::TopicName::Unsafe("/map/query"));
spec.transport = core::TransportKind::kIpc;
spec.request = transport::MessageDescriptor {"map.Query", "application/x-map"};
spec.reply = transport::MessageDescriptor {"map.QueryResult", "application/x-map"};

// Server: one per service.
auto server = service::Server::Create(runtime, spec, [](const service::Request& request) {
    return core::Result<transport::ByteBuffer>(Handle(request.payload));
});

// Client: any number, in any process.
auto client = service::Client::Create(runtime, spec);
auto reply = client.value()->Call(transport::ByteView(request_bytes), 500ms);
```

`ServiceSpec` also sets the request queue depth (default 16) and the
heartbeat interval (default 100 ms). A first `Call` from a new client waits
up to one liveness window (three heartbeat intervals) to see the server, so
`WaitForService` is only needed to wait longer, for example at startup.

With typed messages,
`service::MakeTypedServer<Request, Reply>` and
`service::MakeTypedClient<Request, Reply>` encode both sides with their
codecs; see [Typed Services](#typed-services).

## Topics

A service named `/map/query` uses:

| Topic | Writer | Reader | Policy | Carries |
|---|---|---|---|---|
| `/map/query/request` | clients | server | queued | header + request payload |
| `/map/query/reply` | server | clients | queued | header + reply payload |
| `/map/query/presence` | server | clients | latest | server instance id, heartbeat |

The request and reply descriptors are those of the payload types, with the
type name prefixed `service.request:` and `service.reply:` so a plain
subscriber of the same type cannot read them by accident. Their versions
carry over, so a client built against another version of the request type is
refused when it is created.

### Wire Header

Every request and reply starts with a fixed 32-byte header, little-endian:

```text
u32 magic            "PMRQ" / "PMRP"
u16 header_version   1
u16 status           reply only: core::StatusCode of the handler result
u64 client_id        random per client instance
u64 request_id       per-client counter, starting at 1
u64 budget_ns        request only: how long the client waits; 0 = no limit
```

A reply with a non-OK status carries the handler's error message as its
payload, so the client returns that status with its message.

### Presence

The server writes `{u64 instance_id, u64 sequence}` on the presence topic at
start and every `heartbeat_interval` (default 100 ms), from a thread of its
own, so a long-running handler does not make the server look dead: presence
means the process is alive, not idle. A slow handler shows up as
`DeadlineExceeded`, a dead process as `Unavailable`.

A client considers the service available while it received a new heartbeat
within `3 * heartbeat_interval`, measured on its own clock when the heartbeat
arrives. Source timestamps are not used, because not every transport carries
them and they do not compare across hosts. A changed `instance_id` means the
server restarted.

`Server::Close()` writes a goodbye (sequence 0) after its last heartbeat, so
clients of a server that stops in order fail their calls at once with
`Unavailable` ("the server stopped") instead of waiting out the liveness
window. Only a crashed server is detected by silence. A goodbye from an older
instance does not affect a newer one.

## Semantics

- `Call` returns the reply, or:
  - `Unavailable`: no live server when the call started, or the server's
    heartbeat stopped while waiting;
  - `DeadlineExceeded`: the server is alive but did not reply in time;
  - the handler's own error status and message;
  - `Cancelled`: the client was closed during the call.
- Calls are at-most-once. A request is never resent, so a timed-out call may
  or may not have been handled. Make handlers idempotent or carry an
  operation id where that matters.
- One reply per request. Replies for other clients, late replies after a
  timeout, and duplicates are dropped by the client.
- The request carries the client's timeout as a duration, not a time point,
  because steady clocks of different hosts do not compare. The server
  measures it from when it received the request, and drops requests whose
  time has run out when it dequeues them, without running the handler.
  Transit time is not counted, so the server errs toward handling a request
  its client may just have given up on.
- A server runs its handler on one thread at a time, in arrival order: its
  own thread by default, or the scheduler's, see
  [Scheduled Servers](#scheduled-servers). Only one
  server per service is supported. The transport does not prevent a second
  one, which would handle every request too; `validate --system` reports it
  before deployment (see [Configuration](#configuration)).
- Clients may call from several threads at once; each call waits for its
  own reply.

## Scheduled Servers

By default a server runs its handler on a thread of its own, outside the
scheduler: a slow handler competes with every component for the CPU, and its
work has no priority or deadline. A server created with `ServeMode::kCaller`
has no handler thread. The component that owns it calls `ServePending()` in
`Execute()`, and a [service trigger](scheduler.md#service-triggers) runs
`Execute()` when a request arrives:

```cpp
class MapServer final : public runtime::Component {
public:
    runtime::ComponentSpec Describe() const override
    {
        runtime::ComponentSpec spec {core::ComponentName::Unsafe("map"), "answers map queries"};
        spec.servers = {query_spec_};
        core::TriggerSpec trigger;
        trigger.kind = core::TriggerKind::kService;
        trigger.service_dependencies = {query_spec_.name};
        trigger.priority = 10;
        spec.triggers = {trigger};
        return spec;
    }

    core::Status Initialize(runtime::ComponentContext& context) override
    {
        service::ServerOptions options;
        options.mode = service::ServeMode::kCaller;
        auto server = service::Server::Create(context.runtime(), query_spec_, Handle, options);
        if (!server.ok()) {
            return server.status();
        }
        server_ = std::move(server).value();
        return core::Status::Ok();
    }

    core::Status Execute(runtime::ComponentContext&) override
    {
        auto served = server_->ServePending();
        return served.ok() ? core::Status::Ok() : served.status();
    }

private:
    service::ServiceSpec query_spec_ {core::TopicName::Unsafe("/map/query")};
    std::unique_ptr<service::Server> server_;
};
```

- `ServePending(limit)` handles what is queued when it is called, at most
  `limit` requests (0: all queued), in arrival order. Requests arriving during
  the pass wait for the next execution, so a burst cannot hold the component.
  Requests that pile up before the component runs coalesce into one
  execution.
- Heartbeats keep their own thread in both modes. A component busy with other
  work is alive; its clients wait for their reply, not report `Unavailable`.
- A request's deadline still applies: one that waited in the queue past its
  client's timeout is dropped when its turn comes, unhandled.
- `ServePending` returns `FailedPrecondition` on a `kOwnThread` server, after
  `Close()`, and when called from its own handler. `Close()` waits for a
  handler that is running, except when called from that handler.

## Asynchronous Calls

`Call` blocks its thread until the reply. From a component's `Execute()` that
holds a scheduler worker for the whole call. With one worker, calling a
[scheduled server](#scheduled-servers) on the same scheduler cannot work: the
server needs the worker the caller is holding, and the call times out.
`CallAsync` sends the request and returns at once:

```cpp
core::Status Execute(runtime::ComponentContext&) override
{
    if (auto reply = TakeReply()) {
        return Use(*reply);  // a later execution consumes the reply
    }
    return client_->CallAsync(request, 500ms, [this](core::Result<transport::ByteBuffer> reply) {
        StoreReply(std::move(reply));
        scheduler_->Trigger(name_);  // run Execute() again to use it
    });
}
```

- `done` is called exactly once, with what `Call` would have returned: on the
  client's dispatcher thread for a reply, a timeout, or a server that stopped;
  on the calling thread before `CallAsync` returns if the call fails before it
  is sent (no live server, client closed); on the closing thread with
  `Cancelled` for calls still pending at `Close()`.
- `done` runs on the dispatcher that delivers every reply of this client, so
  it should only hand the result on. It may chain another `CallAsync`; it may
  not use `Call`, which returns `FailedPrecondition` there.
- Like `Call`, `CallAsync` may first wait up to one liveness window for a
  server it has not seen yet, except from `done`.
- `PendingCalls()` counts calls sent and not completed. Keep it within the
  service's `queue_depth`: requests beyond it are dropped on overflow and
  time out.
- `TypedClient::CallAsync` decodes the reply; an undecodable reply reaches
  `done` as the codec's error.

## Typed Services

```cpp
#include <puppet_master/service/typed_service.h>

const auto name = core::TopicName::Unsafe("/system/register");

auto server = service::MakeTypedServer<RegisterRequest, RegisterReply>(
    runtime, name, core::TransportKind::kIpc,
    [](const RegisterRequest& request) { return core::Result<RegisterReply>(Register(request)); });

auto client = service::MakeTypedClient<RegisterRequest, RegisterReply>(
    runtime, name, core::TransportKind::kIpc);
auto reply = client.value().Call(RegisterRequest {...}, 500ms);   // Result<RegisterReply>
```

Both use `transport::Codec<T>`, so versions declared on the request and
reply types apply to the service: a client built against another version of
`RegisterRequest` is refused when it is created, naming both versions. A
request that fails to decode is answered with the decode error instead of
reaching the handler. For other settings, build the spec with
`TypedServiceSpec<Request, Reply>(name, transport)`, adjust it, and pass it
to the overloads that take a `ServiceSpec`.

## Configuration

Services are declared like topics, with a local id and a wire name, and
components list the ones they provide (`servers`) and call (`clients`):

```json
{
    "services": [{
        "id": "query", "name": "/map/query",
        "request_type": "map.Query", "request_version": 2,
        "reply_type": "map.QueryResult", "encoding": "application/x-map",
        "queue_depth": 16, "heartbeat_interval_ns": 100000000
    }],
    "components": [
        {"name": "map", "type": "map_server", "servers": ["query"]},
        {"name": "planner", "type": "planner", "clients": ["query"]}
    ]
}
```

`request_version` and `reply_version` default to 0, `encoding` to
`application/octet-stream`, `queue_depth` to 16, and the heartbeat to 100 ms.

A [service trigger](scheduler.md#service-triggers) names service ids from the
component's `servers`:

```json
{"name": "map", "type": "map_server", "servers": ["query"],
 "triggers": [{"kind": "service", "services": ["query"], "priority": 10, "deadline_ns": 5000000}]}
```

`CreateServer` and `CreateTypedServer` (below) then create that server with
`ServeMode::kCaller`, and the component calls `ServePending()` in `Execute()`.
Without the trigger they create it with its own thread, and `ServePending()`
returns `FailedPrecondition`.

Interface files may declare `services` next to `topics`, so the team that
serves a service publishes its contract once and every caller imports it.
Deployment files place them by wire name; the settings apply to all three of
the service's topics, and `shm::OptionsFromProject` sizes them:

```json
{"services": [{"name": "/map/query", "transport": "shm",
               "transport_parameters": {"slot_payload_size": 65536}}]}
```

The component spec then carries each service's full `ServiceSpec`, and
`puppet_master/service/component_services.h` creates servers and clients from
it by service name, so component code does not repeat transport or queue
settings:

```cpp
core::Status Initialize(runtime::ComponentContext& context) override
{
    auto server = service::CreateTypedServer<MapQuery, MapQueryResult>(
        context, spec_, "/map/query", [this](const MapQuery& query) { return Lookup(query); });
    ...
}
```

`CreateServer` and `CreateClient` return `NotFound` when the spec does not
declare that service in that role. The typed variants also refuse a
configured request or reply descriptor that differs from the types' codecs,
naming both versions.

`--describe` lists services and which components serve and call them.
`validate --system` checks them across processes like topics: the same
contract (types, versions, encoding, transport) everywhere, exactly one
serving process, which is how a second server is caught before it runs, and
a warning for a service that no process calls.

## Example

`demo/service_demo.cpp` runs a parameter store service with two clients:
typed get/set calls, handler errors, a slow handler that times out
(`DeadlineExceeded`), an orderly server stop that fails calls at once
(`Unavailable`), and a restart picked up through a new instance id.

## Limits

- Requests and replies are bounded by the transport's message size; on
  shared memory, by the topic's `slot_payload_size`.
- Every client receives every reply on the service and drops the ones that
  are not its own, so very many concurrent clients cost extra copies. This is
  the price of running on any pub/sub transport without changes.
- A request queue that overflows drops the oldest requests, which their
  clients see as `DeadlineExceeded`. Size the request topic's queue depth
  for the expected burst.
