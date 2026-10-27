# FeatherOS — agent quick start

Hobby kernel for **x86-64 and AArch64**, booted by Limine, freestanding C/C++ + ASM, built with CMake/Ninja via `build.sh`, run and tested in QEMU. Read this first; open the docs below only when the task needs them.

## Status (keep this section current)

- Milestones **M1–M9, M11, M12, M13 and M15 complete** on both architectures: multi-arch build, exceptions/IRQs, physical/virtual memory + heap, timers + keyboard input, initramfs/VFS/FAT16 over virtio-blk, ELF64 + user mode + syscalls, preemptive scheduler + processes + shell, SMP (4 CPUs in QEMU).
- **M8 done:** every CPU schedules from one shared run queue (scheduler lock held across switches, per-CPU idle tasks, IPI wakeups). Per-CPU index via x86-64 `GS` + `SWAPGS` / AArch64 `TPIDR_EL1`. x86-64 uses the local APIC for IPIs and timers, with the legacy PIC kept for devices. The AArch64 kernel runs on `SP_EL1`.
- **M12 and M13 done** (merged to `main`): dynamic tasks and processes, guard-paged kernel stacks, demand-grown user stacks, `ExtendHeap`/`Sleep` syscalls, priorities, static-PIE programs with full ASLR, SMEP/SMAP/PAN plus `RDRAND`/`RNDR` when present (tested with `QEMU_CPU=max`), and the `hardening` negative tests. Deferred items moved to M9 (open-file limit) and M11 (threads/TLS, `fork`/copy-on-write).
- **M15 done:** top status bar (foreground program, busy/online CPUs, UTC time), inline prompt `> ` with a cursor, `ReadUptime` syscall + `uptime` command, no periodic uptime log; expression `calc`, `primes` multicore demo (`CurrentCpu` syscall), spawned programs time-sliced on timer ticks, Ctrl+C interrupts the foreground job (children die with their parent).
- **M9 done** (merged to `main`): virtio-blk writes, writable FAT16 with subdirectories and VFAT long names on MBR or GPT disks, file-descriptor syscalls (`src/kernel/files.cpp`), per-process working directories (`filesystem/path.cpp`), and shell tools `ls`/`cat`/`write`/`rm`/`mkdir`/`rmdir` plus `cd`/`pwd` built-ins. `STORAGE_TEST` checks persistence across two boots with mtools.
- **M11 done** (on `feat/system_service`, to be squash-merged): Part 1 done, `init` with `/system/init.conf` services (`once`/`respawn`/`console`, restart limits). Part 2 done, `shutdown`/`reboot` (`src/kernel/power.cpp`; PSCI on AArch64, ACPI on x86-64; `POWER_TEST=shutdown|reboot`). Part 3 (C library, `src/libc/`) complete: step 1 done, startup (`main`, constructors, `exit`/`atexit`), `string.h`, `ctype.h`, `errno.h`, `setjmp.h`, integer `stdlib.h`, C++ runtime hooks. Step 2 done: console descriptors 0-2, cause-specific syscall errors mapped to `errno`, `Stat`/`StatDescriptor`/`ProcessId`/`ParentProcessId`, Spawn environments (`env` lines in `init.conf`). Step 3 done: `malloc`/`free`/`realloc`/`calloc` (`src/libc/heap_allocator.h`), `new`/`delete`, `strdup`. Step 4 done: stdio (`src/libc/format.h` printf engine, buffered `FILE*` streams), `assert.h`. Step 5 done: `open`/`lseek`/`unlink`/`mkdir`/`chdir`/`getcwd`, `dirent.h`, `time.h`, `posix_spawn`/`waitpid`, `scanf`. Step 6 done: per-task FPU/SIMD state (`src/kernel/arch/floating_point.h`, kernel built without FP registers), exact float `printf`/`strtod`/`scanf`, `math.h`. Step 7 done, so Part 3 is complete: every program but `test_program` uses `main` and libc (`src/user/command_line.h`, `posix_spawnattr` flags, `sbrk`/`sched_getcpu`/`reboot`, `<featheros.h>`), and `docs/posix.md` documents the subset. The heap-shrinking follow-up is done too (negative `ExtendHeap`/`sbrk`, `Memory::UnmapUserPage`, `malloc` returns a free heap tail over 256 KiB). Remaining parts in order (roadmap M11): 4 virtual files (done: `/dev`, `/tmp`, `/proc` in `src/kernel/filesystem/{devices,tmpfs,procfs}.cpp`), 5 signals/pipes/jobs (done; step 1 done: shared open files, `dup`/`dup2`/`fcntl`, descriptor inheritance, `posix_spawn` file actions, shell redirection; step 2 done: `pipe` in `src/kernel/pipe.cpp`, shell `|`; step 3 done: process groups, a foreground group instead of a foreground process, `tcsetpgrp`, shell `&`/`jobs`/`fg`/`wait`; step 4 done: signals in `src/kernel/signals.cpp` with handler frames in each `arch/*/exceptions.cpp`, `signal.h`, `kill`; step 5 done: `primes` workers report through pipes), 6 process tools (done: per-task CPU ticks, `/proc/stat`, `ps`, `top`, `sysconf`, shell `kill %n`). 7 terminal and `vi` (done; step 1 done: VT100 cursor control and reverse video in `src/kernel/framebuffer/term.cpp`, `TerminalSize` syscall, `ioctl(TIOCGWINSZ)`; step 2 done: byte-stream input with a shared key map in `src/kernel/input/keyboard.cpp`, `TerminalMode` syscall, `termios.h`, `keys` with `src/user/key_decoder.h`; step 3 done: shell line editing and history in `src/user/line_editor.h` over `src/user/raw_terminal.h`; plus `clear`; step 4 done: `vi` (`src/user/vi_editor.cpp`, `src/user/vi.cpp`), with the alternate screen in `term.cpp`; step 5 done: `top` redraws in place on the alternate screen, with `RawTerminal::ReadKeyWithin`). 8 threads (done; step 1 done: threads in a process with `ThreadCreate`/`ThreadExit`/`ThreadJoin`/`ThreadId`, stack slots in `src/kernel/thread_stacks.cpp`, `pthread.h`, `threads` demo; step 2 done: TLS from `PT_TLS` in each stack slot (`src/kernel/thread_local_storage.cpp`), thread pointer saved per task via `src/kernel/arch/thread_pointer.h`; step 3 done: `FutexWait`/`FutexWake` in `src/kernel/futex.cpp`, `ThreadDetach`, mutexes, condition variables and `pthread_once` in `src/libc/pthread.cpp`; step 4 done: thread-safe libc with locks in `src/libc/lock.cpp`, per-thread `errno`, locked `malloc` and stdio, `__cxa_guard_*`, `__cxa_thread_atexit`; step 5 done: TLB shootdown via `Memory::ShootDownUserMappings` (x86-64 IPIs in `arch/x86_64/virtual_memory.cpp`, AArch64 broadcast `tlbi`), heap shrinking and joined stacks freed with threads). 9 core utilities (done; step 1 done: `cp`, `touch`, `head`, `tail`, `wc`, `grep`, `sort` over `src/user/text_tools.cpp`; step 2 done: `Rename` syscall in `files.cpp`/`fat16.cpp`/`tmpfs.cpp`, libc `rename`, `mv` sharing `src/user/file_copy.cpp` with `cp`). 10 programs from the disk (done: `Spawn` runs any VFS path through `Files::ProgramFile`, at most 1 MiB, a bare name meaning `/system/<name>`; the initramfs holds programs as `/system/<name>`; `posix_spawnp` searches `PATH=/system:/bin`). `cron`, packaging and a more capable `calc` moved to `docs/ideas.md`.
- Known hard limits: 16 KiB kernel stacks (guard-paged pool), 1024 pages per process, 32 descriptors per process (64 open disk files system-wide), FAT16 long names ASCII-only and at most 63 characters, `/tmp` at most 128 entries and 4 MiB, 63-page user stack, only the top of the heap can be returned to the kernel (no `mmap`), ≤16 program args in 63 chars, program path included (double quotes group words), program files ≤1 MiB, environments ≤1 KiB and 32 variables, no console end-of-file key, no `long double` arithmetic on AArch64 (no compiler runtime), sin/cos/tan imprecise above about 10^6, 16 threads per process with 63-page stacks, no per-CPU run queues, `KLogger` not locked.

