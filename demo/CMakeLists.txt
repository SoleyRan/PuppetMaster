add_executable(puppet_master_skeleton_demo
    skeleton_demo.cpp
)

target_link_libraries(puppet_master_skeleton_demo
    PRIVATE
        PuppetMaster::PuppetMaster
)

add_executable(puppet_master_inmemory_pubsub_demo
    inmemory_pubsub_demo.cpp
)

target_link_libraries(puppet_master_inmemory_pubsub_demo
    PRIVATE
        PuppetMaster::PuppetMaster
)

add_executable(puppet_master_runtime_context_demo
    runtime_context_demo.cpp
)

target_link_libraries(puppet_master_runtime_context_demo
    PRIVATE
        PuppetMaster::PuppetMaster
)

add_executable(puppet_master_component_model_demo
    component_model_demo.cpp
)

target_link_libraries(puppet_master_component_model_demo
    PRIVATE
        PuppetMaster::PuppetMaster
)

add_executable(puppet_master_scheduler_triggers_demo
    scheduler_triggers_demo.cpp
)

target_link_libraries(puppet_master_scheduler_triggers_demo
    PRIVATE
        PuppetMaster::PuppetMaster
)

add_executable(puppet_master_configuration_system_demo
    configuration_system_demo.cpp
)

target_link_libraries(puppet_master_configuration_system_demo
    PRIVATE
        PuppetMaster::PuppetMaster
)

add_executable(puppet_master_compatibility_facade_demo
    compatibility_facade_demo.cpp
)

target_link_libraries(puppet_master_compatibility_facade_demo
    PRIVATE
        PuppetMaster::PuppetMaster
)

add_executable(puppet_master_observability_demo
    observability_demo.cpp
)

target_link_libraries(puppet_master_observability_demo
    PRIVATE
        PuppetMaster::PuppetMaster
)

if(PUPPETMASTER_ENABLE_GOODLOG)
    target_link_libraries(puppet_master_observability_demo
        PRIVATE
            PuppetMaster::GoodLogAdapter
    )
    target_compile_definitions(puppet_master_observability_demo
        PRIVATE
            PUPPETMASTER_DEMO_WITH_GOODLOG
    )
endif()
