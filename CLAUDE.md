# FeatherOS — agent quick start

Hobby kernel for **x86-64 and AArch64**, booted by Limine, freestanding C/C++ + ASM, built with CMake/Ninja via `build.sh`, run and tested in QEMU. Read this first; open the docs below only when the task needs them.

## Status (keep this section current)

- Milestones **M1–M9, M12, M13 and M15 complete** on both architectures: multi-arch build, exceptions/IRQs, physical/virtual memory + heap, timers + keyboard input, initramfs/VFS/FAT16 over virtio-blk, ELF64 + user mode + syscalls, preemptive scheduler + processes + shell, SMP (4 CPUs in QEMU).
- **M8 done:** every CPU schedules from one shared run queue (scheduler lock held across switches, per-CPU idle tasks, IPI wakeups). Per-CPU index via x86-64 `GS` + `SWAPGS` / AArch64 `TPIDR_EL1`. x86-64 uses the local APIC for IPIs and timers, with the legacy PIC kept for devices. The AArch64 kernel runs on `SP_EL1`.
- **M12 and M13 done** (merged to `main`): dynamic tasks and processes, guard-paged kernel stacks, demand-grown user stacks, `ExtendHeap`/`Sleep` syscalls, priorities, static-PIE programs with full ASLR, SMEP/SMAP/PAN plus `RDRAND`/`RNDR` when present (tested with `QEMU_CPU=max`), and the `hardening` negative tests. Deferred items moved to M9 (open-file limit) and M11 (threads/TLS, `fork`/copy-on-write).
- **M15 done:** top status bar (foreground program, busy/online CPUs, UTC time), inline prompt `> ` with a cursor, `ReadUptime` syscall + `uptime` command, no periodic uptime log; expression `calc`, `primes` multicore demo (`CurrentCpu` syscall), spawned programs time-sliced on timer ticks, Ctrl+C interrupts the foreground job (children die with their parent).
- **M9 done** (merged to `main`): virtio-blk writes, writable FAT16 with subdirectories and VFAT long names on MBR or GPT disks, file-descriptor syscalls (`src/kernel/files.cpp`), per-process working directories (`filesystem/path.cpp`), and shell tools `ls`/`cat`/`write`/`rm`/`mkdir`/`rmdir` plus `cd`/`pwd` built-ins. `STORAGE_TEST` checks persistence across two boots with mtools.
- **M11 in progress** (on `feat/system_service`): Part 1 done, `init` with `/system/init.conf` services (`once`/`respawn`/`console`, restart limits). Part 2 done, `shutdown`/`reboot` (`src/kernel/power.cpp`; PSCI on AArch64, ACPI on x86-64; `POWER_TEST=shutdown|reboot`). Part 3 (C library, `src/libc/`) in progress: step 1 done, startup (`main`, constructors, `exit`/`atexit`), `string.h`, `ctype.h`, `errno.h`, `setjmp.h`, integer `stdlib.h`, C++ runtime hooks. Step 2 done: console descriptors 0-2, cause-specific syscall errors mapped to `errno`, `Stat`/`StatDescriptor`/`ProcessId`/`ParentProcessId`, Spawn environments (`env` lines in `init.conf`). Step 3 done: `malloc`/`free`/`realloc`/`calloc` (`src/libc/heap_allocator.h`), `new`/`delete`, `strdup`. Step 4 done: stdio (`src/libc/format.h` printf engine, buffered `FILE*` streams), `assert.h`. Next: step 5, the remaining POSIX calls (`open`/`lseek`/`unlink`/`mkdir`/`chdir`/`getcwd`, `dirent.h`, `time.h`, `posix_spawn`/`waitpid`).
- Known hard limits: 16 KiB kernel stacks (guard-paged pool), 1024 pages per process, 16 open files per process (64 open disk files system-wide), FAT16 long names ASCII-only and at most 63 characters, 63-page user stack, no heap shrinking (freed memory is reused but not returned), single-threaded `malloc`, ≤16 program args in 63 chars (double quotes group words), environments ≤1 KiB and 32 variables, no console end-of-file key, no SSE state saved on x86-64 (user code builds with `-mgeneral-regs-only`), no TLB shootdown or per-CPU run queues, `KLogger` not locked.

## Branches

- `main` — default branch with M1–M9, M12, M13 and M15, each squash-merged from a feature branch.
- Do feature work on `feat/<topic>` branches cut from `main`, and squash-merge each one back into `main`.

