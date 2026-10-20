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

The integration tests use Python's built-in `unittest` framework and keep QEMU as the execution backend. `./build.sh test-all` runs 26 cases across x86-64 and AArch64, covering panic and exception reporting, normal startup, interrupts, memory, timers, cooperative scheduling, user-mode preemption, multi-CPU scheduling, shell sessions on QEMU's `max` CPU with hardware protections enabled, UART input, window-keyboard input, and shell sessions that run user programs. Both keyboard cases inject keys through QEMU's QMP interface. Each case builds its kernel and ISO, boots QEMU, and validates serial output. QEMU is stopped as soon as the expected record appears, with a timeout as a fallback. Failures include the captured QEMU output.

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
| 6 | `ExtendHeap(increment)` | Grows the program break by `increment` bytes and returns the previous break; `0` returns the current one |
| 7 | `Sleep(milliseconds)` | Blocks for at least that long (rounded up to timer ticks, at most one day) without using a CPU |
| 8 | `ReadUptime()` | Returns milliseconds since the kernel's timer started |
| 9 | `CurrentCpu()` | Returns the index of the CPU running the caller; it can change at any preemption |

The kernel resolves every user pointer through the caller's page tables before touching it, so unmapped, read-only, or kernel addresses fail with `SyscallErrorInvalidArgument` instead of faulting. Blocking syscalls re-enable interrupts and yield to other tasks while they wait.

Ctrl+C (serial byte `0x03`, or Ctrl+C on the PS/2 or virtio keyboard) interrupts the console's foreground job: the process the shell started, and all of its descendants. The shell is the session leader and is never interrupted; Ctrl+C at its prompt only discards the line. The console echoes `^C` and marks each process in the job, waking any that are sleeping or blocked. A marked program ends with `ExitStatusInterrupted` the next time it would resume in user mode: at its next timer tick, or when its current syscall returns (`ReadLine` returns `SyscallErrorInterrupted` first). The shell prints nothing for that status. Children never outlive their parent: when a process exits, the kernel marks its remaining descendants the same way and reaps its children before the parent's own exit completes.

### Device registers

`Memory::MapDevice(physical, size)` (`src/kernel/memory/device_memory.cpp`) maps device registers uncached into the kernel half at `0xFFFFF00000000000 + physical`, so drivers reach them whichever address space is active. `PageFlags::Device` selects Device-nGnRnE memory on AArch64 and `PCD`/`PWT` on x86-64. The AArch64 UART, PL031 RTC, GICv2, and virtio-mmio devices, and the x86-64 local APIC, all use it. The AArch64 UART uses Limine's identity map only until virtual memory is initialized, so the earliest panics still print.

### Position-independent programs

User programs are built as static position-independent executables (`-fpie -Wl,-pie -Wl,--no-dynamic-linker -Wl,-z,text`), linked at 0 by `src/user/linker_<arch>.ld`. The ELF loader accepts `ET_DYN` images, adds the load bias to every segment, and applies the `RELATIVE` relocations listed under `PT_DYNAMIC` while the segments are still writable, then sets their final permissions. It rejects relocations that need symbols, a PLT, or a write into a non-writable segment. Spawned programs get a random image base; the kernel's own test programs load at `0x400000` (x86-64) or `0x0000400000000000` (AArch64).

**Dynamic-linking policy:** FeatherOS runs only static PIEs. Every program carries its own copy of `src/user/runtime.h`, and the kernel never resolves symbols. If shared libraries are added later, a user-space dynamic linker will load and relocate them, the way `ld.so` does on other systems; the kernel will keep applying only `RELATIVE` relocations, to the dynamic linker itself.

### CPU protections

`src/kernel/arch/<arch>/cpu_features.cpp` enables hardware protections on every CPU when the boot CPU reports them, and startup logs `CPU protections: ...`:

- x86-64: SMEP stops the kernel from executing user pages, and SMAP stops it from reading or writing them. `RDRAND` feeds the random source.
- AArch64: every user page is privileged-execute-never anyway; PAN stops kernel access to user pages, and `RNDR` feeds the random source.

The loader copies into a new program through its user addresses inside an `Arch::CpuFeatures::UserAccessWindow` (`stac`/`clac`, or clearing and setting PAN). Interrupt entry closes any open window: x86-64 runs `clac`, and AArch64 clears `SCTLR_EL1.SPAN` so taking an exception sets PAN. QEMU's default `qemu64` and `cortex-a72` CPUs have none of these features. Set `QEMU_CPU=max` to test with them, for example `QEMU_CPU=max SHELL_TEST=ON ./build.sh --arch x86_64 test`; the smoke test then requires the full protections line.

### Security invariants

