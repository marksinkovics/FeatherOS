# FeatherOS — agent quick start

Hobby kernel for **x86-64 and AArch64**, booted by Limine, freestanding C/C++ + ASM, built with CMake/Ninja via `build.sh`, run and tested in QEMU. Read this first; open the docs below only when the task needs them.

## Status (keep this section current)

A finished milestone gets one line here. Its full summary (design, files, tests) goes in `docs/roadmap.md`. Only the milestone in progress gets detail.

- **Complete, all on `main`:** M1–M13, M15, M20, M21, M23–M26.
  - M1–M7: multi-arch build, exceptions/IRQs, physical/virtual memory and heap, timers and keyboard, initramfs/VFS/FAT16 over virtio-blk, ELF64 and user mode with syscalls, preemptive scheduler, processes and shell.
  - M8 SMP: one shared run queue (4 CPUs in QEMU), per-CPU index via x86-64 `GS`/`SWAPGS` and AArch64 `TPIDR_EL1`, local APIC IPIs and timers on x86-64, AArch64 kernel on `SP_EL1`.
  - M9 persistent storage: virtio-blk writes, writable FAT16 (subdirectories, VFAT long names, MBR/GPT), file descriptors, per-process working directories.
  - M10 networking: virtio-net, ARP/NDP, IPv4/IPv6, ICMP, UDP, TCP (window scaling, NewReno, out-of-order ranges), DHCP, DNS (`/etc/hosts`, `/etc/resolv.conf`, TCP fallback), sockets and `poll`, `nc`, `fetch`, `httpd`, `ntp`, `net up/down`.
  - M11 system services: `init` with `/system/init.conf`, shutdown/reboot, the C library (`src/libc/`, documented in `docs/posix.md`), `/dev`, `/tmp`, `/proc`, `/etc`, signals, pipes and jobs, process tools, terminal and `vi`, threads (TLS, futexes, TLB shootdown), core utilities, programs run from disk.
  - M12/M13 hardening and scalability: dynamic tasks, guard-paged kernel stacks, demand-grown user stacks, priorities, static-PIE programs with ASLR, SMEP/SMAP/PAN, `hardening` tests.
  - M15 status bar and inline prompt. M20 `gnu++23`, ASan+UBSan unit tests, `constinit` locks.
  - M23 terminal object, sessions, pseudo-terminals, four virtual consoles. M24 stopped processes, Ctrl+Z, background terminal access.
  - M21 Embedded Swift user programs (`FeatherSupport`, a concurrency runtime built from `deps/swift`, `swiftbench` against `cppbench`, `BUILD_TYPE`). M25 Swift beyond the Embedded defaults (clocks, `MainActor`, custom executors, C++ interop, our own build of the Embedded `Swift` module, `Codable`, libc `regex.h`).
  - M26 a C++23-subset standard library of our own (`src/libcxx/include/`, compiled part `featheros_cxx`, host tests as `featheros_std` in `tests/unit/libcxx/`, `cxxtest` in QEMU; `std::__detail::LibraryAbort` where the standard throws), used by the kernel, libc, all programs, Swift's C++ programs (Clang module `std`) and both Swift runtimes, so libc++ is no longer fetched.
