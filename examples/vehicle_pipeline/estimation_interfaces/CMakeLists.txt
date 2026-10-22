# Interface package of the estimation team. It republishes the sensor topics
# it consumes through an import, so its users get both from one dependency.
cmake_minimum_required(VERSION 3.16)

project(EstimationInterfaces VERSION 1.0.0 LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

find_package(SensorInterfaces 2 CONFIG REQUIRED)

add_library(estimation_interfaces INTERFACE)
add_library(EstimationInterfaces::estimation_interfaces ALIAS estimation_interfaces)
target_include_directories(estimation_interfaces INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
target_link_libraries(estimation_interfaces INTERFACE SensorInterfaces::sensor_interfaces)

install(TARGETS estimation_interfaces EXPORT EstimationInterfacesTargets)
install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(FILES topics.json DESTINATION "${CMAKE_INSTALL_DATADIR}/estimation_interfaces")

install(EXPORT EstimationInterfacesTargets NAMESPACE EstimationInterfaces::
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/EstimationInterfaces")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/EstimationInterfacesConfig.cmake" [=[
include(CMakeFindDependencyMacro)
find_dependency(SensorInterfaces 2 CONFIG)
include("${CMAKE_CURRENT_LIST_DIR}/EstimationInterfacesTargets.cmake")
]=])
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/EstimationInterfacesConfigVersion.cmake"
    COMPATIBILITY SameMajorVersion)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/EstimationInterfacesConfig.cmake"
              "${CMAKE_CURRENT_BINARY_DIR}/EstimationInterfacesConfigVersion.cmake"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/EstimationInterfaces")
