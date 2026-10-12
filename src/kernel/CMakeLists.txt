set(KERNEL_BINARY "kernel.elf")
set(SRCROOT ${PROJECT_SOURCE_DIR}/src/kernel)

if (ARCH STREQUAL "x86_64")
    set(SERIAL_SRC ${SRCROOT}/arch/x86_64/serial.cpp)
    set(EXCEPTION_SRC
        ${SRCROOT}/arch/x86_64/exceptions.cpp
        ${SRCROOT}/arch/x86_64/exceptions.asm
        ${SRCROOT}/arch/x86_64/interrupts.cpp
    )
elseif (ARCH STREQUAL "aarch64")
    set(SERIAL_SRC ${SRCROOT}/arch/aarch64/serial.cpp)
    set(EXCEPTION_SRC
        ${SRCROOT}/arch/aarch64/exceptions.cpp
        ${SRCROOT}/arch/aarch64/exceptions.S
        ${SRCROOT}/arch/aarch64/interrupts.cpp
    )
else ()
    message(FATAL_ERROR "Unsupported kernel ARCH='${ARCH}'. Choose x86_64 or aarch64.")
endif ()

set(SRC
    ${SRCROOT}/exception.cpp
    ${SRCROOT}/halt.cpp
    ${SRCROOT}/klogger.cpp
    ${SRCROOT}/panic.cpp
    ${SERIAL_SRC}
    ${EXCEPTION_SRC}
    ${SRCROOT}/framebuffer/font.c
    ${SRCROOT}/framebuffer/framebuffer.cpp
    ${SRCROOT}/framebuffer/term.cpp
    ${SRCROOT}/kernel.cpp
)

if (FEATHEROS_KERNEL_TEST)
    list(APPEND SRC ${SRCROOT}/kernel_test.cpp)
endif ()

target_sources(${KERNEL_BINARY} PRIVATE ${SRC})