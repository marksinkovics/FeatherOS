set(KERNEL_BINARY "kernel.elf")
set(SRCROOT ${PROJECT_SOURCE_DIR}/src/kernel)

set(SRC
    ${SRCROOT}/framebuffer/framebuffer.cpp
    ${SRCROOT}/kernel.cpp
)

target_sources(${KERNEL_BINARY} PRIVATE ${SRC})