- **Also on `main`:** M27 full Swift with runtime type information (`src/user/swift_runtime/`, `FEATHEROS_SWIFT_FULL_PROGRAMS`, `./build.sh benchmark`) and M28 Parts 1–2 (`tools/smoke/` harness, `test=<mode>` kernel hooks, `tools/matrix.py` three cases at a time). M28 Part 3 is optional; its roadmap heading is not yet marked complete.
- **In progress: M22** (mount table, FAT and ext2/ext3/ext4, roadmap), on `feat/disk`.
  - Part 1 done: `Filesystem::Driver` (`src/kernel/filesystem/filesystem.h`, drivers in `drivers.cpp`), `VFS::MountTable` with per-mount user counts and `VFS::TypeRegistry` (`mount_table.cpp`, `type_registry.cpp`), `mount`/`umount` syscalls, libc `<sys/mount.h>` and programs, `/proc/mounts`, busy checks and `Sync` on unmount.
  - Part 2 done: the shared filesystem core `src/fs/core/` (namespace `FeatherDisk`, later split into the Feather Disk repo; driver interface in `include/fs/filesystem.h`, MBR/GPT in `partition/`, FAT16 as `FeatherDisk::Fat::Fat16Type()` in `fat/`, tests in `tests/` incl. mtools images), the kernel adapter `Filesystem::CoreDriver` (`src/kernel/filesystem/core_driver.cpp`, with `core_host.cpp` and `storage/core_block_device.cpp`), `quality.sh` stage `fs-core`, and the host tool `fscli` (`tools/fscli/`: `info`, `ls`, `cat`, `cp` out; CTest `fs.core:fscli *`).
  - Part 3 done: read-only ext2 as `FeatherDisk::Ext::Ext2Type()` (`src/fs/core/ext/`, metadata through `common/block_cache.cpp`; ext4 features refused), read by `fscli` and host tests over `mke2fs -d` images (Homebrew `e2fsprogs`, keg-only). The test disk (`tools/create_test_disk.py`, which now needs `mke2fs`) has an ext2 partition after FAT16; the kernel registers it as `ext2 vda2` (`Drivers::Ext2()`, `RegisterExt2` in `vfs.cpp`) without mounting it, and the FAT16 root's source is now `vda1` (`Drivers::PartitionSource`). Checked by `STORAGE` (`CheckExt2`, MBR and GPT) and the shell part `tools/smoke/cases/shell/ext2.py`.
  - Part 4 done (ext2 write; details per step in the roadmap): the POSIX file ABI (`PATH_MAX` 1024, `NAME_MAX` 255, full `stat`, `src/user/syscall_abi.h`); symbolic links resolved in the VFS walk (`VFS::ResolvePath`, `path_walk.cpp`; syscalls 66–70, libc `lstat`/`readlink`/`symlink`/`link`/`realpath`); ext2 writes in the core (`src/fs/core/ext/`: `write.cpp`, `namespace.cpp`, `allocation.cpp`; crash-ordered write-through, read-only fallback; `fscli cp-in`, `rm`, `mkdir`, `rmdir`, `mv`, `ln`); ext2 mounted read-write in FeatherOS (`mount [-r] -t ext2 vda2 /mnt`, `User::MountRequest`/`MS_RDONLY`; `Driver::PrepareMount`/`OpenForMount`/`IsMountedReadOnly`, the core read-only while unmounted, `Files::Quiesce` syncs every mount), core errors as errno (`Filesystem::SyscallError`, `Driver::LastError`, `VFS::LastError`; `ENOSPC`/`EMLINK`/`EFBIG`) and Linux's `.`/`..` codes in `files.cpp`. `STORAGE` writes ext2 on both boots, then the harness runs `e2fsck -fn` and `debugfs` on the partition (`tools/smoke/cases/storage.py`); the shell part `ext2.py` writes on a read-write mount and the shell case runs `e2fsck -fn` after.
  - Part 4b done: the root is ext2 on the test disk's `vda1` (`tools/create_test_disk.py`: `mke2fs -d` from a staging tree with `BLOCK.TXT`, `home`, `bin`, `samples/`; owners 0, `samples/` 1000), FAT16 moved to `vda2` (registered, mounted by tests at `/fat` or `/mnt`). `VFS::MountBlockDevice` puts the lower-numbered FAT16/ext2 partition at `/` (read-only with a warning if ext2 refuses writes; `VFS::DescribeRoot`, logged at boot); syscall 71 `Sync`, libc `sync()`, program `sync`. `STORAGE` checks FAT16 at `/fat` with mtools and the root with `e2fsck -fn`/`debugfs`; the shell session syncs before its last part.
  - Part 5 done (ext3 and ext4 read; details per step in the roadmap): the ext3/ext4 layout in the core (`src/fs/core/ext/`: features, 64bit, flex_bg, CRC16/CRC32C verified on read) and extent trees (`ext/extent.cpp`), one driver under the names `Ext2Type()`/`Ext3Type()`/`Ext4Type()` whose `Probe` takes only its own variant (`fscli info` names it); the test disk (64 MiB) adds `vda3` ext3 and `vda4` ext4 (depth-2 extent file), registered as `ext3 vda3`/`ext4 vda4` (`Drivers::Ext3()`/`Ext4()`, never the root) and always mounted read-only; checked by `STORAGE` (`CheckExt3AndExt4`, partitions hashed unchanged and `e2fsck -fn`) and the shell part `ext4.py`.
  - Next: Part 6, ext3 and ext4 write.
