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
    ntp:ntp echod:echod script:script stty:stty cppbench:cppbench
)
# Programs that define _start themselves instead of linking the C library's, which calls main.
# The kernel's own tests run test_program and read the raw arguments block.
set(FEATHEROS_START_PROGRAMS
    test:test_program
)
# Programs written in Embedded Swift, <program name>:<source file> with a .swift source. They
# link against the C library like the others, and are skipped when no usable swiftc was found.
set(FEATHEROS_SWIFT_PROGRAMS
    hello:hello rev:rev json:json life:life swiftbench:swiftbench regex:regex
)
# Swift programs that use concurrency, built when deps/swift holds the runtime's sources (see
# swift_concurrency/). Their top-level code runs on MainActor.
set(FEATHEROS_SWIFT_CONCURRENCY_PROGRAMS
    tasks:tasks taskprimes:taskprimes executors:executors
)
# Swift programs that import C++ headers, the modules in swift_cxx/module.modulemap, built when
# deps/ holds the Cxx module's and libc++'s sources (see swift_cxx/). GCC builds the C++ they call
# like other user code; a program's own C++ sources are listed by program name.
set(FEATHEROS_SWIFT_CXX_PROGRAMS
    http:http cxxinterop:cxxinterop
)
set(FEATHEROS_SWIFT_CXX_SOURCES_cxxinterop cxx_interop.cpp)
# Programs in full Swift, with runtime type information (see swift_runtime/), built when deps/
# holds the runtime's sources. They import a FeatherSupport built in full mode, and cannot import
# the Embedded modules.
set(FEATHEROS_SWIFT_FULL_PROGRAMS
    fullhello:fullhello reflect:reflect fulltasks:tasks fullexecutors:executors
    fullregex:fullregex fullcodable:fullcodable fulljson:json fulllife:life
    fullswiftbench:swiftbench
)
# Full-Swift programs that import C++ headers, as FEATHEROS_SWIFT_CXX_PROGRAMS do.
set(FEATHEROS_SWIFT_FULL_CXX_PROGRAMS
    fullcxxinterop:cxxinterop fullhttp:http
)
set(FEATHEROS_SWIFT_CXX_SOURCES_fullcxxinterop cxx_interop.cpp)
# Sources a Swift program compiles in from other programs' directories, by program name.
set(FEATHEROS_SWIFT_SHARED_SOURCES_swiftbench life/grid.swift)
set(FEATHEROS_SWIFT_SHARED_SOURCES_fullswiftbench life/grid.swift)

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

