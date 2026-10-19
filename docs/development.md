# Develop on Linux

## Code Quality Tools

The repository uses clang-format and clang-tidy for C/C++, Ruff for Python, and ShellCheck and shfmt for Bash.

On macOS with Homebrew:

```sh
brew install llvm ruff shellcheck shfmt pre-commit
brew install ninja
export PATH="$(brew --prefix llvm)/bin:$PATH"
```

On Ubuntu:

```sh
sudo apt-get install clang-format clang-tidy shellcheck shfmt pre-commit pipx ninja-build
pipx install ruff
```

Run `bash tools/quality.sh --fix` to apply available formatting and lint fixes, then install the pre-commit hook with `pre-commit install` once per clone. The hook quietly runs the quality checks before each commit and prints diagnostics only on failure. Build at least one architecture first so clang-tidy can use `build/<arch>/compile_commands.json`; issues without automatic fixes are reported for manual repair.

## Prerequisite for the GCC + Binutils

- sudo apt-get install libgmp3-dev
- sudo apt-get install libmpc-dev

## Prerequisite for Running

On macOS:

```sh
brew install xorriso qemu mtools
```

Install Ninja as well; kernel and unit-test CMake builds use the Ninja generator. GNU Make is still used to build Limine.

- sudo apt-get install -y xorriso
- sudo apt-get install -y qemu-system-x86-64
- sudo apt-get install -y qemu-system-arm
- sudo apt-get install -y mtools
- sudo apt-get install -y python3

## Architecture Builds

An architecture must be specified for `build`, `iso`, `run`, `test`, `debug`, and `all`; there is no implicit architecture default. Select it with `--arch <x86_64|aarch64>` or the `ARCH` environment variable. The command-line option takes precedence if both are supplied.

```sh
./build.sh --arch x86_64 test
ARCH=x86_64 ./build.sh test
./build.sh test-all
```

## Smoke Test

Run `./build.sh --arch x86_64 test` to configure and build the kernel, create the bootable ISO, and boot it in QEMU without a display. The test passes when the kernel emits its ready message over serial without reporting a panic. Build artifacts and compile databases are isolated under `build/x86_64` and `build/aarch64`.

The integration tests use Python's built-in `unittest` framework and keep QEMU as the execution backend. `./build.sh test-all` runs 24 cases across x86-64 and AArch64, covering panic and exception reporting, normal startup, interrupts, memory, timers, cooperative scheduling, user-mode preemption, multi-CPU scheduling, UART input, window-keyboard input, and shell sessions that run user programs. Both keyboard cases inject keys through QEMU's QMP interface. Each case builds its kernel and ISO, boots QEMU, and validates serial output. QEMU is stopped as soon as the expected record appears, with a timeout as a fallback. Failures include the captured QEMU output.

## Unit Tests

Run `./build.sh unit-test` for native host tests of USTAR, FAT16, VFS, ELF loading, terminal rendering, syscall dispatch, program-argument parsing, and calendar conversion. CMake fetches the pinned Catch2 `v3.7.1` release on the first run, then CTest runs the cases from `build/unit-tests-ninja`. These tests use an in-memory block device; QEMU remains responsible for exercising real PCI/MMIO and boot integration.

## Initramfs

The ISO build packages `initramfs/hello.txt` as `build/<arch>/initramfs.tar` and adds it as a Limine module on both architectures. The kernel mounts the read-only USTAR archive; the shared VFS supports open, read, seek, stat, and close over it. The build also creates `build/<arch>/virtio-disk.img` with an MBR-partitioned FAT16 volume and a known `BLOCK.TXT`; x86-64 attaches the disk through virtio-PCI and AArch64 through virtio-MMIO. Normal startup reads and verifies the test file through VFS. Run `./build.sh --arch x86_64 test` or `./build.sh --arch aarch64 test` to check the full boot path.

## User Programs And Syscalls

