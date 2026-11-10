# FeatherOS — agent quick start

Hobby kernel for **x86-64 and AArch64**, booted by Limine, freestanding C/C++ + ASM, built with CMake/Ninja via `build.sh`, run and tested in QEMU. Read this first; open the docs below only when the task needs them.

## Status (keep this section current)

A finished milestone gets one line here. Its full summary (design, files, tests) goes in `docs/roadmap.md`. Only the milestone in progress gets detail.

- **Complete, all on `main`:** M1–M13, M15, M20, M21, M23, M24, M25.
  - M1–M7: multi-arch build, exceptions/IRQs, physical/virtual memory and heap, timers and keyboard, initramfs/VFS/FAT16 over virtio-blk, ELF64 and user mode with syscalls, preemptive scheduler, processes and shell.
  - M8 SMP: one shared run queue (4 CPUs in QEMU), per-CPU index via x86-64 `GS`/`SWAPGS` and AArch64 `TPIDR_EL1`, local APIC IPIs and timers on x86-64, AArch64 kernel on `SP_EL1`.
  - M9 persistent storage: virtio-blk writes, writable FAT16 (subdirectories, VFAT long names, MBR/GPT), file descriptors, per-process working directories.
  - M10 networking: virtio-net, ARP/NDP, IPv4/IPv6, ICMP, UDP, TCP (window scaling, NewReno, out-of-order ranges), DHCP, DNS (`/etc/hosts`, `/etc/resolv.conf`, TCP fallback), sockets and `poll`, `nc`, `fetch`, `httpd`, `ntp`, `net up/down`.
  - M11 system services: `init` with `/system/init.conf`, shutdown/reboot, the C library (`src/libc/`, documented in `docs/posix.md`), `/dev`, `/tmp`, `/proc`, `/etc`, signals, pipes and jobs, process tools, terminal and `vi`, threads (TLS, futexes, TLB shootdown), core utilities, programs run from disk.
  - M12/M13 hardening and scalability: dynamic tasks, guard-paged kernel stacks, demand-grown user stacks, priorities, static-PIE programs with ASLR, SMEP/SMAP/PAN, `hardening` tests.
  - M15 status bar and inline prompt. M20 `gnu++23`, ASan+UBSan unit tests, `constinit` locks.
  - M23 terminal object, sessions, pseudo-terminals, four virtual consoles. M24 stopped processes, Ctrl+Z, background terminal access.
  - M21 Embedded Swift user programs (`FeatherSupport`, a concurrency runtime built from `deps/swift`, `swiftbench` against `cppbench`, `BUILD_TYPE`). M25 Swift beyond the Embedded defaults (clocks, `MainActor`, custom executors, C++ interop, our own build of the Embedded `Swift` module, `Codable`, libc `regex.h`).
- **M27 done, not yet merged** (branch `feat/swift`, cut from `main`): full Swift with runtime type information (roadmap M27, all six parts). `src/user/swift_runtime/` builds upstream's `Swift` module, runtime, `_Concurrency`, `Cxx`, `_StringProcessing`/`RegexBuilder` in full mode; `FEATHEROS_SWIFT_FULL_PROGRAMS` (and `_CXX_`) link with the toolchain's `ld.lld`, Release sealed (`-lto=llvm-full -experimental-hermetic-seal-at-link -Xllvm -enable-vfe=false`, link `-wholeprogramdevirt-skip=*`; both flags fix miscompiles, see the CMake comments). `FeatherSupport` builds in both modes (`swift_support/codable_containers.swift` gives full Swift the Embedded `EncoderBase`/`DecoderBase`). Programs: `fullhello`, `reflect`, `fulltasks`, `fullexecutors`, `fullcxxinterop`, `fullregex`, `fullcodable`, and the ports `fulljson`, `fulllife`, `fullswiftbench`, `fullhttp`. `./build.sh benchmark` compares C++, Embedded and full Swift (two rounds of 1 s, under a minute per arch). Full matrix passed (38 cases, 341 s). Next: squash-merge into `main`, then M26.
- **M28 in progress** (on the `feat/swift` working tree until a branch is cut): a smaller smoke test and a parallel matrix (roadmap M28). Part 1 done: `tools/smoke_test.py` is the entry point, the harness is `tools/smoke/` (one `Case` per mode in `cases/`, 22 shell parts in `cases/shell/`, host servers in `hosts/`), behaviour unchanged. Part 2 done: whole-line typing, Ctrl+C/Z 0.5 s after the echoed newline, Release in `build/<arch>-release`, kernel test hooks chosen by `test=<mode>` on the Limine command line (`kernel_test_mode.cpp`), `build.sh smoke`/`OUT_DIR`, and `tools/matrix.py` running cases three at a time: 38 cases in 451 s (was 1854 s). Part 3 (splitting the shell case) is optional; M28 can close once merged.
- After M27: M26, a C++ standard library of our own (roadmap).
- Known hard limits: 16 KiB kernel stacks (guard-paged pool), 16384 pages (64 MiB) per process, 32 descriptors per process (64 open disk files system-wide), TCP rings of at most 256 KiB each way (64 KiB receive window unscaled), 4 out-of-order TCP ranges, a listen backlog of 16, `poll` on at most 64 descriptors, 16 pseudo-terminal pairs, FAT16 long names ASCII-only and at most 63 characters, `/tmp` and `/etc` together at most 128 entries and 4 MiB, `/etc` a RAM copy of the initramfs whose edits are lost at reboot (stopgap; better design in `docs/ideas.md`, "A persistent `/etc`"), 63-page user stack, only the top of the heap can be returned to the kernel (no `mmap`), ≤16 program args in 63 chars, program path included (double quotes group words), environments ≤1 KiB and 32 variables, no console end-of-file key, no `long double` arithmetic on AArch64 (no compiler runtime), sin/cos/tan imprecise above about 10^6 (sinl/cosl above 2^30), full-Swift programs cannot import Embedded modules (`FeatherSupport` is built in both modes), and are 2.4 to 3.1 MB in Release, 16 threads per process with 63-page stacks, no per-CPU run queues, `KLogger` not locked.

