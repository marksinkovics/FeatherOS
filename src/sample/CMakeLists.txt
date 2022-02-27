set(KERNEL_BINARY "kernel.elf")
set(SRCROOT ${PROJECT_SOURCE_DIR}/src/sample)

set(SRC
    ${SRCROOT}/sample.h
    ${SRCROOT}/sample.cpp
)

target_sources(${KERNEL_BINARY} PRIVATE ${SRC})