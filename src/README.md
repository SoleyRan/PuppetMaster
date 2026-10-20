# Source Layout

This directory contains the compiled library entry point and implementation
details that are not part of the public API. Public headers live under
`../include/puppet_master` so downstream projects get a stable include path
independent of internal source organization.

Each subdirectory implements the matching public module:

- `compat`: `itage_engine` compatibility facade.
- `configuration`: project configuration model and the optional JSON loader.
- `logging`: default console logger.
- `observability`: observer, exporters, and the optional GoodLog sink.
- `runtime`: runtime context, component registry, and lifecycle.
- `scheduler`: trigger evaluation and task dispatch.
- `transport/inmemory`: in-process transport compiled into the main library.
- `transport/fastdds`: optional FastDDS adapter.

`CMakeLists.txt` lists every compiled source explicitly. Optional adapters are
separate targets so the main library keeps no third-party dependencies.
