cmake_minimum_required(VERSION 3.5)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)

project(PuppetMaster)

add_subdirectory(src)

option(BUILD_DEMO "Build demo target" ON)
option(BUILD_TEST "Build test target" ON)

if(BUILD_DEMO)
    message("=============================================================")
    message("-- BUILD DEMO --")
    message("=============================================================")
    add_subdirectory(demo)
endif()

if(BUILD_TEST)
    message("=============================================================")
    message("-- BUILD TEST --")
    message("=============================================================")
    add_subdirectory(test)
endif()