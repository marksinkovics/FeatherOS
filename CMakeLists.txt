cmake_minimum_required(VERSION 3.15)

if (NOT DEFINED ARCH)
    set(ARCH "x86_64" CACHE STRING "Kernel target architecture")
endif()

set_property(CACHE ARCH PROPERTY STRINGS x86_64 aarch64)

if (ARCH STREQUAL "x86_64")
    set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_LIST_DIR}/toolchains/x86_64.cmake")
    project(FeatherOS LANGUAGES C CXX ASM_NASM)
    set(CMAKE_ASM_NASM_OBJECT_FORMAT elf64)
    add_compile_definitions(ARCH_x86_64)
elseif (ARCH STREQUAL "aarch64")
    set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_LIST_DIR}/toolchains/aarch64.cmake")
    project(FeatherOS LANGUAGES C CXX ASM)
    add_compile_definitions(ARCH_aarch64)
else ()
    message(FATAL_ERROR "Unsupported ARCH='${ARCH}'. Choose x86_64 or aarch64.")
endif ()

set(KERNEL_BINARY "kernel.elf")
set(CMAKE_EXPORT_COMPILE_COMMANDS 1)

add_executable(${KERNEL_BINARY})

set(SRCROOT ${PROJECT_SOURCE_DIR})

target_include_directories(${KERNEL_BINARY} PRIVATE ${SRCROOT} ${SRCROOT}/deps/stivale ${SRCROOT}/src)

add_subdirectory(${SRCROOT}/deps)
add_subdirectory(${SRCROOT}/src)

target_link_libraries(${KERNEL_BINARY} PRIVATE limine)