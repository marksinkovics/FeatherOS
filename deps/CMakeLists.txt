set(KERNEL_BINARY "kernel.elf")
set(SRCROOT ${PROJECT_SOURCE_DIR}/deps)

set(SRC
    ${SRCROOT}/stivale/stivale2.h
)

target_sources(${KERNEL_BINARY} PRIVATE ${SRC})