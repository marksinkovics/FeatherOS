# Swift's standard library for Embedded Swift, built from the swift.org sources build.sh fetches
# into deps/swift and patches with ../swift_concurrency/swift.patch, in place of the toolchain's
# prebuilt module. It is a module only, as upstream builds it: Embedded Swift compiles the parts a
# program uses into the program. Every Swift module and program is compiled against it, so its
# flags follow upstream's (stdlib/public/core/CMakeLists.txt and stdlib/cmake/modules/
# SwiftSource.cmake) for an Embedded build.

set(core_source ${FEATHEROS_SWIFT_SOURCE}/stdlib/public/core)
set(output ${CMAKE_CURRENT_BINARY_DIR})

# Upstream's own list: each "EMBEDDED <file>" entry of split_embedded_sources, and the files it
# appends to SWIFTLIB_EMBEDDED_SOURCES afterwards.
set(swift_sources)
set(gyb_sources)
file(STRINGS ${core_source}/CMakeLists.txt core_cmake)
set(in_append FALSE)
foreach (line ${core_cmake})
    if (line MATCHES "^ *EMBEDDED ([^ ]+)")
        set(name ${CMAKE_MATCH_1})
    elseif (line MATCHES "^ *list\\(APPEND SWIFTLIB_EMBEDDED_SOURCES ([^ )$]+)\\)")
        set(name ${CMAKE_MATCH_1})
    elseif (line MATCHES "^list\\(APPEND SWIFTLIB_EMBEDDED_SOURCES$")
        set(in_append TRUE)
        continue ()
    elseif (in_append AND line MATCHES "^ *\\)")
        set(in_append FALSE)
        continue ()
    elseif (in_append)
        string(STRIP "${line}" name)
    else ()
        continue ()
    endif ()
    if (name MATCHES "\\.gyb$")
        list(APPEND gyb_sources ${name})
    else ()
        list(APPEND swift_sources ${core_source}/${name})
    endif ()
endforeach ()

foreach (name ${gyb_sources})
    string(REGEX REPLACE "\\.gyb$" "" generated_name ${name})
    set(generated ${output}/gyb/${generated_name})
    add_custom_command(
        OUTPUT ${generated}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${output}/gyb
        COMMAND ${CMAKE_COMMAND} -E env PYTHONPATH=${FEATHEROS_SWIFT_SOURCE}/utils
            python3 ${FEATHEROS_SWIFT_SOURCE}/utils/gyb.py -DCMAKE_SIZEOF_VOID_P=8
            --line-directive "" -o ${generated} ${core_source}/${name}
        DEPENDS ${core_source}/${name}
        COMMENT "Expanding ${name}"
        VERBATIM
    )
    list(APPEND swift_sources ${generated})
endforeach ()

# Every availability macro expanded to "*", as upstream does for Embedded Swift, here and for the
# other modules built from the Swift sources.
set(availability)
file(STRINGS ${FEATHEROS_SWIFT_SOURCE}/utils/availability-macros.def definitions
    REGEX "^[^#].*:")
foreach (definition ${definitions})
    string(REGEX REPLACE ":.*" "" name "${definition}")
    list(APPEND availability -Xfrontend -define-availability -Xfrontend "${name}:*")
    if (name MATCHES "^SwiftStdlib (.*)")
        list(APPEND availability
            -Xfrontend -define-availability -Xfrontend "StdlibDeploymentTarget ${CMAKE_MATCH_1}:*")
    endif ()
endforeach ()

set(module_directory ${output}/module)
set(module ${module_directory}/Swift.swiftmodule/${FEATHEROS_SWIFT_TARGET}.swiftmodule)
set(features Embedded Macros FreestandingMacros Extern BitwiseCopyable ValueGenerics
    AddressableParameters AddressableTypes AllowUnsafeAttribute
    SuppressedAssociatedTypesWithDefaults Reparenting Lifetimes BorrowAndMutateAccessors
    BorrowInout NonescapableTypes LifetimeDependence InoutLifetimeDependence
    LifetimeDependenceMutableAccessors)
list(TRANSFORM features PREPEND "-enable-experimental-feature;")
add_custom_command(
    OUTPUT ${module}
    COMMAND ${FEATHEROS_SWIFTC} -target ${FEATHEROS_SWIFT_TARGET} ${features}
        -enable-upcoming-feature MemberImportVisibility -strict-memory-safety
        -nostdimport -parse-stdlib -module-name Swift -swift-version 5 -parse-as-library -wmo -O
        -Xllvm -sil-inline-generics -Xllvm -sil-partial-specialization
        -Xfrontend -enable-experimental-concise-pound-file
        -Xfrontend -group-info-path -Xfrontend ${core_source}/GroupInfo.json
        -Xfrontend -enforce-exclusivity=unchecked -Xfrontend -empty-abi-descriptor
        -runtime-compatibility-version none -Xfrontend -disable-objc-interop
        -Xfrontend -disable-reflection-metadata
        -Xfrontend -disable-standard-substitutions-in-reflection-mangling
        -Xcc -ffreestanding ${availability}
        -module-cache-path ${output}/module-cache
        -emit-module -emit-module-path ${module} ${swift_sources}
    DEPENDS ${swift_sources} ${core_source}/GroupInfo.json
    COMMENT "Building Swift module Swift"
    VERBATIM
)
add_custom_target(featheros_swift_stdlib_module DEPENDS ${module})

set(FEATHEROS_SWIFT_STDLIB_MODULE_DIRECTORY ${module_directory} PARENT_SCOPE)
set(FEATHEROS_SWIFT_STDLIB_MODULE ${module} PARENT_SCOPE)
set(FEATHEROS_SWIFT_AVAILABILITY ${availability} PARENT_SCOPE)