## Layout

- `src/kernel/` — architecture-neutral kernel: `kernel.cpp` (startup), `scheduler.*`, `process.*`, `syscall.*`, `console.*`, `exception.*`, `panic.*`, `klogger.*`, `smp.*` (secondary CPU startup), `cpu.*` (per-CPU index), `kernel_test.*` (in-kernel QEMU test hooks), plus `memory/`, `filesystem/` (USTAR, FAT16, VFS), `elf/`, `framebuffer/`, `input/`, `storage/`, `sync/` (spinlocks), `time/`, `string/`.
- `src/kernel/arch/{x86_64,aarch64}/` — entry, GDT/IDT/TSS or vectors/GIC, paging, context switch, syscall entry, UART, RTC, PIC/PIT or Generic Timer, virtio PCI/MMIO.
- `src/libc/` — the C library: C headers in `include/`, freestanding C++ implementation, `start.cpp` (`_start` → `main`), `arch/<arch>/setjmp.cpp`.
- `src/user/` — userspace programs (`shell`, `calc`, `date`, `echo`, `upper`, `uptime`, `ls`, `cat`, `write`, `rm`, `test_program`, …) and the shared ABI (`syscall_abi.h`, `syscall.h`, `runtime.h`).
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

You must always pass an architecture (`--arch` or `ARCH=`). Feature tests are env toggles, e.g. `SHELL_TEST=ON`, `SCHEDULER_TEST=ON`, `SMP_TEST=ON`, `STORAGE_TEST=ON`, `PANIC_TEST=ON` (add `QEMU_CPU=max` for the hardware-protection CPU model; `QEMU_NO_REBOOT=1` makes interactive runs exit on reset), which map to `FEATHEROS_*_TEST` CMake options. Outputs go under `build/<arch>/`.

## Change workflow: lint before testing

Get the code lint-clean **before** running any QEMU smoke test or `test-all`. Alternating between smoke runs and lint runs wastes runs, because lint fixes change code that then has to be tested again.

1. Edit the code.
2. Build **both** architectures (`./build.sh --arch x86_64 build` and `./build.sh --arch aarch64 build`). clang-tidy reads `build/<arch>/compile_commands.json` and only checks architectures that have been built, so a build that skips one architecture leaves that architecture's `arch/` code unlinted.
3. Run `bash tools/quality.sh --fix`, then fix every remaining diagnostic by hand. Repeat until `bash tools/quality.sh check` passes. The pre-commit hook runs this same check.
4. Run `./build.sh unit-test`, then the relevant `./build.sh --arch <arch> test` modes, then `./build.sh test-all`.
5. If a test failure forces a code change, write the fix to be lint-clean and run `quality.sh check` before re-running the test.

## Conventions

- Shared code goes in `src/kernel/`. Anything CPU- or board-specific goes under `arch/<arch>/`. Never share GDT/TSS/IDT concepts with AArch64. Full rules: "Architecture Boundaries" in `docs/roadmap.md`.
- Keep comments minimal: only for education (hardware quirks, non-obvious invariants) or when the code can't be understood without them. Never describe what a function does; that goes stale when the code changes.
- Namespaces are `FeatherOS::<Subsystem>`, with PascalCase functions. clang-format uses LLVM style, 4-space indent, 100 columns.
- Early logging and panic must not allocate. Physical addresses use explicit 64-bit types, and code must not hard-code physical memory addresses (obtain them from the Limine memory map).
- Every feature should land with tests on **both** architectures: a QEMU case in `test_boot_matrix.py` and, where logic is host-testable, a unit test.
- To add a user program, create `src/user/<name>.cpp` and add `<name>:<name>` to `src/user/CMakeLists.txt` (`FEATHEROS_MAIN_PROGRAMS` for a C `main`, `FEATHEROS_PROGRAMS` for its own `_start`).
- When a milestone completes, update `docs/roadmap.md` (mark it Complete and add a summary), `docs/development.md` (user-facing details), and the Status section above.

## Deeper docs

- `docs/roadmap.md` — planned and completed milestones, with completion summaries.
- `docs/ideas.md` — unscheduled ideas; one moves to the roadmap with the next milestone number when planned.
- `docs/development.md` — tool setup, every test mode, syscall table, scheduler/process/shell/RTC details, and AArch64 toolchain notes.