ISO generation cross-compiles each program listed in the user-program loop in `build.sh` (`src/user/<name>.cpp`) with the matching freestanding cross-compiler and architecture-specific user linker script, then packages it in the initramfs as `<name>-<arch>.elf`. x86-64 programs are built with `-mgeneral-regs-only` because context switches do not save SSE state. Each ELF has separate RX, RO, and RW load segments. `src/kernel/process.cpp` loads a program into its own address space, maps a user stack, and enters ring 3 on x86-64 or EL0 on AArch64. A CPU exception in user mode terminates only that program; the kernel reports it with the `ExitStatusFault` or `ExitStatusKernelAccessFault` status from `src/user/syscall_abi.h`.

`src/user/syscall_abi.h` defines the syscall ABI shared by the kernel and programs, and `src/user/syscall.h` wraps it. x86-64 enters through `int 0x80` with the number in `rax` and arguments in `rdi`, `rsi`, and `rdx`; AArch64 uses `svc #0` with the number in `x8` and arguments in `x0`-`x2`. The result returns in `rax` or `x0`, and values at or above `SyscallErrorBase` are errors.

| Number | Syscall | Behavior |
|---|---|---|
| 0 | `Exit(status)` | Ends the program with an exit status |
| 1 | `WriteCharacter(character)` | Writes printable ASCII or `\n` to the console |
| 2 | `ReadLine(buffer, capacity, prompt)` | Shows the prompt and blocks until Enter; only the foreground process may read |
| 3 | `Spawn(command_line)` | Starts the program named by the first word and returns its process ID |
| 4 | `Wait(process_id, status_pointer)` | Blocks until a child exits, stores its status, and releases it |
| 5 | `ReadClock()` | Returns the current UTC time in seconds since 1970-01-01 |

The kernel resolves every user pointer through the caller's page tables before touching it, so unmapped, read-only, or kernel addresses fail with `SyscallErrorInvalidArgument` instead of faulting. Blocking syscalls re-enable interrupts and yield to other tasks while they wait.

At entry, each program receives a pointer to a read-only `FeatherOS::User::ProgramArguments` block containing its process ID, the full command line, and `argument_count`/`argument_values` (the line split at spaces, at most 8 arguments in 63 characters). After a program exits, `Memory::DestroyAddressSpace` frees its user-mapped frames, private page-table frames, and root frame; it refuses to destroy an active address space and preserves shared kernel mappings. QEMU memory tests verify that free-frame accounting returns to its pre-test value on both architectures.

Normal boot first runs `src/user/test_program.cpp` twice as scheduled tasks to verify console output, exit statuses, and that user mode cannot read a mapped kernel address.

## Scheduler And Processes

The scheduler supports up to four tasks with fixed 16 KiB kernel stacks and per-task user exception state; context switches also preserve the AArch64 user stack pointer (`SP_EL0`), and x86-64 restores the TSS kernel stack after blocking syscalls. `Scheduler::Initialize` adopts the boot thread as a task and creates an idle task that halts the CPU (`hlt`/`wfi`) whenever no other task is ready. Tasks yield cooperatively or are preempted by periodic timer interrupts when preemption is enabled; the kernel can wait for exit statuses and reap completed tasks for slot reuse.

Waiting tasks do not run. `Scheduler::Wait` blocks until the task exits, `SleepUntil` sleeps until a timer tick, and `WaitOn`/`WakeAll` block on a `WaitQueue` with an optional deadline tick. `ReadLine` uses this to block until a keyboard or UART event arrives, or until the next second for the uptime display; the timer interrupt moves UART input into the event queue. `Sync::SpinLock` (`src/kernel/sync/spinlock.h`) protects the scheduler, input queue, page allocator, heap, and process table; `LockIrqSave` and `SpinLockGuard` also mask interrupts. The scheduler lock is held across context switches: the switched-in task releases it and then restores its own interrupt state. Code that waits on a condition holds its own lock and passes it to `WaitOn`, which releases it while the task waits; subsystem locks are always taken before the scheduler lock. Page-table updates are not yet locked. The process table in `src/kernel/process.cpp` holds up to three processes, each with a parent and its own address space. Spawning a process makes it the foreground process for console input; waiting for it returns the foreground to its parent. Run `SCHEDULER_TEST=ON ./build.sh --arch x86_64 test` or `SCHEDULER_TEST=ON ./build.sh --arch aarch64 test` for kernel-task lifecycle checks. Run `USER_PREEMPTION_TEST=ON ./build.sh --arch x86_64 test` or `USER_PREEMPTION_TEST=ON ./build.sh --arch aarch64 test` to verify timer preemption while user code is executing. The scheduler test also covers wait-queue blocking and wakeup, timed sleep, and wait deadlines. SMP scheduling is not implemented yet.

