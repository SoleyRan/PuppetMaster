add_executable(puppet_master_skeleton_demo
    skeleton_demo.cpp
)

target_link_libraries(puppet_master_skeleton_demo
    PRIVATE
        PuppetMaster::PuppetMaster
)
