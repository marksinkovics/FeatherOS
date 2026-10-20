# FeatherOS Roadmap

## Goal

Grow FeatherOS from a Limine-booted kernel into a small operating system that supports both x86-64 and AArch64. The kernel should share architecture-neutral services where practical while keeping CPU entry, exceptions, interrupts, page tables, context switching, and userspace ABI code in separate architecture backends.

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

### 3. Physical And Virtual Memory (Complete)

- Parse Limine's memory map and reserve memory used by the kernel, bootloader, modules, and framebuffer.
- Implement an architecture-neutral physical page-frame allocator with allocation/free tests and accounting.
- Define common virtual-memory operations and permission flags, backed by x86-64 four-level paging and AArch64 translation tables.
- Implement architecture-specific TLB invalidation and memory attributes; begin with 4 KiB pages on both targets.
- Add a kernel heap on top of page allocation only after page allocation and mapping are reliable.
- Track physical addresses separately from virtual addresses using 64-bit types.

**Complete:** Both targets validate the Limine memory map, report architecture and memory-map coverage/usable RAM at startup, allocate and free physical frames with accounting, and create/remove 4 KiB mappings with permission checks and TLB invalidation. Dedicated QEMU tests verify adjacent mappings, read-only and NX protections, unmapped faults, and stale-translation invalidation. A page-backed, coalescing heap provides `malloc`, `calloc`, `free`, and C++ `new`/`delete`, with alignment, zeroing, overflow, and multi-page allocation tests.

### 4. Timers And Input (Complete)

- Provide a shared timer/tick interface with an x86-64 timer backend and the AArch64 Generic Timer backend.
- Add keyboard input through devices available on each QEMU machine, such as PS/2 on x86 and virtio-input on AArch64 `virt`.
- Buffer input in a shared event queue; keep interrupt handlers short and consume events in normal kernel context.
- Add terminal input and basic line editing.

**Complete:** Both targets provide monotonic timer ticks through x86 PIT IRQ0 and the AArch64 Generic Timer PPI 30. Keyboard events reach a shared input queue through x86 PS/2 IRQ1 and a DTB-discovered AArch64 virtio-mmio keyboard/GIC interrupt. The terminal supports printable ASCII, Backspace, and Enter. QEMU tests verify timer behavior and inject keyboard input on both targets; UART input remains available as a fallback.

### 5. Storage, VFS, And Filesystem (Complete)

- Start with a Limine-loaded initramfs so program loading does not depend on a disk driver.
- Define shared block-device and VFS interfaces; use a QEMU-supported virtio block device to reduce duplicated driver work across architectures.
- Add partition discovery and a read-only FAT filesystem as the first disk-backed filesystem.
- Support open, read, seek, stat, and close through the common VFS interface.

**Complete:** Both Limine boot paths load a read-only USTAR initramfs. The shared VFS provides open, read, seek, stat, and close over initramfs files and a read-only FAT16 volume discovered in a primary MBR partition. The shared 512-byte block API uses x86-64 virtio-PCI and AArch64 virtio-MMIO backends. QEMU tests verify known-file reads from both sources on both targets.

### 6. ELF64 And User Mode (Complete)

- Implement ELF64 validation and segment loading for both `EM_X86_64` and `EM_AARCH64` binaries.
- Define isolated user address spaces and a shared virtual address-space policy, with architecture-specific page mappings.
- Enter x86-64 ring 3 using the TSS kernel stack; enter AArch64 EL0 using its exception-level and stack conventions.
- Define one architecture-neutral syscall API with x86-64 and AArch64 entry/return mechanisms.
- Build separate minimal userspace runtimes and test programs for the AMD64 and AArch64 ABIs.

**Complete:** The shared ELF64 parser validates x86-64 and AArch64 executable headers, program-table bounds, load-segment ranges/alignment, and executable entry points. ISO generation cross-compiles target-matched freestanding C++ test programs with RX/RO/RW ELF segments; the kernel loads them into separate address spaces, zeros BSS, and applies final permissions. Both programs use a shared syscall wrapper and dispatcher, enter ring 3/EL0, write a character, exit, and verify kernel-memory isolation. Host tests cover syscall request validation, and QEMU verifies these behaviors on both architectures.

### 7. Processes, Scheduling, And Shell (Complete)

- Define shared process/thread lifecycle and scheduler policy, with architecture-specific context frames and switch code.
- Begin with cooperative task switching on both targets, then add timer-driven preemption after exception and context-switch paths are tested.
- Add wait/exit behavior, process arguments, and foreground input.
- Build a shared shell and compile it for each supported userspace ABI.

**Complete:** A fixed-capacity scheduler switches up to four tasks on both architectures; tasks can be preempted by periodic timer IRQs, return exit statuses, be waited on, and be reaped, and context switches preserve per-task user state (the AArch64 `SP_EL0` and the x86-64 TSS kernel stack). A shared process table spawns initramfs programs into separate address spaces with `argc`/`argv`-style arguments, tracks parents, and gives console input to the foreground process. Spawn, wait, read-line, and newline-capable output syscalls validate every user pointer through the caller's page tables, and a CPU exception in user mode terminates only that program. The kernel starts a shell from the initramfs that runs `calc`, `echo`, and `upper` and reports their exit statuses; host tests cover syscall dispatch and argument parsing, and QEMU shell sessions verify arguments, foreground input in a child process, and exit statuses on both targets.