## Branches

- `main` — default branch with M1–M9, M12, M13 and M15 (M11 waits on `feat/system_service`), each squash-merged from a feature branch.
- Do feature work on `feat/<topic>` branches cut from `main`, and squash-merge each one back into `main`.

## Layout

- `src/kernel/` — architecture-neutral kernel: `kernel.cpp` (startup), `scheduler.*`, `process.*`, `syscall.*`, `console.*`, `exception.*`, `panic.*`, `klogger.*`, `smp.*` (secondary CPU startup), `cpu.*` (per-CPU index), `kernel_test.*` (in-kernel QEMU test hooks), plus `memory/`, `filesystem/` (USTAR, FAT16, VFS), `elf/`, `framebuffer/`, `input/`, `storage/`, `sync/` (spinlocks), `time/`, `string/`.
- `src/kernel/arch/{x86_64,aarch64}/` — entry, GDT/IDT/TSS or vectors/GIC, paging, context switch, syscall entry, UART, RTC, PIC/PIT or Generic Timer, virtio PCI/MMIO.
- `src/libc/` — the C library: C headers in `include/`, freestanding C++ implementation, `start.cpp` (`_start` → `main`), `arch/<arch>/setjmp.cpp`.
- `src/user/` — userspace programs (`shell`, `calc`, `date`, `echo`, `upper`, `uptime`, `ls`, `cat`, `write`, `rm`, `test_program`, …) the shared `command_line.h`, and the ABI the C library and kernel tests use (`syscall_abi.h`, `syscall.h`).
- `tests/test_boot_matrix.py` — QEMU integration matrix (Python `unittest`). `tests/unit/` — Catch2 host tests.
- `tools/` — `smoke_test.py`, `qemu_run.py`, `create_test_disk.py`, `quality.sh`, font generation. `toolchains/` — cross-compiler builder. `configs/<arch>/` — Limine configs.