## Multiple CPUs

QEMU runs with four CPUs (`-smp 4`) in tests and interactive runs. Once the boot CPU's timer runs, `src/kernel/smp.cpp` uses Limine's MP response to start each secondary CPU (up to `Smp::MaximumCpus`, 8) and logs `SMP: N CPUs online with IPIs and timers`. Each secondary CPU installs its CPU-local state, loads its own exception state (on x86-64 a GDT, TSS, and IST stacks with the shared IDT; on AArch64 `VBAR_EL1`), enables its local interrupt controller and timer, prints `CPU N online`, and waits for interrupts. Each secondary CPU then enters the scheduler on its own idle task. Before logging the summary, the boot CPU sends each CPU an IPI, waits for it to be handled, and checks that each CPU's timer is ticking. The normal-boot smoke test requires every CPU's online line and the summary.

IPIs and per-CPU timers use `Arch::Interrupts::SetIpiHandler`/`SendIpi` and `Time::StartSecondaryCpuTimer`. On AArch64 the kernel runs on `SP_EL1` (`SPSel=1`) on every CPU, so exception frames land on the current task's kernel stack and `SP_EL0` holds only user stack pointers; Limine enters on `SP_EL0`, and exception setup moves the stack. Each CPU enables its banked GICv2 CPU interface, SGIs, and PPIs; IPIs are SGI 1, addressed by the GIC target mask each CPU reads at startup, and each core runs its own Generic Timer. On x86-64, `src/kernel/arch/x86_64/local_apic.cpp` maps the local APIC at `0xFFFFFE0000000000` and enables it on every CPU. IPIs use vector 48 and the APIC timer vector 49; the boot CPU calibrates the APIC timer against 10 PIT ticks. The legacy PIC and PIT still deliver device interrupts and global time to the boot CPU through LINT0 (virtual-wire mode); the I/O APIC is not used yet. Only the boot CPU advances `Time::TickCount()` and polls the UART; every CPU's tick drives the scheduler (sleeper wakeups, preemption, and leaving its idle task).

### Scheduling on every CPU

All CPUs share one run queue under the scheduler lock, so any CPU runs any ready task: user programs, the shell, and the boot thread may move between CPUs at each switch. Making a task ready sends an IPI to an idle CPU, if there is one, so it leaves `hlt`/`wfi` at once instead of at its next tick. The lock is held across each context switch and released by the task that switches in, so no CPU can resume a task before its registers are saved. The framebuffer console takes a display lock, and page-table edits take a page-table lock (heap, then page tables, then the page allocator). TLB shootdown is not needed yet: kernel mappings are only added after the secondary CPUs start, and switching address spaces flushes user translations (x86-64 `CR3` reload, AArch64 `tlbi vmalle1`). Unmapping a live kernel page or sharing an address space between CPUs will need one.

Run `SMP_TEST=ON ./build.sh --arch x86_64 test` or `--arch aarch64` to check that three tasks run at the same time on three CPUs and that a lock-protected counter they share comes out exact. `SCHEDULER_TEST=ON` runs now also check the scheduler success line.

`src/kernel/cpu.cpp` gives each CPU an index that `Cpu::CurrentIndex()` reads from a CPU register: the `GS` base on x86-64 and `TPIDR_EL1` on AArch64. On x86-64 the entry stubs run `SWAPGS` when an interrupt or syscall comes from ring 3, and again before `IRETQ` back to ring 3, so user code never sees the kernel's `GS` base. Per-CPU data lives in arrays indexed by `Cpu::CurrentIndex()`: the scheduler's current and idle tasks, each CPU's active page-table root, and the x86-64 TSS. `Serial::Write` takes a lock so lines from different CPUs do not interleave; panic and fatal-exception reports force it open first.

