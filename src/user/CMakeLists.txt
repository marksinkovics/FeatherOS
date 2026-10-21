set(CMAKE_CXX_FLAGS "")
set(CMAKE_EXE_LINKER_FLAGS "")

# Each entry is <program name>:<source file>. The build places <name>-<arch>.elf in
# build/<arch>/programs, and every program there goes into the initramfs.
set(FEATHEROS_PROGRAMS
    test:test_program shell:shell calc:calc echo:echo upper:upper date:date fanout:fanout
    memtest:memtest sleep:sleep hardening:hardening aslr:aslr uptime:uptime primes:primes
    filetest:filetest ls:ls cat:cat write:write rm:rm mkdir:mkdir rmdir:rmdir init:init
)
# Programs that start at main() rather than defining _start themselves.
set(FEATHEROS_MAIN_PROGRAMS
    env:env fptest:fptest libctest:libctest
)

set(USER_LINKER_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/linker_${ARCH}.ld)

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
    target_link_libraries(${target} PRIVATE ${ARGN} featheros_c)
    set_target_properties(${target} PROPERTIES
        OUTPUT_NAME ${name}-${ARCH}
        SUFFIX .elf
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/programs
        CXX_STANDARD 17
        LINK_DEPENDS ${USER_LINKER_SCRIPT}
    )
endfunction()

foreach (entry ${FEATHEROS_PROGRAMS})
    featheros_add_program(${entry})
endforeach ()
foreach (entry ${FEATHEROS_MAIN_PROGRAMS})
    featheros_add_program(${entry} featheros_crt0)
endforeach ()
