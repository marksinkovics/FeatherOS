cmake_minimum_required(VERSION 3.20)
project(FeatherOSUnitTests LANGUAGES C CXX)

include(CTest)

get_filename_component(FEATHEROS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

option(FEATHEROS_SANITIZERS "Build the unit tests with AddressSanitizer and UBSan" ON)
option(FEATHEROS_THREAD_SANITIZER "Build the unit tests with ThreadSanitizer, without ASan" OFF)
if (FEATHEROS_SANITIZERS AND FEATHEROS_THREAD_SANITIZER)
    message(FATAL_ERROR "FEATHEROS_SANITIZERS and FEATHEROS_THREAD_SANITIZER exclude each other")
endif ()
if (FEATHEROS_THREAD_SANITIZER)
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
endif ()
if (FEATHEROS_SANITIZERS)
    # Applied before Catch2 is added so it is instrumented too; a library built without ASan can
    # report false container overflows in std::string and std::vector shared with the tests.
    add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all
        -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif ()

include("${FEATHEROS_ROOT}/deps/cmake/Catch2.cmake")
featheros_fetch_catch2()

list(APPEND CMAKE_MODULE_PATH "${FEATHEROS_CATCH2_SOURCE_DIR}/extras")
include(Catch)

# The shared filesystem core builds its own library and tests, which leave with it at the split.
set(FEATHERDISK_CATCH2_TARGET Catch2::Catch2WithMain)
add_subdirectory("${FEATHEROS_ROOT}/src/fs/core" "${CMAKE_CURRENT_BINARY_DIR}/fs_core")

add_executable(featheros_unit_tests
    kernel/filesystem/concurrency_tests.cpp
    kernel/filesystem/disk_volumes_tests.cpp
    kernel/filesystem/filesystem_tests.cpp
    kernel/filesystem/mount_table_tests.cpp
    kernel/filesystem/path_tests.cpp
    kernel/filesystem/symbolic_link_tests.cpp
    kernel/filesystem/pipe_tests.cpp
    kernel/filesystem/virtual_files_tests.cpp
    kernel/elf/elf_loader_tests.cpp
    kernel/framebuffer/framebuffer_tests.cpp
    kernel/framebuffer/graphics_mode_tests.cpp
    kernel/input/input_tests.cpp
    kernel/network/address_tests.cpp
    kernel/network/arp_tests.cpp
    kernel/network/checksum_tests.cpp
    kernel/network/ethernet_tests.cpp
    kernel/network/icmp_tests.cpp
    kernel/network/icmpv6_tests.cpp
    kernel/network/ipv4_tests.cpp
    kernel/network/ipv6_tests.cpp
    kernel/network/name_servers_tests.cpp
    kernel/network/neighbor_cache_tests.cpp
    kernel/network/socket_tests.cpp
    kernel/network/tcp_connection_tests.cpp
    kernel/network/tcp_socket_tests.cpp
    kernel/network/tcp_tests.cpp
    kernel/network/udp_tests.cpp
    kernel/pci/msi_tests.cpp
    kernel/pci/pci_device_tree_tests.cpp
    kernel/pci/pci_tests.cpp
    kernel/process/futex_tests.cpp
    kernel/storage/disk_tests.cpp
    kernel/storage/model_disk.cpp
    kernel/storage/model_disk_tests.cpp
    kernel/storage/nvme_command_tests.cpp
    kernel/storage/nvme_identify_tests.cpp
    kernel/storage/page_cache_tests.cpp
    kernel/storage/storage_rig_tests.cpp
    kernel/storage/write_policy_tests.cpp
    kernel/usb/bot_tests.cpp
    kernel/usb/bulk_tests.cpp
    kernel/usb/context_tests.cpp
    kernel/usb/controller_tests.cpp
    kernel/usb/descriptor_tests.cpp
    kernel/usb/device_tests.cpp
    kernel/usb/fake_device.cpp
    kernel/usb/fake_hardware.cpp
    kernel/usb/fake_xhci.cpp
    kernel/usb/hid_driver_tests.cpp
    kernel/usb/hid_keyboard_tests.cpp
    kernel/usb/hid_report_tests.cpp
    kernel/usb/hub_tests.cpp
    kernel/usb/ring_tests.cpp
    kernel/usb/recording_host.cpp
    kernel/usb/scsi_tests.cpp
    kernel/usb/service_tests.cpp
    kernel/usb/storage_driver_tests.cpp
    kernel/sync/volume_mutex_tests.cpp
    kernel/support/physical_memory.cpp
    support/network_stubs.cpp
    support/page_cache_host.cpp
    support/scheduler_stubs.cpp
    support/terminal_device.cpp
    kernel/input/keyboard_tests.cpp
    kernel/input/pointer_tests.cpp
    kernel/boot_options_tests.cpp
    kernel/input/i8042_tests.cpp
    kernel/kernel_test/kernel_test_mode_tests.cpp
    kernel/klogger/klogger_tests.cpp
    kernel/klogger/log_ring_tests.cpp
    kernel/memory/dma_tests.cpp
    kernel/memory/memory_map_tests.cpp
    kernel/process/calculator_tests.cpp
    kernel/process/command_line_tests.cpp
    kernel/process/dhcp_lease_tests.cpp
    kernel/process/dhcp_message_tests.cpp
    kernel/process/http_message_tests.cpp
    kernel/process/line_editor_tests.cpp
    kernel/process/ntp_message_tests.cpp
    kernel/process/text_tools_tests.cpp
    kernel/process/thread_local_storage_tests.cpp
    kernel/process/thread_stacks_tests.cpp
    kernel/process/vi_editor_tests.cpp
    kernel/process/init_config_tests.cpp
    kernel/process/process_arguments_tests.cpp
    kernel/process/process_table_tests.cpp
    kernel/process/signals_tests.cpp
    kernel/syscall/syscall_tests.cpp
    kernel/terminal/pseudo_terminal_tests.cpp
    kernel/terminal/terminal_tests.cpp
    kernel/time/calendar_tests.cpp
    kernel/time/wall_clock_tests.cpp
    kernel/string/format_tests.cpp
    support/font_builder.cpp
    user/font_data_tests.cpp
    user/font_tests.cpp
    user/graphics_tests.cpp
    user/tile_sheet_tests.cpp
    user/utf8_tests.cpp
    user/wickdeep_art_tests.cpp
    libc/address_text_tests.cpp
    libc/dns_message_tests.cpp
    libc/environment_table_tests.cpp
    libc/float_parse_tests.cpp
    libc/heap_allocator_tests.cpp
    libc/heap_sort_tests.cpp
    libc/hosts_file_tests.cpp
    libc/integer_parse_tests.cpp
    libc/math_tests.cpp
    libc/printf_tests.cpp
    libc/regex_tests.cpp
    libc/resolv_conf_tests.cpp
    libc/scanf_tests.cpp
    libc/spawn_request_tests.cpp
    libc/time_format_tests.cpp
    libc/unicode_conversion_tests.cpp
    libc/wide_division_tests.cpp
    libcxx/algorithm_tests.cpp
    libcxx/array_span_tests.cpp
    libcxx/atomic_tests.cpp
    libcxx/bitset_adaptor_tests.cpp
    libcxx/charconv_tests.cpp
    libcxx/chrono_format_tests.cpp
    libcxx/chrono_tests.cpp
    libcxx/cmath_exception_tests.cpp
    libcxx/deque_tests.cpp
    libcxx/expected_tests.cpp
    libcxx/format_range_tests.cpp
    libcxx/format_tests.cpp
    libcxx/functional_tests.cpp
    libcxx/iterator_functional_tests.cpp
    libcxx/limits_bit_tests.cpp
    libcxx/map_set_tests.cpp
    libcxx/memory_tests.cpp
    libcxx/numeric_tests.cpp
    libcxx/optional_tests.cpp
    libcxx/string_tests.cpp
    libcxx/string_view_tests.cpp
    libcxx/thread_tests.cpp
    libcxx/tuple_tests.cpp
    libcxx/type_traits_tests.cpp
    libcxx/unordered_tests.cpp
    libcxx/utility_tests.cpp
    libcxx/variant_tests.cpp
    libcxx/vector_tests.cpp
    libcxx/wait_tests.cpp
    libcxx/host_futex.cpp
    "${FEATHEROS_ROOT}/src/kernel/filesystem/core_driver.cpp"
    "${FEATHEROS_ROOT}/src/kernel/filesystem/path.cpp"
    "${FEATHEROS_ROOT}/src/kernel/filesystem/path_walk.cpp"
    "${FEATHEROS_ROOT}/src/kernel/storage/cache_sizing.cpp"
    "${FEATHEROS_ROOT}/src/kernel/storage/core_block_device.cpp"
    "${FEATHEROS_ROOT}/src/kernel/storage/disk.cpp"
    "${FEATHEROS_ROOT}/src/kernel/storage/nvme/command.cpp"
    "${FEATHEROS_ROOT}/src/kernel/storage/nvme/identify.cpp"
    "${FEATHEROS_ROOT}/src/kernel/storage/page_cache.cpp"
    "${FEATHEROS_ROOT}/src/kernel/storage/worker.cpp"
    "${FEATHEROS_ROOT}/src/kernel/storage/write_policy.cpp"
    "${FEATHEROS_ROOT}/src/kernel/filesystem/initramfs.cpp"
    "${FEATHEROS_ROOT}/src/kernel/filesystem/devices.cpp"
    "${FEATHEROS_ROOT}/src/kernel/filesystem/procfs.cpp"
    "${FEATHEROS_ROOT}/src/kernel/filesystem/tmpfs.cpp"
    "${FEATHEROS_ROOT}/src/kernel/filesystem/drivers.cpp"
    "${FEATHEROS_ROOT}/src/kernel/filesystem/filesystem.cpp"
    "${FEATHEROS_ROOT}/src/kernel/filesystem/mount_table.cpp"
    "${FEATHEROS_ROOT}/src/kernel/filesystem/type_registry.cpp"
    "${FEATHEROS_ROOT}/src/kernel/filesystem/vfs.cpp"
    "${FEATHEROS_ROOT}/src/kernel/elf/elf_loader.cpp"
    "${FEATHEROS_ROOT}/src/kernel/framebuffer/console_font.c"
    "${FEATHEROS_ROOT}/src/kernel/framebuffer/framebuffer.cpp"
    "${FEATHEROS_ROOT}/src/kernel/framebuffer/graphics_mode.cpp"
    "${FEATHEROS_ROOT}/src/kernel/framebuffer/term.cpp"
    "${FEATHEROS_ROOT}/src/kernel/input/input.cpp"
    "${FEATHEROS_ROOT}/src/kernel/input/keyboard.cpp"
    "${FEATHEROS_ROOT}/src/kernel/input/pointer.cpp"
    "${FEATHEROS_ROOT}/src/kernel/boot_options.cpp"
    "${FEATHEROS_ROOT}/src/kernel/kernel_test_mode.cpp"
    "${FEATHEROS_ROOT}/src/kernel/network/address.cpp"
    "${FEATHEROS_ROOT}/src/kernel/network/arp.cpp"
    "${FEATHEROS_ROOT}/src/kernel/network/checksum.cpp"
    "${FEATHEROS_ROOT}/src/kernel/network/ethernet.cpp"
    "${FEATHEROS_ROOT}/src/kernel/network/icmp.cpp"
    "${FEATHEROS_ROOT}/src/kernel/network/icmpv6.cpp"
    "${FEATHEROS_ROOT}/src/kernel/network/ipv4.cpp"
    "${FEATHEROS_ROOT}/src/kernel/network/ipv6.cpp"
    "${FEATHEROS_ROOT}/src/kernel/network/name_servers.cpp"
    "${FEATHEROS_ROOT}/src/kernel/network/neighbor_cache.cpp"
    "${FEATHEROS_ROOT}/src/kernel/network/socket.cpp"
    "${FEATHEROS_ROOT}/src/kernel/network/tcp.cpp"
    "${FEATHEROS_ROOT}/src/kernel/network/tcp_connection.cpp"
    "${FEATHEROS_ROOT}/src/kernel/network/udp.cpp"
    "${FEATHEROS_ROOT}/src/libc/address_text.cpp"
    "${FEATHEROS_ROOT}/src/libc/big_integer.cpp"
    "${FEATHEROS_ROOT}/src/libc/dns_message.cpp"
    "${FEATHEROS_ROOT}/src/libc/environment_table.cpp"
    "${FEATHEROS_ROOT}/src/libc/float_bits.cpp"
    "${FEATHEROS_ROOT}/src/libc/float_format.cpp"
    "${FEATHEROS_ROOT}/src/libc/float_parse.cpp"
    "${FEATHEROS_ROOT}/src/libc/format.cpp"
    "${FEATHEROS_ROOT}/src/libc/heap_allocator.cpp"
    "${FEATHEROS_ROOT}/src/libc/heap_sort.cpp"
    "${FEATHEROS_ROOT}/src/libc/hosts_file.cpp"
    "${FEATHEROS_ROOT}/src/libc/math_functions.cpp"
    "${FEATHEROS_ROOT}/src/libc/regex_engine.cpp"
    "${FEATHEROS_ROOT}/src/libc/resolv_conf.cpp"
    "${FEATHEROS_ROOT}/src/libc/scan.cpp"
    "${FEATHEROS_ROOT}/src/libc/spawn_request.cpp"
    "${FEATHEROS_ROOT}/src/libc/time_format.cpp"
    "${FEATHEROS_ROOT}/src/libc/unicode_conversion.cpp"
    "${FEATHEROS_ROOT}/src/libc/wide_division.cpp"
    "${FEATHEROS_ROOT}/src/libcxx/src/atomic_wait.cpp"
    "${FEATHEROS_ROOT}/src/libcxx/src/charconv.cpp"
    "${FEATHEROS_ROOT}/src/libcxx/src/chrono.cpp"
    "${FEATHEROS_ROOT}/src/libcxx/src/chrono_format.cpp"
    "${FEATHEROS_ROOT}/src/libcxx/src/condition_variable.cpp"
    "${FEATHEROS_ROOT}/src/libcxx/src/exception.cpp"
    "${FEATHEROS_ROOT}/src/libcxx/src/format.cpp"
    "${FEATHEROS_ROOT}/src/libcxx/src/mutex.cpp"
    "${FEATHEROS_ROOT}/src/libcxx/src/print.cpp"
    "${FEATHEROS_ROOT}/src/libcxx/src/stop_token.cpp"
    "${FEATHEROS_ROOT}/src/libcxx/src/string.cpp"
    "${FEATHEROS_ROOT}/src/libcxx/src/thread.cpp"
    "${FEATHEROS_ROOT}/src/libcxx/src/tree.cpp"
    "${FEATHEROS_ROOT}/src/user/calculator.cpp"
    "${FEATHEROS_ROOT}/src/user/calendar.cpp"
    "${FEATHEROS_ROOT}/src/user/command_line.cpp"
    "${FEATHEROS_ROOT}/src/user/dhcp_lease.cpp"
    "${FEATHEROS_ROOT}/src/user/dhcp_message.cpp"
    "${FEATHEROS_ROOT}/src/user/gfx_check.cpp"
    "${FEATHEROS_ROOT}/src/user/graphics/dirty_region.cpp"
    "${FEATHEROS_ROOT}/src/user/graphics/display.cpp"
    "${FEATHEROS_ROOT}/src/user/graphics/font.cpp"
    "${FEATHEROS_ROOT}/src/user/graphics/font_console.cpp"
    "${FEATHEROS_ROOT}/src/user/graphics/surface.cpp"
    "${FEATHEROS_ROOT}/src/user/graphics/tile_sheet.cpp"
    "${FEATHEROS_ROOT}/src/user/http_message.cpp"
    "${FEATHEROS_ROOT}/src/user/init_config.cpp"
    "${FEATHEROS_ROOT}/src/user/key_decoder.cpp"
    "${FEATHEROS_ROOT}/src/user/line_editor.cpp"
    "${FEATHEROS_ROOT}/src/user/ntp_message.cpp"
    "${FEATHEROS_ROOT}/src/user/process_table.cpp"
    "${FEATHEROS_ROOT}/src/user/text/font_data.cpp"
    "${FEATHEROS_ROOT}/src/user/text/utf8.cpp"
    "${FEATHEROS_ROOT}/src/user/text_tools.cpp"
    "${FEATHEROS_ROOT}/src/user/vi_editor.cpp"
    "${FEATHEROS_ROOT}/src/user/wickdeep/art.cpp"
    "${FEATHEROS_ROOT}/src/kernel/arch/aarch64/pci_device_tree.cpp"
    "${FEATHEROS_ROOT}/src/kernel/arch/x86_64/i8042.cpp"
    "${FEATHEROS_ROOT}/src/kernel/klogger.cpp"
    "${FEATHEROS_ROOT}/src/kernel/log_ring.cpp"
    "${FEATHEROS_ROOT}/src/kernel/memory/dma.cpp"
    "${FEATHEROS_ROOT}/src/kernel/memory/memory_map.cpp"
    "${FEATHEROS_ROOT}/src/kernel/memory/page_allocator.cpp"
    "${FEATHEROS_ROOT}/src/kernel/pci/msi.cpp"
    "${FEATHEROS_ROOT}/src/kernel/pci/pci.cpp"
    "${FEATHEROS_ROOT}/src/kernel/usb/descriptors.cpp"
    "${FEATHEROS_ROOT}/src/kernel/usb/device.cpp"
    "${FEATHEROS_ROOT}/src/kernel/usb/driver.cpp"
    "${FEATHEROS_ROOT}/src/kernel/usb/hub.cpp"
    "${FEATHEROS_ROOT}/src/kernel/usb/hub_state.cpp"
    "${FEATHEROS_ROOT}/src/kernel/usb/hid/driver.cpp"
    "${FEATHEROS_ROOT}/src/kernel/usb/hid/keyboard.cpp"
    "${FEATHEROS_ROOT}/src/kernel/usb/hid/report.cpp"
    "${FEATHEROS_ROOT}/src/kernel/usb/manager.cpp"
    "${FEATHEROS_ROOT}/src/kernel/usb/storage/bot.cpp"
    "${FEATHEROS_ROOT}/src/kernel/usb/storage/drive.cpp"
    "${FEATHEROS_ROOT}/src/kernel/usb/storage/driver.cpp"
    "${FEATHEROS_ROOT}/src/kernel/usb/storage/scsi.cpp"
    "${FEATHEROS_ROOT}/src/kernel/usb/text.cpp"
    "${FEATHEROS_ROOT}/src/kernel/usb/xhci/context.cpp"
    "${FEATHEROS_ROOT}/src/kernel/usb/xhci/controller.cpp"
    "${FEATHEROS_ROOT}/src/kernel/usb/xhci/registers.cpp"
    "${FEATHEROS_ROOT}/src/kernel/usb/xhci/ring.cpp"
    "${FEATHEROS_ROOT}/src/kernel/usb/xhci/td.cpp"
    "${FEATHEROS_ROOT}/src/kernel/usb/xhci/transfer.cpp"
    "${FEATHEROS_ROOT}/src/kernel/process_arguments.cpp"
    "${FEATHEROS_ROOT}/src/kernel/sync/volume_mutex.cpp"
    "${FEATHEROS_ROOT}/src/kernel/syscall.cpp"
    "${FEATHEROS_ROOT}/src/kernel/time/wall_clock.cpp"
    "${FEATHEROS_ROOT}/src/kernel/thread_local_storage.cpp"
    "${FEATHEROS_ROOT}/src/kernel/thread_stacks.cpp"
    "${FEATHEROS_ROOT}/src/kernel/files.cpp"
    "${FEATHEROS_ROOT}/src/kernel/futex.cpp"
    "${FEATHEROS_ROOT}/src/kernel/pipe.cpp"
    "${FEATHEROS_ROOT}/src/kernel/poll.cpp"
    "${FEATHEROS_ROOT}/src/kernel/signals.cpp"
    "${FEATHEROS_ROOT}/src/kernel/terminal/pseudo_terminal.cpp"
    "${FEATHEROS_ROOT}/src/kernel/terminal/terminal.cpp"
    "${FEATHEROS_ROOT}/src/kernel/string/format.cpp"
)
target_compile_features(featheros_unit_tests PRIVATE cxx_std_23)
# FeatherOS's C++ library is tested in a namespace of its own, beside the host's std, and with
# the warnings its usual place as a system include directory hides.
target_compile_definitions(featheros_unit_tests PRIVATE FEATHEROS_HOST_TEST
    FEATHEROS_STD_NAMESPACE=featheros_std
    FEATHEROS_FONT_DIRECTORY="${FEATHEROS_ROOT}/initramfs/fonts"
    FEATHEROS_FONT_REFERENCE="${CMAKE_CURRENT_SOURCE_DIR}/user/data/font_reference.txt")
set_source_files_properties(libcxx/algorithm_tests.cpp libcxx/array_span_tests.cpp
    libcxx/atomic_tests.cpp libcxx/charconv_tests.cpp libcxx/chrono_tests.cpp libcxx/deque_tests.cpp
    libcxx/map_set_tests.cpp libcxx/string_tests.cpp libcxx/unordered_tests.cpp
    "${FEATHEROS_ROOT}/src/libcxx/src/charconv.cpp" "${FEATHEROS_ROOT}/src/libcxx/src/chrono.cpp"
    "${FEATHEROS_ROOT}/src/libcxx/src/string.cpp" "${FEATHEROS_ROOT}/src/libcxx/src/tree.cpp"
    "${FEATHEROS_ROOT}/src/libcxx/src/condition_variable.cpp"
    "${FEATHEROS_ROOT}/src/libcxx/src/mutex.cpp" "${FEATHEROS_ROOT}/src/libcxx/src/stop_token.cpp"
    "${FEATHEROS_ROOT}/src/libcxx/src/thread.cpp" libcxx/thread_tests.cpp
    "${FEATHEROS_ROOT}/src/libcxx/src/atomic_wait.cpp" libcxx/wait_tests.cpp
    "${FEATHEROS_ROOT}/src/libcxx/src/format.cpp" "${FEATHEROS_ROOT}/src/libcxx/src/print.cpp"
    "${FEATHEROS_ROOT}/src/libcxx/src/chrono_format.cpp" libcxx/chrono_format_tests.cpp
    libcxx/format_range_tests.cpp libcxx/format_tests.cpp libcxx/bitset_adaptor_tests.cpp
    libcxx/cmath_exception_tests.cpp "${FEATHEROS_ROOT}/src/libcxx/src/exception.cpp"
    libcxx/expected_tests.cpp libcxx/functional_tests.cpp libcxx/iterator_functional_tests.cpp
    libcxx/limits_bit_tests.cpp libcxx/memory_tests.cpp libcxx/numeric_tests.cpp
    libcxx/optional_tests.cpp libcxx/string_view_tests.cpp
    libcxx/tuple_tests.cpp libcxx/type_traits_tests.cpp libcxx/utility_tests.cpp
    libcxx/variant_tests.cpp libcxx/vector_tests.cpp
    PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra;-Wshadow;-Werror")
target_include_directories(featheros_unit_tests PRIVATE
    "${FEATHEROS_ROOT}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${FEATHEROS_ROOT}/deps/limine"
)
target_link_libraries(featheros_unit_tests PRIVATE Catch2::Catch2WithMain featherdisk_core
    featherdisk_standard_host)
# The VFS reads the core's 1 KiB ext2, ext3 and ext4 images as partitions, when e2fsprogs made
# them.
if (TARGET featherdisk_ext_test_images)
    add_dependencies(featheros_unit_tests featherdisk_ext_test_images)
    target_compile_definitions(featheros_unit_tests PRIVATE
        FEATHEROS_EXT2_TEST_IMAGE="${CMAKE_CURRENT_BINARY_DIR}/fs_core/ext-images/1k.img"
        FEATHEROS_EXT3_TEST_IMAGE="${CMAKE_CURRENT_BINARY_DIR}/fs_core/ext-images/ext3.img"
        FEATHEROS_EXT4_TEST_IMAGE="${CMAKE_CURRENT_BINARY_DIR}/fs_core/ext-images/ext4.img")
endif ()

if (FEATHERDISK_E2FSCK)
    target_compile_definitions(featheros_unit_tests PRIVATE FEATHEROS_E2FSCK="${FEATHERDISK_E2FSCK}")
endif ()

catch_discover_tests(featheros_unit_tests)

# The committed fonts: their checksums and files, and byte-identical regeneration, which needs
# freetype-py (tools/fonts/requirements.txt) and is skipped without it.
add_test(NAME fonts.files COMMAND python3 "${FEATHEROS_ROOT}/tools/fonts/check.py")
add_test(NAME fonts.reproducible
    COMMAND python3 "${FEATHEROS_ROOT}/tools/fonts/generate.py" --check)
set_tests_properties(fonts.reproducible PROPERTIES SKIP_RETURN_CODE 77)
# The Swift programs' host-testable parts, as one host executable per program built with the
# toolchain's own standard library rather than Embedded Swift. Skipped without a Swift toolchain.
set(FEATHEROS_SWIFTC "" CACHE FILEPATH "swiftc from a swift.org toolchain, for the Swift tests")
if (FEATHEROS_SWIFTC)
    set(sdk_flags)
    if (APPLE)
        execute_process(COMMAND xcrun --show-sdk-path OUTPUT_VARIABLE sdk
            OUTPUT_STRIP_TRAILING_WHITESPACE)
        set(sdk_flags -sdk ${sdk})
    endif ()
    set(swift_flags -O -parse-as-library ${sdk_flags})
    # Each program directory's sources but main.swift, with tests/unit/swift/<name>_tests.swift.
    file(GLOB FEATHEROS_SWIFT_TEST_SOURCES_json CONFIGURE_DEPENDS
        "${FEATHEROS_ROOT}/src/user/swift_support/json_*.swift")
    list(FILTER FEATHEROS_SWIFT_TEST_SOURCES_json EXCLUDE REGEX "json_(en|de)coder\\.swift$")
    foreach (program json life)
        file(GLOB sources CONFIGURE_DEPENDS "${FEATHEROS_ROOT}/src/user/${program}/*.swift")
        list(REMOVE_ITEM sources "${FEATHEROS_ROOT}/src/user/${program}/main.swift")
        list(APPEND sources ${FEATHEROS_SWIFT_TEST_SOURCES_${program}}
            "${CMAKE_CURRENT_SOURCE_DIR}/swift/${program}_tests.swift")
        set(tests ${CMAKE_CURRENT_BINARY_DIR}/swift/${program}_tests)
        add_custom_command(
            OUTPUT ${tests}
            COMMAND ${FEATHEROS_SWIFTC} ${swift_flags} -module-name ${program}_tests ${sources}
                -module-cache-path ${CMAKE_CURRENT_BINARY_DIR}/swift/module-cache -o ${tests}
            DEPENDS ${sources}
            COMMENT "Building Swift test swift/${program}_tests"
            VERBATIM
        )
        add_custom_target(swift_${program}_tests ALL DEPENDS ${tests})
        add_test(NAME swift.${program} COMMAND ${tests})
    endforeach ()

    # FeatherOS's Codable and FeatherSupport's JSON coders. The toolchain's standard library has
    # upstream's Codable, so these are built with Embedded Swift for the host against the module
    # FeatherOS's programs use (src/user/swift_stdlib/), at -Onone and -O as in Debug and Release.
    set(FEATHEROS_SWIFT_SOURCE ${FEATHEROS_ROOT}/deps/swift)
    execute_process(COMMAND ${FEATHEROS_SWIFTC} -print-target-info OUTPUT_VARIABLE target_info)
    string(JSON host_triple GET ${target_info} target triple)
    string(JSON host_module_triple GET ${target_info} target moduleTriple)
    get_filename_component(swift_bin ${FEATHEROS_SWIFTC} DIRECTORY)
    set(unicode_tables
        ${swift_bin}/../lib/swift/embedded/${host_module_triple}/libswiftUnicodeDataTables.a)
    if (NOT EXISTS ${FEATHEROS_SWIFT_SOURCE}/stdlib/public/core)
        message(STATUS "No Swift sources in deps/swift, so the Codable tests are skipped")
    elseif (NOT EXISTS ${unicode_tables})
        message(STATUS "No Embedded Swift libraries for ${host_module_triple}, "
            "so the Codable tests are skipped")
    else ()
        include(${FEATHEROS_ROOT}/src/user/swift_stdlib/swift_stdlib.cmake)
        featheros_add_swift_stdlib(${host_triple} ${host_module_triple}
            ${CMAKE_CURRENT_BINARY_DIR}/swift/stdlib swift_host_stdlib)
        file(GLOB sources CONFIGURE_DEPENDS "${FEATHEROS_ROOT}/src/user/swift_support/json_*.swift")
        list(APPEND sources "${CMAKE_CURRENT_SOURCE_DIR}/swift/codable_tests.swift")
        foreach (optimization Onone O)
            set(tests ${CMAKE_CURRENT_BINARY_DIR}/swift/codable_tests_${optimization})
            add_custom_command(
                OUTPUT ${tests}
                COMMAND ${FEATHEROS_SWIFTC} -target ${host_triple} ${sdk_flags}
                    -enable-experimental-feature Embedded -wmo -${optimization} -parse-as-library
                    -I ${FEATHEROS_SWIFT_STDLIB_MODULE_DIRECTORY} -module-name codable_tests
                    ${sources} -module-cache-path ${CMAKE_CURRENT_BINARY_DIR}/swift/module-cache
                    -o ${tests} -Xlinker ${unicode_tables}
                DEPENDS ${sources} ${FEATHEROS_SWIFT_STDLIB_MODULE}
                COMMENT "Building Swift test swift/codable_tests_${optimization}"
                VERBATIM
            )
            add_custom_target(swift_codable_tests_${optimization} ALL DEPENDS ${tests})
            add_test(NAME swift.codable.${optimization} COMMAND ${tests})
        endforeach ()
    endif ()
    # The same tests in full Swift, as FeatherOS's full-Swift programs use the JSON coders:
    # through the host library's Codable and FeatherSupport's codable_containers.swift.
    file(GLOB sources CONFIGURE_DEPENDS "${FEATHEROS_ROOT}/src/user/swift_support/json_*.swift")
    list(APPEND sources "${FEATHEROS_ROOT}/src/user/swift_support/codable_containers.swift"
        "${CMAKE_CURRENT_SOURCE_DIR}/swift/codable_tests.swift")
    set(tests ${CMAKE_CURRENT_BINARY_DIR}/swift/codable_tests_full)
    add_custom_command(
        OUTPUT ${tests}
        COMMAND ${FEATHEROS_SWIFTC} ${swift_flags} -module-name codable_tests ${sources}
            -module-cache-path ${CMAKE_CURRENT_BINARY_DIR}/swift/module-cache -o ${tests}
        DEPENDS ${sources}
        COMMENT "Building Swift test swift/codable_tests_full"
        VERBATIM
    )
    add_custom_target(swift_codable_tests_full ALL DEPENDS ${tests})
    add_test(NAME swift.codable.full COMMAND ${tests})
else ()
    message(STATUS "No Swift toolchain found, so the Swift tests are skipped")
endif ()
