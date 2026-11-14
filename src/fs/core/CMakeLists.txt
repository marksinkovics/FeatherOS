# SPDX-License-Identifier: MIT OR Apache-2.0
#
# The shared filesystem core. A parent build chooses how it is built:
#   FEATHERDISK_FREESTANDING  ON for a kernel: only the library, with the parent's compiler
#                             flags, against the standard library target FEATHERDISK_STD_TARGET
#   FEATHERDISK_CATCH2_TARGET the Catch2 main target for the host tests; configured on its own,
#                             the core fetches Catch2 itself
cmake_minimum_required(VERSION 3.20)

if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(FeatherDiskCore LANGUAGES CXX)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
    include(CTest)
    set(featherdisk_top_level ON)
else ()
    set(featherdisk_top_level OFF)
endif ()

option(FEATHERDISK_FREESTANDING "Build only the library, for a kernel" OFF)

add_library(featherdisk_core STATIC
    common/host_buffer.cpp
    filesystem.cpp
    partition/partition.cpp
)
target_include_directories(featherdisk_core
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)
target_compile_features(featherdisk_core PUBLIC cxx_std_23)

if (FEATHERDISK_FREESTANDING)
    if (NOT FEATHERDISK_STD_TARGET)
        message(FATAL_ERROR "A freestanding core needs FEATHERDISK_STD_TARGET")
    endif ()
    target_link_libraries(featherdisk_core PUBLIC ${FEATHERDISK_STD_TARGET})
    return()
endif ()

# The kernel build rejects exceptions and RTTI. Here only exceptions are switched off: drivers
# subclass the core's interfaces in the tests, which need RTTI, and a base class compiled without
# it would leave its type information undefined.
set(featherdisk_warnings)
if (NOT MSVC)
    set(featherdisk_warnings -Wall -Wextra -Wshadow -Werror)
    target_compile_options(featherdisk_core PRIVATE ${featherdisk_warnings} -fno-exceptions)
endif ()

# host.h for programs on an ordinary operating system.
add_library(featherdisk_standard_host STATIC host/standard_host.cpp)
target_include_directories(featherdisk_standard_host PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(featherdisk_standard_host PRIVATE cxx_std_23)
target_compile_options(featherdisk_standard_host PRIVATE ${featherdisk_warnings})

if (NOT FEATHERDISK_CATCH2_TARGET)
    if (NOT featherdisk_top_level)
        return()
    endif ()
    include(FetchContent)
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.7.1
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(Catch2)
    list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
    include(Catch)
    set(FEATHERDISK_CATCH2_TARGET Catch2::Catch2WithMain)
endif ()

add_executable(featherdisk_core_tests
    tests/common_tests.cpp
    tests/memory_device.cpp
    tests/partition_tests.cpp
)
target_include_directories(featherdisk_core_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(featherdisk_core_tests PRIVATE ${featherdisk_warnings})
target_link_libraries(featherdisk_core_tests PRIVATE
    featherdisk_core featherdisk_standard_host ${FEATHERDISK_CATCH2_TARGET})
if (COMMAND catch_discover_tests)
    catch_discover_tests(featherdisk_core_tests TEST_PREFIX "fs.core:")
else ()
    add_test(NAME fs.core COMMAND featherdisk_core_tests)
endif ()