- User programs never reach kernel memory: user mappings must lie below `0x0000800000000000` on both targets. AArch64 user page tables contain no kernel mappings at all, because the kernel and its devices live in `TTBR1`, and the ELF loader rejects segments outside that range before it copies anything.
- Every syscall pointer is translated through the caller's page tables before use, so kernel, unmapped, or read-only addresses return `SyscallErrorInvalidArgument`.
- No page is both writable and executable: the ELF loader rejects W+X segments, and stacks, heap, and arguments are non-executable. On AArch64 every user page is also privileged-execute-never, so the kernel cannot run user code.
- A CPU exception in user mode terminates only that program.
- Each process has at most 1024 pages and 16 live children.
- Spawned processes get a randomized layout (`Process::RandomUserLayout`): the stack top lands on a random page in a 1 TiB window (`0x7000…` on x86-64, `0x6000…` on AArch64), the arguments page sits just above it past a guard page, and the heap starts up to 256 MiB above the image. `src/kernel/random.cpp` mixes the CPU cycle counter through splitmix64; it is not a cryptographic generator. Programs the kernel spawns also load at a random image base in a 1 TiB window at `0x1000…`.

User memory grows on demand within the per-process page limit. The stack occupies a 64-page region ending at `Process::UserStackTop`: one page is mapped at load, and a not-present user page fault inside the region maps another zeroed page and resumes the program. The region's lowest page is never mapped, so a stack overflow faults there and terminates only that program. The heap starts at the page after the highest ELF segment; `ExtendHeap` maps whole pages eagerly up to the stack region's guard page.

At entry, each program receives a pointer to a read-only `FeatherOS::User::ProgramArguments` block containing its process ID, the full command line, and `argument_count`/`argument_values` (the line split at spaces, at most 16 arguments in 63 characters). After a program exits, `Memory::DestroyAddressSpace` frees its user-mapped frames, private page-table frames, and root frame; it refuses to destroy an active address space and preserves shared kernel mappings. QEMU memory tests verify that free-frame accounting returns to its pre-test value on both architectures.

Normal boot first runs `src/user/test_program.cpp` twice as scheduled tasks to verify console output, exit statuses, and that user mode cannot read a mapped kernel address.

## Scheduler And Processes

Tasks are heap-allocated, so their number is limited only by memory. Each has a 16 KiB kernel stack from `src/kernel/memory/kernel_stack.cpp`: stacks live at `0xFFFFFC0000000000` with an unmapped guard page below each, so an overflow faults instead of corrupting a neighbour. Reaped stacks return to a pool still mapped, so reuse never unmaps a kernel page and needs no TLB shootdown. On x86-64 every kernel-half PML4 entry is created at boot, so kernel mappings added later appear in every address space. Each task also keeps its own user exception state; context switches also preserve the AArch64 user stack pointer (`SP_EL0`), and x86-64 restores the TSS kernel stack after blocking syscalls. `Scheduler::Initialize` adopts the boot thread as a task and creates an idle task that halts the CPU (`hlt`/`wfi`) whenever no other task is ready. Tasks have a priority (`Low`, `Normal`, or `High`; set with `Scheduler::SetPriority`). Each CPU runs the highest-priority ready task, rotating among tasks of equal priority, and a yielding task keeps running if no ready task outranks it. Lower priorities can starve while higher ones stay ready. Tasks yield cooperatively or are preempted by periodic timer interrupts when preemption is enabled; the kernel can wait for exit statuses and reap completed tasks, which frees them.

Waiting tasks do not run. `Scheduler::Wait` blocks until the task exits, `SleepUntil` sleeps until a timer tick, and `WaitOn`/`WakeAll` block on a `WaitQueue` with an optional deadline tick. `ReadLine` uses this to block until a keyboard or UART event arrives; the timer interrupt moves UART input into the event queue. `Sync::SpinLock` (`src/kernel/sync/spinlock.h`) protects the scheduler, input queue, page allocator, heap, and process table; `LockIrqSave` and `SpinLockGuard` also mask interrupts. The scheduler lock is held across context switches: the switched-in task releases it and then restores its own interrupt state. Code that waits on a condition holds its own lock and passes it to `WaitOn`, which releases it while the task waits; subsystem locks are always taken before the scheduler lock. Process records in `src/kernel/process.cpp` are heap-allocated too; each has a parent, its own address space, and a page limit (1024 pages by default) covering its ELF segments, stack, and arguments block. Spawning a process makes it the foreground process for console input; waiting for it returns the foreground to its parent. Run `SCHEDULER_TEST=ON ./build.sh --arch x86_64 test` or `SCHEDULER_TEST=ON ./build.sh --arch aarch64 test` for kernel-task lifecycle checks. Spawned programs are time-sliced: they run in user mode with interrupts unmasked, and every timer tick on their CPU may switch to a higher-priority or other ready task. Kernel tasks are switched only when they block or yield, except in the scheduler test. Run `USER_PREEMPTION_TEST=ON ./build.sh --arch x86_64 test` or `USER_PREEMPTION_TEST=ON ./build.sh --arch aarch64 test` to verify timer preemption while user code is executing. The scheduler test also covers wait-queue blocking and wakeup, timed sleep, wait deadlines, and three rounds of 12 concurrent tasks without leaking page frames, priority ordering; the memory test checks that a kernel stack's guard page faults.

