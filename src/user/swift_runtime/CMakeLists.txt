# Full Swift: the standard library with runtime type information, and the runtime that reads it,
# built from the swift.org sources build.sh fetches into deps/swift and patches with
# ../swift_concurrency/swift.patch. The settings follow upstream's "minimal" freestanding preset
# (utils/build-presets.ini) on ELF: a static library over pthreads, no dladdr or dlsym, no
# backtraces. Unlike that preset, reflection, type printing, the environment, standard input and
# CommandLine are on. Full and Embedded Swift modules cannot be mixed in one program, so full-Swift
# programs build against this module only.

set(swift_source ${FEATHEROS_SWIFT_SOURCE})
set(core_source ${swift_source}/stdlib/public/core)
set(output ${CMAKE_CURRENT_BINARY_DIR})

get_filename_component(swift_bin ${FEATHEROS_SWIFTC} DIRECTORY)
set(clang ${swift_bin}/clang++)
file(GLOB clang_resource_include ${swift_bin}/../lib/clang/*/include)

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
# Upstream builds the runtime optimized and without assertions. __featheros__ picks the cases
# swift.patch adds; FeatherOS is little-endian on both targets. include/ holds a cxxabi.h for the
# declaration the runtime includes.
set(cxx_flags
    ${target_flags} -std=c++17 -O2 ${debug_flags} -DNDEBUG
    -nostdinc -isystem ${FEATHEROS_LIBCXX_CONFIG_DIRECTORY} -isystem ${FEATHEROS_LIBCXX_SOURCE}/include
    -isystem ${PROJECT_SOURCE_DIR}/src/libc/include -isystem ${clang_resource_include}
    -isystem ${CMAKE_CURRENT_SOURCE_DIR}/include
    -ffreestanding -fno-exceptions -fno-cxx-exceptions -fno-rtti -fno-unwind-tables
    -fno-stack-protector -fpie -ffunction-sections -fdata-sections -fswift-async-fp=never
    -D__featheros__ -DLITTLE_ENDIAN=1234 -DBIG_ENDIAN=4321 -DBYTE_ORDER=LITTLE_ENDIAN
    -DSWIFT_RUNTIME -DSWIFT_OBJC_INTEROP=0 -DSWIFT_LIBRARY_EVOLUTION=0 -DSWIFT_STDLIB_HAS_DLSYM=0
    -DSWIFT_STDLIB_HAS_DLADDR=0 -DSWIFT_STDLIB_HAS_DARWIN_LIBMALLOC=0 -DSWIFT_THREADING_PTHREADS
    -DSWIFT_ENABLE_REFLECTION -DSWIFT_STDLIB_HAS_TYPE_PRINTING -DSWIFT_STDLIB_HAS_STDIN
    -DSWIFT_STDLIB_HAS_ENVIRON -DSWIFT_STDLIB_ENABLE_UNICODE_DATA
    -DSWIFT_STDLIB_SHORT_MANGLING_LOOKUPS -DSWIFT_RUNTIME_ENABLE_LEAK_CHECKER=0
    -DSWIFT_INLINE_NAMESPACE=__runtime
    -isystem ${output}/include -isystem ${swift_source}/include
    -isystem ${swift_source}/stdlib/include -isystem ${swift_source}/stdlib/include/llvm/Support
    -isystem ${swift_source}/stdlib/public/SwiftShims -I${swift_source}/stdlib/public/runtime
)

set(c_flags
    ${target_flags} -std=c11 -O2 ${debug_flags} -DNDEBUG -nostdinc
    -isystem ${PROJECT_SOURCE_DIR}/src/libc/include -isystem ${clang_resource_include}
    -ffreestanding -fno-stack-protector -fpie -ffunction-sections -fdata-sections
)

# Upstream's runtime, its C stubs and command-line support, the LLVM support and demangler parts
# it compiles in, and the threading package. SwiftObject.mm and ErrorObject.mm are C++ once Objective-C is off.
set(runtime_sources
    ${swift_source}/stdlib/public/CommandLineSupport/CommandLine.cpp
    ${swift_source}/stdlib/public/CompatibilityOverride/CompatibilityOverride.cpp
    ${swift_source}/lib/Threading/Errors.cpp
    ${swift_source}/lib/Threading/Pthreads.cpp
    ${swift_source}/stdlib/public/runtime/SwiftObject.mm
    ${swift_source}/stdlib/public/runtime/ErrorObject.mm
)
foreach (name AccessibleFunction AnyHashableSupport Array AutoDiffSupport Bincompat Borrow
        BytecodeLayouts Casting CrashReporter Demangle DynamicCast Enum EnvironmentVariables
        ErrorDefaultImpls ErrorObjectCommon ErrorObjectNative Exception Exclusivity
        ExistentialContainer Float16Support FoundationSupport FunctionReplacement
        GenericMetadataBuilder Heap HeapObject ImageInspectionCommon ImageInspectionELF
        ImageInspectionStatic KeyPaths KnownMetadata LibPrespecialized Metadata MetadataLookup
        Numeric Once Paths Portability ProtocolConformance RefCount ReflectionMirror
        RuntimeErrorReporting RuntimeInvocationsTracking SwiftTLSContext SymbolInfo
        ThreadingError Tracing)
    list(APPEND runtime_sources ${swift_source}/stdlib/public/runtime/${name}.cpp)
endforeach ()
foreach (name Assert GlobalObjects LibcShims MathStubs Random Stubs ThreadLocalStorage
        Unicode/UnicodeData Unicode/UnicodeEmoji Unicode/UnicodeGrapheme
        Unicode/UnicodeNormalization Unicode/UnicodeScalarProps Unicode/UnicodeWord)
    list(APPEND runtime_sources ${swift_source}/stdlib/public/stubs/${name}.cpp)
endforeach ()
foreach (name ErrorHandling Hashing MemAlloc SmallPtrSet SmallVector StringRef)
    list(APPEND runtime_sources ${swift_source}/stdlib/public/LLVMSupport/${name}.cpp)
endforeach ()
foreach (name Context Demangler DemanglingErrorHandling ManglingUtils NodeDumper NodePrinter
        Punycode Remangler)
    list(APPEND runtime_sources ${swift_source}/lib/Demangling/${name}.cpp)
endforeach ()
# Compiles each source into objects/<directory>/ with cxx_flags and the given flags, into the list
# <objects_variable>. Upstream's files are compiled without warnings, our own with -Werror.
function(featheros_swift_runtime_objects objects_variable directory)
    cmake_parse_arguments(PARSE_ARGV 2 arg "" "" "FLAGS;SOURCES")
    set(objects)
    foreach (source ${arg_SOURCES})
        if (source MATCHES "^${PROJECT_SOURCE_DIR}/src/")
            get_filename_component(name ${source} NAME_WE)
            set(warnings -Wall -Wextra -Werror)
        else ()
            file(RELATIVE_PATH name ${swift_source} ${source})
            string(REGEX REPLACE "\\.[a-z]+$" "" name ${name})
            string(REPLACE "/" "_" name ${name})
            set(warnings -w)
        endif ()
        set(object ${output}/objects/${directory}/${name}.o)
        if (source MATCHES "\\.c$")
            set(language_flags ${c_flags} -x c)
        else ()
            set(language_flags ${cxx_flags} -x c++)
        endif ()
        add_custom_command(
            OUTPUT ${object}
            COMMAND ${clang} ${language_flags} ${arg_FLAGS} ${warnings} -MD -MF ${object}.d
                -c ${source} -o ${object}
            DEPENDS ${source} ${FEATHEROS_LIBCXX_CONFIG_DIRECTORY}/__config_site
            DEPFILE ${object}.d
            COMMENT "Building Swift ${directory} object ${name}.o"
            VERBATIM
        )
        list(APPEND objects ${object})
    endforeach ()
    set_source_files_properties(${objects} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
    set(${objects_variable} ${objects} PARENT_SCOPE)
endfunction()

featheros_swift_runtime_objects(objects runtime
    FLAGS -DSWIFT_TARGET_LIBRARY_NAME=swiftRuntimeCore
    SOURCES ${runtime_sources} ${CMAKE_CURRENT_SOURCE_DIR}/platform.cpp)
# Registers the program's swift5_* sections with the runtime from a constructor, through the
# linker's __start_ and __stop_ symbols. Nothing refers to it, so programs link the object itself.
featheros_swift_runtime_objects(registration_object runtime
    FLAGS -DSWIFT_TARGET_LIBRARY_NAME=swiftRuntimeCore
    SOURCES ${swift_source}/stdlib/public/runtime/SwiftRT-ELF-WASM.cpp)
add_library(featheros_swift_runtime STATIC ${objects})
set_target_properties(featheros_swift_runtime PROPERTIES LINKER_LANGUAGE CXX)

# The Swift module: upstream's whole file list, each EMBEDDED and NORMAL entry, with the SIMD
# types. ObjectIdentifier+DebugDescription.swift is left out, as for upstream's freestanding
# builds, since it needs the compiler's macro plugins.
set(swift_sources)
file(STRINGS ${core_source}/CMakeLists.txt core_cmake)
foreach (line ${core_cmake})
    if (line MATCHES "^ *(EMBEDDED|NORMAL) ([^ \"]+\\.swift(\\.gyb)?)( *#.*)?$")
        set(name ${CMAKE_MATCH_2})
    elseif (line MATCHES "^ *list\\(APPEND SWIFTLIB_SOURCES ([^ )]+\\.swift)\\)")
        set(name ${CMAKE_MATCH_1})
    else ()
        continue ()
    endif ()
    if (name MATCHES "\\.gyb$")
        string(REGEX REPLACE "\\.gyb$" "" generated_name ${name})
        set(generated ${output}/gyb/${generated_name})
        add_custom_command(
            OUTPUT ${generated}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${output}/gyb
            COMMAND ${CMAKE_COMMAND} -E env PYTHONPATH=${swift_source}/utils
                python3 ${swift_source}/utils/gyb.py -DCMAKE_SIZEOF_VOID_P=8
                --line-directive "" -o ${generated} ${core_source}/${name}
            DEPENDS ${core_source}/${name}
            COMMENT "Expanding ${name}"
            VERBATIM
        )
        list(APPEND swift_sources ${generated})
    else ()
        list(APPEND swift_sources ${core_source}/${name})
    endif ()
endforeach ()

# Release programs are sealed: the compiler sees the standard library and the program as one
# LLVM module at link time (-lto=llvm-full, so each object is bitcode) and drops what nothing
# uses, including the type records of unused types, about half of a program.
# The link turns off LLVM's whole-program devirtualization: its virtual constant propagation
# replaces a call to a method that returns a constant with a load next to the class's vtable in
# the metadata global, but a generic class's metadata is built on the heap from that pattern, so
# the load reads whatever follows it there (KeyPath.kind did, and appending key paths failed).
# The seal's virtual function and witness method elimination must wait for the link. swiftc marks
# the vtables and witness tables of private types as seen only by their own file, so its own
# GlobalDCE pass would drop every entry that file does not call, although a conformance can reach
# other modules: FeatherSupport's JSON encoder calls stringValue on a type's synthesized, private
# CodingKeys through any CodingKey, and found null. -enable-vfe=false keeps the compile from
# removing them; the link, which sees every call, still does.
if (CMAKE_BUILD_TYPE STREQUAL "Release")
    set(seal -lto=llvm-full -experimental-hermetic-seal-at-link -Xllvm -enable-vfe=false)
    set(FEATHEROS_SWIFT_FULL_LINK_FLAGS -mllvm -wholeprogramdevirt-skip=* PARENT_SCOPE)
endif ()
# swiftc imports SwiftShims from its resource directory, so the toolchain's unpatched headers would
# shadow swift.patch's (LibcShims.h's malloc_usable_size, without which ManagedBuffer.capacity is
# negative). Like upstream's build tree, this resource directory is the toolchain's with the
# shims from the sources.
set(resource_directory ${output}/resource)
file(GLOB toolchain_resources ${swift_bin}/../lib/swift/*)
file(MAKE_DIRECTORY ${resource_directory})
foreach (resource ${toolchain_resources})
    get_filename_component(name ${resource} NAME)
    if (NOT name STREQUAL "shims")
        file(CREATE_LINK ${resource} ${resource_directory}/${name} SYMBOLIC)
    endif ()
endforeach ()
file(CREATE_LINK ${swift_source}/stdlib/public/SwiftShims/swift/shims ${resource_directory}/shims
    SYMBOLIC)

# Flags every full-Swift module and program shares: the C library through FeatherLibC, and
# __featheros__ for the shims' cases, which inlined standard library code compiles into programs.
set(FEATHEROS_SWIFT_FULL_FLAGS
    -target ${FEATHEROS_SWIFT_TARGET} -resource-dir ${resource_directory}
    -wmo ${seal} -runtime-compatibility-version none
    -Xfrontend -disable-stack-protector
    -I ${PROJECT_SOURCE_DIR}/src/libc/swift -Xcc -I${PROJECT_SOURCE_DIR}/src/libc/include
    -Xcc -ffreestanding -Xcc -D__featheros__ -module-cache-path ${output}/module-cache
)

# Programs import _Concurrency and _StringProcessing implicitly, as on other platforms; the
# modules built here must not.
set(below_string_processing -Xfrontend -disable-implicit-string-processing-module-import)
set(below_concurrency ${below_string_processing}
    -Xfrontend -disable-implicit-concurrency-module-import)

set(module_directory ${output}/module)
set(module ${module_directory}/Swift.swiftmodule/${FEATHEROS_SWIFT_TARGET}.swiftmodule)
set(swift_object ${output}/Swift.o)
set(features Macros FreestandingMacros Extern BitwiseCopyable ValueGenerics AddressableParameters
    AddressableTypes AllowUnsafeAttribute SuppressedAssociatedTypesWithDefaults Reparenting
    Lifetimes BorrowAndMutateAccessors BorrowInout NonescapableTypes LifetimeDependence
    InoutLifetimeDependence LifetimeDependenceMutableAccessors)
list(TRANSFORM features PREPEND "-enable-experimental-feature;")
add_custom_command(
    OUTPUT ${module} ${swift_object}
    COMMAND ${FEATHEROS_SWIFTC} ${FEATHEROS_SWIFT_FULL_FLAGS} ${below_concurrency} ${features}
        -enable-upcoming-feature MemberImportVisibility -strict-memory-safety
        -nostdimport -parse-stdlib -module-name Swift -swift-version 5 -parse-as-library -O
        -library-level api -Xllvm -sil-inline-generics -Xllvm -sil-partial-specialization
        -Xfrontend -enable-experimental-concise-pound-file
        -Xfrontend -group-info-path -Xfrontend ${core_source}/GroupInfo.json
        -Xfrontend -enforce-exclusivity=unchecked -Xfrontend -empty-abi-descriptor
        -Xfrontend -disable-objc-interop -Xfrontend -disable-objc-attr-requires-foundation-module
        -Xfrontend -require-explicit-availability=ignore
        -Xfrontend -target-min-inlining-version -Xfrontend min
        -Xfrontend -enable-lexical-lifetimes=false
        -D SWIFT_ENABLE_REFLECTION -D SWIFT_STDLIB_HAS_STDIN -D SWIFT_STDLIB_HAS_COMMANDLINE
        -D SWIFT_STDLIB_ENABLE_VECTOR_TYPES ${FEATHEROS_SWIFT_AVAILABILITY}
        -emit-module -emit-module-path ${module} -c ${swift_sources} -o ${swift_object}
    DEPENDS ${swift_sources} ${core_source}/GroupInfo.json
    COMMENT "Building full Swift module Swift"
    VERBATIM
)
set(FEATHEROS_SWIFT_FULL_OBJECTS ${swift_object})
set(module_dependencies ${module})

# -Onone programs call the standard library's generic code through prespecializations from
# SwiftOnoneSupport, which the compiler imports into each of them.
if (NOT CMAKE_BUILD_TYPE STREQUAL "Release")
    set(onone_module
        ${module_directory}/SwiftOnoneSupport.swiftmodule/${FEATHEROS_SWIFT_TARGET}.swiftmodule)
    set(onone_object ${output}/SwiftOnoneSupport.o)
    set(onone_source ${swift_source}/stdlib/public/SwiftOnoneSupport/SwiftOnoneSupport.swift)
    add_custom_command(
        OUTPUT ${onone_module} ${onone_object}
        COMMAND ${FEATHEROS_SWIFTC} ${FEATHEROS_SWIFT_FULL_FLAGS} ${below_concurrency}
            -I ${module_directory} -parse-stdlib -module-name SwiftOnoneSupport -parse-as-library -O
            -enable-experimental-feature AllowUnsafeAttribute -strict-memory-safety
            -Xfrontend -disable-objc-interop -Xllvm -sil-inline-generics=false
            -Xfrontend -validate-tbd-against-ir=none -Xfrontend -check-onone-completeness
            -Xfrontend -disable-access-control
            -emit-module -emit-module-path ${onone_module} -c ${onone_source} -o ${onone_object}
        DEPENDS ${onone_source} ${module}
        COMMENT "Building full Swift module SwiftOnoneSupport"
        VERBATIM
    )
    list(APPEND FEATHEROS_SWIFT_FULL_OBJECTS ${onone_object})
    list(APPEND module_dependencies ${onone_module})
endif ()

# _Concurrency as upstream builds it for full Swift, with its executors from DispatchExecutor.swift
# over ../swift_concurrency/global_executor.cpp instead of Dispatch. The threading package is in
# the runtime already. Programs import it implicitly and link it from an archive, so only those
# that use concurrency carry it.
set(concurrency_source ${swift_source}/stdlib/public/Concurrency)
set(concurrency_cxx_sources
    ${swift_source}/stdlib/public/CompatibilityOverride/CompatibilityOverride.cpp
    ${PROJECT_SOURCE_DIR}/src/user/swift_concurrency/global_executor.cpp
)
foreach (name Actor AsyncLet AsyncStream Clock ConcurrencyHooks EmbeddedSupport Error
        ExecutorBridge ExecutorChecks ExecutorImpl GlobalExecutor Task TaskAlloc TaskGroup
        TaskLocal TaskStatus ThreadingError TracingSignpost)
    list(APPEND concurrency_cxx_sources ${concurrency_source}/${name}.cpp)
endforeach ()
featheros_swift_runtime_objects(concurrency_objects concurrency
    FLAGS -DSWIFT_TARGET_LIBRARY_NAME=swift_Concurrency -Dswift_Concurrency_EXPORTS
        -DSWIFT_CONCURRENCY_ENABLE_DISPATCH=0 -D__STDC_WANT_LIB_EXT1__=1 -I${concurrency_source}
    SOURCES ${concurrency_cxx_sources})

set(concurrency_swift_sources)
file(STRINGS ${concurrency_source}/CMakeLists.txt concurrency_cmake)
set(in_list FALSE)
foreach (line ${concurrency_cmake})
    if (line MATCHES "^set\\(SWIFT_RUNTIME_CONCURRENCY_SWIFT_SOURCES")
        set(in_list TRUE)
    elseif (in_list AND line MATCHES "^\\)")
        break ()
    elseif (in_list)
        string(STRIP "${line}" line)
        list(APPEND concurrency_swift_sources ${concurrency_source}/${line})
    endif ()
endforeach ()
list(APPEND concurrency_swift_sources ${concurrency_source}/DispatchExecutor.swift
    ${concurrency_source}/ExecutorImpl.swift
    ${CMAKE_CURRENT_SOURCE_DIR}/PlatformExecutorFeatherOS.swift)
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
    list(APPEND concurrency_swift_sources ${generated})
endforeach ()

set(concurrency_module
    ${module_directory}/_Concurrency.swiftmodule/${FEATHEROS_SWIFT_TARGET}.swiftmodule)
set(concurrency_object ${output}/objects/concurrency/_Concurrency.o)
set(features IsolatedAny AllowUnsafeAttribute Extern SuppressedAssociatedTypesWithDefaults
    NonescapableTypes LifetimeDependence InoutLifetimeDependence
    LifetimeDependenceMutableAccessors Lifetimes)
list(TRANSFORM features PREPEND "-enable-experimental-feature;")
add_custom_command(
    OUTPUT ${concurrency_module} ${concurrency_object}
    COMMAND ${FEATHEROS_SWIFTC} ${FEATHEROS_SWIFT_FULL_FLAGS} ${below_concurrency}
        -I ${module_directory} ${features} -enable-upcoming-feature MemberImportVisibility
        -strict-memory-safety -parse-stdlib -module-name _Concurrency -swift-version 5
        -parse-as-library -O -library-level api -I ${concurrency_source}/InternalShims
        -Xfrontend -enforce-exclusivity=unchecked -Xfrontend -empty-abi-descriptor
        -Xfrontend -disable-objc-interop ${FEATHEROS_SWIFT_AVAILABILITY}
        -emit-module -emit-module-path ${concurrency_module}
        -c ${concurrency_swift_sources} -o ${concurrency_object}
    DEPENDS ${concurrency_swift_sources} ${module}
    COMMENT "Building full Swift module _Concurrency"
    VERBATIM
)
set_source_files_properties(${concurrency_object} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
add_library(featheros_swift_full_concurrency STATIC ${concurrency_object} ${concurrency_objects})
set_target_properties(featheros_swift_full_concurrency PROPERTIES LINKER_LANGUAGE CXX)
list(APPEND module_dependencies ${concurrency_module})

# The Cxx overlay as upstream builds it, unpatched: full Swift compiles its non-inlinable parts
# into an object, so C++ programs link it from an archive. It needs no C++ standard library.
set(cxx_source ${swift_source}/stdlib/public/Cxx)
set(cxx_swift_sources)
foreach (name CxxConvertibleToBool CxxConvertibleToCollection CxxDictionary CxxPair CxxOptional
        CxxSet CxxRandomAccessCollection CxxSequence CxxBorrowingSequence CxxVector CxxSpan
        UnsafeCxxIterators)
    list(APPEND cxx_swift_sources ${cxx_source}/${name}.swift)
endforeach ()
set(cxx_module ${module_directory}/Cxx.swiftmodule/${FEATHEROS_SWIFT_TARGET}.swiftmodule)
set(cxx_object ${output}/objects/cxx/Cxx.o)
set(features BuiltinModule AllowUnsafeAttribute Lifetimes LifetimeDependence NonescapableTypes
    SuppressedAssociatedTypesWithDefaults)
list(TRANSFORM features PREPEND "-enable-experimental-feature;")
add_custom_command(
    OUTPUT ${cxx_module} ${cxx_object}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${output}/objects/cxx
    COMMAND ${FEATHEROS_SWIFTC} ${FEATHEROS_SWIFT_FULL_FLAGS} ${below_string_processing}
        -I ${module_directory} ${features} -cxx-interoperability-mode=default -Xcc -nostdinc++ -strict-memory-safety
        -module-name Cxx -parse-as-library -O -library-level api
        -Xfrontend -disable-objc-interop ${FEATHEROS_SWIFT_AVAILABILITY}
        -emit-module -emit-module-path ${cxx_module} -c ${cxx_swift_sources} -o ${cxx_object}
    DEPENDS ${cxx_swift_sources} ${module} ${concurrency_module}
    COMMENT "Building full Swift module Cxx"
    VERBATIM
)
set_source_files_properties(${cxx_object} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
add_library(featheros_swift_full_cxx STATIC ${cxx_object})
set_target_properties(featheros_swift_full_cxx PROPERTIES LINKER_LANGUAGE CXX)
list(APPEND module_dependencies ${cxx_module})

# Swift's regular expressions from swift-experimental-string-processing, as upstream builds them:
# the parser _RegexParser, the engine _StringProcessing (with the C tables of _CUnicode), which
# programs import implicitly, and RegexBuilder. Programs link them from one archive.
set(string_processing_source ${FEATHEROS_STRING_PROCESSING_SOURCE}/Sources)
featheros_swift_runtime_objects(cunicode_objects string_processing
    FLAGS -Dswift_StringProcessing_EXPORTS
    SOURCES ${string_processing_source}/_CUnicode/UnicodeData.c
        ${string_processing_source}/_CUnicode/UnicodeScalarProps.c)
set(features SuppressedAssociatedTypesWithDefaults NonescapableTypes LifetimeDependence
    InoutLifetimeDependence LifetimeDependenceMutableAccessors Lifetimes)
list(TRANSFORM features PREPEND "-enable-experimental-feature;")
set(string_processing_flags ${FEATHEROS_SWIFT_FULL_FLAGS} ${below_concurrency}
    -I ${module_directory} ${features} -enable-upcoming-feature MemberImportVisibility
    -swift-version 5 -parse-as-library -O -library-level api
    -Xfrontend -enforce-exclusivity=unchecked -Xfrontend -target-min-inlining-version
    -Xfrontend min -Xfrontend -enable-lexical-lifetimes=false -Xfrontend -disable-objc-interop
    ${FEATHEROS_SWIFT_AVAILABILITY})
set(string_processing_objects ${cunicode_objects})
set(string_processing_modules)
foreach (module_name _RegexParser _StringProcessing RegexBuilder)
    if (module_name STREQUAL "_RegexParser")
        set(extra_flags -enable-experimental-feature AllowRuntimeSymbolDeclarations)
    elseif (module_name STREQUAL "_StringProcessing")
        set(extra_flags -D RESILIENT_LIBRARIES)
    else ()
        set(extra_flags)
    endif ()
    file(GLOB_RECURSE sources CONFIGURE_DEPENDS ${string_processing_source}/${module_name}/*.swift)
    set(module_file
        ${module_directory}/${module_name}.swiftmodule/${FEATHEROS_SWIFT_TARGET}.swiftmodule)
    set(object ${output}/objects/string_processing/${module_name}.o)
    add_custom_command(
        OUTPUT ${module_file} ${object}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${output}/objects/string_processing
        COMMAND ${FEATHEROS_SWIFTC} ${string_processing_flags} ${extra_flags}
            -module-name ${module_name} -emit-module -emit-module-path ${module_file}
            -c ${sources} -o ${object}
        DEPENDS ${sources} ${module} ${string_processing_modules}
        COMMENT "Building full Swift module ${module_name}"
        VERBATIM
    )
    list(APPEND string_processing_objects ${object})
    list(APPEND string_processing_modules ${module_file})
endforeach ()
set_source_files_properties(${string_processing_objects} PROPERTIES EXTERNAL_OBJECT TRUE
    GENERATED TRUE)
add_library(featheros_swift_full_string_processing STATIC ${string_processing_objects})
set_target_properties(featheros_swift_full_string_processing PROPERTIES LINKER_LANGUAGE CXX)
list(APPEND module_dependencies ${string_processing_modules})

# Programs in the parent directory depend on this target for the files above.
add_custom_target(featheros_swift_full
    DEPENDS ${module_dependencies} ${FEATHEROS_SWIFT_FULL_OBJECTS} ${registration_object})

set(FEATHEROS_SWIFT_FULL_FLAGS ${FEATHEROS_SWIFT_FULL_FLAGS} -I ${module_directory} PARENT_SCOPE)
set(FEATHEROS_SWIFT_FULL_MODULES ${module_dependencies} PARENT_SCOPE)
set(FEATHEROS_SWIFT_FULL_OBJECTS ${registration_object} ${FEATHEROS_SWIFT_FULL_OBJECTS}
    PARENT_SCOPE)
set(FEATHEROS_SWIFT_LLD ${swift_bin}/ld.lld PARENT_SCOPE)
set(FEATHEROS_SWIFT_FULL_CXX_MODULE ${cxx_module} PARENT_SCOPE)
