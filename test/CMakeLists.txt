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