### 8. SMP And Concurrency (Complete)

- Bring up multiple processors on x86-64 and AArch64, with architecture-specific startup and per-CPU state.
- Add synchronization primitives and inter-processor interrupts (IPIs) for shared kernel state and cross-CPU coordination.
- Extend scheduling to run work across CPUs while preserving the shared process and thread interfaces.
- Exercise concurrency and scheduling under multi-CPU QEMU configurations.
- Make the scheduler, process table, and page allocator SMP-safe with per-CPU run queues and locking.

**Single-CPU groundwork (complete):** An interrupt-saving `Sync::SpinLock` protects the scheduler, input queue, page allocator, heap, and process table, and the scheduler lock is handed across context switches. The boot thread is a schedulable task, an idle task halts the CPU when nothing is ready, and tasks block on wait queues (with optional deadlines) or sleep until a tick instead of busy-yielding; `Wait` and console `ReadLine` use them. QEMU scheduler tests verify blocking and wakeup, timed sleep, and wait deadlines on both targets.

**CPU bring-up (complete):** Both targets start every Limine-reported CPU (up to eight), give each its own exception state (x86-64 GDT/TSS/IST stacks, AArch64 vector base), and park it. QEMU boots with four CPUs, and the normal-boot test requires all of them to come online. Each CPU has a register-based CPU index (x86-64 `GS` base with `SWAPGS` on ring transitions, AArch64 `TPIDR_EL1`), per-CPU current and idle tasks, active page-table roots, and TSS, and serial output is locked. Every CPU takes IPIs and runs its own timer (AArch64 GICv2 SGIs and Generic Timer; x86-64 local APIC with a PIT-calibrated timer, keeping the legacy PIC for device interrupts), verified at boot on both targets.

**Complete:** Every CPU schedules from a shared run queue under the scheduler lock, with per-CPU idle tasks and IPI wakeups of idle CPUs, so tasks and user programs run on any CPU. The framebuffer console and page-table edits are locked, and AArch64 kernel code runs on `SP_EL1` so exception frames stay on each task's stack. QEMU tests verify that tasks run concurrently on three CPUs with an exact shared locked counter, and every shell and user-mode test runs with four CPUs. Follow-on work: per-CPU run queues and load balancing, TLB shootdown once kernel pages are unmapped or address spaces are shared, I/O APIC routing for x86-64 device interrupts, and a lock for `KLogger`.

### 9. Persistent Storage

- Extend block-device and VFS support to allow writes as well as reads.
- Add a small writable filesystem and define clear behavior for errors and filesystem inconsistencies.
- Test persistence across kernel and QEMU restarts.
- Extend the FAT driver with GPT partition tables, subdirectories, and long filenames.
- Add storage drivers for physical hardware beyond the QEMU virtio devices.
- Expose files to user programs through file-descriptor syscalls (open, read, write, close) with a per-process open-file limit (moved from M12).

**Complete when:** both targets can create, write, read, and delete files on disk, with data surviving a reboot.

### 10. Networking

- Add the first network driver for QEMU virtio-net, with architecture-appropriate device discovery and DMA handling.
- Build the network stack in stages: Ethernet, IP, ICMP, UDP, then TCP.
- Add basic socket interfaces and simple diagnostic and client/server programs.

**Complete when:** both targets can exchange packets in QEMU, pass basic ICMP/UDP/TCP tests, and run a small network client and server.

### 11. System Services And Compatibility

- Add an init process and define service startup and shutdown behavior.
- Expand the userspace runtime and libc around a documented, deliberately scoped POSIX subset.
- Add useful shell utilities and a way to package and launch programs.
- Broaden syscall coverage and the userspace library, and add pipes, background jobs, and process signals.
- Improve the shell with line editing beyond Backspace, command history, a documented command set, and a more capable `calc`.
- Add kill/abort for a stuck child and reclaim a child's resources when its parent exits first.
- Add user threads with thread-local storage (`PT_TLS`, moved from M13).
- If a `fork`-style API is adopted, build it on copy-on-write and demand paging (moved from M13); processes are currently created with `Spawn`.

**Complete when:** init starts and supervises basic services, and users can run a documented set of utilities and programs through the shell on both targets.

### 12. Security And Hardening (Complete)

**Part 1 (complete):** The security invariants are documented in `docs/development.md`. The loader checks that segments lie in user space, AArch64 user pages are privileged-execute-never, and each process may have at most 16 live children. The shell test's `hardening` program runs 44 negative checks: bad syscall pointers and arguments, a kernel-half ELF, heap execution, code writes, kernel reads, an invalid syscall, and the child limit.

