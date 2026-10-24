set(KERNEL_BINARY "kernel.elf")
set(SRCROOT ${PROJECT_SOURCE_DIR}/src/kernel)

if (ARCH STREQUAL "x86_64")
    set(SERIAL_SRC ${SRCROOT}/arch/x86_64/serial.cpp)
    set(TIMER_SRC ${SRCROOT}/arch/x86_64/timer.cpp)
    set(VIRTUAL_MEMORY_SRC ${SRCROOT}/arch/x86_64/virtual_memory.cpp)
    set(EXCEPTION_SRC
        ${SRCROOT}/arch/x86_64/exceptions.cpp
        ${SRCROOT}/arch/x86_64/exceptions.asm
        ${SRCROOT}/arch/x86_64/interrupts.cpp
        ${SRCROOT}/arch/x86_64/local_apic.cpp
        ${SRCROOT}/arch/x86_64/cpu_features.cpp
        ${SRCROOT}/arch/x86_64/floating_point.cpp
        ${SRCROOT}/arch/x86_64/keyboard.cpp
        ${SRCROOT}/arch/x86_64/block_device.cpp
        ${SRCROOT}/arch/x86_64/rtc.cpp
        ${SRCROOT}/arch/x86_64/power.cpp
    )
    set(SCHEDULER_CONTEXT_SRC ${SRCROOT}/arch/x86_64/context_switch.asm)
elseif (ARCH STREQUAL "aarch64")
    set(SERIAL_SRC ${SRCROOT}/arch/aarch64/serial.cpp)
    set(TIMER_SRC ${SRCROOT}/arch/aarch64/timer.cpp)
    set(VIRTUAL_MEMORY_SRC ${SRCROOT}/arch/aarch64/virtual_memory.cpp)
    set(EXCEPTION_SRC
        ${SRCROOT}/arch/aarch64/exceptions.cpp
        ${SRCROOT}/arch/aarch64/exceptions.S
        ${SRCROOT}/arch/aarch64/interrupts.cpp
        ${SRCROOT}/arch/aarch64/cpu_features.cpp
        ${SRCROOT}/arch/aarch64/floating_point.cpp
        ${SRCROOT}/arch/aarch64/keyboard.cpp
        ${SRCROOT}/arch/aarch64/block_device.cpp
        ${SRCROOT}/arch/aarch64/rtc.cpp
        ${SRCROOT}/arch/aarch64/power.cpp
    )
    set(SCHEDULER_CONTEXT_SRC ${SRCROOT}/arch/aarch64/context_switch.S)
else ()
    message(FATAL_ERROR "Unsupported kernel ARCH='${ARCH}'. Choose x86_64 or aarch64.")
endif ()

set(SRC
    ${SRCROOT}/exception.cpp
    ${SRCROOT}/halt.cpp
    ${SRCROOT}/klogger.cpp
    ${SRCROOT}/memory/memory_map.cpp
    ${SRCROOT}/memory/heap.cpp
    ${SRCROOT}/memory/kernel_stack.cpp
    ${SRCROOT}/memory/device_memory.cpp
    ${SRCROOT}/memory/page_allocator.cpp
    ${SRCROOT}/memory/virtual_memory.cpp
    ${SRCROOT}/panic.cpp
    ${SRCROOT}/console.cpp
    ${SRCROOT}/files.cpp
    ${SRCROOT}/pipe.cpp
    ${SRCROOT}/power.cpp
    ${SRCROOT}/process.cpp
    ${SRCROOT}/process_arguments.cpp
    ${SRCROOT}/scheduler.cpp
    ${SRCROOT}/smp.cpp
    ${SRCROOT}/random.cpp
    ${SRCROOT}/cpu.cpp
    ${SRCROOT}/syscall.cpp
    ${SRCROOT}/string/format.cpp
    ${SRCROOT}/time/timer.cpp
    ${SERIAL_SRC}
    ${TIMER_SRC}
    ${VIRTUAL_MEMORY_SRC}
    ${EXCEPTION_SRC}
    ${SCHEDULER_CONTEXT_SRC}
    ${SRCROOT}/framebuffer/font.c
    ${SRCROOT}/framebuffer/framebuffer.cpp
    ${SRCROOT}/framebuffer/term.cpp
    ${SRCROOT}/filesystem/initramfs.cpp
    ${SRCROOT}/filesystem/fat16.cpp
    ${SRCROOT}/filesystem/fat_names.cpp
    ${SRCROOT}/filesystem/path.cpp
    ${SRCROOT}/storage/partition.cpp
    ${SRCROOT}/filesystem/devices.cpp
    ${SRCROOT}/filesystem/procfs.cpp
    ${SRCROOT}/filesystem/tmpfs.cpp
    ${SRCROOT}/filesystem/vfs.cpp
    ${SRCROOT}/elf/elf_loader.cpp
    ${SRCROOT}/input/input.cpp
    ${SRCROOT}/kernel_runtime.cpp
    ${SRCROOT}/kernel.cpp
)

if (FEATHEROS_KERNEL_TEST)
    list(APPEND SRC ${SRCROOT}/kernel_test.cpp)
endif ()

target_sources(${KERNEL_BINARY} PRIVATE ${SRC})