## Branches

- `main` — default branch; every completed milestone was squash-merged into it from a feature branch.
- `feat/swift` — M27 (done, awaiting its squash-merge) and M28.
- Do feature work on `feat/<topic>` branches cut from `main`, and squash-merge each one back into `main`.

## Layout

- `src/kernel/` — architecture-neutral kernel: `kernel.cpp` (startup), `scheduler.*`, `process.*`, `syscall.*`, `console.*`, `exception.*`, `panic.*`, `klogger.*`, `smp.*` (secondary CPU startup), `cpu.*` (per-CPU index), `kernel_test.*` (in-kernel QEMU test hooks), plus `memory/`, `filesystem/` (USTAR, FAT16, VFS), `network/` (virtio-net, Ethernet, ARP, IPv4, IPv6, ICMP, ICMPv6, neighbor discovery, UDP, sockets, settings and name servers), `virtio/` (shared virtqueue), `elf/`, `framebuffer/`, `input/`, `storage/`, `sync/` (spinlocks), `time/`, `string/`.
- `src/kernel/arch/{x86_64,aarch64}/` — entry, GDT/IDT/TSS or vectors/GIC, paging, context switch, syscall entry, UART, RTC, PIC/PIT or Generic Timer, virtio PCI/MMIO.
- `src/libc/` — the C library: C headers in `include/`, freestanding C++ implementation, `start.cpp` (`_start` → `main`), `arch/<arch>/setjmp.cpp`.
- `src/user/` — userspace programs (`shell`, `calc`, `date`, `echo`, `upper`, `uptime`, `ls`, `cat`, `write`, `rm`, `test_program`, …) the shared `command_line.h`, and the ABI the C library and kernel tests use (`syscall_abi.h`, `syscall.h`).
- `tools/matrix.py` — QEMU integration matrix (cases run side by side, `-j N`, `-k <name>`). `tests/unit/` — Catch2 host tests.
- `tools/` — `smoke_test.py` (entry point; the harness, one file per mode and per shell feature, in `tools/smoke/`), `qemu_run.py`, `create_test_disk.py`, `quality.sh`, font generation. `toolchains/` — cross-compiler builder. `configs/<arch>/` — Limine configs.

## Commands

```sh
./build.sh --arch x86_64 all        # build + ISO + run in QEMU (aarch64 also needs QEMU_EFI if not auto-found)
./build.sh --arch <arch> test       # headless serial smoke test
./build.sh test-all                 # full QEMU matrix, both arches (tools/matrix.py, 3 at a time)
./build.sh unit-test                # host Catch2 tests
./build.sh --arch <arch> benchmark  # cppbench, swiftbench and fullswiftbench in QEMU (~1 min)
BUILD_TYPE=Release ./build.sh --arch <arch> test  # -O2 instead of the default -O0 -g
bash tools/quality.sh --fix         # clang-format/tidy, ruff, shellcheck/shfmt
```