**Part 2 (complete):** AArch64 device registers (UART, RTC, GIC, virtio-mmio) are mapped in the kernel half through `Memory::MapDevice`, as is the x86-64 local APIC, and AArch64 user page tables no longer copy the kernel's low `TTBR0` mappings. The memory test maps a user page at the UART's identity address to prove the slot is free.

**Part 3 (complete):** Spawned processes get a randomized stack, arguments page, and heap start, using a cycle-counter-seeded generator. The shell test runs `aslr` twice and requires different addresses. AArch64 user space now ends at `2^47` to match its canonical-address check.

**Part 4 (complete):** Spawned programs load as static PIEs at a random image base (see M13). The kernel enables x86-64 SMEP/SMAP and AArch64 PAN when the CPU has them; the loader's copies into user memory open a short user-access window, and interrupt entry closes it again. `RDRAND`/`RNDR` are mixed into the random source when present. Matrix cases run the shell session on QEMU's `max` CPU and require these protections. Moved: the open-file limit to M9, with file syscalls.

- Audit syscall argument validation, ELF loading, address-space isolation, and resource cleanup.
- Enforce writable/executable memory policy and tighten privilege transitions.
- Add negative tests for invalid syscalls, malformed binaries, and attempts to access protected memory.
- Remove kernel low-address mappings (such as the UART identity map) from AArch64 user roots by moving them to the higher half.
- Add user stack guard pages, per-process resource limits (pages, tasks, open files), and address-space layout randomization. Randomizing the program image depends on PIE support in M13.

**Complete when:** security invariants are documented and automated tests demonstrate that malformed inputs and user programs cannot access or corrupt protected kernel state.

### 13. Memory Management And Process Scalability (Complete)

- Replace fixed limits with dynamic ones: a growable task table, heap-allocated kernel stacks, and a per-process page limit instead of the fixed ELF page cap.
- Give user programs a growable stack, a heap (`brk` or `mmap`-style syscall), and optional demand paging with copy-on-write.
- Extend the ELF loader beyond static executables: PIE/relocations (which also enables image ASLR for M12), `PT_TLS`, and a documented policy for dynamic linking.
- Add scheduler policy beyond round-robin, such as priorities; blocked/sleeping states, timed sleep, and an idle task already exist.

**Part 1 (complete):** Tasks and process records are heap-allocated. Kernel stacks come from a guard-paged pool, and a per-process page limit replaces the fixed ELF page cap. QEMU tests run 12 concurrent tasks for three rounds without leaking frames, check that a guard page faults, and have the shell's `fanout 6` run six children at once.

**Part 2 (complete):** User stacks grow on demand through a 64-page region whose lowest page is a guard, and the `ExtendHeap` syscall grows a program break above the ELF image. The shell test's `memtest` checks deep recursion and heap reads and writes, and `memtest overflow` is terminated at the guard page.

**Part 3 (complete):** Tasks have Low, Normal, or High priority, and the scheduler runs the highest-priority ready task, round-robin within a level. The `Sleep` syscall blocks a program without using a CPU. A scheduler test checks priority order, and the shell test runs `sleep 1100`.

**Part 4 (complete):** User programs are built as static PIEs. The ELF loader accepts `ET_DYN`, applies `R_X86_64_RELATIVE`/`R_AARCH64_RELATIVE` relocations before setting final permissions, and rejects symbol, PLT, and text relocations; unit tests cover each case. `memtest exhaust` checks that hitting the page limit fails cleanly and that pages are reclaimed. The dynamic-linking policy is documented in `docs/development.md`. Moved: `PT_TLS` and copy-on-write to M11.

**Complete when:** more than four processes can run with independently growing memory on both targets, sleeping tasks do not consume CPU, and QEMU tests cover allocation failure and resource reclamation under load.

### 14. USB Host And HID Input

- Add xHCI host-controller support and enumerate USB devices on both architectures.
- Implement USB HID keyboard reports and route key events through the shared input queue from M4.
- Add QEMU xHCI keyboard devices to both machine configurations for repeatable tests.

**Complete when:** both targets enumerate a QEMU USB keyboard and deliver key events through the shared input path.

## Optional Tracks

These are separately tracked directions and are not prerequisites for the core milestones above:

- **Graphical desktop:** build windowing and user-facing graphics on top of the framebuffer and input systems.
- **Broader hardware support:** add devices and platforms beyond the initial QEMU targets, including network adapters beyond QEMU virtio-net, prioritizing reusable driver interfaces.
- **Installation and updates:** produce an installable image and define a reliable system update and recovery workflow.

## Architecture Boundaries

- Keep framebuffer terminal, logging API, panic/assert API, VFS, syscall definitions, and scheduler policy architecture-neutral when possible.
- Keep port I/O, UART drivers, exception entry, interrupt controllers, page-table manipulation, context switching, and syscall entry under the matching `arch` backend.
- Do not share x86 GDT/TSS/IDT structures with AArch64; AArch64 uses exception levels, vector tables, and the GIC instead.
- Avoid fixed physical addresses. Obtain usable memory from Limine's memory map and use explicit 64-bit address types.
- Keep early logging independent of dynamic allocation so it remains usable during allocator failures.
