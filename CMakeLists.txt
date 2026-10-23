cmake_minimum_required(VERSION 3.16)

project(PuppetMaster
    VERSION 0.2.0
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

# Before 1.0, every minor release may break the ABI and the API (0.2 added a
# virtual function to transport::Writer, for example), so the shared library
# soname and the CMake package compatibility both include the minor version.
# From 1.0 on, only a major release breaks them.
if(PROJECT_VERSION_MAJOR EQUAL 0)
    set(PUPPETMASTER_SOVERSION "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}")
    set(PUPPETMASTER_PACKAGE_COMPATIBILITY SameMinorVersion)
else()
    set(PUPPETMASTER_SOVERSION "${PROJECT_VERSION_MAJOR}")
    set(PUPPETMASTER_PACKAGE_COMPATIBILITY SameMajorVersion)
endif()

option(PUPPETMASTER_BUILD_DEMOS "Build PuppetMaster example programs." ${PUPPETMASTER_IS_TOP_LEVEL})
option(PUPPETMASTER_BUILD_TESTS "Build PuppetMaster tests." ${PUPPETMASTER_IS_TOP_LEVEL})
option(PUPPETMASTER_ENABLE_FASTDDS "Build the optional FastDDS transport adapter." OFF)
option(PUPPETMASTER_BUILD_FASTDDS_CROSS_PROCESS_SMOKE
    "Build the opt-in FastDDS cross-process smoke executable." OFF)
option(PUPPETMASTER_ENABLE_GOODLOG "Build the optional GoodLog observability adapter." OFF)
option(PUPPETMASTER_ENABLE_JSON_CONFIG "Build the optional JSON configuration adapter." OFF)
option(PUPPETMASTER_ENABLE_SHM "Build the shared-memory transport (Linux and Windows only)." ON)
option(PUPPETMASTER_BUILD_BENCHMARKS "Build transport benchmarks." OFF)
set(
    PUPPETMASTER_GOODLOG_GIT_TAG
    "2eaaa2b67388dcfd41eaf8f3dd57e7f987bc31fa"
    CACHE STRING
    "GoodLog Git tag or commit used when the package is not installed."
)

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

if(PUPPETMASTER_ENABLE_JSON_CONFIG AND NOT TARGET nlohmann_json::nlohmann_json)
    find_package(nlohmann_json 3.11 CONFIG QUIET)

    if(NOT nlohmann_json_FOUND)
        include(FetchContent)
        FetchContent_Declare(
            nlohmann_json
            GIT_REPOSITORY https://github.com/nlohmann/json.git
            GIT_TAG v3.11.3
        )
        FetchContent_MakeAvailable(nlohmann_json)
    endif()
endif()

if(PUPPETMASTER_ENABLE_GOODLOG AND NOT TARGET GoodLog::good_log)
    find_package(GoodLog CONFIG QUIET)

    if(NOT GoodLog_FOUND)
        include(FetchContent)

        # GoodLog currently uses generic BUILD_DEMO and BUILD_TEST options.
        # Keep those variables local so embedding PuppetMaster cannot alter a
        # parent project's options.
        function(puppetmaster_fetch_goodlog)
            set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
            set(BUILD_DEMO OFF)
            set(BUILD_TEST OFF)
            FetchContent_Declare(
                GoodLog
                GIT_REPOSITORY https://github.com/SoleyRan/GoodLog.git
                GIT_TAG "${PUPPETMASTER_GOODLOG_GIT_TAG}"
            )
            FetchContent_MakeAvailable(GoodLog)
        endfunction()

        puppetmaster_fetch_goodlog()
    endif()
endif()

add_subdirectory(src)

if(PUPPETMASTER_BUILD_DEMOS)
    add_subdirectory(demo)
endif()

if(PUPPETMASTER_ENABLE_JSON_CONFIG)
    add_subdirectory(tools)
endif()

if(PUPPETMASTER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()

if(PUPPETMASTER_BUILD_BENCHMARKS)
    add_subdirectory(bench)
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
    COMPATIBILITY ${PUPPETMASTER_PACKAGE_COMPATIBILITY}
)

install(
    FILES
        "${PROJECT_BINARY_DIR}/PuppetMasterConfig.cmake"
        "${PROJECT_BINARY_DIR}/PuppetMasterConfigVersion.cmake"
    DESTINATION "${PUPPETMASTER_CMAKE_INSTALL_DIR}"
)