## Multiple CPUs

QEMU runs with four CPUs (`-smp 4`) in tests and interactive runs. Once the boot CPU's timer runs, `src/kernel/smp.cpp` uses Limine's MP response to start each secondary CPU (up to `Smp::MaximumCpus`, 8) and logs `SMP: N CPUs online with IPIs and timers`. Each secondary CPU installs its CPU-local state, loads its own exception state (on x86-64 a GDT, TSS, and IST stacks with the shared IDT; on AArch64 `VBAR_EL1`), enables its local interrupt controller and timer, prints `CPU N online`, and waits for interrupts. Each secondary CPU then enters the scheduler on its own idle task. Before logging the summary, the boot CPU sends each CPU an IPI, waits for it to be handled, and checks that each CPU's timer is ticking. The normal-boot smoke test requires every CPU's online line and the summary.

IPIs and per-CPU timers use `Arch::Interrupts::SetIpiHandler`/`SendIpi` and `Time::StartSecondaryCpuTimer`. On AArch64 the kernel runs on `SP_EL1` (`SPSel=1`) on every CPU, so exception frames land on the current task's kernel stack and `SP_EL0` holds only user stack pointers; Limine enters on `SP_EL0`, and exception setup moves the stack. Each CPU enables its banked GICv2 CPU interface, SGIs, and PPIs; IPIs are SGI 1, addressed by the GIC target mask each CPU reads at startup, and each core runs its own Generic Timer. On x86-64, `src/kernel/arch/x86_64/local_apic.cpp` maps the local APIC through `Memory::MapDevice` and enables it on every CPU. IPIs use vector 48 and the APIC timer vector 49; the boot CPU calibrates the APIC timer against 10 PIT ticks. The legacy PIC and PIT still deliver device interrupts and global time to the boot CPU through LINT0 (virtual-wire mode); the I/O APIC is not used yet. Only the boot CPU advances `Time::TickCount()` and polls the UART; every CPU's tick drives the scheduler (sleeper wakeups, preemption, and leaving its idle task).

### Scheduling on every CPU

All CPUs share one run queue under the scheduler lock, so any CPU runs any ready task: user programs, the shell, and the boot thread may move between CPUs at each switch. Making a task ready sends an IPI to an idle CPU, if there is one, so it leaves `hlt`/`wfi` at once instead of at its next tick. The lock is held across each context switch and released by the task that switches in, so no CPU can resume a task before its registers are saved. The framebuffer console takes a display lock, and page-table edits take a page-table lock (heap, then page tables, then the page allocator). TLB shootdown is not needed yet: kernel mappings are only added after the secondary CPUs start, and switching address spaces flushes user translations (x86-64 `CR3` reload, AArch64 `tlbi vmalle1`). Unmapping a live kernel page or sharing an address space between CPUs will need one.

Run `SMP_TEST=ON ./build.sh --arch x86_64 test` or `--arch aarch64` to check that three tasks run at the same time on three CPUs that a lock-protected counter they share comes out exact, and that the scheduler counts each of those CPUs as busy. `SCHEDULER_TEST=ON` runs now also check the scheduler success line.

`src/kernel/cpu.cpp` gives each CPU an index that `Cpu::CurrentIndex()` reads from a CPU register: the `GS` base on x86-64 and `TPIDR_EL1` on AArch64. On x86-64 the entry stubs run `SWAPGS` when an interrupt or syscall comes from ring 3, and again before `IRETQ` back to ring 3, so user code never sees the kernel's `GS` base. Per-CPU data lives in arrays indexed by `Cpu::CurrentIndex()`: the scheduler's current and idle tasks, each CPU's active page-table root, and the x86-64 TSS. `Serial::Write` takes a lock so lines from different CPUs do not interleave; panic and fatal-exception reports force it open first.

## Wall Clock

