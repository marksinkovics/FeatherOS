# Swift's concurrency runtime, built from the swift.org sources build.sh fetches into deps/swift
# and patches with swift.patch, as upstream builds it for Embedded Swift but with the pthreads
# threading package over the C library and global_executor.cpp's thread pool in place of the
# single-threaded cooperative executor. Its C++ half needs Clang and a C++ standard library, so
# it uses the toolchain's clang++ and libc++'s headers from deps/llvm-project, configured by
# libcxx/__config_site; nothing calls into a compiled libc++. The Swift half is only the
# _Concurrency module: Embedded Swift compiles the parts a program uses into the program.

set(swift_source ${FEATHEROS_SWIFT_SOURCE})
set(concurrency_source ${swift_source}/stdlib/public/Concurrency)
set(libcxx_source ${FEATHEROS_LIBCXX_SOURCE})
set(output ${CMAKE_CURRENT_BINARY_DIR})

get_filename_component(swift_bin ${FEATHEROS_SWIFTC} DIRECTORY)
set(clang ${swift_bin}/clang++)
file(GLOB clang_resource_include ${swift_bin}/../lib/clang/*/include)

# libc++'s generated headers: the settings, and its default assertion handler.
configure_file(libcxx/__config_site ${output}/libcxx/__config_site COPYONLY)
configure_file(${libcxx_source}/vendor/llvm/default_assertion_handler.in
    ${output}/libcxx/__assertion_handler COPYONLY)
file(READ ${PROJECT_SOURCE_DIR}/.swift-version swift_version)
string(REGEX MATCH "^([0-9]+)\\.([0-9]+)" swift_version "${swift_version}")
set(SWIFT_VERSION_MAJOR ${CMAKE_MATCH_1})
set(SWIFT_VERSION_MINOR ${CMAKE_MATCH_2})
configure_file(${swift_source}/include/swift/Runtime/CMakeConfig.h.in
    ${output}/include/swift/Runtime/CMakeConfig.h)

if (ARCH STREQUAL "x86_64")
    # The runtime's double-word atomics need cmpxchg16b.
    set(target_flags -target x86_64-unknown-none-elf -mcx16 -mno-red-zone)
else ()
    set(target_flags -target aarch64-none-none-elf -march=armv8-a -mno-outline-atomics)
endif ()
if (CMAKE_BUILD_TYPE STREQUAL "Release")
    set(debug_flags -g0)
else ()
    set(debug_flags -g)
endif ()
# Upstream builds the Embedded runtime only optimized, without assertions. __featheros__ picks
# the cases swift.patch adds; FeatherOS is little-endian on both targets.
set(cxx_flags
    ${target_flags} -std=c++17 -O2 ${debug_flags} -DNDEBUG
    -nostdinc -isystem ${output}/libcxx -isystem ${libcxx_source}/include
    -isystem ${PROJECT_SOURCE_DIR}/src/libc/include -isystem ${clang_resource_include}
    -ffreestanding -fno-exceptions -fno-cxx-exceptions -fno-rtti -fno-unwind-tables
    -fno-stack-protector -fpie -ffunction-sections -fdata-sections -fswift-async-fp=never
    -D__featheros__ -DLITTLE_ENDIAN=1234 -DBIG_ENDIAN=4321 -DBYTE_ORDER=LITTLE_ENDIAN
    -DSWIFT_RUNTIME -DSWIFT_OBJC_INTEROP=0 -DSWIFT_LIBRARY_EVOLUTION=0 -DSWIFT_STDLIB_HAS_DLSYM=0
    -DSWIFT_STDLIB_HAS_DARWIN_LIBMALLOC=0 -DSWIFT_THREADING_PTHREADS
    -DSWIFT_CONCURRENCY_ENABLE_DISPATCH=0 -D__STDC_WANT_LIB_EXT1__=1
    -DSWIFT_CONCURRENCY_EMBEDDED=1 -DSWIFT_RUNTIME_EMBEDDED=1 -Dswift_Concurrency_EXPORTS
    -DSWIFT_TARGET_LIBRARY_NAME=swift_Concurrency
    -isystem ${output}/include -isystem ${swift_source}/include
    -isystem ${swift_source}/stdlib/include -isystem ${swift_source}/stdlib/public/SwiftShims
    -I${concurrency_source}
)

set(runtime_sources
    ${swift_source}/stdlib/public/CompatibilityOverride/CompatibilityOverride.cpp
    ${swift_source}/lib/Threading/Errors.cpp
    ${swift_source}/lib/Threading/Pthreads.cpp
)
foreach (name Actor AsyncLet AsyncStream Clock ConcurrencyHooks EmbeddedSupport Error
        ExecutorBridge ExecutorChecks GlobalExecutor Task TaskAlloc TaskGroup TaskLocal TaskStatus
        ThreadingError TracingSignpost)
    list(APPEND runtime_sources ${concurrency_source}/${name}.cpp)
endforeach ()

set(objects)
set(own_sources
    ${CMAKE_CURRENT_SOURCE_DIR}/global_executor.cpp ${CMAKE_CURRENT_SOURCE_DIR}/platform.cpp)
foreach (source ${runtime_sources} ${own_sources})
    get_filename_component(name ${source} NAME_WE)
    set(object ${output}/objects/${name}.o)
    if (source IN_LIST own_sources)
        set(warnings -Wall -Wextra -Werror)
    else ()
        set(warnings -w)
    endif ()
    add_custom_command(
        OUTPUT ${object}
        COMMAND ${clang} ${cxx_flags} ${warnings} -MD -MF ${object}.d -c ${source} -o ${object}
        DEPENDS ${source} ${output}/libcxx/__config_site
        DEPFILE ${object}.d
        COMMENT "Building Swift concurrency runtime object ${name}.o"
        VERBATIM
    )
    list(APPEND objects ${object})
endforeach ()
set_source_files_properties(${objects} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
add_library(featheros_swift_concurrency STATIC ${objects})
set_target_properties(featheros_swift_concurrency PROPERTIES LINKER_LANGUAGE CXX)

# The _Concurrency module, with every availability macro expanded to "*", as for Embedded Swift
# upstream.
set(swift_sources)
file(STRINGS ${concurrency_source}/CMakeLists.txt concurrency_cmake)
set(in_list FALSE)
foreach (line ${concurrency_cmake})
    if (line MATCHES "^set\\(SWIFT_RUNTIME_CONCURRENCY_SWIFT_SOURCES")
        set(in_list TRUE)
    elseif (in_list AND line MATCHES "^\\)")
        break ()
    elseif (in_list)
        string(STRIP "${line}" line)
        list(APPEND swift_sources ${concurrency_source}/${line})
    endif ()
endforeach ()
list(APPEND swift_sources ${concurrency_source}/PlatformExecutorNone.swift)
foreach (name Task+init TaskGroup+addTask Task+immediate)
    set(generated ${output}/gyb/${name}.swift)
    add_custom_command(
        OUTPUT ${generated}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${output}/gyb
        COMMAND python3 ${swift_source}/utils/gyb.py --line-directive "" -o ${generated}
            ${concurrency_source}/${name}.swift.gyb
        DEPENDS ${concurrency_source}/${name}.swift.gyb
        COMMENT "Expanding ${name}.swift.gyb"
        VERBATIM
    )
    list(APPEND swift_sources ${generated})
endforeach ()

set(availability)
file(STRINGS ${swift_source}/utils/availability-macros.def definitions REGEX "^[^#].*:")
foreach (definition ${definitions})
    string(REGEX REPLACE ":.*" "" name "${definition}")
    list(APPEND availability -Xfrontend -define-availability -Xfrontend "${name}:*")
    if (name MATCHES "^SwiftStdlib (.*)")
        list(APPEND availability
            -Xfrontend -define-availability -Xfrontend "StdlibDeploymentTarget ${CMAKE_MATCH_1}:*")
    endif ()
endforeach ()

set(module_directory ${output}/module)
set(module ${module_directory}/_Concurrency.swiftmodule)
set(features IsolatedAny AllowUnsafeAttribute Extern SuppressedAssociatedTypesWithDefaults
    NonescapableTypes LifetimeDependence InoutLifetimeDependence
    LifetimeDependenceMutableAccessors Lifetimes)
list(TRANSFORM features PREPEND "-enable-experimental-feature;")
add_custom_command(
    OUTPUT ${module}
    COMMAND ${FEATHEROS_SWIFTC} -target ${FEATHEROS_SWIFT_TARGET}
        -enable-experimental-feature Embedded -parse-stdlib -parse-as-library -wmo -O
        -module-name _Concurrency -swift-version 5 -DSWIFT_CONCURRENCY_EMBEDDED
        -I ${concurrency_source}/InternalShims ${features} -strict-memory-safety
        -enable-upcoming-feature MemberImportVisibility
        -Xfrontend -enforce-exclusivity=unchecked -Xfrontend -empty-abi-descriptor
        -runtime-compatibility-version none -Xfrontend -disable-objc-interop
        -Xfrontend -disable-standard-substitutions-in-reflection-mangling
        -Xfrontend -disable-stack-protector ${availability}
        -module-cache-path ${output}/module-cache
        -emit-module -emit-module-path ${module} ${swift_sources}
    DEPENDS ${swift_sources}
    COMMENT "Building Swift module _Concurrency"
    VERBATIM
)
add_custom_target(featheros_swift_concurrency_module DEPENDS ${module})
add_dependencies(featheros_swift_concurrency featheros_swift_concurrency_module)

set(FEATHEROS_SWIFT_CONCURRENCY_MODULE_DIRECTORY ${module_directory} PARENT_SCOPE)
set(FEATHEROS_SWIFT_CONCURRENCY_MODULE ${module} PARENT_SCOPE)
