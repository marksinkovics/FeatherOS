# SPDX-License-Identifier: MIT OR Apache-2.0
#
# fscli, the core's command-line host: a host program only, never part of a kernel build.
add_executable(fscli
    disk.cpp
    file_device.cpp
    main.cpp
)
target_compile_features(fscli PRIVATE cxx_std_23)
target_compile_options(fscli PRIVATE ${featherdisk_warnings})
target_link_libraries(fscli PRIVATE featherdisk_core featherdisk_standard_host)

if (NOT BUILD_TESTING OR NOT FEATHERDISK_SH)
    return()
endif ()

# Exit status 77 means skipped: the script returns it when mtools or e2fsprogs made no images, or
# e2fsck is missing for a case that writes ext2.
set(fscli_ext_images)
if (featherdisk_ext_images)
    set(fscli_ext_images "${featherdisk_ext_image_directory}")
endif ()
set(fscli_e2fsck)
if (FEATHERDISK_E2FSCK)
    set(fscli_e2fsck "${FEATHERDISK_E2FSCK}")
endif ()
set(fscli_debugfs)
if (FEATHERDISK_DEBUGFS)
    set(fscli_debugfs "${FEATHERDISK_DEBUGFS}")
endif ()
foreach (case help info ls cat cp cp-in tree partition errors
         ext2-info ext2-ls ext2-cat ext2-partition ext2-errors ext2-cp-in ext2-rm
         ext2-mkdir ext2-mv ext2-ln ext4-read ext4-extents ext-journal)
    add_test(NAME "fs.core:fscli ${case}"
        COMMAND "${FEATHERDISK_SH}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/fscli_tests.sh"
            "$<TARGET_FILE:fscli>" "${featherdisk_fat16_image}" "${featherdisk_mbr_image}"
            "${CMAKE_CURRENT_BINARY_DIR}/test-output/${case}" "${case}" "${fscli_ext_images}"
            "${fscli_e2fsck}" "${fscli_debugfs}")
    set_tests_properties("fs.core:fscli ${case}" PROPERTIES SKIP_RETURN_CODE 77)
endforeach ()