The kernel reads UTC wall-clock time from the platform real-time clock with one-second resolution: the CMOS RTC on x86-64 (`src/kernel/arch/x86_64/rtc.cpp`, BCD or binary and 12- or 24-hour formats, century register `0x32`, with reads serialized by a lock because the index/data port pair is shared by all CPUs) and the PL031 at its fixed QEMU `virt` address on AArch64 (`src/kernel/arch/aarch64/rtc.cpp`). QEMU initializes both from the host clock in UTC. Startup logs the time as `Wall clock: YYYY-MM-DD HH:MM:SS UTC`, and programs read it with the `ReadClock` syscall. `src/user/calendar.h` converts between UNIX seconds and UTC dates for both the kernel and user programs; there is no time-zone support.

## Shell And Console Input

After startup, the kernel spawns `src/user/shell.cpp` from the initramfs and restarts it if it exits. The shell reads lines with the `> ` prompt and runs each line's first word as the initramfs program `<name>-<arch>.elf` (names are limited to 32 letters, digits, `_`, or `-`), waits for it, and prints `exit status N` for nonzero statuses. Missing programs report `Unknown command: <name>`; `help` lists the bundled programs:

- `calc [expression]` evaluates an expression such as `4 + 66 - (3 / 4)` with `+ - * /`, parentheses, unary signs, and decimals, using exact fractions (`src/user/calculator.h`) and printing up to six decimal places. Without an expression it reads one per line at a `calc> ` prompt until `exit` or `quit`.
- `date` prints the current UTC time, such as `2026-10-19 03:07:39 UTC`.
- `echo <words...>` prints its arguments.
- `fanout <n>` starts `n` (1–16) `echo` children before waiting for any of them.
- `primes <workers> [limit]` demonstrates multicore scheduling. It counts the primes below `limit` (default 10,000,000) with 1–16 worker processes, each testing every n-th odd number by trial division. Each worker reports its count and the CPUs it ran on through its exit status, and the parent prints one line per worker plus the total time. Compare `primes 1` with `primes 4` and watch the status bar's busy-CPU count.
- `sleep <ms>` sleeps and checks that the wall clock advanced by the whole seconds requested.
- `hardening` runs 44 negative checks: syscalls with kernel or invalid pointers and oversized arguments, a kernel-half ELF (`badelf`, generated by `tools/create_bad_elf.py`), children that execute the heap, write their code, read kernel memory, or use an invalid syscall (each must be terminated), and the per-process child limit.
- `aslr` prints where its stack, arguments block, and heap landed; the shell test runs it twice and requires different addresses.
- `memtest [overflow|exhaust]` grows its stack and heap and checks them, recurses into the stack guard page to show that the kernel terminates it, or grows its heap until the page limit refuses more (the shell test runs it twice and requires the same count).
- `upper` reads one line as the foreground process and prints it in upper case.
- `uptime` prints the time since boot as `H:MM:SS`.

Every program except the boot self-test prints its usage and purpose for `<name> -h` or `<name> --help` (`ExitWithHelpIfRequested` in `src/user/runtime.h`), and the shell's `help` points to this. To add a program, create `src/user/<name>.cpp` with a `_start(const ProgramArguments*)` entry point and add `<name>:<name>` to the user-program list in `build.sh`.

Interactive runs accept keys from the QEMU window on both architectures. x86-64 uses PS/2; AArch64 uses virtio-input on QEMU `virt`. The launching terminal's serial console also remains available. The kernel console (`src/kernel/console.cpp`) handles line editing for `ReadLine`: the prompt and the typed line appear inline in the scrolling text with a block cursor, like a normal terminal, and it supports printable ASCII, Backspace (also across a wrapped line), and Enter, echoing input to serial. Program output appears on both serial and the framebuffer console.

The framebuffer's top row is a status bar. Its left side names the foreground program (`kernel` before the shell starts). Its right side shows busy CPUs over online CPUs, then the UTC date and time as `Mon Oct 19 9:36 PM`. A CPU counts as busy when the scheduler's timer ticks found it running a task other than its idle task for at least half of the last second. A high-priority kernel task redraws the bar once a second and when the foreground program changes. Its clock is read from the RTC once and then advanced by timer ticks.

`CALC_TEST=ON` checks a calculator expression, an unknown command, and a blank line; `SHELL_TEST=ON` checks `--help`/`-h`, Ctrl+C on a CPU-bound `primes`, a `sleep`, `upper` and `calc` waiting for input, and the shell prompt, plus `echo` arguments, foreground input in `upper`, the interactive `calc> ` prompt and its errors, `primes 4 100000` (the count and four worker reports), a nonzero exit status, the `uptime` format, and that `date` is within two minutes of the host clock. `INPUT_TEST=ON` runs the keyboard test, including Ctrl+C at the prompt. Pass each with `./build.sh test --arch x86_64` or `--arch aarch64`.

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