# The Swift programs' shared code, the FeatherSupport module. Embedded Swift serializes every
# function body into the .swiftmodule, so programs compile in the parts they use; the archive is
# only a fallback for symbols they do not.
set(FEATHEROS_SWIFT_SUPPORT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/swift/support)
function(featheros_add_swift_support)
    file(GLOB sources CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/swift_support/*.swift)
    set(object ${FEATHEROS_SWIFT_SUPPORT_DIRECTORY}/FeatherSupport.o)
    set(module ${FEATHEROS_SWIFT_SUPPORT_DIRECTORY}/FeatherSupport.swiftmodule)
    add_custom_command(
        OUTPUT ${object} ${module}
        COMMAND ${FEATHEROS_SWIFTC} ${FEATHEROS_SWIFT_FLAGS} -parse-as-library
            -module-name FeatherSupport -emit-module -emit-module-path ${module}
            -c ${sources} -o ${object}
        DEPENDS ${sources} ${FEATHEROS_SWIFT_LIBC_FILES}
        COMMENT "Building Swift module FeatherSupport"
        VERBATIM
    )
    set_source_files_properties(${object} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
    add_library(featheros_swift_support STATIC ${object})
    set_target_properties(featheros_swift_support PROPERTIES LINKER_LANGUAGE CXX)
    set(FEATHEROS_SWIFT_SUPPORT_MODULE ${module} PARENT_SCOPE)
endfunction()

# Each Swift program is one module compiled whole (-wmo, which Embedded Swift requires) into one
# object, with the parts of the standard library and FeatherSupport it uses.
function(featheros_add_swift_program entry)
    cmake_parse_arguments(PARSE_ARGV 1 program "CONCURRENCY;CXX" "" "")
    string(REPLACE ":" ";" parts ${entry})
    list(GET parts 0 name)
    list(GET parts 1 source)
    set(source ${CMAKE_CURRENT_SOURCE_DIR}/${source})
    # A program of several files is a directory, with its top-level code in main.swift.
    if (IS_DIRECTORY ${source})
        file(GLOB sources CONFIGURE_DEPENDS ${source}/*.swift)
    else ()
        set(sources ${source}.swift)
    endif ()
    list(TRANSFORM FEATHEROS_SWIFT_SHARED_SOURCES_${name} PREPEND ${CMAKE_CURRENT_SOURCE_DIR}/
        OUTPUT_VARIABLE shared_sources)
    list(APPEND sources ${shared_sources})
    set(object ${CMAKE_CURRENT_BINARY_DIR}/swift/${name}.o)
    if (program_CONCURRENCY)
        set(mode -I ${FEATHEROS_SWIFT_CONCURRENCY_MODULE_DIRECTORY})
        set(module_dependency ${FEATHEROS_SWIFT_CONCURRENCY_MODULE})
        set(libraries featheros_swift_concurrency)
    elseif (program_CXX)
        # libc++'s headers must come before the C library's, which FEATHEROS_SWIFT_FLAGS adds.
        set(cxx_headers -Xcc -nostdinc++ -Xcc -I${FEATHEROS_LIBCXX_CONFIG_DIRECTORY}
            -Xcc -I${FEATHEROS_LIBCXX_SOURCE}/include)
        set(mode -cxx-interoperability-mode=default -Xcc -std=gnu++23 -Xcc -fno-exceptions
            -Xcc -fno-rtti -I ${FEATHEROS_SWIFT_CXX_MODULE_DIRECTORY}
            -I ${CMAKE_CURRENT_SOURCE_DIR}/swift_cxx)
        set(module_dependency ${FEATHEROS_SWIFT_CXX_MODULE} ${FEATHEROS_SWIFT_CXX_HEADERS})
    endif ()
    add_custom_command(
        OUTPUT ${object}
        COMMAND ${FEATHEROS_SWIFTC} ${cxx_headers} ${FEATHEROS_SWIFT_FLAGS}
            ${FEATHEROS_SWIFT_PROGRAM_DEBUG} ${mode} -I ${FEATHEROS_SWIFT_SUPPORT_DIRECTORY}
            -module-name ${name} -c ${sources} -o ${object}
        DEPENDS ${sources} ${FEATHEROS_SWIFT_LIBC_FILES} ${FEATHEROS_SWIFT_SUPPORT_MODULE}
            ${module_dependency}
        COMMENT "Building Swift object swift/${name}.o"
        VERBATIM
    )
    set_source_files_properties(${object} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
    featheros_link_program(${name} ${object} featheros_crt0 featheros_swift_support
        ${FEATHEROS_SWIFT_UNICODE_TABLES} ${libraries})
    target_sources(user_${name} PRIVATE ${FEATHEROS_SWIFT_CXX_SOURCES_${name}})
endfunction()

# FeatherSupport again for full-Swift programs, from the same sources: codable_containers.swift
# gives its JSON coders the EncoderBase and DecoderBase that Embedded Swift's library has. Unlike
# Embedded Swift, full Swift links the module's code from its object rather than compiling it into
# each program.
set(FEATHEROS_SWIFT_FULL_SUPPORT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/swift_full/support)
function(featheros_add_full_swift_support)
    file(GLOB sources CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/swift_support/*.swift)
    set(object ${FEATHEROS_SWIFT_FULL_SUPPORT_DIRECTORY}/FeatherSupport.o)
    set(module ${FEATHEROS_SWIFT_FULL_SUPPORT_DIRECTORY}/FeatherSupport.swiftmodule)
    add_custom_command(
        OUTPUT ${object} ${module}
        COMMAND ${FEATHEROS_SWIFTC} ${FEATHEROS_SWIFT_FULL_FLAGS} ${FEATHEROS_SWIFT_OPTIMIZATION}
            -parse-as-library -module-name FeatherSupport -emit-module -emit-module-path ${module}
            -c ${sources} -o ${object}
        DEPENDS ${sources} ${FEATHEROS_SWIFT_LIBC_FILES} ${FEATHEROS_SWIFT_FULL_MODULES}
        COMMENT "Building full Swift module FeatherSupport"
        VERBATIM
    )
    set_source_files_properties(${object} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
    add_library(featheros_swift_full_support STATIC ${object})
    set_target_properties(featheros_swift_full_support PROPERTIES LINKER_LANGUAGE CXX)
    add_dependencies(featheros_swift_full_support featheros_swift_full)
    set(FEATHEROS_SWIFT_FULL_SUPPORT_MODULE ${module} PARENT_SCOPE)
endfunction()

# A full-Swift program links the standard library's object, the runtime and the C library with
# the toolchain's ld.lld, which also does the link-time optimization of Release builds. GCC
# cannot drive it here: the cross compilers ignore -fuse-ld=lld.
function(featheros_add_full_swift_program entry)
    cmake_parse_arguments(PARSE_ARGV 1 program "CXX" "" "")
    string(REPLACE ":" ";" parts ${entry})
    list(GET parts 0 name)
    list(GET parts 1 source)
    set(source ${CMAKE_CURRENT_SOURCE_DIR}/${source})
    if (IS_DIRECTORY ${source})
        file(GLOB sources CONFIGURE_DEPENDS ${source}/*.swift)
    else ()
        set(sources ${source}.swift)
    endif ()
    list(TRANSFORM FEATHEROS_SWIFT_SHARED_SOURCES_${name} PREPEND ${CMAKE_CURRENT_SOURCE_DIR}/
        OUTPUT_VARIABLE shared_sources)
    list(APPEND sources ${shared_sources})
    set(object ${CMAKE_CURRENT_BINARY_DIR}/swift_full/${name}.o)
    set(cxx_headers)
    set(mode)
    set(module_dependency)
    set(cxx_objects)
    set(cxx_libraries)
    if (program_CXX)
        set(cxx_headers -Xcc -nostdinc++ -Xcc -I${FEATHEROS_LIBCXX_CONFIG_DIRECTORY}
            -Xcc -I${FEATHEROS_LIBCXX_SOURCE}/include)
        # The link names Cxx's archive. Left in, the compiler's request for swiftCxx becomes an
        # ELF dependent library in Release's bitcode, which ld.lld fails to find.
        set(mode -cxx-interoperability-mode=default -Xcc -std=gnu++23 -Xcc -fno-exceptions
            -Xcc -fno-rtti -I ${CMAKE_CURRENT_SOURCE_DIR}/swift_cxx
            -Xfrontend -disable-autolink-library -Xfrontend swiftCxx)
        set(module_dependency ${FEATHEROS_SWIFT_CXX_HEADERS})
        set(cxx_libraries $<TARGET_FILE:featheros_swift_full_cxx>
            $<TARGET_FILE:featheros_user_support>)
        if (FEATHEROS_SWIFT_CXX_SOURCES_${name})
            add_library(user_${name}_cxx OBJECT ${FEATHEROS_SWIFT_CXX_SOURCES_${name}})
            target_compile_options(user_${name}_cxx PRIVATE ${FEATHEROS_USER_FLAGS})
            target_link_libraries(user_${name}_cxx PRIVATE featheros_c)
            set(cxx_objects $<TARGET_OBJECTS:user_${name}_cxx>)
        endif ()
    endif ()
    add_custom_command(
        OUTPUT ${object}
        COMMAND ${FEATHEROS_SWIFTC} ${cxx_headers} ${FEATHEROS_SWIFT_FULL_FLAGS}
            ${FEATHEROS_SWIFT_OPTIMIZATION} ${FEATHEROS_SWIFT_PROGRAM_DEBUG} ${mode}
            -I ${FEATHEROS_SWIFT_FULL_SUPPORT_DIRECTORY}
            -enable-bare-slash-regex -module-name ${name} -c ${sources} -o ${object}
        DEPENDS ${sources} ${FEATHEROS_SWIFT_LIBC_FILES} ${FEATHEROS_SWIFT_FULL_MODULES}
            ${FEATHEROS_SWIFT_FULL_SUPPORT_MODULE} ${module_dependency}
        COMMENT "Building full Swift object swift_full/${name}.o"
        VERBATIM
    )
    set(program ${CMAKE_BINARY_DIR}/programs/${name}-${ARCH}.elf)
    add_custom_command(
        OUTPUT ${program}
        COMMAND ${FEATHEROS_SWIFT_LLD} -pie --no-dynamic-linker -z text -T ${USER_LINKER_SCRIPT}
            -e _start --build-id=none -z max-page-size=0x1000 --gc-sections
            ${FEATHEROS_SWIFT_FULL_LINK_FLAGS}
            ${object} ${cxx_objects} $<TARGET_OBJECTS:featheros_crt0>
            ${FEATHEROS_SWIFT_FULL_OBJECTS} --start-group ${cxx_libraries}
            $<TARGET_FILE:featheros_swift_full_support>
            $<TARGET_FILE:featheros_swift_full_string_processing>
            $<TARGET_FILE:featheros_swift_full_concurrency>
            $<TARGET_FILE:featheros_swift_runtime> $<TARGET_FILE:featheros_c>
            --end-group -o ${program}
        DEPENDS ${object} ${cxx_objects} ${cxx_libraries} ${FEATHEROS_SWIFT_FULL_OBJECTS}
            $<TARGET_OBJECTS:featheros_crt0> featheros_swift_full_support
            featheros_swift_full_string_processing
            featheros_swift_full_concurrency
            featheros_swift_runtime featheros_c ${USER_LINKER_SCRIPT}
        COMMENT "Linking full Swift program ${name}-${ARCH}.elf"
        COMMAND_EXPAND_LISTS
        VERBATIM
    )
    add_custom_target(user_${name} ALL DEPENDS ${program})
    add_dependencies(user_${name} featheros_swift_full)
    if (program_CXX)
        add_dependencies(user_${name} featheros_swift_full_cxx featheros_user_support)
    endif ()
    if (cxx_objects)
        add_dependencies(user_${name} user_${name}_cxx)
    endif ()
endfunction()

foreach (entry ${FEATHEROS_PROGRAMS})
    featheros_add_program(${entry} featheros_crt0)
endforeach ()
foreach (entry ${FEATHEROS_START_PROGRAMS})
    featheros_add_program(${entry})
endforeach ()

set(FEATHEROS_SWIFTC "" CACHE FILEPATH "swiftc from a swift.org toolchain, for Swift programs")
set(FEATHEROS_SWIFT_SOURCE ${PROJECT_SOURCE_DIR}/deps/swift CACHE PATH
    "The Swift sources, patched, for the standard library, the concurrency runtime and Cxx")
set(FEATHEROS_LIBCXX_SOURCE ${PROJECT_SOURCE_DIR}/deps/llvm-project/libcxx CACHE PATH
    "libc++'s sources, for the headers of the concurrency runtime and Swift's C++ programs")
set(FEATHEROS_STRING_PROCESSING_SOURCE
    ${PROJECT_SOURCE_DIR}/deps/swift-experimental-string-processing CACHE PATH
    "swift-experimental-string-processing's sources, for full Swift's regular expressions")
if (ARCH STREQUAL "x86_64")
    set(FEATHEROS_SWIFT_TARGET x86_64-unknown-none-elf)
else ()
    set(FEATHEROS_SWIFT_TARGET aarch64-none-none-elf)
endif ()
# The kernel leaves a red zone below the user stack pointer for signal frames, so Swift code may
# use it, unlike the C++ programs built with -mno-red-zone.
# Programs import the C library as the FeatherLibC module, described by src/libc/swift.
set(libc_source ${PROJECT_SOURCE_DIR}/src/libc)
# The optimization follows the C++ code's: -Onone -g for Debug, -O for Release. Programs compile
# in the FeatherSupport code they use, so only they need -g, which swiftc also cannot combine with
# -emit-module for ELF (it wraps the module into the same object).
if (CMAKE_BUILD_TYPE STREQUAL "Release")
    set(FEATHEROS_SWIFT_OPTIMIZATION -O)
else ()
    set(FEATHEROS_SWIFT_OPTIMIZATION -Onone)
    set(FEATHEROS_SWIFT_PROGRAM_DEBUG -g)
endif ()
set(FEATHEROS_SWIFT_FLAGS
    -target ${FEATHEROS_SWIFT_TARGET} -enable-experimental-feature Embedded -wmo
    ${FEATHEROS_SWIFT_OPTIMIZATION}
    -Xfrontend -disable-stack-protector
    -I ${libc_source}/swift -Xcc -I${libc_source}/include -Xcc -ffreestanding
    -module-cache-path ${CMAKE_CURRENT_BINARY_DIR}/swift/module-cache
)
# swiftc's dependency files name the module map but not the headers behind it. The list also
# gets the standard library module once it is built from source.
file(GLOB_RECURSE FEATHEROS_SWIFT_LIBC_FILES CONFIGURE_DEPENDS ${libc_source}/include/*.h)
list(APPEND FEATHEROS_SWIFT_LIBC_FILES ${libc_source}/swift/module.modulemap)
file(GLOB FEATHEROS_SWIFT_CXX_HEADERS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.h)
list(APPEND FEATHEROS_SWIFT_CXX_HEADERS ${CMAKE_CURRENT_SOURCE_DIR}/swift_cxx/module.modulemap)
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
    # String hashing and Characters need the toolchain's Unicode tables; the linker takes only the
    # parts a program uses.
    get_filename_component(swift_bin ${FEATHEROS_SWIFTC} DIRECTORY)
    set(swift_libraries ${swift_bin}/../lib/swift/embedded/${FEATHEROS_SWIFT_TARGET})
    set(FEATHEROS_SWIFT_UNICODE_TABLES ${swift_libraries}/libswiftUnicodeDataTables.a)
    if (NOT EXISTS ${FEATHEROS_SWIFT_UNICODE_TABLES})
        set(probe_result "no ${FEATHEROS_SWIFT_UNICODE_TABLES}")
    endif ()
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
if (swift_usable AND NOT (EXISTS ${FEATHEROS_SWIFT_SOURCE}/stdlib/public/core AND
        EXISTS ${FEATHEROS_LIBCXX_SOURCE}/include AND
        EXISTS ${FEATHEROS_STRING_PROCESSING_SOURCE}/Sources/_StringProcessing))
    set(swift_usable FALSE)
    message(STATUS "No Swift, libc++ or string-processing sources in ${FEATHEROS_SWIFT_SOURCE}, "
        "${FEATHEROS_LIBCXX_SOURCE} and ${FEATHEROS_STRING_PROCESSING_SOURCE}, so the Swift "
        "programs are skipped")
endif ()
if (swift_usable)
    # The probe used the toolchain's standard library; everything else uses ours.
    add_subdirectory(swift_stdlib)
    list(APPEND FEATHEROS_SWIFT_FLAGS -I ${FEATHEROS_SWIFT_STDLIB_MODULE_DIRECTORY})
    list(APPEND FEATHEROS_SWIFT_LIBC_FILES ${FEATHEROS_SWIFT_STDLIB_MODULE})
    featheros_add_swift_support()
    foreach (entry ${FEATHEROS_SWIFT_PROGRAMS})
        featheros_add_swift_program(${entry})
    endforeach ()
    add_subdirectory(swift_concurrency)
    add_subdirectory(swift_cxx)
    foreach (entry ${FEATHEROS_SWIFT_CONCURRENCY_PROGRAMS})
        featheros_add_swift_program(${entry} CONCURRENCY)
    endforeach ()
    foreach (entry ${FEATHEROS_SWIFT_CXX_PROGRAMS})
        featheros_add_swift_program(${entry} CXX)
    endforeach ()
    add_subdirectory(swift_runtime)
    featheros_add_full_swift_support()
    foreach (entry ${FEATHEROS_SWIFT_FULL_PROGRAMS})
        featheros_add_full_swift_program(${entry})
    endforeach ()
    foreach (entry ${FEATHEROS_SWIFT_FULL_CXX_PROGRAMS})
        featheros_add_full_swift_program(${entry} CXX)
    endforeach ()
endif ()
