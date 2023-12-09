cmake_minimum_required(VERSION 3.0)

project(uarch-toolkit VERSION 2.0)

include("CMakeHelper.cmake")
include("CMakeExtern.cmake")

if(NOT ${CMAKE_BUILD_TYPE} STREQUAL "Debug")
    message(FATAL_ERROR "Unsupportted build type ${CMAKE_BUILD_TYPE}, "
                        "only \"Debug\" is allowed")
endif()

if(NOT ${CMAKE_SYSTEM_NAME} STREQUAL "Linux")
    message(FATAL_ERROR "Unsupported platform ${CMAKE_SYSTEM_NAME}, "
                        "only \"Linux\" is supported")
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS true) # always export compile db
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_EXTERNAL_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/extern")

set(CMAKE_C_FLAGS "-std=gnu99 -g -Wall -O1 -Wno-unknown-pragmas -Wno-unused-function")
add_definitions(-D_GNU_SOURCE) # make our life easier with Linux

detect_arch()
check_external()

# finally, include and add things
include_directories(include)

add_subdirectory(libs)
add_subdirectory(src)
add_subdirectory(tests)
