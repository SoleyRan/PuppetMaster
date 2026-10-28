if(MSVC)
    add_compile_options(/UNDEBUG)
else()
    add_compile_options(-UNDEBUG)
endif()

add_executable(puppet_master_project_skeleton_test
    project_skeleton_test.cpp
)

target_link_libraries(puppet_master_project_skeleton_test
    PRIVATE
        PuppetMaster::PuppetMaster
)

add_test(
    NAME puppet_master.project_skeleton
    COMMAND puppet_master_project_skeleton_test
)

add_executable(puppet_master_core_types_test
    core_types_test.cpp
)

target_link_libraries(puppet_master_core_types_test
    PRIVATE
        PuppetMaster::PuppetMaster
)

add_test(
    NAME puppet_master.core_types
    COMMAND puppet_master_core_types_test
)

add_executable(puppet_master_transport_abstraction_test
    transport_abstraction_test.cpp
)

target_link_libraries(puppet_master_transport_abstraction_test
    PRIVATE
        PuppetMaster::PuppetMaster
)

add_test(
    NAME puppet_master.transport_abstraction
    COMMAND puppet_master_transport_abstraction_test
)

add_executable(puppet_master_typed_transport_test
    typed_transport_test.cpp
)

target_link_libraries(puppet_master_typed_transport_test
    PRIVATE
        PuppetMaster::PuppetMaster
)

add_test(
    NAME puppet_master.typed_transport
    COMMAND puppet_master_typed_transport_test
)

add_executable(puppet_master_portable_codec_test
    portable_codec_test.cpp
)

target_link_libraries(puppet_master_portable_codec_test
    PRIVATE
        PuppetMaster::PuppetMaster
)

add_test(
    NAME puppet_master.portable_codec
    COMMAND puppet_master_portable_codec_test
)

add_executable(puppet_master_component_parameters_test
    component_parameters_test.cpp
)

target_link_libraries(puppet_master_component_parameters_test
    PRIVATE
        PuppetMaster::PuppetMaster
)

add_test(
    NAME puppet_master.component_parameters
    COMMAND puppet_master_component_parameters_test
)

add_executable(puppet_master_scheduled_service_test
    scheduled_service_test.cpp
)

target_link_libraries(puppet_master_scheduled_service_test
    PRIVATE
        PuppetMaster::PuppetMaster
)

add_test(
    NAME puppet_master.scheduled_service
    COMMAND puppet_master_scheduled_service_test
)
set_tests_properties(puppet_master.scheduled_service PROPERTIES TIMEOUT 60)

add_executable(puppet_master_service_test
    service_test.cpp
)

target_link_libraries(puppet_master_service_test
    PRIVATE
        PuppetMaster::PuppetMaster
)

add_test(
    NAME puppet_master.service
    COMMAND puppet_master_service_test
)
set_tests_properties(puppet_master.service PROPERTIES TIMEOUT 60)

add_executable(puppet_master_inmemory_transport_test
    inmemory_transport_test.cpp
)

target_link_libraries(puppet_master_inmemory_transport_test
    PRIVATE
        PuppetMaster::PuppetMaster
)

add_test(
    NAME puppet_master.inmemory_transport
    COMMAND puppet_master_inmemory_transport_test
)

add_executable(puppet_master_runtime_context_test
    runtime_context_test.cpp
)

target_link_libraries(puppet_master_runtime_context_test
    PRIVATE
        PuppetMaster::PuppetMaster
)

add_test(
    NAME puppet_master.runtime_context
    COMMAND puppet_master_runtime_context_test
)

add_executable(puppet_master_component_model_test
    component_model_test.cpp
)

target_link_libraries(puppet_master_component_model_test
    PRIVATE
        PuppetMaster::PuppetMaster
)

add_test(
    NAME puppet_master.component_model
    COMMAND puppet_master_component_model_test
)

add_executable(puppet_master_scheduler_triggers_test
    scheduler_triggers_test.cpp
)

target_link_libraries(puppet_master_scheduler_triggers_test
    PRIVATE
        PuppetMaster::PuppetMaster
)

add_test(
    NAME puppet_master.scheduler_triggers
    COMMAND puppet_master_scheduler_triggers_test
)