- Known hard limits: 16 KiB kernel stacks (guard-paged pool), 16384 pages (64 MiB) per process, 32 descriptors per process (64 open disk files system-wide), TCP rings of at most 256 KiB each way (64 KiB receive window unscaled), 4 out-of-order TCP ranges, a listen backlog of 16, `poll` on at most 64 descriptors, 16 pseudo-terminal pairs, 16 mounts with one instance per filesystem (no multi-instance mounts), FAT16 long names ASCII-only and at most 63 characters, ext2 with blocks of at most 4 KiB (read-only fallback for unclean volumes or unknown features), ext3 and ext4 read-only in FeatherOS (the core writes only plain ext2; `mount -t` must name the volume's variant; meta_bg, inline_data, encryption, casefold, large_dir and bigalloc refused; a journal needing recovery is not replayed; bitmap checksums not checked on read), ext2 files under 2 GiB on revision 0 volumes, 32000 links or subdirectories per ext2 inode, ext2 directories that never shrink, paths of at most 1023 characters (`PATH_MAX` 1024, also once links are expanded) and names of 255 bytes, 40 symbolic links per path, hard links only on ext2 (FAT16 and tmpfs hold none), a rename onto an existing file removes it first (not atomic), tmpfs and virtual-filesystem failures give `EIO` without a reason, one disk (`vda`) with at most 16 partitions, the root is the first partition holding FAT16 or ext2 (no `root=`), an ext2 root left unsynced (machine stopped without `sync` or shutdown) mounts read-only at the next boot, `/tmp` and `/etc` together at most 128 entries and 4 MiB, `/etc` a RAM copy of the initramfs whose edits are lost at reboot (stopgap; better design in `docs/ideas.md`, "A persistent `/etc`"), 63-page user stack, only the top of the heap can be returned to the kernel (no `mmap`), ≤16 program args in 1023 chars, program path included (double quotes group words), environments ≤1 KiB and 32 variables, no console end-of-file key, no `long double` arithmetic on AArch64 (no compiler runtime), sin/cos/tan imprecise above about 10^6 (sinl/cosl above 2^30), full-Swift programs cannot import Embedded modules (`FeatherSupport` is built in both modes), and are 2.4 to 3.1 MB in Release, 16 threads per process with 63-page stacks, no per-CPU run queues, `KLogger` not locked.

## Branches

- `main` — default branch; every completed milestone was squash-merged into it from a feature branch.
- `feat/disk` — M22 in progress.
- Do feature work on `feat/<topic>` branches cut from `main`, and squash-merge each one back into `main`.

## Layout

- `src/kernel/` — architecture-neutral kernel: `kernel.cpp` (startup), `scheduler.*`, `process.*`, `syscall.*`, `console.*`, `exception.*`, `panic.*`, `klogger.*`, `smp.*` (secondary CPU startup), `cpu.*` (per-CPU index), `kernel_test.*` (in-kernel QEMU test hooks), plus `memory/`, `filesystem/` (USTAR, VFS, tmpfs, `core_driver.*` for the shared core's filesystems), `network/` (virtio-net, Ethernet, ARP, IPv4, IPv6, ICMP, ICMPv6, neighbor discovery, UDP, sockets, settings and name servers), `virtio/` (shared virtqueue), `elf/`, `framebuffer/`, `input/`, `storage/`, `sync/` (spinlocks), `time/`, `string/`.
- `src/kernel/arch/{x86_64,aarch64}/` — entry, GDT/IDT/TSS or vectors/GIC, paging, context switch, syscall entry, UART, RTC, PIC/PIT or Generic Timer, virtio PCI/MMIO.
- `src/fs/core/` — the shared filesystem core (M22, namespace `FeatherDisk`, MIT OR Apache-2.0): self-contained, with its own `CMakeLists.txt` and `tests/`, built into the kernel and `unit-test`; `fat/` is the FAT16 driver, `ext/` the ext2 driver, `partition/` MBR/GPT, `tools/fscli/` the host command-line tool.
- `src/libcxx/` — the C++ standard library (M26), in `std`: headers in `include/`, the compiled part `featheros_cxx` in `src/`, the Clang module `std` for Swift in `swift/`; `cxxtest` checks them.
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
- At the end of a step or milestone, update the docs with the `finish-step` skill (`.claude/skills/finish-step/`). Adding a program is the `add-program` skill. Working through steps with one subagent each, reviewed and committed on the feature branch, is the `autopilot` skill.

## Deeper docs

- `docs/roadmap.md` — planned and completed milestones, with completion summaries.
- `docs/ideas.md` — unscheduled ideas; one moves to the roadmap with the next milestone number when planned.
- `docs/posix.md` — the C library: every header and function, differences from POSIX, and what is left out.
- `docs/development.md` — tool setup, every test mode, syscall table, scheduler/process/shell/RTC details, and AArch64 toolchain notes.