## Wall Clock

The kernel reads UTC wall-clock time from the platform real-time clock with one-second resolution: the CMOS RTC on x86-64 (`src/kernel/arch/x86_64/rtc.cpp`, BCD or binary and 12- or 24-hour formats, century register `0x32`) and the PL031 at its fixed QEMU `virt` address on AArch64 (`src/kernel/arch/aarch64/rtc.cpp`). QEMU initializes both from the host clock in UTC. Startup logs the time as `Wall clock: YYYY-MM-DD HH:MM:SS UTC`, and programs read it with the `ReadClock` syscall. `src/user/calendar.h` converts between UNIX seconds and UTC dates for both the kernel and user programs; there is no time-zone support.

## Shell And Console Input

After startup, the kernel spawns `src/user/shell.cpp` from the initramfs and restarts it if it exits. The shell reads lines with the `FeatherOS> ` prompt and runs each line's first word as the initramfs program `<name>-<arch>.elf` (names are limited to 32 letters, digits, `_`, or `-`), waits for it, and prints `exit status N` for nonzero statuses. Missing programs report `Unknown command: <name>`; `help` lists the bundled programs:

- `calc <number> <number>` prints the sum of two unsigned numbers.
- `date` prints the current UTC time, such as `2026-10-19 03:07:39 UTC`.
- `echo <words...>` prints its arguments.
- `upper` reads one line as the foreground process and prints it in upper case.

To add a program, create `src/user/<name>.cpp` with a `_start(const ProgramArguments*)` entry point and add `<name>:<name>` to the user-program list in `build.sh`.

Interactive runs accept keys from the QEMU window on both architectures. x86-64 uses PS/2; AArch64 uses virtio-input on QEMU `virt`. The launching terminal's serial console also remains available. The kernel console (`src/kernel/console.cpp`) handles line editing for `ReadLine`: it shows the program's prompt on the framebuffer input row, supports printable ASCII, Backspace, and Enter, and echoes input to serial. Program output appears on both serial and the framebuffer console.

`CALC_TEST=ON` checks a calculator command, an unknown command, and a blank line; `SHELL_TEST=ON` checks `echo` arguments, foreground input in `upper`, a nonzero exit status, and that `date` is within two minutes of the host clock. `INPUT_TEST=ON` runs the keyboard test. Pass each with `./build.sh test --arch x86_64` or `--arch aarch64`.

To run only one architecture's panic check, use `PANIC_TEST=ON`; this intentionally panics during startup, and the smoke test passes only when it observes the expected panic record:

```sh
PANIC_TEST=ON ./build.sh test --arch x86_64
PANIC_TEST=ON QEMU_EFI=/path/to/AAVMF_CODE.fd ./build.sh test --arch aarch64
```

## AArch64 Target

The cross-toolchain builder installs under `~/.local` by default and does not need `sudo`. It adds `~/.local/bin` to `PATH` when building. Build the AArch64 cross-compiler, then select that target for each build action:

```sh
./toolchains/build_toolchain.sh --binutils --gcc --arch aarch64-elf
ARCH=aarch64 ./build.sh build
ARCH=aarch64 ./build.sh iso
QEMU_EFI=/path/to/AAVMF_CODE.fd ARCH=aarch64 ./build.sh test
```

The AArch64 ISO is UEFI-only and uses Limine's `BOOTAA64.EFI`. QEMU runs with the `virt` machine. Interactive runs for both architectures mirror raw serial output to `build/<arch>/serial.log` and print it to the launching terminal with firmware terminal-control sequences removed. Set `QEMU_EFI` to an AArch64 UEFI firmware image if the script cannot find one in a common system location.

On macOS, the toolchain builder automatically selects Apple Clang, the active Xcode SDK, GNU89 host flags, and system zlib for Binutils. Build each stage separately:

```sh
./toolchains/build_toolchain.sh --binutils --arch aarch64-elf
./toolchains/build_toolchain.sh --gcc --arch aarch64-elf
```

The builder accepts `HOST_CC`, `HOST_CFLAGS`, and `SDKROOT` overrides if needed.