You must always pass an architecture (`--arch` or `ARCH=`). Feature tests are env toggles, e.g. `SHELL_TEST=ON`, `SCHEDULER_TEST=ON`, `SMP_TEST=ON`, `STORAGE_TEST=ON`, `NETWORK_TEST=ON`, `INPUT_TEST=ON`, `POWER_TEST=shutdown|reboot`, `PANIC_TEST=ON` (add `QEMU_CPU=max` for the hardware-protection CPU model; `QEMU_NO_REBOOT=1` makes interactive runs exit on reset; the window scales with its size by default, `QEMU_ZOOM=0` shows the guest's 1024x768 unscaled), the kernel ones (`PANIC`, `EXCEPTION`, `INTERRUPT`, `MEMORY`, `TIMER`, `SCHEDULER`, `USER_PREEMPTION`, `SMP`, `STORAGE`, `NETWORK`, `INPUT`) as `test=<mode>` on the Limine kernel command line, in a kernel built with `KERNEL_TESTS=ON` (off by default; `test`, `smoke`, `test-all` and `check.sh` turn it on), so switching modes needs no rebuild. Builds go to `build/<arch>/` (Release: `build/<arch>-release/`); `OUT_DIR` moves the ISO, disk and session elsewhere.

## Change workflow: lint before testing

Get the code lint-clean **before** running any QEMU smoke test or `test-all`. Alternating between smoke runs and lint runs wastes runs, because lint fixes change code that then has to be tested again.

1. Edit the code.
2. Build **both** architectures with the test hooks (`KERNEL_TESTS=ON ./build.sh --arch x86_64 build`, the same for `aarch64`), or `kernel_test.cpp` goes unlinted. clang-tidy reads `build/<arch>/compile_commands.json` and only checks architectures that have been built, so a build that skips one architecture leaves that architecture's `arch/` code unlinted.
3. Run `bash tools/quality.sh --fix`, then fix every remaining diagnostic by hand. Repeat until `bash tools/quality.sh check` passes. The pre-commit hook runs this same check.
4. Run `./build.sh unit-test`, then the relevant `./build.sh --arch <arch> test` modes, then `./build.sh test-all`.
5. If a test failure forces a code change, write the fix to be lint-clean and run `quality.sh check` before re-running the test.

`tools/check.sh` does steps 2-4 in one command, printing one line per stage and only the relevant lines of a failure (full logs in `build/check/`): `tools/check.sh --modes "SHELL" --arch x86_64`, `--arch both`, `--matrix`. Every smoke test saves its whole session to `smoke-output.txt` in the build folder (`build/<arch>/`), so a failure is read there instead of re-run; each matrix case keeps its log and session in `build/matrix/<case>/`. A shell failure ending in "(stopped at the N s limit)" ran out of time; check how far the session got. Cadence: while working on a step, unit tests plus the affected smoke mode on x86-64; at the end of a step, that mode on both architectures; the full matrix once per milestone part, not per step.

## Conventions

- Shared code goes in `src/kernel/`. Anything CPU- or board-specific goes under `arch/<arch>/`. Never share GDT/TSS/IDT concepts with AArch64. Full rules: "Architecture Boundaries" in `docs/roadmap.md`.
- Put implementations in `.cpp` files and only declarations in headers; templates are the exception.
- Keep comments minimal: only for education (hardware quirks, non-obvious invariants) or when the code can't be understood without them. Never describe what a function does; that goes stale when the code changes.
- Shared state names its lock: `GUARDED_BY(lock)` on the data and `REQUIRES(lock)` on `...Locked` helpers, from `src/user/thread_safety.h`; clang-tidy's thread-safety analysis rejects unlocked access ("Lock annotations" in `docs/development.md`). clang-tidy warnings are errors.
- Namespaces are `FeatherOS::<Subsystem>`, with PascalCase functions. clang-format uses LLVM style, 4-space indent, 100 columns.
- Early logging and panic must not allocate. Physical addresses use explicit 64-bit types, and code must not hard-code physical memory addresses (obtain them from the Limine memory map).
- Every feature should land with tests on **both** architectures: a QEMU case in `tools/matrix.py` and, where logic is host-testable, a unit test.
- To add a user program, create `src/user/<name>.cpp` with a `main` and add `<name>:<name>` to `FEATHEROS_PROGRAMS` in `src/user/CMakeLists.txt` (a Swift one: `src/user/<name>.swift` with top-level code, in `FEATHEROS_SWIFT_PROGRAMS`, or `FEATHEROS_SWIFT_FULL_PROGRAMS` for full Swift). Use libc, not raw syscalls, unless the program tests the kernel ABI itself. Code shared between programs goes in a header with declarations and a `.cpp` listed in `featheros_user_support` (and in `tests/unit/CMakeLists.txt` when host-tested). A missing POSIX call belongs in `src/libc/`, documented in `docs/posix.md`.
- At the end of a step or milestone, update the docs with the `finish-step` skill (`.claude/skills/finish-step/`). Adding a program is the `add-program` skill.

## Deeper docs

- `docs/roadmap.md` — planned and completed milestones, with completion summaries.
- `docs/ideas.md` — unscheduled ideas; one moves to the roadmap with the next milestone number when planned.
- `docs/posix.md` — the C library: every header and function, differences from POSIX, and what is left out.
- `docs/development.md` — tool setup, every test mode, syscall table, scheduler/process/shell/RTC details, and AArch64 toolchain notes.
