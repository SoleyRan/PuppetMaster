# Sensor team's process, released on its own. It depends on interface packages for
# message types and topics, and installs its binary and node configuration.
cmake_minimum_required(VERSION 3.16)

project(SensorNode VERSION 1.0.0 LANGUAGES CXX)

include(GNUInstallDirs)

find_package(PuppetMaster 0.2 CONFIG REQUIRED)
find_package(SensorInterfaces 2 CONFIG REQUIRED)

add_executable(sensor_node src/main.cpp)
target_compile_features(sensor_node PRIVATE cxx_std_17)
target_link_libraries(sensor_node
    PRIVATE
        SensorInterfaces::sensor_interfaces
        PuppetMaster::ShmAdapter
        PuppetMaster::JsonConfigAdapter
)
if(MSVC)
    target_compile_options(sensor_node PRIVATE /W4 /permissive-)
else()
    target_compile_options(sensor_node PRIVATE -Wall -Wextra -Wpedantic)
endif()

install(TARGETS sensor_node RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(FILES config/sensors.json DESTINATION "${CMAKE_INSTALL_DATADIR}/sensor_node")
