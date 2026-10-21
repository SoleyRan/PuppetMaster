# Command-line tools. They read JSON, so they need the JSON adapter.

add_executable(puppet_master_config
    puppet_master_config.cpp
)

target_link_libraries(puppet_master_config
    PRIVATE
        PuppetMaster::JsonConfigAdapter
)

if(MSVC)
    target_compile_options(puppet_master_config PRIVATE /W4 /permissive-)
else()
    target_compile_options(puppet_master_config PRIVATE -Wall -Wextra -Wpedantic)
endif()

install(
    TARGETS puppet_master_config
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
