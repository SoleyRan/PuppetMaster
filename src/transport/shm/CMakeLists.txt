if(NOT (WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Linux"))
    message(STATUS "PuppetMaster: shared-memory transport needs Linux or Windows; skipping")
    return()
endif()

set(PUPPETMASTER_SHM_PLATFORM_SOURCES
    platform.h
    platform_common.cpp
)

if(WIN32)
    list(APPEND PUPPETMASTER_SHM_PLATFORM_SOURCES platform_windows.cpp)
else()
    list(APPEND PUPPETMASTER_SHM_PLATFORM_SOURCES platform_linux.cpp)
endif()

# Internal OS layer. An object library so the adapter target can absorb it
# later without exporting an extra target.
add_library(puppet_master_shm_platform OBJECT ${PUPPETMASTER_SHM_PLATFORM_SOURCES})

target_compile_features(puppet_master_shm_platform PUBLIC cxx_std_17)

target_include_directories(puppet_master_shm_platform
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>"
)

target_link_libraries(puppet_master_shm_platform
    PUBLIC
        PuppetMaster::PuppetMaster
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # shm_open lives in librt before glibc 2.34.
    target_link_libraries(puppet_master_shm_platform PUBLIC rt)
endif()

if(MSVC)
    target_compile_options(puppet_master_shm_platform PRIVATE /W4 /permissive-)
else()
    target_compile_options(puppet_master_shm_platform PRIVATE -Wall -Wextra -Wpedantic)
endif()

set_target_properties(puppet_master_shm_platform
    PROPERTIES
        POSITION_INDEPENDENT_CODE ON
)