add_executable(puppet_master_configuration_system_test
    configuration_system_test.cpp
)

target_link_libraries(puppet_master_configuration_system_test
    PRIVATE
        PuppetMaster::PuppetMaster
)

add_test(
    NAME puppet_master.configuration_system
    COMMAND puppet_master_configuration_system_test
)

if(TARGET puppet_master_configuration_system_demo)
    add_test(
        NAME puppet_master.configuration_system_demo
        COMMAND puppet_master_configuration_system_demo
    )
endif()

if(TARGET puppet_master_service_demo)
    add_test(
        NAME puppet_master.service_demo
        COMMAND puppet_master_service_demo
    )
    set_tests_properties(puppet_master.service_demo
        PROPERTIES
            PASS_REGULAR_EXPRESSION "service demo finished"
            TIMEOUT 30
    )
endif()

if(TARGET puppet_master_scheduled_service_demo)
    add_test(
        NAME puppet_master.scheduled_service_demo
        COMMAND puppet_master_scheduled_service_demo
    )
    set_tests_properties(puppet_master.scheduled_service_demo
        PROPERTIES
            PASS_REGULAR_EXPRESSION "scheduled service demo finished"
            TIMEOUT 30
    )
endif()

if(TARGET puppet_master_launcher_demo)
    set(PUPPETMASTER_DEMO_CONFIG "${PROJECT_SOURCE_DIR}/demo/config/speed_pipeline.json")

    add_test(
        NAME puppet_master.launcher_demo_validate
        COMMAND puppet_master_launcher_demo --validate "${PUPPETMASTER_DEMO_CONFIG}"
    )

    add_test(
        NAME puppet_master.launcher_demo_run
        COMMAND puppet_master_launcher_demo "${PUPPETMASTER_DEMO_CONFIG}"
    )
    set_tests_properties(puppet_master.launcher_demo_run
        PROPERTIES
            ENVIRONMENT "PUPPETMASTER_DEMO_MAX_SAMPLES=5"
            PASS_REGULAR_EXPRESSION "sample 5: "
            TIMEOUT 30
    )
endif()

if(TARGET puppet_master_config)
    add_test(
        NAME puppet_master.config_tool_validate
        COMMAND puppet_master_config validate "${PROJECT_SOURCE_DIR}/demo/config/speed_pipeline.json"
    )

    # The example manifest resolves node configs through ${VEHICLE_PREFIX},
    # which points at packages built separately. Without that install, the
    # tool must name the missing node config instead of passing.
    add_test(
        NAME puppet_master.config_tool_system_missing_node
        COMMAND puppet_master_config system validate
            "${PROJECT_SOURCE_DIR}/examples/vehicle_pipeline/integration/system.json"
    )
    set_tests_properties(puppet_master.config_tool_system_missing_node
        PROPERTIES
            ENVIRONMENT "VEHICLE_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/no-vehicle-install"
            PASS_REGULAR_EXPRESSION "config monitor_node/monitor.json of node monitor not found"
    )
endif()

if(PUPPETMASTER_ENABLE_JSON_CONFIG)
    add_executable(puppet_master_system_manifest_test
        system_manifest_test.cpp
    )

    target_link_libraries(puppet_master_system_manifest_test
        PRIVATE PuppetMaster::JsonConfigAdapter
    )

    add_test(
        NAME puppet_master.system_manifest
        COMMAND puppet_master_system_manifest_test
    )

    add_executable(puppet_master_supervisor_test
        supervisor_test.cpp
    )

    target_link_libraries(puppet_master_supervisor_test
        PRIVATE PuppetMaster::JsonConfigAdapter
    )

    add_test(
        NAME puppet_master.supervisor
        COMMAND puppet_master_supervisor_test
    )
endif()

if(PUPPETMASTER_ENABLE_JSON_CONFIG AND TARGET PuppetMaster::ShmAdapter)
    add_executable(puppet_master_topic_tool_test
        topic_tool_test.cpp
    )

    target_link_libraries(puppet_master_topic_tool_test
        PRIVATE
            PuppetMaster::JsonConfigAdapter
            PuppetMaster::ShmAdapter
    )

    add_test(
        NAME puppet_master.topic_tool
        COMMAND puppet_master_topic_tool_test $<TARGET_FILE:puppet_master_topic>
    )

    add_executable(puppet_master_json_imports_test
        json_imports_test.cpp
    )

    target_link_libraries(puppet_master_json_imports_test
        PRIVATE
            PuppetMaster::JsonConfigAdapter
            PuppetMaster::ShmAdapter
    )

    add_test(
        NAME puppet_master.json_imports
        COMMAND puppet_master_json_imports_test
    )
