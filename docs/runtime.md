# Runtime Context

The runtime layer is the first place where PuppetMaster starts to look like a
middleware instead of a collection of transport utilities. It owns the shared
registries and provides one entry point for assembling components, transports,
readers, and writers.

## Scope

This milestone adds:

- `ComponentSpec`
- `Component`
- `ComponentContext`
- `ComponentRegistry`
- `RuntimeOptions`
- `RuntimeContext`

It does not execute tasks automatically yet. Scheduling policy, timers, and
dependency resolution remain separate milestones.

## ComponentSpec

`ComponentSpec` is a declarative description of a component:

- component name
- optional description
- reader endpoints
- writer endpoints
- trigger specs

The spec is deliberately data-oriented. Later code can bind real task bodies
and lifecycle callbacks to this declaration without changing the public
transport API.

`Component` is the executable algorithm module interface. A component returns
its `ComponentSpec` from `Describe()`, then receives explicit lifecycle calls
such as `Configure()`, `Initialize()`, `Start()`, `Execute()`, `Stop()`, and
`Shutdown()`.

## ComponentRegistry

`ComponentRegistry` stores component declarations by name. It is thread-safe and
returns copies of registered specs, so runtime internals stay protected from
accidental external mutation.

Duplicate names return `StatusCode::kAlreadyExists`. Missing names return
`StatusCode::kNotFound`.

## RuntimeContext

`RuntimeContext` is the shared runtime assembly point:

- owns the component registry
- owns component instances and lifecycle states
- owns the transport registry
- installs a default in-memory transport
- opens and closes registered transports
- creates readers and writers from `EndpointConfig`
- drives explicit component lifecycle calls

Endpoint creation is still routed through the transport abstraction. The context
only selects the backend from `TopicSpec::transport`; it does not know how a DDS,
ZMQ, IPC, or in-memory adapter actually moves data.

## Default Local Backend

By default, `RuntimeContext::Create()` registers and opens an in-memory
transport named `inmemory`. This gives demos, tests, and the future scheduler a
reliable local backend that does not depend on FastDDS or other external
services.

The default can be disabled:

```cpp
puppet_master::runtime::RuntimeOptions options;
options.register_inmemory_transport = false;
auto context = puppet_master::runtime::RuntimeContext::Create(options);
```

## Example

```cpp
auto context = puppet_master::runtime::RuntimeContext::Create();

auto reader = context.value()->CreateReader(endpoint);
auto writer = context.value()->CreateWriter(endpoint);

writer.value()->Write(puppet_master::transport::ByteView::From(data, size));
auto message = reader.value()->Read();
```

This is the shape the scheduler can use next: register components, inspect
their declared triggers, create transport endpoints, and then drive task
execution from data or time.
