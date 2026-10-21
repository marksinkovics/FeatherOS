cmake_minimum_required(VERSION 3.16)
project(FeatherOSUnitTests LANGUAGES C CXX)

include(CTest)

get_filename_component(FEATHEROS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
include("${FEATHEROS_ROOT}/deps/cmake/Catch2.cmake")
featheros_fetch_catch2()

list(APPEND CMAKE_MODULE_PATH "${FEATHEROS_CATCH2_SOURCE_DIR}/extras")
include(Catch)

add_executable(featheros_unit_tests
    kernel/filesystem/fat_names_tests.cpp
    kernel/filesystem/filesystem_tests.cpp
    kernel/filesystem/path_tests.cpp
    kernel/elf/elf_loader_tests.cpp
    kernel/framebuffer/framebuffer_tests.cpp
    kernel/input/input_tests.cpp
    kernel/klogger/klogger_tests.cpp
    kernel/memory/memory_map_tests.cpp
    kernel/process/calculator_tests.cpp
    kernel/process/init_config_tests.cpp
    kernel/process/process_arguments_tests.cpp
    kernel/syscall/syscall_tests.cpp
    kernel/time/calendar_tests.cpp
    kernel/string/format_tests.cpp
    libc/environment_table_tests.cpp
    libc/heap_sort_tests.cpp
    libc/integer_parse_tests.cpp
    "${FEATHEROS_ROOT}/src/kernel/filesystem/fat16.cpp"
    "${FEATHEROS_ROOT}/src/kernel/filesystem/fat_names.cpp"
    "${FEATHEROS_ROOT}/src/kernel/filesystem/path.cpp"
    "${FEATHEROS_ROOT}/src/kernel/storage/partition.cpp"
    "${FEATHEROS_ROOT}/src/kernel/filesystem/initramfs.cpp"
    "${FEATHEROS_ROOT}/src/kernel/filesystem/vfs.cpp"
    "${FEATHEROS_ROOT}/src/kernel/elf/elf_loader.cpp"
    "${FEATHEROS_ROOT}/src/kernel/framebuffer/font.c"
    "${FEATHEROS_ROOT}/src/kernel/framebuffer/framebuffer.cpp"
    "${FEATHEROS_ROOT}/src/kernel/framebuffer/term.cpp"
    "${FEATHEROS_ROOT}/src/kernel/input/input.cpp"
    "${FEATHEROS_ROOT}/src/kernel/klogger.cpp"
    "${FEATHEROS_ROOT}/src/kernel/memory/memory_map.cpp"
    "${FEATHEROS_ROOT}/src/kernel/process_arguments.cpp"
    "${FEATHEROS_ROOT}/src/kernel/syscall.cpp"
    "${FEATHEROS_ROOT}/src/kernel/files.cpp"
    "${FEATHEROS_ROOT}/src/kernel/string/format.cpp"
)
target_compile_features(featheros_unit_tests PRIVATE cxx_std_17)
target_compile_definitions(featheros_unit_tests PRIVATE FEATHEROS_HOST_TEST)
target_include_directories(featheros_unit_tests PRIVATE
    "${FEATHEROS_ROOT}/src"
    "${FEATHEROS_ROOT}/deps/limine"
)
target_link_libraries(featheros_unit_tests PRIVATE Catch2::Catch2WithMain)

catch_discover_tests(featheros_unit_tests)