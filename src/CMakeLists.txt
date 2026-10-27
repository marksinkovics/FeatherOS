set(KERNEL_BINARY "kernel.elf")
set(SRCROOT ${PROJECT_SOURCE_DIR}/src)

add_subdirectory(${SRCROOT}/kernel)
add_subdirectory(${SRCROOT}/sample)

# User programs and the C library are position-independent and built without the kernel's flags.
set(FEATHEROS_USER_FLAGS
    -ffreestanding -fno-builtin -fno-exceptions -fno-rtti -fno-stack-protector
    -fpie -nostdlib -nodefaultlibs -Wall -Wextra -Werror
)
if (ARCH STREQUAL "x86_64")
    list(APPEND FEATHEROS_USER_FLAGS -mno-red-zone)
else ()
    list(APPEND FEATHEROS_USER_FLAGS
        -mcpu=generic -march=armv8-a -mno-outline-atomics -mcmodel=small)
endif ()
add_subdirectory(${SRCROOT}/libc)
add_subdirectory(${SRCROOT}/user)
