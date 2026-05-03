# Source Layout

This directory contains the compiled library entry point and implementation
details that are not part of the public API.

## Current Build Boundary

The compiled library currently builds only:

- `CMakeLists.txt`
- `puppet_master.cpp`

Public headers live under `../include/puppet_master` so downstream projects get
a stable include path independent of internal refactors. The core API is
currently header-only, and the public transport abstraction is also header-only.

## Migration Areas

The existing subdirectories are kept as migration material from the previous
middleware implementation:

- `base`
- `common`
- `communication`
- `trigger`
- `utils`

They are intentionally not wired into the default library target yet. Each area
should be cleaned up in a focused branch after the public boundary is stable.
This avoids mixing project skeleton work with transport, scheduler, logger, or
queue behavior changes.

## Planned Direction

Future branches should move code into these implementation layers:

- `core`: internal helpers behind the public core API.
- `transport`: backend-neutral transport interfaces and adapters.
- `runtime`: registries, lifecycle, and executor ownership.
- `component`: implementation support for user-facing components.
- `scheduler`: trigger evaluation and task dispatch.

When a migration area becomes buildable and tested, it can be connected from
`src/CMakeLists.txt` as an explicit target source or subdirectory.
