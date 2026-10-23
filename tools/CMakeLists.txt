# Command-line tools. They read JSON, so they need the JSON adapter.

add_executable(puppet_master_config
    puppet_master_config.cpp
)

target_link_libraries(puppet_master_config
    PRIVATE
        PuppetMaster::JsonConfigAdapter
)

# `shm list|clean` needs the shared-memory adapter.
if(TARGET PuppetMaster::ShmAdapter)
    target_link_libraries(puppet_master_config PRIVATE PuppetMaster::ShmAdapter)
    target_compile_definitions(puppet_master_config PRIVATE PUPPETMASTER_CONFIG_WITH_SHM)
endif()

if(MSVC)
    target_compile_options(puppet_master_config PRIVATE /W4 /permissive-)
else()
    target_compile_options(puppet_master_config PRIVATE -Wall -Wextra -Wpedantic)
endif()

install(
    TARGETS puppet_master_config
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
