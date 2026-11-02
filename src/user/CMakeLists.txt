set(CMAKE_CXX_FLAGS "")
set(CMAKE_EXE_LINKER_FLAGS "")

# Each entry is <program name>:<source file>. The build places <name>-<arch>.elf in
# build/<arch>/programs, and every program there goes into the initramfs.
set(FEATHEROS_PROGRAMS
    init:init shell:shell calc:calc echo:echo upper:upper date:date fanout:fanout
    memtest:memtest sleep:sleep hardening:hardening aslr:aslr uptime:uptime primes:primes
    filetest:filetest ls:ls cat:cat write:write rm:rm mkdir:mkdir rmdir:rmdir env:env
    fptest:fptest libctest:libctest kill:kill ps:ps top:top keys:keys clear:clear vi:vi threads:threads cp:cp mv:mv touch:touch head:head tail:tail wc:wc
    grep:grep sort:sort ping:ping dhcp:dhcp net:net host:host nc:nc fetch:fetch httpd:httpd
    ntp:ntp echod:echod script:script stty:stty
)
# Programs that define _start themselves instead of linking the C library's, which calls main.
# The kernel's own tests run test_program and read the raw arguments block.
set(FEATHEROS_START_PROGRAMS
    test:test_program
)
# Programs written in Embedded Swift, <program name>:<source file> with a .swift source. They
# link against the C library like the others, and are skipped when no usable swiftc was found.
set(FEATHEROS_SWIFT_PROGRAMS
    hello:hello
)

set(USER_LINKER_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/linker_${ARCH}.ld)

# Helpers the programs share; each program links only the parts it uses.
add_library(featheros_user_support STATIC
    calculator.cpp
    command_line.cpp
    dhcp_lease.cpp
    dhcp_message.cpp
    file_copy.cpp
    http_message.cpp
    init_config.cpp
    key_decoder.cpp
    line_editor.cpp
    ntp_message.cpp
    prime_count.cpp
    process_table.cpp
    raw_terminal.cpp
    tcp_client.cpp
    text_tools.cpp
    vi_editor.cpp
    window_size.cpp
)
target_compile_options(featheros_user_support PRIVATE ${FEATHEROS_USER_FLAGS})
target_link_libraries(featheros_user_support PRIVATE featheros_c)

function(featheros_link_program name source)
    set(target user_${name})
    add_executable(${target} ${source})
    target_compile_options(${target} PRIVATE ${FEATHEROS_USER_FLAGS})
    target_link_options(${target} PRIVATE ${FEATHEROS_USER_FLAGS}
        -Wl,-pie -Wl,--no-dynamic-linker -Wl,-z,text -Wl,-T,${USER_LINKER_SCRIPT}
        -Wl,-e,_start -Wl,--build-id=none -Wl,-z,max-page-size=0x1000)
    target_link_libraries(${target} PRIVATE ${ARGN} featheros_user_support featheros_c)
    set_target_properties(${target} PROPERTIES
        OUTPUT_NAME ${name}-${ARCH}
        SUFFIX .elf
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/programs
        LINK_DEPENDS ${USER_LINKER_SCRIPT}
        LINKER_LANGUAGE CXX
    )
endfunction()

function(featheros_add_program entry)
    string(REPLACE ":" ";" parts ${entry})
    list(GET parts 0 name)
    list(GET parts 1 source)
    featheros_link_program(${name} ${source}.cpp ${ARGN})
endfunction()

# Each Swift program is one module compiled whole (-wmo, which Embedded Swift requires) into one
# object, with the parts of the standard library it uses.
function(featheros_add_swift_program entry)
    string(REPLACE ":" ";" parts ${entry})
    list(GET parts 0 name)
    list(GET parts 1 source)
    set(object ${CMAKE_CURRENT_BINARY_DIR}/swift/${name}.o)
    add_custom_command(
        OUTPUT ${object}
        COMMAND ${FEATHEROS_SWIFTC} ${FEATHEROS_SWIFT_FLAGS} -module-name ${name}
            -c ${CMAKE_CURRENT_SOURCE_DIR}/${source}.swift -o ${object}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${source}.swift
        COMMENT "Building Swift object swift/${name}.o"
        VERBATIM
    )
    set_source_files_properties(${object} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
    featheros_link_program(${name} ${object} featheros_crt0)
endfunction()

foreach (entry ${FEATHEROS_PROGRAMS})
    featheros_add_program(${entry} featheros_crt0)
endforeach ()
foreach (entry ${FEATHEROS_START_PROGRAMS})
    featheros_add_program(${entry})
endforeach ()

set(FEATHEROS_SWIFTC "" CACHE FILEPATH "swiftc from a swift.org toolchain, for Swift programs")
if (ARCH STREQUAL "x86_64")
    set(FEATHEROS_SWIFT_TARGET x86_64-unknown-none-elf)
else ()
    set(FEATHEROS_SWIFT_TARGET aarch64-none-none-elf)
endif ()
# The kernel leaves a red zone below the user stack pointer for signal frames, so Swift code may
# use it, unlike the C++ programs built with -mno-red-zone.
set(FEATHEROS_SWIFT_FLAGS
    -target ${FEATHEROS_SWIFT_TARGET} -enable-experimental-feature Embedded -wmo -Osize
    -Xfrontend -disable-stack-protector
)
set(swift_usable FALSE)
if (FEATHEROS_SWIFTC)
    set(probe ${CMAKE_CURRENT_BINARY_DIR}/swift/probe.swift)
    file(WRITE ${probe} "print(\"probe\")\n")
    execute_process(
        COMMAND ${FEATHEROS_SWIFTC} ${FEATHEROS_SWIFT_FLAGS} -module-name probe -c ${probe}
            -o ${CMAKE_CURRENT_BINARY_DIR}/swift/probe.o
        RESULT_VARIABLE probe_result
        OUTPUT_QUIET ERROR_VARIABLE probe_error
    )
    if (probe_result EQUAL 0)
        set(swift_usable TRUE)
    else ()
        message(WARNING "${FEATHEROS_SWIFTC} cannot build Embedded Swift for "
            "${FEATHEROS_SWIFT_TARGET} (${probe_result}), so the Swift programs are skipped\n"
            "${probe_error}")
    endif ()
else ()
    message(STATUS "No Swift toolchain found, so the Swift programs are skipped")
endif ()
if (swift_usable)
    foreach (entry ${FEATHEROS_SWIFT_PROGRAMS})
        featheros_add_swift_program(${entry})
    endforeach ()
endif ()
