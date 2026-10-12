# FeatherOS Roadmap

## Goal

Grow FeatherOS from its current Limine-booted x86-64 framebuffer kernel into a small operating system that supports both x86-64 and AArch64. The kernel should share architecture-neutral services where practical while keeping CPU entry, exceptions, interrupts, page tables, context switching, and userspace ABI code in separate architecture backends.

## Current Foundation

- Limine boot protocol and x86-64/AArch64 kernel entry
- Freestanding C and C++ builds for x86-64 and AArch64
- Framebuffer drawing, text terminal, and screen logger
- COM1/PL011 serial logging, panic/assert helpers, and QEMU smoke tests for both architectures

## Milestones

### 1. Multi-Architecture Build And Debugging (Complete)

- Keep the existing x86-64 build and add an AArch64 cross-compile target with its own toolchain, entry code, and linker script.
- Select architecture-specific assembly and linker inputs in CMake instead of requiring NASM for every target.
- Keep common kernel services separate from `arch/x86_64` and `arch/aarch64` implementations.
- Add architecture-specific serial backends: COM1 for x86-64 and the platform UART (PL011 on QEMU `virt`) for AArch64.
- Make the build script create the appropriate boot image and launch QEMU for each architecture; use the x86 BIOS/UEFI path and AArch64 UEFI path supported by Limine.
- Keep panic/assert output allocation-free and available before framebuffer setup.
- Add a headless serial smoke test for each architecture.

**Complete:** clean builds produce bootable images for both targets; both boot in QEMU and report readiness over serial; deliberate panic tests report on both targets.

### 2. CPU Exceptions And Interrupts (Complete)

- Define a shared exception-reporting interface and architecture-specific saved-context structures.
- On x86-64, establish the GDT, 64-bit TSS, kernel/IST stacks, 16-byte IDT gates, and assembly exception stubs.
- On AArch64, verify the Limine entry exception level, install the exception vector table through `VBAR_EL1`, and capture the exception syndrome and fault address from `ESR_EL1` and `FAR_EL1`.
- Add fatal exception reporting before enabling hardware interrupts.
- Configure the x86 interrupt controller and the AArch64 Generic Interrupt Controller (GIC) in their respective backends.

**Complete:** x86-64 and AArch64 use architecture-specific saved contexts with a shared fatal exception-reporting interface. QEMU verifies x86 divide errors, AArch64 undefined instructions, and registered/acknowledged x86 PIC/PIT IRQ0 and AArch64 GICv2 SGI0 interrupts.

### 3. Physical And Virtual Memory

- Parse Limine's memory map and reserve memory used by the kernel, bootloader, modules, and framebuffer.
- Implement an architecture-neutral physical page-frame allocator with allocation/free tests and accounting.
- Define common virtual-memory operations and permission flags, backed by x86-64 four-level paging and AArch64 translation tables.
- Implement architecture-specific TLB invalidation and memory attributes; begin with 4 KiB pages on both targets.
- Add a kernel heap on top of page allocation only after page allocation and mapping are reliable.
- Track physical addresses separately from virtual addresses using 64-bit types.

**Complete when:** both backends allocate/free page frames, create/remove mappings, enforce permissions, and report expected page/translation faults for unmapped and protected addresses.

### 4. Timers And Input

- Provide a shared timer/tick interface with an x86-64 timer backend and the AArch64 Generic Timer backend.
- Add keyboard input through devices available on each QEMU machine, such as PS/2 on x86 and virtio-input on AArch64 `virt`.
- Buffer input in a shared event queue; keep interrupt handlers short and consume events in normal kernel context.
- Add terminal input and basic line editing.

**Complete when:** both targets produce monotonic timer ticks and deliver keyboard events without polling or lost input during normal testing.

### 5. Storage, VFS, And Filesystem

- Start with a Limine-loaded initramfs so program loading does not depend on a disk driver.
- Define shared block-device and VFS interfaces; use a QEMU-supported virtio block device to reduce duplicated driver work across architectures.
- Add partition discovery and a read-only FAT filesystem as the first disk-backed filesystem.
- Support open, read, seek, stat, and close through the common VFS interface.

**Complete when:** both targets can read known files through the same VFS API from the initramfs and a disk-backed filesystem.

### 6. ELF64 And User Mode

