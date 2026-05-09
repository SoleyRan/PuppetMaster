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
