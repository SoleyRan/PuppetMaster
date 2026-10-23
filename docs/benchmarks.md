# Benchmarks

`bench/transport_bench.cpp` measures the in-memory and shared-memory
transports. Build it with `PUPPETMASTER_BUILD_BENCHMARKS=ON` in a Release
build:

```bash
cmake -S . -B build-bench -DCMAKE_BUILD_TYPE=Release -DPUPPETMASTER_BUILD_BENCHMARKS=ON
cmake --build build-bench --parallel
build-bench/bench/puppet_master_transport_bench [--quick] [--json results.json]
```

## Cases

Each runs with 64 B, 4 KiB, and 64 KiB payloads.

- **pingpong**: two threads bounce a message over two topics, so every hop
  wakes a blocked reader. Reported as one-way latency (half the round trip):
  median, p99, and max over 20,000 rounds after a warm-up. This is the cost
  per hop of a request/reply call or a data-triggered chain.
- **throughput**: one writer, one queued reader (depth 1024). The writer keeps
  at most half a queue in flight, so the queue never overflows and the rate is
  what a reader that keeps up receives. Any lost message fails the run;
  without the window, the writer would only measure how fast it can overwrite
  the queue.

Shared memory runs both threads in one process here: it measures the ring,
the futex (Linux) or event (Windows) wake-up, and the copies, not
cross-process scheduling.

## Reading Results

Numbers depend on the CPU, its frequency scaling, the kernel, and what else
is running; WSL and virtual machines add wake-up latency. Compare runs only on
the same host and build, and look at medians before maxima, which a single
preemption can dominate.

`--json FILE` writes every result for scripting. `--quick` runs a few hundred
rounds, enough to show that every case works but not to measure; the
`puppet_master.transport_bench_smoke` test runs it.

CI runs the smoke test and uploads a full run's JSON as the
`transport-bench-<commit>` artifact. It does not fail on slow numbers: shared
runners vary too much between runs for a threshold to catch regressions
without false alarms. To check a change for a regression, run the benchmark
before and after on one quiet machine.