- Implement ELF64 validation and segment loading for both `EM_X86_64` and `EM_AARCH64` binaries.
- Define isolated user address spaces and a shared virtual address-space policy, with architecture-specific page mappings.
- Enter x86-64 ring 3 using the TSS kernel stack; enter AArch64 EL0 using its exception-level and stack conventions.
- Define one architecture-neutral syscall API with x86-64 and AArch64 entry/return mechanisms.
- Build separate minimal userspace runtimes and test programs for the AMD64 and AArch64 ABIs.

**Complete when:** each target runs its matching ELF64 test program in user mode, performs a console syscall, exits cleanly, and cannot access protected kernel memory.

### 7. Processes, Scheduling, And Shell

- Define shared process/thread lifecycle and scheduler policy, with architecture-specific context frames and switch code.
- Begin with cooperative task switching on both targets, then add timer-driven preemption after exception and context-switch paths are tested.
- Add wait/exit behavior, process arguments, and foreground input.
- Build a shared shell and compile it for each supported userspace ABI.

**Complete when:** each kernel can launch multiple matching user programs, switch tasks without corrupting state, and run the shell from the initramfs or VFS.

## Future Milestones (Provisional Order)

The milestones below capture possible work after M7. Their order is intentionally provisional and can change as implementation experience clarifies dependencies and priorities.

### 8. SMP And Concurrency

- Bring up multiple processors on x86-64 and AArch64, with architecture-specific startup and per-CPU state.
- Add synchronization primitives and inter-processor interrupts (IPIs) for shared kernel state and cross-CPU coordination.
- Extend scheduling to run work across CPUs while preserving the shared process and thread interfaces.
- Exercise concurrency and scheduling under multi-CPU QEMU configurations.

**Complete when:** both targets boot with multiple CPUs, schedule work on more than one CPU, and pass synchronization and cross-CPU coordination tests.

### 9. Persistent Storage

- Extend block-device and VFS support to allow writes as well as reads.
- Add a small writable filesystem and define clear behavior for errors and filesystem inconsistencies.
- Test persistence across kernel and QEMU restarts.

**Complete when:** both targets can create, write, read, and delete files on disk, with data surviving a reboot.

### 10. Networking

- Add a QEMU virtio network driver with architecture-appropriate device discovery and DMA handling.
- Build the network stack in stages: Ethernet, IP, ICMP, UDP, then TCP.
- Add basic socket interfaces and simple diagnostic and client/server programs.

**Complete when:** both targets can exchange packets in QEMU, pass basic ICMP/UDP/TCP tests, and run a small network client and server.

### 11. System Services And Compatibility

- Add an init process and define service startup and shutdown behavior.
- Expand the userspace runtime and libc around a documented, deliberately scoped POSIX subset.
- Add useful shell utilities and a way to package and launch programs.

**Complete when:** init starts and supervises basic services, and users can run a documented set of utilities and programs through the shell on both targets.

### 12. Security And Hardening

- Audit syscall argument validation, ELF loading, address-space isolation, and resource cleanup.
- Enforce writable/executable memory policy and tighten privilege transitions.
- Add negative tests for invalid syscalls, malformed binaries, and attempts to access protected memory.

**Complete when:** security invariants are documented and automated tests demonstrate that malformed inputs and user programs cannot access or corrupt protected kernel state.

## Optional Tracks

These are separately tracked directions and are not prerequisites for the core milestones above:

- **Graphical desktop:** build windowing and user-facing graphics on top of the framebuffer and input systems.
- **Broader hardware support:** add devices and platforms beyond the initial QEMU targets, prioritizing reusable driver interfaces.
- **Installation and updates:** produce an installable image and define a reliable system update and recovery workflow.

## Architecture Boundaries

- Keep framebuffer terminal, logging API, panic/assert API, VFS, syscall definitions, and scheduler policy architecture-neutral when possible.
- Keep port I/O, UART drivers, exception entry, interrupt controllers, page-table manipulation, context switching, and syscall entry under the matching `arch` backend.
- Do not share x86 GDT/TSS/IDT structures with AArch64; AArch64 uses exception levels, vector tables, and the GIC instead.
- Avoid fixed physical addresses. Obtain usable memory from Limine's memory map and use explicit 64-bit address types.
- Keep early logging independent of dynamic allocation so it remains usable during allocator failures.
