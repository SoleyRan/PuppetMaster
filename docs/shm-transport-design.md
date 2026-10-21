# Shared-Memory Transport Design

Status: accepted. Implementation follows the steps below; see
[Decisions](#decisions) for choices made during review.

## Goals

- Same-host, cross-process pub/sub behind the existing `Transport`, `Reader`,
  and `Writer` contract. Components switch backends by changing the topic's
  transport kind, not their code.
- Linux and Windows, both exercised in CI.
- No third-party dependencies. Only OS APIs.
- Survive a peer process crashing at any point without deadlocking or
  corrupting the topic for the remaining processes.
- Carry real source timestamps, so topic latency metrics work across processes.
- Leave room for zero-copy publishing in a follow-up step without changing the
  v1 behavior.

## Non-Goals

- Cross-host communication. That remains FastDDS.
- Reader-side zero-copy (see [Zero-Copy Writes](#zero-copy-writes)).
- Keep-all retention and writer backpressure (`kBlock`, `kReject`,
  `kDropNewest`) in v1.
- Service discovery beyond "processes that agree on a domain and topic name".
- Security isolation between processes of the same user (see
  [Security](#security)).

## Alternatives Considered

| Option | Why not |
|---|---|
| FastDDS `TransportMode::kSharedMemory` | Already available, but requires FastDDS, copies twice through DDS history, and cannot loan unbounded byte payloads. |
| iceoryx / Boost.Interprocess | Third-party dependency; iceoryx also needs a broker daemon. |
| Unix domain sockets / named pipes | Simple, but a kernel copy per reader makes large payloads (point clouds, images) expensive. |
| Variable-size byte ring | Better memory use, but overwrite and torn-read handling get much harder. Fixed slots with per-topic sizing are simpler and O(1) to index. |

## Overview

Each topic maps to one shared-memory segment per domain. The segment holds a
header and a fixed ring of slots. Writers copy a payload into the next slot
under a cross-process writer lock. Readers never take a lock: each reader keeps
a private cursor and validates every slot with a sequence check, so a slow or
dead reader can never block writers.

```text
process A                          shared segment "pm.<domain>.<topic-hash>"
+-----------------+                +-----------------------------------------+
| ShmWriter ------+--- lock ------>| header: magic, layout, sizes, write_seq |
+-----------------+    copy        |         descriptor, participant table   |
                                   | slot 0: seq | ts | size | payload...    |
process B                          | slot 1: seq | ts | size | payload...    |
+-----------------+                | ...                                     |
| dispatcher -----+<-- notify -----| slot N-1                                |
|  ShmReader x2   |--- copy out ---|                                         |
+-----------------+                +-----------------------------------------+
```

Within a process, one dispatcher thread per subscribed topic waits for
notifications and fans them out to that process's readers on the topic. Each
reader has its own cursor, so two readers in one process behave like two
independent subscribers.

## Public API

The backend reuses the existing `core::TransportKind::kIpc` and follows the
FastDDS adapter's shape:

```cpp
#include <puppet_master/transport/shm/shm_transport.h>

namespace shm = puppet_master::transport::shm;

shm::Options options;
options.domain_id = 3;
options.topics["/lidar/points"] = shm::TopicOptions {4 * 1024 * 1024, 8};

auto status = shm::RegisterTransport(runtime, name, options);
```

```cpp
struct TopicOptions {
    std::size_t slot_payload_size {64 * 1024};  // max bytes per message
    std::size_t slot_count {16};                // ring capacity
};

struct Options {
    core::DomainId domain_id {0};
    TopicOptions default_topic;
    std::map<std::string, TopicOptions> topics;  // per-topic override by name
    core::Nanoseconds wait_poll_interval {std::chrono::milliseconds(50)};
};
```

Slot size is a per-topic deployment decision, so it lives in adapter options
rather than in `core::TopicSpec`. Every process on a topic must agree on the
layout; a mismatch is reported when the segment is opened.

CMake adds `PuppetMaster::ShmAdapter`, built by default and controlled by
`PUPPETMASTER_ENABLE_SHM`. It has no third-party dependencies; on Linux it
links `rt` for `shm_open` on older glibc. The configuration parser
accepts `"shm"` as an alias for `"ipc"`.

## Segment Layout

All fields are fixed-width, naturally aligned, and lock-free atomics where
noted. Each slot starts on a 64-byte boundary to avoid false sharing.

```text
SegmentHeader (one per topic)
  u64 magic              "PMSHM001", written last by the creator
  u32 layout_version
  u32 header_size
  u64 slot_payload_size
  u32 slot_count
  char topic_name[256]   full name, detects hash collisions
  char type_name[256]
  char encoding[256]
  atomic<u64> write_seq  last committed sequence, 0 = none yet
  atomic<u32> notify_word  futex word / change counter
  WriterLock lock        Linux: robust process-shared pthread mutex
  Participant participants[64]

Participant
  atomic<u32> state      2-bit kind (free / claiming / active) + 30-bit generation
  atomic<u32> pid
  atomic<u64> process_start_time  guards against pid reuse

Slot (slot_count entries, 64-byte header then payload)
  atomic<u64> seq        committed sequence; high bit set while writing
  atomic<i64> source_timestamp_ns   relaxed; read speculatively under the seqlock
  atomic<u64> payload_size          relaxed; validated before use
  byte payload[slot_payload_size]
```

The generation in `state` changes on every claim, so a process reclaiming a
dead entry cannot free one that was released and claimed again in between.

## Publish And Read Protocol

Slots use a seqlock. Sequence `s` lives in slot `s % slot_count`.

Writer:

1. Acquire the writer lock.
2. `s = write_seq + 1`; store `slot.seq = s | kWritingBit`, then issue a
   release fence so payload stores cannot become visible before the bit.
3. Copy the payload, size, and timestamp into the slot.
4. Store `slot.seq = s` (release), then `write_seq = s` (release).
5. Release the lock and notify.

Reader (lock-free, private `cursor` = next sequence to read):

1. `w = write_seq` (acquire). If `cursor > w`, there is no data.
2. Apply freshness: `kLatest` sets `cursor = w`; `kQueued` with depth `d`
   clamps `cursor` to at least `w - d + 1`. Skipped sequences are drops.
3. Load `slot.seq` (acquire). If it equals `cursor`, continue. Otherwise a
   writer has overwritten the slot or is overwriting it right now (masked
   sequence greater than `cursor`), so message `cursor` is lost: count a drop,
   advance `cursor`, and go back to step 1. Any other value means a corrupt
   slot and is treated the same way. This rule also covers a writer that
   crashed mid-write, which would otherwise leave the reader retrying forever.
4. Copy the payload out, then re-load `slot.seq`. If it changed, the copy may
   be torn: discard it, count a drop, advance `cursor`, and go back to step 1.
5. Return the message and advance `cursor`.

Every pass through steps 1 to 4 either returns a message or advances `cursor`,
so a read finishes in at most `queue_depth` passes.

A reader that falls a full ring behind loses the oldest messages, which is the
`kDropOldest` behavior. Because readers never write shared state, any number of
them can read, stall, or crash without affecting writers or each other.

The copy in step 4 races with a writer by design, and the sequence re-check
discards any torn result. The implementation places an acquire fence between
the copy and the re-check, and copies into a private buffer that is only
handed to the caller after validation, as in standard seqlock readers.

## Policy Mapping

| Policy | Behavior |
|---|---|
| `kLatest` | Reader jumps to the newest message. |
| `kQueued` + `kKeepLast` | Reader sees up to `queue_depth` unread messages; `queue_depth > slot_count` is `InvalidArgument`. |
| `kKeepAll` | `Unsupported`. A fixed ring cannot retain unbounded history. |
| `kDropOldest` | Supported; this is ring overwrite. |
| `kDropNewest`, `kBlock`, `kReject` | `Unsupported` in v1. Each needs writers to track the slowest reader. |
| `kBestEffort`, `kReliable` | Both accepted. Delivery on one host never loses messages except by the overflow policy, matching the in-memory transport. |

Other contract points:

- Readers are volatile: a new reader starts after the current `write_seq`.
- A payload larger than `slot_payload_size` returns `OutOfRange` from `Write()`.
- `metadata.sequence` is the topic-wide sequence, so gaps reveal drops.
- `source_timestamp` is preserved, including explicit `WriteOptions` values.
  `std::chrono::steady_clock` is system-wide on both platforms
  (`CLOCK_MONOTONIC` on Linux, `QueryPerformanceCounter` on Windows), so
  cross-process latency is meaningful, unlike the FastDDS adapter.
- `PendingMessageCount()` returns unread messages after the depth clamp.
- After `Close()`, remaining handles return `Unavailable`.

## Notification And Callbacks

Each process runs one dispatcher thread per topic it reads. The dispatcher
waits for a writer notification, compares `write_seq` with the last value it
saw, then:

- wakes blocking `Read()` calls through a process-local condition variable, and
- invokes each reader's callback once per new message, capped at that reader's
  effective depth, outside all locks.

The per-message callback matches the in-memory transport, so the scheduler's
data triggers behave the same on both backends.

Waits use `wait_poll_interval` as an upper bound, so a lost wakeup delays
delivery by at most one interval instead of stalling it.

## Platform Layer

An internal layer hides the OS differences behind three small classes:
`SharedRegion` (create or open a named mapping), `WriterLock`, and `Notifier`.

| Concern | Linux | Windows |
|---|---|---|
| Segment | `shm_open` + `ftruncate` + `mmap`, name `/pm.<domain>.<hash>` | `CreateFileMappingW` backed by the page file, name `Local\pm.<domain>.<hash>` |
| Writer lock | Robust, process-shared `pthread_mutex_t` inside the header | Named mutex `Local\pm.<domain>.<hash>.lock` |
| Dead lock owner | `EOWNERDEAD`, then `pthread_mutex_consistent` | `WAIT_ABANDONED` |
| Notify | `futex` wake-all on `notify_word` (shared, not private) | One auto-reset named event per participant; the writer signals each active one |
| Liveness | pid plus start time from `/proc/<pid>/stat` | `OpenProcess` plus `GetProcessTimes` |

Windows `Local\` names are per logon session, so a service in session 0 and
a desktop process cannot share a topic in v1. `<hash>` is a 64-bit FNV-1a
hash of the topic name. The full name is stored in
the header, and a collision is reported as `FailedPrecondition`. macOS lacks
robust mutexes and futexes, so the adapter is not built there.

## Lifecycle And Crash Recovery

- `Open()` only validates options. A topic's segment is opened on the first
  `CreateReader` or `CreateWriter` for it.
- Creation race: the creator uses exclusive create, initializes the header,
  and publishes `magic` last. Other processes wait up to one second for
  `magic`, then check the layout. A mismatch in sizes, type name, or encoding
  returns `FailedPrecondition`.
- Each process claims one participant entry per topic; 64 per topic, then
  `ResourceExhausted`. Entries of dead processes are reclaimed on join.
- Writer crash inside the lock: the next writer takes ownership of the
  abandoned lock and continues. The interrupted slot still has `kWritingBit`
  set and `write_seq` never advanced, so readers never see it.
- Reader crash: no shared state to repair.
- `Close()` stops dispatchers, releases participant entries, and unmaps.
- Segment removal: Windows frees the mapping when the last handle closes. On
  Linux the segment persists in `/dev/shm` after all processes exit, so a
  restarted system reuses it. `shm::RemoveSegment(domain, topic)` deletes one
  explicitly. See [Decisions](#decisions).
- Liveness checks compare pids, so on Linux every process on a topic must
  share one PID namespace. Containers that share `/dev/shm` but not the PID
  namespace are not supported in v1.

## Security

Segments are created with owner-only permissions (`0600` on Linux, the default
`Local\` session DACL on Windows). Any process of the same user can read and
write a topic. Readers validate every size and index read from shared memory
before using it, so a corrupt or hostile segment can cause dropped messages
but not out-of-bounds access in the reader.

## Zero-Copy Writes

`transport::Writer` has a virtual `WriteWith` next to `Write`:

```cpp
using FillFunction = std::function<core::Result<std::size_t>(MutableByteView buffer)>;

virtual core::Status WriteWith(std::size_t capacity, const FillFunction& fill,
                               WriteOptions options = {});
```

The shm writer runs `fill` on the slot payload itself: it takes the writer
lock, marks the slot as writing, calls `fill`, then commits steps 4 and 5 of
the writer protocol with the size `fill` returns. The producer serializes
directly into shared memory, saving one copy. A failed or throwing `fill`
releases the lock without advancing `write_seq`, so nothing becomes visible.
Every other backend inherits a default that fills a temporary buffer and calls
`Write()`, so callers use one API everywhere and check
`TransportCapabilities::supports_zero_copy` only to know whether it avoids the
copy.

This replaces the loan objects (`LoaningWriter::Loan()` returning a
`LoanedBuffer`) proposed in the first draft, for two reasons found during
implementation:

- `RuntimeContext::CreateWriter` wraps every writer in an observing decorator,
  so a `dynamic_cast` from the returned writer to `LoaningWriter` would always
  fail. A virtual on `Writer` is forwarded by the decorator like `Write()`,
  which also keeps topic metrics correct.
- A callback bounds how long the lock is held to the duration of `fill`. A
  loan object can be kept indefinitely, which needed a lock timeout; a scoped
  call does not.

`fill` runs while the cross-process writer lock is held, so it should be short
and must not write to the same topic. A producer that crashes inside `fill` is
handled like any writer crash inside the lock.

Under ThreadSanitizer, `fill` writes to a private buffer that is then copied
with instrumented atomic stores, so the intended seqlock race is not reported.
Release builds always fill in place.

Reader-side zero-copy (borrowed read-only views) is out of scope: under ring
overwrite, a borrowed slot can be rewritten while the reader still uses it,
which requires reader reference counts that writers must honor. That
reintroduces the slow-reader coupling the seqlock avoids. It can be revisited
together with `kBlock` support.

## Testing

- Unit tests for the layout, seqlock, depth clamp, and option validation,
  using two transports in one process that share segments.
- Contract tests: the existing in-memory scenarios (fan-out, freshness,
  callbacks, blocking reads, close semantics) run against the shm backend.
- Cross-process test: the test binary re-launches itself as publisher and
  subscriber children, following `test/fastdds_cross_process_smoke.cpp`.
  Unlike the FastDDS smoke test it needs no network, so it runs in normal
  `ctest` on both Linux and Windows CI.
- Crash test: a child process is killed while holding the writer lock
  (forced through a test-only hook); the parent must keep publishing and
  reading.
- Linux CI runs the full test suite under AddressSanitizer with
  UndefinedBehaviorSanitizer, and under ThreadSanitizer.

## Implementation Steps

Each step is one commit:

1. Platform layer (`SharedRegion`, `WriterLock`, `Notifier`) with unit tests.
   Done.
2. Segment layout, seqlock ring, `ShmTransport` with reader, writer, and
   dispatcher; contract and cross-process tests; docs page. Done; participant
   reclamation landed here as well.
3. Crash test for a writer dying inside the lock; sanitizer CI job. Done.
4. Zero-copy writes through `Writer::WriteWith` and
   `supports_zero_copy = true`. Done.

## Decisions

Resolved during review (2026-10-20):

1. Linux segment cleanup: segments stay in `/dev/shm` after all processes
   exit; `RemoveSegment()` deletes one explicitly. Automatic unlink by the last
   participant was rejected because it races with a process that is joining.
2. Default slot size: 64 KiB x 16 slots (about 1 MiB per topic). Large-payload
   topics configure their own size.
3. JSON configuration does not expose `TopicOptions` in v1; they are set in
   code through `shm::Options`.
