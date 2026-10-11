set(KERNEL_BINARY "kernel.elf")
set(SRCROOT ${PROJECT_SOURCE_DIR}/src/kernel)

set(SRC
    ${SRCROOT}/klogger.cpp
    ${SRCROOT}/panic.cpp
    ${SRCROOT}/arch/x86_64/serial.cpp
    ${SRCROOT}/framebuffer/font.c
    ${SRCROOT}/framebuffer/framebuffer.cpp
    ${SRCROOT}/framebuffer/term.cpp
    ${SRCROOT}/kernel.cpp
)

target_sources(${KERNEL_BINARY} PRIVATE ${SRC})