cmake_minimum_required(VERSION 3.15)

if (ARCH STREQUAL "x86_64")
    set(CMAKE_TOOLCHAIN_FILE "toolchains/x86_64.cmake")
endif()

project(FeatherOS LANGUAGES C CXX ASM_NASM)

set(KERNEL_BINARY "kernel.elf")
set(CMAKE_ASM_NASM_OBJECT_FORMAT elf64)
set(CMAKE_EXPORT_COMPILE_COMMANDS 1)

# Enable nasm
enable_language(ASM_NASM)

add_executable(${KERNEL_BINARY})

set(SRCROOT ${PROJECT_SOURCE_DIR})

if (ARCH STREQUAL "x86_64")
    add_compile_definitions(ARCH_x86_64)
endif()

target_include_directories(${KERNEL_BINARY} PRIVATE ${SRCROOT} ${SRCROOT}/deps/stivale ${SRCROOT}/src)

add_subdirectory(${SRCROOT}/deps)
add_subdirectory(${SRCROOT}/src)
