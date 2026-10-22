cmake_minimum_required(VERSION 3.16)

project(VehiclePipelineExample
    VERSION 1.0.0
    DESCRIPTION "Multi-process vehicle pipeline consuming PuppetMaster via find_package"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PuppetMaster 0.1 CONFIG REQUIRED)

if(NOT PUPPETMASTER_WITH_SHM)
    message(FATAL_ERROR "This example requires PuppetMaster::ShmAdapter")
endif()

if(NOT PUPPETMASTER_WITH_JSON_CONFIG)
    message(FATAL_ERROR "This example requires PuppetMaster::JsonConfigAdapter")
endif()

add_executable(vehicle_node src/vehicle_node.cpp)
target_link_libraries(vehicle_node
    PRIVATE
        PuppetMaster::PuppetMaster
        PuppetMaster::ShmAdapter
        PuppetMaster::JsonConfigAdapter
)

if(MSVC)
    target_compile_options(vehicle_node PRIVATE /W4 /permissive-)
else()
    target_compile_options(vehicle_node PRIVATE -Wall -Wextra -Wpedantic)
endif()
