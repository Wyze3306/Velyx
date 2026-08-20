cmake_minimum_required(VERSION 3.24)

project(Velyx
        VERSION 0.1.0
        DESCRIPTION "Velyx - Minecraft Bedrock Edition utility client"
        LANGUAGES C CXX)

if (NOT WIN32)
    message(FATAL_ERROR
            "Velyx targets Windows x64.\n"
            "From Linux, configure with:\n"
            "  cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw64.cmake")
endif ()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif ()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

option(VELYX_BUILD_DLL "Build the Velyx client DLL" ON)
option(VELYX_BUILD_LAUNCHER "Build the Velyx launcher" ON)
option(VELYX_CONSOLE "Allocate a debug console inside the game process" OFF)
option(VELYX_WERROR "Treat warnings as errors" OFF)

set(VELYX_CHANNEL "nightly" CACHE STRING "Release channel: stable | beta | nightly")
set_property(CACHE VELYX_CHANNEL PROPERTY STRINGS stable beta nightly)

find_package(Git QUIET)
set(VELYX_COMMIT "unknown")
if (GIT_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
    execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --short HEAD
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            OUTPUT_VARIABLE VELYX_COMMIT
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
endif ()

configure_file("${CMAKE_SOURCE_DIR}/src/core/Version.hpp.in"
        "${CMAKE_BINARY_DIR}/generated/velyx/Version.hpp" @ONLY)

add_library(velyx_flags INTERFACE)

target_compile_definitions(velyx_flags INTERFACE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _WIN32_WINNT=0x0A00
        WINVER=0x0A00
        $<$<BOOL:${VELYX_CONSOLE}>:VELYX_CONSOLE>
        $<$<CONFIG:Debug>:VELYX_DEBUG>)

target_include_directories(velyx_flags INTERFACE
        "${CMAKE_SOURCE_DIR}/src"
        "${CMAKE_BINARY_DIR}/generated")

if (MSVC)
    target_compile_options(velyx_flags INTERFACE
            /utf-8 /bigobj /permissive- /W4 /wd4100 /wd4201 /wd4324
            /Zc:preprocessor
            $<$<CONFIG:Release>:/O2 /Oi /Ot /Gy /Gw /GF /Ob2>
            $<$<BOOL:${VELYX_WERROR}>:/WX>)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else ()
    target_compile_options(velyx_flags INTERFACE
            -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
            -Wno-cast-function-type -Wno-unknown-pragmas
            -fno-strict-aliasing
            $<$<CONFIG:Release>:-O2>
            $<$<BOOL:${VELYX_WERROR}>:-Werror>)
    target_compile_definitions(velyx_flags INTERFACE __STDC_FORMAT_MACROS)
endif ()

add_subdirectory(external)
add_subdirectory(src/core)

if (VELYX_BUILD_DLL)
    add_subdirectory(src/dll)
endif ()

if (VELYX_BUILD_LAUNCHER)
    add_subdirectory(src/launcher)
endif ()

message(STATUS "")
message(STATUS "  Velyx ${PROJECT_VERSION} (${VELYX_CHANNEL} / ${VELYX_COMMIT})")
message(STATUS "  compiler : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  build    : ${CMAKE_BUILD_TYPE}")
message(STATUS "  dll      : ${VELYX_BUILD_DLL}")
message(STATUS "  launcher : ${VELYX_BUILD_LAUNCHER}")
message(STATUS "")
