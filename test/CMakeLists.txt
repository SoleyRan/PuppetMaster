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
