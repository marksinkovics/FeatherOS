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
        ${SRCROOT}/arch/x86_64/msi.cpp
        ${SRCROOT}/arch/x86_64/cpu_features.cpp
        ${SRCROOT}/arch/x86_64/floating_point.cpp
        ${SRCROOT}/arch/x86_64/thread_pointer.cpp
        ${SRCROOT}/arch/x86_64/acpi.cpp
        ${SRCROOT}/arch/x86_64/i8042.cpp
        ${SRCROOT}/arch/x86_64/keyboard.cpp
        ${SRCROOT}/arch/x86_64/block_device.cpp
        ${SRCROOT}/arch/x86_64/pci.cpp
        ${SRCROOT}/arch/x86_64/port_io.cpp
        ${SRCROOT}/arch/x86_64/virtio_net.cpp
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
        ${SRCROOT}/arch/aarch64/thread_pointer.cpp
        ${SRCROOT}/arch/aarch64/keyboard.cpp
        ${SRCROOT}/arch/aarch64/block_device.cpp
        ${SRCROOT}/arch/aarch64/virtio_mmio.cpp
        ${SRCROOT}/arch/aarch64/virtio_net.cpp
        ${SRCROOT}/arch/aarch64/gicv2m.cpp
        ${SRCROOT}/arch/aarch64/pci_device_tree.cpp
        ${SRCROOT}/arch/aarch64/pci_ecam.cpp
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
    ${SRCROOT}/log_ring.cpp
    ${SRCROOT}/memory/memory_map.cpp
    ${SRCROOT}/memory/heap.cpp
    ${SRCROOT}/memory/kernel_stack.cpp
    ${SRCROOT}/memory/device_memory.cpp
    ${SRCROOT}/memory/dma.cpp
    ${SRCROOT}/memory/page_allocator.cpp
    ${SRCROOT}/memory/virtual_memory.cpp
    ${SRCROOT}/panic.cpp
    ${SRCROOT}/pci/msi.cpp
    ${SRCROOT}/pci/pci.cpp
    ${SRCROOT}/usb/descriptors.cpp
    ${SRCROOT}/usb/device.cpp
    ${SRCROOT}/usb/driver.cpp
    ${SRCROOT}/usb/hub.cpp
    ${SRCROOT}/usb/hub_state.cpp
    ${SRCROOT}/usb/hid/driver.cpp
    ${SRCROOT}/usb/hid/keyboard.cpp
    ${SRCROOT}/usb/hid/report.cpp
    ${SRCROOT}/usb/manager.cpp
    ${SRCROOT}/usb/storage/bot.cpp
    ${SRCROOT}/usb/storage/drive.cpp
    ${SRCROOT}/usb/storage/driver.cpp
    ${SRCROOT}/storage/kernel_environment.cpp
    ${SRCROOT}/usb/storage/scsi.cpp
    ${SRCROOT}/usb/text.cpp
    ${SRCROOT}/usb/usb.cpp
    ${SRCROOT}/usb/xhci/context.cpp
    ${SRCROOT}/usb/xhci/controller.cpp
    ${SRCROOT}/usb/xhci/probe.cpp
    ${SRCROOT}/usb/xhci/registers.cpp
    ${SRCROOT}/usb/xhci/ring.cpp
    ${SRCROOT}/usb/xhci/td.cpp
    ${SRCROOT}/usb/xhci/transfer.cpp
    ${SRCROOT}/console.cpp
    ${SRCROOT}/writeback.cpp
    ${SRCROOT}/terminal/pseudo_terminal.cpp
    ${SRCROOT}/terminal/terminal.cpp
    ${SRCROOT}/files.cpp
    ${SRCROOT}/futex.cpp
    ${SRCROOT}/pipe.cpp
    ${SRCROOT}/poll.cpp
    ${SRCROOT}/signals.cpp
    ${SRCROOT}/power.cpp
    ${SRCROOT}/process.cpp
    ${SRCROOT}/process_arguments.cpp
    ${SRCROOT}/scheduler.cpp
    ${SRCROOT}/sync/volume_mutex.cpp
    ${SRCROOT}/sync/volume_violation.cpp
    ${SRCROOT}/smp.cpp
    ${SRCROOT}/random.cpp
    ${SRCROOT}/cpu.cpp
    ${SRCROOT}/syscall.cpp
    ${SRCROOT}/string/format.cpp
    ${SRCROOT}/string/memory_functions.cpp
    ${SRCROOT}/thread_local_storage.cpp
    ${SRCROOT}/thread_stacks.cpp
    ${SRCROOT}/../user/calendar.cpp
    ${SRCROOT}/../user/text/font_data.cpp
    ${SRCROOT}/../user/text/utf8.cpp
    ${SRCROOT}/time/timer.cpp
    ${SRCROOT}/time/wall_clock.cpp
    ${SERIAL_SRC}
    ${TIMER_SRC}
    ${VIRTUAL_MEMORY_SRC}
    ${EXCEPTION_SRC}
    ${SCHEDULER_CONTEXT_SRC}
    ${SRCROOT}/framebuffer/console_font.c
    ${SRCROOT}/framebuffer/framebuffer.cpp
    ${SRCROOT}/framebuffer/graphics_mode.cpp
    ${SRCROOT}/framebuffer/term.cpp
    ${SRCROOT}/filesystem/initramfs.cpp
    ${SRCROOT}/filesystem/core_driver.cpp
    ${SRCROOT}/filesystem/core_host.cpp
    ${SRCROOT}/filesystem/path.cpp
    ${SRCROOT}/filesystem/path_walk.cpp
    ${SRCROOT}/storage/cache_sizing.cpp
    ${SRCROOT}/storage/controllers.cpp
    ${SRCROOT}/storage/core_block_device.cpp
    ${SRCROOT}/storage/disk.cpp
    ${SRCROOT}/storage/page_cache.cpp
    ${SRCROOT}/storage/pci_hardware.cpp
    ${SRCROOT}/storage/virtio_disk.cpp
    ${SRCROOT}/storage/write_policy.cpp
    ${SRCROOT}/storage/worker.cpp
    ${SRCROOT}/network/address.cpp
    ${SRCROOT}/network/arp.cpp
    ${SRCROOT}/network/checksum.cpp
    ${SRCROOT}/network/device.cpp
    ${SRCROOT}/network/ethernet.cpp
    ${SRCROOT}/network/icmp.cpp
    ${SRCROOT}/network/icmpv6.cpp
    ${SRCROOT}/network/interface.cpp
    ${SRCROOT}/network/ip.cpp
    ${SRCROOT}/network/ipv4.cpp
    ${SRCROOT}/network/ipv6.cpp
    ${SRCROOT}/network/name_servers.cpp
    ${SRCROOT}/network/neighbor_cache.cpp
    ${SRCROOT}/network/neighbor_discovery.cpp
    ${SRCROOT}/network/settings.cpp
    ${SRCROOT}/network/socket.cpp
    ${SRCROOT}/network/tcp.cpp
    ${SRCROOT}/network/tcp_connection.cpp
    ${SRCROOT}/network/udp.cpp
    ${SRCROOT}/virtio/virtqueue.cpp
    ${SRCROOT}/filesystem/devices.cpp
    ${SRCROOT}/filesystem/procfs.cpp
    ${SRCROOT}/filesystem/tmpfs.cpp
    ${SRCROOT}/filesystem/drivers.cpp
    ${SRCROOT}/filesystem/filesystem.cpp
    ${SRCROOT}/filesystem/mount_table.cpp
    ${SRCROOT}/filesystem/type_registry.cpp
    ${SRCROOT}/filesystem/vfs.cpp
    ${SRCROOT}/elf/elf_loader.cpp
    ${SRCROOT}/input/input.cpp
    ${SRCROOT}/input/keyboard.cpp
    ${SRCROOT}/input/pointer.cpp
    ${SRCROOT}/boot_options.cpp
    ${SRCROOT}/kernel_runtime.cpp
    ${SRCROOT}/kernel.cpp
)

if (FEATHEROS_KERNEL_TESTS)
    list(APPEND SRC ${SRCROOT}/kernel_test.cpp ${SRCROOT}/kernel_test_mode.cpp)
endif ()

target_sources(${KERNEL_BINARY} PRIVATE ${SRC})
target_link_libraries(${KERNEL_BINARY} PRIVATE featheros_cxx_headers featherdisk_core)
