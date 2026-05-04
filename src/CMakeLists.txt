set(PUPPETMASTER_GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")

configure_file(
    "${PROJECT_SOURCE_DIR}/cmake/puppet_master/version.h.in"
    "${PUPPETMASTER_GENERATED_INCLUDE_DIR}/puppet_master/version.h"
    @ONLY
)

set(PUPPETMASTER_PUBLIC_HEADERS
    "${PROJECT_SOURCE_DIR}/include/puppet_master/puppet_master.h"
    "${PROJECT_SOURCE_DIR}/include/puppet_master/export.h"
    "${PROJECT_SOURCE_DIR}/include/puppet_master/core/message_policy.h"
    "${PROJECT_SOURCE_DIR}/include/puppet_master/core/result.h"
    "${PROJECT_SOURCE_DIR}/include/puppet_master/core/status.h"
    "${PROJECT_SOURCE_DIR}/include/puppet_master/core/types.h"
    "${PROJECT_SOURCE_DIR}/include/puppet_master/transport/message.h"
    "${PROJECT_SOURCE_DIR}/include/puppet_master/transport/registry.h"
    "${PROJECT_SOURCE_DIR}/include/puppet_master/transport/transport.h"
    "${PROJECT_SOURCE_DIR}/include/puppet_master/transport/fastdds/fastdds_transport.h"
    "${PROJECT_SOURCE_DIR}/include/puppet_master/transport/fastdds/options.h"
    "${PROJECT_SOURCE_DIR}/include/puppet_master/transport/fastdds/policy_mapping.h"
    "${PUPPETMASTER_GENERATED_INCLUDE_DIR}/puppet_master/version.h"
)

add_library(puppet_master
    puppet_master.cpp
    ${PUPPETMASTER_PUBLIC_HEADERS}
)

add_library(PuppetMaster::PuppetMaster ALIAS puppet_master)

target_compile_features(puppet_master PUBLIC cxx_std_17)

target_include_directories(puppet_master
    PUBLIC
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
        "$<BUILD_INTERFACE:${PUPPETMASTER_GENERATED_INCLUDE_DIR}>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)

target_compile_definitions(puppet_master
    PRIVATE
        PUPPET_MASTER_EXPORTS
)

if(NOT BUILD_SHARED_LIBS)
    target_compile_definitions(puppet_master PUBLIC PUPPET_MASTER_STATIC_DEFINE)
endif()

if(MSVC)
    target_compile_options(puppet_master PRIVATE /W4 /permissive-)
else()
    target_compile_options(puppet_master PRIVATE -Wall -Wextra -Wpedantic)
endif()

set_target_properties(puppet_master
    PROPERTIES
        EXPORT_NAME PuppetMaster
        OUTPUT_NAME puppet_master
        POSITION_INDEPENDENT_CODE ON
        VERSION "${PROJECT_VERSION}"
        SOVERSION "${PROJECT_VERSION_MAJOR}"
)

install(
    TARGETS puppet_master
    EXPORT PuppetMasterTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)

if(PUPPETMASTER_ENABLE_FASTDDS)
    add_subdirectory(transport/fastdds)
endif()

install(
    DIRECTORY "${PROJECT_SOURCE_DIR}/include/"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)

install(
    DIRECTORY "${PUPPETMASTER_GENERATED_INCLUDE_DIR}/"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)

install(
    EXPORT PuppetMasterTargets
    NAMESPACE PuppetMaster::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/PuppetMaster"
)
