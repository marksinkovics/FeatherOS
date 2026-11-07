# The Cxx module, Swift's overlay for C++ types (std::optional's value, std::vector and other
# containers as Swift collections, std::span from buffers), which upstream does not build for
# Embedded Swift. It is built from the swift.org sources in deps/swift, patched by
# ../swift_concurrency/swift.patch, with the runtime's availability macros, as a module only:
# Embedded Swift compiles the parts a program uses into the program. It needs no C++ standard
# library itself.

set(cxx_source ${FEATHEROS_SWIFT_SOURCE}/stdlib/public/Cxx)
set(module_directory ${CMAKE_CURRENT_BINARY_DIR}/module)
set(module ${module_directory}/Cxx.swiftmodule)

set(swift_sources)
foreach (name CxxConvertibleToBool CxxConvertibleToCollection CxxDictionary CxxPair CxxOptional
        CxxSet CxxRandomAccessCollection CxxSequence CxxBorrowingSequence CxxVector CxxSpan
        UnsafeCxxIterators)
    list(APPEND swift_sources ${cxx_source}/${name}.swift)
endforeach ()

set(features BuiltinModule AllowUnsafeAttribute Lifetimes LifetimeDependence NonescapableTypes
    SuppressedAssociatedTypesWithDefaults)
list(TRANSFORM features PREPEND "-enable-experimental-feature;")
add_custom_command(
    OUTPUT ${module}
    COMMAND ${FEATHEROS_SWIFTC} -target ${FEATHEROS_SWIFT_TARGET}
        -enable-experimental-feature Embedded -parse-as-library -wmo -O -module-name Cxx
        -cxx-interoperability-mode=default -Xcc -nostdinc++ ${features} -strict-memory-safety
        ${FEATHEROS_SWIFT_AVAILABILITY} -I ${FEATHEROS_SWIFT_STDLIB_MODULE_DIRECTORY}
        -module-cache-path ${CMAKE_CURRENT_BINARY_DIR}/module-cache
        -emit-module -emit-module-path ${module} ${swift_sources}
    DEPENDS ${swift_sources} ${FEATHEROS_SWIFT_STDLIB_MODULE}
    COMMENT "Building Swift module Cxx"
    VERBATIM
)
add_custom_target(featheros_swift_cxx_module DEPENDS ${module})

set(FEATHEROS_SWIFT_CXX_MODULE_DIRECTORY ${module_directory} PARENT_SCOPE)
set(FEATHEROS_SWIFT_CXX_MODULE ${module} PARENT_SCOPE)
