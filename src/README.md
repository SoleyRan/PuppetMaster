# Source Layout

This directory contains the compiled library entry point and implementation
details that are not part of the public API.

## Current Build Boundary

The main library target now compiles configuration, compatibility, logging,
observability, runtime, scheduler, and in-memory transport implementations.
Public headers remain under `../include/puppet_master` so downstream projects
get a stable include path independent of internal source organization.

## Migration Areas

The existing subdirectories are kept as migration material from the previous
middleware implementation:

- `base`
- `common`
- `communication`
- `trigger`
- `utils` except for the removed legacy logger copy

They are intentionally not wired into the default library target yet. Each area
should be cleaned up in a focused branch after the public boundary is stable.
Logging has moved to the public `include/puppet_master/logging` frontend and the
compiled `logging/logging.cpp` implementation. The old local Boost.Log copy was
removed; GoodLog now lives behind the optional observability adapter.

## Planned Direction

Future branches should move code into these implementation layers:

- `core`: internal helpers behind the public core API.
- `transport`: backend-neutral transport interfaces and adapters.
- `transport/fastdds`: optional FastDDS adapter implementation.
- `runtime`: registries, lifecycle, and executor ownership.
- `component`: implementation support for user-facing components.
- `scheduler`: trigger evaluation and task dispatch.

When a migration area becomes buildable and tested, it can be connected from
`src/CMakeLists.txt` as an explicit target source or subdirectory.
