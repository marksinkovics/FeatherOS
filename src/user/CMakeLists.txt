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
    ntp:ntp echod:echod
)
# Programs that define _start themselves instead of linking the C library's, which calls main.
# The kernel's own tests run test_program and read the raw arguments block.
set(FEATHEROS_START_PROGRAMS
    test:test_program
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
)
target_compile_options(featheros_user_support PRIVATE ${FEATHEROS_USER_FLAGS})
target_link_libraries(featheros_user_support PRIVATE featheros_c)

function(featheros_add_program entry)
    string(REPLACE ":" ";" parts ${entry})
    list(GET parts 0 name)
    list(GET parts 1 source)
    set(target user_${name})
    add_executable(${target} ${source}.cpp)
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
    )
endfunction()

foreach (entry ${FEATHEROS_PROGRAMS})
    featheros_add_program(${entry} featheros_crt0)
endforeach ()
foreach (entry ${FEATHEROS_START_PROGRAMS})
    featheros_add_program(${entry})
endforeach ()
