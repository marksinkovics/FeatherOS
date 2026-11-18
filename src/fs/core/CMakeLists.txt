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
    common/bitmap.cpp
    common/block_cache.cpp
    common/bytes.cpp
    common/crc.cpp
    common/host_buffer.cpp
    ext/allocation.cpp
    ext/checksum.cpp
    ext/directory.cpp
    ext/extent.cpp
    ext/extent_write.cpp
    ext/ext2.cpp
    ext/features.cpp
    ext/inode.cpp
    ext/journal.cpp
    ext/namespace.cpp
    ext/recovery.cpp
    ext/superblock.cpp
    ext/transaction.cpp
    ext/write.cpp
    fat/fat16.cpp
    fat/names.cpp
    filesystem.cpp
    partition/partition.cpp
    partition/volume.cpp
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

# mtools, a FAT implementation independent of the core, makes images for the tests to read and
# then reads back what the core wrote. Without it those tests are skipped.
find_program(FEATHERDISK_SH sh)
find_program(FEATHERDISK_MFORMAT mformat)
find_program(FEATHERDISK_MMD mmd)
find_program(FEATHERDISK_MCOPY mcopy)
find_program(FEATHERDISK_MTYPE mtype)
find_program(FEATHERDISK_MDIR mdir)
set(featherdisk_fat16_image)
set(featherdisk_mbr_image)
if (FEATHERDISK_SH AND FEATHERDISK_MFORMAT AND FEATHERDISK_MMD AND FEATHERDISK_MCOPY
    AND FEATHERDISK_MTYPE AND FEATHERDISK_MDIR)
    set(featherdisk_fat16_image "${CMAKE_CURRENT_BINARY_DIR}/fat16-mtools.img")
    set(featherdisk_mbr_image "${CMAKE_CURRENT_BINARY_DIR}/fat16-mbr-mtools.img")
    add_custom_command(
        OUTPUT "${featherdisk_fat16_image}"
        COMMAND "${FEATHERDISK_SH}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/make_fat16_image.sh"
            "${featherdisk_fat16_image}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/make_fat16_image.sh"
        COMMENT "Making a FAT16 image with mtools"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${featherdisk_mbr_image}"
        COMMAND "${FEATHERDISK_SH}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/make_fat16_image.sh"
            "${featherdisk_mbr_image}" mbr
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/make_fat16_image.sh"
        COMMENT "Making an MBR disk with a FAT16 partition with mtools"
        VERBATIM
    )
    add_custom_target(featherdisk_test_images ALL
        DEPENDS "${featherdisk_fat16_image}" "${featherdisk_mbr_image}")
else ()
    message(STATUS "mtools not found, so the FAT16 image tests are skipped")
endif ()

# mke2fs -d from e2fsprogs makes the ext2 images, debugfs fixes their owners and times and reads
# back what the core wrote, and e2fsck -fn checks every volume the core changed. Homebrew
# installs e2fsprogs keg-only, outside PATH, so its directories come first.
find_program(FEATHERDISK_MKE2FS mke2fs
    PATHS /opt/homebrew/opt/e2fsprogs/sbin /usr/local/opt/e2fsprogs/sbin NO_DEFAULT_PATH)
find_program(FEATHERDISK_MKE2FS mke2fs)
if (FEATHERDISK_MKE2FS)
    get_filename_component(featherdisk_e2fsprogs "${FEATHERDISK_MKE2FS}" DIRECTORY)
    find_program(FEATHERDISK_DEBUGFS debugfs PATHS "${featherdisk_e2fsprogs}" NO_DEFAULT_PATH)
    find_program(FEATHERDISK_DEBUGFS debugfs)
    find_program(FEATHERDISK_E2FSCK e2fsck PATHS "${featherdisk_e2fsprogs}" NO_DEFAULT_PATH)
    find_program(FEATHERDISK_E2FSCK e2fsck)
endif ()
if (NOT FEATHERDISK_E2FSCK)
    message(STATUS "e2fsck (e2fsprogs) not found, so the ext2 write tests check no volumes")
