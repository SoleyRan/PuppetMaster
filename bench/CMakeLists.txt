# Benchmarks. Built with PUPPETMASTER_BUILD_BENCHMARKS; not installed.

add_executable(puppet_master_transport_bench
    transport_bench.cpp
)

target_link_libraries(puppet_master_transport_bench
    PRIVATE
        PuppetMaster::PuppetMaster
)

if(TARGET PuppetMaster::ShmAdapter)
    target_link_libraries(puppet_master_transport_bench PRIVATE PuppetMaster::ShmAdapter)
    target_compile_definitions(puppet_master_transport_bench PRIVATE PUPPETMASTER_BENCH_WITH_SHM)
endif()

if(MSVC)
    target_compile_options(puppet_master_transport_bench PRIVATE /W4 /permissive-)
else()
    target_compile_options(puppet_master_transport_bench PRIVATE -Wall -Wextra -Wpedantic)
endif()

# A quick pass that only proves every case still runs, not a measurement.
if(PUPPETMASTER_BUILD_TESTS)
    add_test(
        NAME puppet_master.transport_bench_smoke
        COMMAND puppet_master_transport_bench --quick
    )
    set_tests_properties(puppet_master.transport_bench_smoke PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
endif()
