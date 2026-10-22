# Estimation team's process, released on its own. It depends on interface packages for
# message types and topics, and installs its binary and node configuration.
cmake_minimum_required(VERSION 3.16)

project(EstimatorNode VERSION 1.0.0 LANGUAGES CXX)

include(GNUInstallDirs)

find_package(PuppetMaster 0.1 CONFIG REQUIRED)
find_package(EstimationInterfaces 1 CONFIG REQUIRED)

add_executable(estimator_node src/main.cpp)
target_compile_features(estimator_node PRIVATE cxx_std_17)
target_link_libraries(estimator_node
    PRIVATE
        EstimationInterfaces::estimation_interfaces
        PuppetMaster::ShmAdapter
        PuppetMaster::JsonConfigAdapter
)
if(MSVC)
    target_compile_options(estimator_node PRIVATE /W4 /permissive-)
else()
    target_compile_options(estimator_node PRIVATE -Wall -Wextra -Wpedantic)
endif()

install(TARGETS estimator_node RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(FILES config/estimator.json DESTINATION "${CMAKE_INSTALL_DATADIR}/estimator_node")