endif ()
set(featherdisk_ext_images)
set(featherdisk_ext_image_directory "${CMAKE_CURRENT_BINARY_DIR}/ext-images")
if (FEATHERDISK_SH AND FEATHERDISK_MKE2FS AND FEATHERDISK_DEBUGFS)
    file(MAKE_DIRECTORY "${featherdisk_ext_image_directory}")
    foreach (variant 1k 4k nofiletype rev0 groups mbr ext3 ext4 ext4-extents ext4-blockmap
             ext4-64bit ext4-gdtcsum ext4-csum ext4-csum64 ext4-htree ext4-htree-gdtcsum
             ext4-nojournal ext4-mixed ext4-journal-gdtcsum ext4-4k)
        set(image "${featherdisk_ext_image_directory}/${variant}.img")
        add_custom_command(
            OUTPUT "${image}"
            COMMAND "${FEATHERDISK_SH}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/make_ext2_image.sh"
                "${FEATHERDISK_MKE2FS}" "${FEATHERDISK_DEBUGFS}" "${image}" "${variant}"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/make_ext2_image.sh"
            COMMENT "Making the ${variant} ext2 test image with mke2fs"
            VERBATIM
        )
        list(APPEND featherdisk_ext_images "${image}")
    endforeach ()
    add_custom_target(featherdisk_ext_test_images ALL DEPENDS ${featherdisk_ext_images})
else ()
    message(STATUS "mke2fs or debugfs (e2fsprogs) not found, so the ext2 image tests are skipped")
endif ()

add_subdirectory(tools/fscli)

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
    # Catch2 prints std::string_view only when it is itself built as C++17 or later.
    set(CMAKE_CXX_STANDARD 23)
    FetchContent_MakeAvailable(Catch2)
    list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
    include(Catch)
    set(FEATHERDISK_CATCH2_TARGET Catch2::Catch2WithMain)
endif ()

add_executable(featherdisk_core_tests
    tests/block_cache_tests.cpp
    tests/common_tests.cpp
    tests/ext2_format_tests.cpp
    tests/ext2_image.cpp
    tests/ext2_image_tests.cpp
    tests/ext2_tree_tests.cpp
    tests/ext2_write_tests.cpp
    tests/ext4_extent_tests.cpp
    tests/ext4_extent_write_tests.cpp
    tests/ext4_layout_tests.cpp
    tests/ext4_write_tests.cpp
    tests/ext_journal_tests.cpp
    tests/ext_journal_write_tests.cpp
    tests/ext_transaction_tests.cpp
    tests/fat_image.cpp
    tests/fat_mtools_tests.cpp
    tests/fat_names_tests.cpp
    tests/fat_tests.cpp
    tests/filesystem_helpers.cpp
    tests/memory_device.cpp
    tests/partition_image.cpp
    tests/partition_tests.cpp
)
target_include_directories(featherdisk_core_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(featherdisk_core_tests PRIVATE ${featherdisk_warnings})
find_package(Threads REQUIRED)
target_link_libraries(featherdisk_core_tests PRIVATE
    featherdisk_core featherdisk_standard_host ${FEATHERDISK_CATCH2_TARGET} Threads::Threads)

if (featherdisk_fat16_image)
    add_dependencies(featherdisk_core_tests featherdisk_test_images)
    target_compile_definitions(featherdisk_core_tests PRIVATE
        FEATHERDISK_MTOOLS_IMAGE="${featherdisk_fat16_image}"
        FEATHERDISK_MTYPE="${FEATHERDISK_MTYPE}"
        FEATHERDISK_MDIR="${FEATHERDISK_MDIR}"
        FEATHERDISK_TEST_OUTPUT="${CMAKE_CURRENT_BINARY_DIR}"
    )
endif ()
if (featherdisk_ext_images)
    add_dependencies(featherdisk_core_tests featherdisk_ext_test_images)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/ext-work")
    target_compile_definitions(featherdisk_core_tests PRIVATE
        FEATHERDISK_EXT_IMAGES="${featherdisk_ext_image_directory}"
        FEATHERDISK_EXT_WORK="${CMAKE_CURRENT_BINARY_DIR}/ext-work"
        FEATHERDISK_DEBUGFS="${FEATHERDISK_DEBUGFS}")
    if (FEATHERDISK_E2FSCK)
        target_compile_definitions(featherdisk_core_tests PRIVATE
            FEATHERDISK_E2FSCK="${FEATHERDISK_E2FSCK}")
    endif ()
endif ()
if (COMMAND catch_discover_tests)
    catch_discover_tests(featherdisk_core_tests TEST_PREFIX "fs.core:")
else ()
    add_test(NAME fs.core COMMAND featherdisk_core_tests)
endif ()
