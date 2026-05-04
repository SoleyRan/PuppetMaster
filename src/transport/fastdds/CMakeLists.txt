find_package(fastcdr CONFIG REQUIRED)
find_package(fastdds CONFIG QUIET)

if(fastdds_FOUND)
    set(PUPPETMASTER_FASTDDS_LIBRARY fastdds)
else()
    find_package(fastrtps CONFIG REQUIRED)
    set(PUPPETMASTER_FASTDDS_LIBRARY fastrtps)
endif()

add_library(puppet_master_fastdds
    fastdds_transport.cpp
    "${PROJECT_SOURCE_DIR}/include/puppet_master/transport/fastdds/fastdds_transport.h"
    "${PROJECT_SOURCE_DIR}/include/puppet_master/transport/fastdds/options.h"
    "${PROJECT_SOURCE_DIR}/include/puppet_master/transport/fastdds/policy_mapping.h"
)

add_library(PuppetMaster::FastDdsAdapter ALIAS puppet_master_fastdds)

target_compile_features(puppet_master_fastdds PUBLIC cxx_std_17)

target_link_libraries(puppet_master_fastdds
    PUBLIC
        PuppetMaster::PuppetMaster
        fastcdr
        ${PUPPETMASTER_FASTDDS_LIBRARY}
)

target_compile_definitions(puppet_master_fastdds
    PUBLIC
        PUPPETMASTER_HAS_FASTDDS=1
)

if(MSVC)
    target_compile_options(puppet_master_fastdds PRIVATE /W4 /permissive-)
else()
    target_compile_options(puppet_master_fastdds PRIVATE -Wall -Wextra -Wpedantic)
endif()

set_target_properties(puppet_master_fastdds
    PROPERTIES
        EXPORT_NAME FastDdsAdapter
        OUTPUT_NAME puppet_master_fastdds
        POSITION_INDEPENDENT_CODE ON
        VERSION "${PROJECT_VERSION}"
        SOVERSION "${PROJECT_VERSION_MAJOR}"
)

install(
    TARGETS puppet_master_fastdds
    EXPORT PuppetMasterTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
