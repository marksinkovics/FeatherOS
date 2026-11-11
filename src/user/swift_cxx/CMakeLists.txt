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

# Swift's CxxStdlib overlay, upstream's sources unchanged, over FeatherOS's C++ library: its
# headers are the Clang module std (src/libcxx/swift/module.modulemap), which Swift imports as
# CxxStdlib, and the toolchain supplies the CxxStdlibShim module the overlay imports.
set(cxx_stdlib_source ${cxx_source}/std)
set(cxx_stdlib_sources ${cxx_stdlib_source}/std.swift ${cxx_stdlib_source}/Chrono.swift
    ${cxx_stdlib_source}/String.swift)
set(cxx_stdlib_module ${module_directory}/CxxStdlib.swiftmodule)
set(features AllowUnsafeAttribute Lifetimes)
list(TRANSFORM features PREPEND "-enable-experimental-feature;")
add_custom_command(
    OUTPUT ${cxx_stdlib_module}
    COMMAND ${FEATHEROS_SWIFTC} -target ${FEATHEROS_SWIFT_TARGET}
        -enable-experimental-feature Embedded -parse-as-library -wmo -O -module-name CxxStdlib
        ${FEATHEROS_SWIFT_CXX_INCLUDES} -cxx-interoperability-mode=default
        ${FEATHEROS_SWIFT_CXX_CLANG_FLAGS} -I ${libc_source}/swift -Xcc -I${libc_source}/include
        -Xcc -ffreestanding ${features} -strict-memory-safety ${FEATHEROS_SWIFT_AVAILABILITY}
        -I ${FEATHEROS_SWIFT_STDLIB_MODULE_DIRECTORY} -I ${module_directory}
        -module-cache-path ${CMAKE_CURRENT_BINARY_DIR}/module-cache
        -emit-module -emit-module-path ${cxx_stdlib_module} ${cxx_stdlib_sources}
    DEPENDS ${cxx_stdlib_sources} ${module} ${FEATHEROS_SWIFT_STDLIB_MODULE}
        ${FEATHEROS_CXX_HEADER_FILES}
    COMMENT "Building Swift module CxxStdlib"
    VERBATIM
)
add_custom_target(featheros_swift_cxx_module DEPENDS ${module} ${cxx_stdlib_module})

set(FEATHEROS_SWIFT_CXX_MODULE_DIRECTORY ${module_directory} PARENT_SCOPE)
set(FEATHEROS_SWIFT_CXX_MODULE ${module} ${cxx_stdlib_module} PARENT_SCOPE)
