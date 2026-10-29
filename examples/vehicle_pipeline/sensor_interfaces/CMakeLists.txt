# Interface package of the sensor team: message types and topic declarations,
# no implementation. Released on its own; consumers depend on a version of it.
cmake_minimum_required(VERSION 3.16)

project(SensorInterfaces VERSION 2.0.0 LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# Wire format versions. Raise one on every change that old readers cannot
# decode; the generated header and topics.json both take it from here.
set(WHEEL_SPEED_VERSION 2 CACHE STRING "vehicle.WheelSpeed wire format version")
set(IMU_SAMPLE_VERSION 1)

find_package(PuppetMaster 0.3 CONFIG REQUIRED)

set(GENERATED "${CMAKE_CURRENT_BINARY_DIR}/generated")
configure_file(include/sensor_interfaces/messages.h.in "${GENERATED}/include/sensor_interfaces/messages.h" @ONLY)
configure_file(topics.json.in "${GENERATED}/share/sensor_interfaces/topics.json" @ONLY)

add_library(sensor_interfaces INTERFACE)
add_library(SensorInterfaces::sensor_interfaces ALIAS sensor_interfaces)
target_include_directories(sensor_interfaces INTERFACE
    "$<BUILD_INTERFACE:${GENERATED}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
target_link_libraries(sensor_interfaces INTERFACE PuppetMaster::PuppetMaster)

install(TARGETS sensor_interfaces EXPORT SensorInterfacesTargets)
install(DIRECTORY "${GENERATED}/include/" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
# Topic declarations land in <prefix>/share/sensor_interfaces/topics.json, so
# configurations import "sensor_interfaces/topics.json" with <prefix>/share on
# PUPPETMASTER_CONFIG_PATH.
install(FILES "${GENERATED}/share/sensor_interfaces/topics.json"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/sensor_interfaces")

install(EXPORT SensorInterfacesTargets NAMESPACE SensorInterfaces::
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/SensorInterfaces")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/SensorInterfacesConfig.cmake" [=[
include(CMakeFindDependencyMacro)
find_dependency(PuppetMaster 0.3 CONFIG)
include("${CMAKE_CURRENT_LIST_DIR}/SensorInterfacesTargets.cmake")
]=])
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/SensorInterfacesConfigVersion.cmake"
    COMPATIBILITY SameMajorVersion)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/SensorInterfacesConfig.cmake"
              "${CMAKE_CURRENT_BINARY_DIR}/SensorInterfacesConfigVersion.cmake"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/SensorInterfaces")
