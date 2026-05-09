# In-Memory Transport

The in-memory transport is PuppetMaster's first concrete transport backend. It
is designed for local demos, unit tests, and scheduler development where the
runtime should not depend on FastDDS, ZMQ, shared memory, or network services.

## Scope

This transport keeps all data inside the current process:

- writers publish byte payloads to a topic channel
- readers subscribe to a topic channel
- each reader owns an independent mailbox
- a writer fans out one published message to every live reader on the topic
- callbacks notify data-driven schedulers when new data arrives
- blocking reads are available through `ReadOptions`

It is not a cross-process IPC backend yet. A future shared-memory or OS IPC
adapter should use the same `Reader`, `Writer`, and `Transport` contracts.

## Freshness Modes

`core::MessagePolicy::freshness` controls how each reader mailbox stores data.

`kLatest` keeps only the newest unread message. If several samples arrive before
the reader calls `Read()`, the reader receives the last one. This mode is useful
for state-like topics such as vehicle speed, pose, localization state, or health
signals.

`kQueued` preserves message order. This mode is useful for event-like topics
where every sample matters.

## Bounded And Unbounded Queues

Queued readers use `retention` and `queue_depth`:

- `kKeepLast` creates a bounded queue with `queue_depth` capacity
- `kKeepAll` creates an unbounded queue for the reader mailbox

For bounded queues, `overflow` decides what happens when the mailbox is full:

- `kDropOldest`: remove the oldest unread message and accept the new one
- `kDropNewest`: keep the existing queue and drop the new message
- `kReject`: return `StatusCode::kResourceExhausted`
- `kBlock`: block the writer until the reader frees space

## Threading Model

The transport is thread-safe for normal multi-module use in one process.
Channels and reader mailboxes use internal mutexes and condition variables.
Callbacks are invoked outside mailbox locks so scheduler code can safely call
back into runtime services.

## Intended Use

Use this backend to validate:

- scheduler data triggers
- component pub/sub wiring
- queue policy behavior
- local demos and tests

FastDDS, ZMQ, and IPC adapters can then focus on native backend integration
without carrying all scheduler validation work.
