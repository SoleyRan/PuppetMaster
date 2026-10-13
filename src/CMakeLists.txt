set(PUPPETMASTER_GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")

find_package(Threads REQUIRED)

configure_file(
    "${PROJECT_SOURCE_DIR}/cmake/puppet_master/version.h.in"
    "${PUPPETMASTER_GENERATED_INCLUDE_DIR}/puppet_master/version.h"
    @ONLY
)

set(PUPPETMASTER_PUBLIC_HEADERS
    "${PROJECT_SOURCE_DIR}/include/puppet_master/puppet_master.h"
    "${PROJECT_SOURCE_DIR}/include/puppet_master/export.h"
    "${PROJECT_SOURCE_DIR}/include/puppet_master/logging/log.h"
    "${PROJECT_SOURCE_DIR}/include/puppet_master/logging/logger.h"
    "${PROJECT_SOURCE_DIR}/include/puppet_master/compat/itage_facade.h"
    "${PROJECT_SOURCE_DIR}/include/puppet_master/configuration/configuration.h"
    "${PROJECT_SOURCE_DIR}/include/puppet_master/core/message_policy.h"
    "${PROJECT_SOURCE_DIR}/include/puppet_master/core/result.h"
    "${PROJECT_SOURCE_DIR}/include/puppet_master/core/status.h"
    "${PROJECT_SOURCE_DIR}/include/puppet_master/core/types.h"
    "${PROJECT_SOURCE_DIR}/include/puppet_master/observability/chrome_trace_exporter.h"
    "${PROJECT_SOURCE_DIR}/include/puppet_master/observability/goodlog_sink.h"
    "${PROJECT_SOURCE_DIR}/include/puppet_master/observability/json_log_exporter.h"
    "${PROJECT_SOURCE_DIR}/include/puppet_master/observability/observability.h"
    "${PROJECT_SOURCE_DIR}/include/puppet_master/observability/prometheus_exporter.h"
    "${PROJECT_SOURCE_DIR}/include/puppet_master/runtime/component.h"
    "${PROJECT_SOURCE_DIR}/include/puppet_master/runtime/context.h"
    "${PROJECT_SOURCE_DIR}/include/puppet_master/runtime/registry.h"
    "${PROJECT_SOURCE_DIR}/include/puppet_master/scheduler/scheduler.h"
    "${PROJECT_SOURCE_DIR}/include/puppet_master/transport/message.h"
    "${PROJECT_SOURCE_DIR}/include/puppet_master/transport/registry.h"
    "${PROJECT_SOURCE_DIR}/include/puppet_master/transport/transport.h"
    "${PROJECT_SOURCE_DIR}/include/puppet_master/transport/inmemory/inmemory_transport.h"
    "${PROJECT_SOURCE_DIR}/include/puppet_master/transport/fastdds/fastdds_transport.h"
    "${PROJECT_SOURCE_DIR}/include/puppet_master/transport/fastdds/options.h"
    "${PROJECT_SOURCE_DIR}/include/puppet_master/transport/fastdds/policy_mapping.h"
    "${PUPPETMASTER_GENERATED_INCLUDE_DIR}/puppet_master/version.h"
)

add_library(puppet_master
    puppet_master.cpp
    compat/itage_facade.cpp
    configuration/configuration.cpp
    logging/logging.cpp
    observability/observability.cpp
    runtime/component.cpp
    runtime/context.cpp
    scheduler/scheduler.cpp
    transport/inmemory/inmemory_transport.cpp
    ${PUPPETMASTER_PUBLIC_HEADERS}
)

add_library(PuppetMaster::PuppetMaster ALIAS puppet_master)

target_compile_features(puppet_master PUBLIC cxx_std_17)

target_link_libraries(puppet_master
    PUBLIC
        Threads::Threads
)

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

add_library(puppet_master_chrome_trace_adapter
    observability/chrome_trace_exporter.cpp
    "${PROJECT_SOURCE_DIR}/include/puppet_master/observability/chrome_trace_exporter.h"
)

add_library(PuppetMaster::ChromeTraceAdapter ALIAS puppet_master_chrome_trace_adapter)

target_compile_features(puppet_master_chrome_trace_adapter PUBLIC cxx_std_17)

target_link_libraries(puppet_master_chrome_trace_adapter
    PUBLIC PuppetMaster::PuppetMaster
)

target_compile_definitions(puppet_master_chrome_trace_adapter
    PRIVATE PUPPET_MASTER_EXPORTS
)

if(NOT BUILD_SHARED_LIBS)
    target_compile_definitions(puppet_master_chrome_trace_adapter PUBLIC PUPPET_MASTER_STATIC_DEFINE)
endif()

target_include_directories(puppet_master_chrome_trace_adapter
    PUBLIC
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)

if(MSVC)
    target_compile_options(puppet_master_chrome_trace_adapter PRIVATE /W4 /permissive-)
else()
    target_compile_options(puppet_master_chrome_trace_adapter PRIVATE -Wall -Wextra -Wpedantic)
endif()

set_target_properties(puppet_master_chrome_trace_adapter
    PROPERTIES
        EXPORT_NAME ChromeTraceAdapter
        OUTPUT_NAME puppet_master_chrome_trace_adapter
        POSITION_INDEPENDENT_CODE ON
        VERSION "${PROJECT_VERSION}"
        SOVERSION "${PROJECT_VERSION_MAJOR}"
)

install(
    TARGETS puppet_master_chrome_trace_adapter
    EXPORT PuppetMasterTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)

add_library(puppet_master_prometheus_adapter
    observability/prometheus_exporter.cpp
    "${PROJECT_SOURCE_DIR}/include/puppet_master/observability/prometheus_exporter.h"
)

add_library(PuppetMaster::PrometheusAdapter ALIAS puppet_master_prometheus_adapter)

target_compile_features(puppet_master_prometheus_adapter PUBLIC cxx_std_17)

target_link_libraries(puppet_master_prometheus_adapter
    PUBLIC PuppetMaster::PuppetMaster
)

target_compile_definitions(puppet_master_prometheus_adapter
    PRIVATE PUPPET_MASTER_EXPORTS
)

if(NOT BUILD_SHARED_LIBS)
    target_compile_definitions(puppet_master_prometheus_adapter PUBLIC PUPPET_MASTER_STATIC_DEFINE)
endif()

target_include_directories(puppet_master_prometheus_adapter
    PUBLIC
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)

if(MSVC)
    target_compile_options(puppet_master_prometheus_adapter PRIVATE /W4 /permissive-)
else()
    target_compile_options(puppet_master_prometheus_adapter PRIVATE -Wall -Wextra -Wpedantic)
endif()

set_target_properties(puppet_master_prometheus_adapter
    PROPERTIES
        EXPORT_NAME PrometheusAdapter
        OUTPUT_NAME puppet_master_prometheus_adapter
        POSITION_INDEPENDENT_CODE ON
        VERSION "${PROJECT_VERSION}"
        SOVERSION "${PROJECT_VERSION_MAJOR}"
)

install(
    TARGETS puppet_master_prometheus_adapter
    EXPORT PuppetMasterTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)

add_library(puppet_master_json_log_adapter
    observability/json_log_exporter.cpp
    "${PROJECT_SOURCE_DIR}/include/puppet_master/observability/json_log_exporter.h"
)

add_library(PuppetMaster::JsonLogAdapter ALIAS puppet_master_json_log_adapter)

target_compile_features(puppet_master_json_log_adapter PUBLIC cxx_std_17)

target_link_libraries(puppet_master_json_log_adapter
    PUBLIC PuppetMaster::PuppetMaster
)

target_compile_definitions(puppet_master_json_log_adapter
    PRIVATE PUPPET_MASTER_EXPORTS
)

if(NOT BUILD_SHARED_LIBS)
    target_compile_definitions(puppet_master_json_log_adapter PUBLIC PUPPET_MASTER_STATIC_DEFINE)
endif()

target_include_directories(puppet_master_json_log_adapter
    PUBLIC
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)

if(MSVC)
    target_compile_options(puppet_master_json_log_adapter PRIVATE /W4 /permissive-)
else()
    target_compile_options(puppet_master_json_log_adapter PRIVATE -Wall -Wextra -Wpedantic)
endif()

set_target_properties(puppet_master_json_log_adapter
    PROPERTIES
        EXPORT_NAME JsonLogAdapter
        OUTPUT_NAME puppet_master_json_log_adapter
        POSITION_INDEPENDENT_CODE ON
        VERSION "${PROJECT_VERSION}"
        SOVERSION "${PROJECT_VERSION_MAJOR}"
)

install(
    TARGETS puppet_master_json_log_adapter
    EXPORT PuppetMasterTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)

if(PUPPETMASTER_ENABLE_GOODLOG)
    add_library(puppet_master_goodlog_adapter
        observability/goodlog_sink.cpp
        "${PROJECT_SOURCE_DIR}/include/puppet_master/observability/goodlog_sink.h"
    )

    add_library(PuppetMaster::GoodLogAdapter ALIAS puppet_master_goodlog_adapter)

    target_compile_features(puppet_master_goodlog_adapter PUBLIC cxx_std_17)

    target_link_libraries(puppet_master_goodlog_adapter
        PUBLIC
            PuppetMaster::PuppetMaster
        PRIVATE
            GoodLog::good_log
    )

    target_compile_definitions(puppet_master_goodlog_adapter
        PRIVATE
            PUPPET_MASTER_EXPORTS
    )

    if(MSVC)
        target_compile_options(puppet_master_goodlog_adapter PRIVATE /W4 /permissive-)
    else()
        target_compile_options(puppet_master_goodlog_adapter PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    set_target_properties(puppet_master_goodlog_adapter
        PROPERTIES
            EXPORT_NAME GoodLogAdapter
            OUTPUT_NAME puppet_master_goodlog_adapter
            POSITION_INDEPENDENT_CODE ON
            VERSION "${PROJECT_VERSION}"
            SOVERSION "${PROJECT_VERSION_MAJOR}"
    )

    install(
        TARGETS puppet_master_goodlog_adapter
        EXPORT PuppetMasterTargets
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    )
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