endif()

if(PUPPETMASTER_ENABLE_JSON_CONFIG)
    add_executable(puppet_master_json_config_loader_test
        json_config_loader_test.cpp
    )

    target_link_libraries(puppet_master_json_config_loader_test
        PRIVATE PuppetMaster::JsonConfigAdapter
    )

    add_test(
        NAME puppet_master.json_config_loader
        COMMAND puppet_master_json_config_loader_test
    )

    add_executable(puppet_master_json_launcher_test
        json_launcher_test.cpp
    )

    target_link_libraries(puppet_master_json_launcher_test
        PRIVATE PuppetMaster::JsonConfigAdapter
    )

    add_test(
        NAME puppet_master.json_launcher
        COMMAND puppet_master_json_launcher_test
    )
endif()

add_executable(puppet_master_compatibility_facade_test
    compatibility_facade_test.cpp
)

target_link_libraries(puppet_master_compatibility_facade_test
    PRIVATE
        PuppetMaster::PuppetMaster
)

add_test(
    NAME puppet_master.compatibility_facade
    COMMAND puppet_master_compatibility_facade_test
)

add_executable(puppet_master_observability_core_test
    observability_core_test.cpp
)

target_link_libraries(puppet_master_observability_core_test
    PRIVATE
        PuppetMaster::PuppetMaster
)

add_test(
    NAME puppet_master.observability_core
    COMMAND puppet_master_observability_core_test
)

add_executable(puppet_master_chrome_trace_exporter_test
    chrome_trace_exporter_test.cpp
)

target_link_libraries(puppet_master_chrome_trace_exporter_test
    PRIVATE
        PuppetMaster::ChromeTraceAdapter
)

add_test(
    NAME puppet_master.chrome_trace_exporter
    COMMAND puppet_master_chrome_trace_exporter_test
)

add_executable(puppet_master_prometheus_exporter_test
    prometheus_exporter_test.cpp
)

target_link_libraries(puppet_master_prometheus_exporter_test
    PRIVATE
        PuppetMaster::PrometheusAdapter
)

add_test(
    NAME puppet_master.prometheus_exporter
    COMMAND puppet_master_prometheus_exporter_test
)

add_executable(puppet_master_json_log_exporter_test
    json_log_exporter_test.cpp
)

target_link_libraries(puppet_master_json_log_exporter_test
    PRIVATE
        PuppetMaster::JsonLogAdapter
)

add_test(
    NAME puppet_master.json_log_exporter
    COMMAND puppet_master_json_log_exporter_test
)

add_executable(puppet_master_logging_test
    logging_test.cpp
)

target_link_libraries(puppet_master_logging_test
    PRIVATE
        PuppetMaster::PuppetMaster
)

add_test(
    NAME puppet_master.logging
    COMMAND puppet_master_logging_test
)

add_executable(puppet_master_transport_observability_test
    transport_observability_test.cpp
)

target_link_libraries(puppet_master_transport_observability_test
    PRIVATE
        PuppetMaster::PuppetMaster
)

add_test(
    NAME puppet_master.transport_observability
    COMMAND puppet_master_transport_observability_test
)

if(PUPPETMASTER_ENABLE_GOODLOG)
    add_executable(puppet_master_goodlog_sink_test
        goodlog_sink_test.cpp
    )

    target_link_libraries(puppet_master_goodlog_sink_test
        PRIVATE
            PuppetMaster::GoodLogAdapter
    )

    add_test(
        NAME puppet_master.goodlog_sink
        COMMAND puppet_master_goodlog_sink_test
    )
endif()

add_executable(puppet_master_fastdds_policy_mapping_test
    fastdds_policy_mapping_test.cpp
)

target_link_libraries(puppet_master_fastdds_policy_mapping_test
    PRIVATE
        PuppetMaster::PuppetMaster
)

