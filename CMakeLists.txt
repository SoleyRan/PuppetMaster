cmake_minimum_required(VERSION 3.16)

project(PuppetMaster
    VERSION 0.1.0
    DESCRIPTION "A C++ real-time middleware runtime for message-driven components and deterministic task scheduling."
    HOMEPAGE_URL "https://github.com/SoleyRan/PuppetMaster"
    LANGUAGES CXX
)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(PUPPETMASTER_IS_TOP_LEVEL ON)
else()
    set(PUPPETMASTER_IS_TOP_LEVEL OFF)
endif()

option(PUPPETMASTER_BUILD_DEMOS "Build PuppetMaster example programs." ${PUPPETMASTER_IS_TOP_LEVEL})
option(PUPPETMASTER_BUILD_TESTS "Build PuppetMaster tests." ${PUPPETMASTER_IS_TOP_LEVEL})
option(PUPPETMASTER_ENABLE_FASTDDS "Build the optional FastDDS transport adapter." OFF)

# Compatibility aliases for the first public refactor branch. New integrations
# should prefer the PUPPETMASTER_* option names because they remain scoped to
# this project when PuppetMaster is embedded as a subdirectory.
if(DEFINED BUILD_DEMO)
    message(DEPRECATION "BUILD_DEMO is deprecated; use PUPPETMASTER_BUILD_DEMOS instead.")
    set(PUPPETMASTER_BUILD_DEMOS "${BUILD_DEMO}")
endif()

if(DEFINED BUILD_TEST)
    message(DEPRECATION "BUILD_TEST is deprecated; use PUPPETMASTER_BUILD_TESTS instead.")
    set(PUPPETMASTER_BUILD_TESTS "${BUILD_TEST}")
endif()

add_subdirectory(src)

if(PUPPETMASTER_BUILD_DEMOS)
    add_subdirectory(demo)
endif()

if(PUPPETMASTER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()

set(PUPPETMASTER_CMAKE_INSTALL_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/PuppetMaster")

configure_package_config_file(
    "${PROJECT_SOURCE_DIR}/cmake/PuppetMasterConfig.cmake.in"
    "${PROJECT_BINARY_DIR}/PuppetMasterConfig.cmake"
    INSTALL_DESTINATION "${PUPPETMASTER_CMAKE_INSTALL_DIR}"
)

write_basic_package_version_file(
    "${PROJECT_BINARY_DIR}/PuppetMasterConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}"
    COMPATIBILITY SameMajorVersion
)

install(
    FILES
        "${PROJECT_BINARY_DIR}/PuppetMasterConfig.cmake"
        "${PROJECT_BINARY_DIR}/PuppetMasterConfigVersion.cmake"
    DESTINATION "${PUPPETMASTER_CMAKE_INSTALL_DIR}"
)
