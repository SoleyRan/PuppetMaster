# Services

A service is a request/reply call between components, possibly in different
processes: a client sends a request and waits for the one reply to it. It
covers what pub/sub does not, such as registration, queries, and commands
that must be acknowledged.

Services are built from three ordinary topics, so they need no transport
changes and inherit message versions and topic metrics. They are tested on
the in-memory transport (one process) and the shared-memory transport
(across processes, including a server crash). Any transport with queued
readers works in principle; FastDDS has not been tested yet.

## Usage

```cpp
#include <puppet_master/service/service.h>

namespace service = puppet_master::service;

service::ServiceSpec spec(core::TopicName::Unsafe("/fmc/report"));
spec.transport = core::TransportKind::kIpc;
spec.request = transport::MessageDescriptor {"fmc.Report", "application/x-fmc"};
spec.reply = transport::MessageDescriptor {"fmc.ReportResult", "application/x-fmc"};

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

A service named `/fmc/report` uses:

| Topic | Writer | Reader | Policy | Carries |
|---|---|---|---|---|
| `/fmc/report/request` | clients | server | queued | header + request payload |
| `/fmc/report/reply` | server | clients | queued | header + reply payload |
| `/fmc/report/presence` | server | clients | latest | server instance id, heartbeat |

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
- A server runs its handler on one thread, in arrival order. Only one
  server per service is supported. A second one is not detected: both would
  handle every request and the client would keep the first reply.
- Clients may call from several threads at once; each call waits for its
  own reply.

## Typed Services

```cpp
#include <puppet_master/service/typed_service.h>

const auto name = core::TopicName::Unsafe("/fmc/register");

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

## Limits

- Requests and replies are bounded by the transport's message size; on
  shared memory, by the topic's `slot_payload_size`.
- Every client receives every reply on the service and drops the ones that
  are not its own, so very many concurrent clients cost extra copies. This is
  the price of running on any pub/sub transport without changes.
- A request queue that overflows drops the oldest requests, which their
  clients see as `DeadlineExceeded`. Size the request topic's queue depth
  for the expected burst.