add_test(
    NAME puppet_master.fastdds_policy_mapping
    COMMAND puppet_master_fastdds_policy_mapping_test
)

if(PUPPETMASTER_ENABLE_FASTDDS)
    if(PUPPETMASTER_BUILD_FASTDDS_CROSS_PROCESS_SMOKE)
        add_executable(puppet_master_fastdds_cross_process_smoke
            fastdds_cross_process_smoke.cpp
        )

        target_link_libraries(puppet_master_fastdds_cross_process_smoke
            PRIVATE
                PuppetMaster::FastDdsAdapter
        )
    endif()

    add_executable(puppet_master_fastdds_transport_test
        fastdds_transport_test.cpp
    )

    target_link_libraries(puppet_master_fastdds_transport_test
        PRIVATE
            PuppetMaster::FastDdsAdapter
    )

    add_test(
        NAME puppet_master.fastdds_transport
        COMMAND puppet_master_fastdds_transport_test
    )
    set_tests_properties(puppet_master.fastdds_transport PROPERTIES TIMEOUT 60)

    add_executable(puppet_master_fastdds_service_test
        fastdds_service_test.cpp
    )

    target_link_libraries(puppet_master_fastdds_service_test
        PRIVATE
            PuppetMaster::FastDdsAdapter
    )

    add_test(
        NAME puppet_master.fastdds_service
        COMMAND puppet_master_fastdds_service_test
    )
    set_tests_properties(puppet_master.fastdds_service PROPERTIES TIMEOUT 120)
endif()

if(TARGET puppet_master_shm_internal)
    add_executable(puppet_master_shm_platform_test
        shm_platform_test.cpp
    )

    target_link_libraries(puppet_master_shm_platform_test
        PRIVATE
            puppet_master_shm_internal
    )

    add_test(
        NAME puppet_master.shm_platform
        COMMAND puppet_master_shm_platform_test
    )
endif()

if(TARGET puppet_master_shm_internal)
    add_executable(puppet_master_shm_segment_test
        shm_segment_test.cpp
    )

    target_link_libraries(puppet_master_shm_segment_test
        PRIVATE
            puppet_master_shm_internal
    )

    add_test(
        NAME puppet_master.shm_segment
        COMMAND puppet_master_shm_segment_test
    )
endif()

if(TARGET PuppetMaster::ShmAdapter)
    add_executable(puppet_master_shm_transport_test
        shm_transport_test.cpp
    )

    target_link_libraries(puppet_master_shm_transport_test
        PRIVATE
            PuppetMaster::ShmAdapter
    )

    add_test(
        NAME puppet_master.shm_transport
        COMMAND puppet_master_shm_transport_test
    )

    add_executable(puppet_master_shm_segments_test
        shm_segments_test.cpp
    )

    target_link_libraries(puppet_master_shm_segments_test
        PRIVATE
            PuppetMaster::ShmAdapter
    )

    # Lists and removes everything in its own shm domain, so it must not run
    # alongside another instance of itself.
    add_test(
        NAME puppet_master.shm_segments
        COMMAND puppet_master_shm_segments_test
    )
    set_tests_properties(puppet_master.shm_segments PROPERTIES RUN_SERIAL TRUE)

    add_executable(puppet_master_shm_service_test
        shm_service_test.cpp
    )

    target_link_libraries(puppet_master_shm_service_test
        PRIVATE
            PuppetMaster::ShmAdapter
    )

    add_test(
        NAME puppet_master.shm_service
        COMMAND puppet_master_shm_service_test
    )
    set_tests_properties(puppet_master.shm_service PROPERTIES TIMEOUT 60)
endif()

if(TARGET puppet_master_shm_internal)
    add_executable(puppet_master_shm_crash_test
        shm_crash_test.cpp
    )

    target_link_libraries(puppet_master_shm_crash_test
        PRIVATE
            puppet_master_shm_internal
    )

    add_test(
        NAME puppet_master.shm_crash
        COMMAND puppet_master_shm_crash_test
    )
endif()

add_executable(puppet_master_application_test
    application_test.cpp
)

target_link_libraries(puppet_master_application_test
    PRIVATE
        PuppetMaster::PuppetMaster
)

add_test(
    NAME puppet_master.application
    COMMAND puppet_master_application_test
)
