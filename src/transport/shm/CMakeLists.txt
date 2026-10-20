if(NOT (WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Linux"))
    message(STATUS "PuppetMaster: shared-memory transport needs Linux or Windows; skipping")
    return()
endif()

set(PUPPETMASTER_SHM_INTERNAL_SOURCES
    platform.h
    platform_common.cpp
    segment.h
    segment.cpp
)

if(WIN32)
    list(APPEND PUPPETMASTER_SHM_INTERNAL_SOURCES platform_windows.cpp)
else()
    list(APPEND PUPPETMASTER_SHM_INTERNAL_SOURCES platform_linux.cpp)
endif()

# Internal OS layer and ring. An object library so unit tests can reach the
# internals while the adapter absorbs the same objects without exporting an
# extra target.
add_library(puppet_master_shm_internal OBJECT ${PUPPETMASTER_SHM_INTERNAL_SOURCES})

target_compile_features(puppet_master_shm_internal PUBLIC cxx_std_17)

target_include_directories(puppet_master_shm_internal
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>"
)

target_link_libraries(puppet_master_shm_internal
    PUBLIC
        PuppetMaster::PuppetMaster
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # shm_open lives in librt before glibc 2.34.
    target_link_libraries(puppet_master_shm_internal PUBLIC rt)
endif()

add_library(puppet_master_shm_adapter
    shm_transport.cpp
    "${PROJECT_SOURCE_DIR}/include/puppet_master/transport/shm/shm_transport.h"
    $<TARGET_OBJECTS:puppet_master_shm_internal>
)

add_library(PuppetMaster::ShmAdapter ALIAS puppet_master_shm_adapter)

target_compile_features(puppet_master_shm_adapter PUBLIC cxx_std_17)

target_include_directories(puppet_master_shm_adapter
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(puppet_master_shm_adapter
    PUBLIC
        PuppetMaster::PuppetMaster
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(puppet_master_shm_adapter PRIVATE rt)
endif()

target_compile_definitions(puppet_master_shm_adapter
    PRIVATE
        PUPPET_MASTER_EXPORTS
)

foreach(target puppet_master_shm_internal puppet_master_shm_adapter)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endforeach()

set_target_properties(puppet_master_shm_internal
    PROPERTIES
        POSITION_INDEPENDENT_CODE ON
)

set_target_properties(puppet_master_shm_adapter
    PROPERTIES
        EXPORT_NAME ShmAdapter
        OUTPUT_NAME puppet_master_shm_adapter
        POSITION_INDEPENDENT_CODE ON
        VERSION "${PROJECT_VERSION}"
        SOVERSION "${PROJECT_VERSION_MAJOR}"
)

install(
    TARGETS puppet_master_shm_adapter
    EXPORT PuppetMasterTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
