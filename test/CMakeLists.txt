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