## Commands

```sh
./build.sh --arch x86_64 all        # build + ISO + run in QEMU (aarch64 also needs QEMU_EFI if not auto-found)
./build.sh --arch <arch> test       # headless serial smoke test
./build.sh test-all                 # full QEMU matrix, both arches
./build.sh unit-test                # host Catch2 tests
bash tools/quality.sh --fix         # clang-format/tidy, ruff, shellcheck/shfmt
```

You must always pass an architecture (`--arch` or `ARCH=`). Feature tests are env toggles, e.g. `SHELL_TEST=ON`, `SCHEDULER_TEST=ON`, `SMP_TEST=ON`, `STORAGE_TEST=ON`, `PANIC_TEST=ON` (add `QEMU_CPU=max` for the hardware-protection CPU model; `QEMU_NO_REBOOT=1` makes interactive runs exit on reset; the window scales with its size by default, `QEMU_ZOOM=0` shows the guest's 1024x768 unscaled), which map to `FEATHEROS_*_TEST` CMake options. Outputs go under `build/<arch>/`.

## Change workflow: lint before testing

Get the code lint-clean **before** running any QEMU smoke test or `test-all`. Alternating between smoke runs and lint runs wastes runs, because lint fixes change code that then has to be tested again.

1. Edit the code.
2. Build **both** architectures (`./build.sh --arch x86_64 build` and `./build.sh --arch aarch64 build`). clang-tidy reads `build/<arch>/compile_commands.json` and only checks architectures that have been built, so a build that skips one architecture leaves that architecture's `arch/` code unlinted.
3. Run `bash tools/quality.sh --fix`, then fix every remaining diagnostic by hand. Repeat until `bash tools/quality.sh check` passes. The pre-commit hook runs this same check.
4. Run `./build.sh unit-test`, then the relevant `./build.sh --arch <arch> test` modes, then `./build.sh test-all`.
5. If a test failure forces a code change, write the fix to be lint-clean and run `quality.sh check` before re-running the test.

`tools/check.sh` does steps 2-4 in one command, printing one line per stage and only the relevant lines of a failure (full logs in `build/check/`): `tools/check.sh --modes "SHELL" --arch x86_64`, `--arch both`, `--matrix`. Every smoke test saves its whole session to `build/<arch>/smoke-output.txt`, so a failure is read there instead of re-run. Cadence: while working on a step, unit tests plus the affected smoke mode on x86-64; at the end of a step, that mode on both architectures; the full matrix once per milestone part, not per step.

## Conventions

- Shared code goes in `src/kernel/`. Anything CPU- or board-specific goes under `arch/<arch>/`. Never share GDT/TSS/IDT concepts with AArch64. Full rules: "Architecture Boundaries" in `docs/roadmap.md`.
- Put implementations in `.cpp` files and only declarations in headers; templates are the exception.
- Keep comments minimal: only for education (hardware quirks, non-obvious invariants) or when the code can't be understood without them. Never describe what a function does; that goes stale when the code changes.
- Namespaces are `FeatherOS::<Subsystem>`, with PascalCase functions. clang-format uses LLVM style, 4-space indent, 100 columns.
- Early logging and panic must not allocate. Physical addresses use explicit 64-bit types, and code must not hard-code physical memory addresses (obtain them from the Limine memory map).
- Every feature should land with tests on **both** architectures: a QEMU case in `test_boot_matrix.py` and, where logic is host-testable, a unit test.
- To add a user program, create `src/user/<name>.cpp` with a `main` and add `<name>:<name>` to `FEATHEROS_PROGRAMS` in `src/user/CMakeLists.txt`. Use libc, not raw syscalls, unless the program tests the kernel ABI itself. Code shared between programs goes in a header with declarations and a `.cpp` listed in `featheros_user_support` (and in `tests/unit/CMakeLists.txt` when host-tested). A missing POSIX call belongs in `src/libc/`, documented in `docs/posix.md`.
- When a milestone completes, update `docs/roadmap.md` (mark it Complete and add a summary), `docs/development.md` (user-facing details), and the Status section above.

## Deeper docs

- `docs/roadmap.md` — planned and completed milestones, with completion summaries.
- `docs/ideas.md` — unscheduled ideas; one moves to the roadmap with the next milestone number when planned.
- `docs/posix.md` — the C library: every header and function, differences from POSIX, and what is left out.
- `docs/development.md` — tool setup, every test mode, syscall table, scheduler/process/shell/RTC details, and AArch64 toolchain notes.
