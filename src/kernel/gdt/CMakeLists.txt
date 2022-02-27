set(KERNEL_BINARY "kernel.elf")
set(SRCROOT ${PROJECT_SOURCE_DIR}/src/kernel)

set(SRC
    ${SRCROOT}/kernel.cpp
)

target_sources(${KERNEL_BINARY} PRIVATE ${SRC})