set(KERNEL_BINARY "kernel.elf")
set(SRCROOT ${PROJECT_SOURCE_DIR}/src/kernel)

if (ARCH STREQUAL "x86_64")
    set(SERIAL_SRC ${SRCROOT}/arch/x86_64/serial.cpp)
elseif (ARCH STREQUAL "aarch64")
    set(SERIAL_SRC ${SRCROOT}/arch/aarch64/serial.cpp)
else ()
    message(FATAL_ERROR "Unsupported kernel ARCH='${ARCH}'. Choose x86_64 or aarch64.")
endif ()

set(SRC
    ${SRCROOT}/klogger.cpp
    ${SRCROOT}/panic.cpp
    ${SERIAL_SRC}
    ${SRCROOT}/framebuffer/font.c
    ${SRCROOT}/framebuffer/framebuffer.cpp
    ${SRCROOT}/framebuffer/term.cpp
    ${SRCROOT}/kernel.cpp
)

target_sources(${KERNEL_BINARY} PRIVATE ${SRC})