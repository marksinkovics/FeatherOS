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

The integration tests use Python's built-in `unittest` framework and keep QEMU as the execution backend. `./build.sh test-all` runs 34 cases across x86-64 and AArch64, covering panic and exception reporting, normal startup, interrupts, memory, timers, cooperative scheduling, user-mode preemption, multi-CPU scheduling, disk-write persistence across two boots on MBR and GPT disks, `shutdown` (QEMU must exit by itself) and `reboot` (the system must boot again; `POWER_TEST=shutdown` or `POWER_TEST=reboot`), shell sessions on QEMU's `max` CPU with hardware protections enabled, UART input, window-keyboard input, and shell sessions that run user programs. Both keyboard cases inject keys through QEMU's QMP interface. Each case builds its kernel and ISO, boots QEMU, and validates serial output. QEMU is stopped as soon as the expected record appears, with a timeout as a fallback. Failures include the captured QEMU output.

## Unit Tests

Run `./build.sh unit-test` for native host tests of USTAR, FAT16 reads and writes, VFS, ELF loading, terminal rendering, syscall dispatch, program-argument parsing, calendar conversion, file-syscall error codes and `Stat`, Spawn environment validation, `init.conf` `env` lines, and the C library's integer parsing, sort, and environment table. CMake fetches the pinned Catch2 `v3.7.1` release on the first run, then CTest runs the cases from `build/unit-tests-ninja`. These tests use an in-memory block device; QEMU remains responsible for exercising real PCI/MMIO and boot integration.

## Initramfs

The ISO build packages `initramfs/hello.txt` as `build/<arch>/initramfs.tar` and adds it as a Limine module on both architectures. The kernel mounts the read-only USTAR archive; the shared VFS supports open, read, seek, stat, and close over it. The build also creates `build/<arch>/virtio-disk.img` with an MBR-partitioned FAT16 volume holding a known `BLOCK.TXT`, a long-named `Read Me First.txt`, and an empty `home` directory (`DISK_LAYOUT=gpt` makes a GPT disk with a protective MBR and a backup table instead); x86-64 attaches the disk through virtio-PCI and AArch64 through virtio-MMIO. Normal startup reads and verifies the test file through VFS. Run `./build.sh --arch x86_64 test` or `./build.sh --arch aarch64 test` to check the full boot path.

Both virtio-blk drivers read and write single sectors (`BlockDevice::ReadSector`/`WriteSector`). Each driver serializes requests with a lock, because one request buffer serves every CPU, and writes fail when the device reports itself read-only. `STORAGE_TEST=ON ./build.sh --arch <arch> test` boots the same disk image twice. On each boot the kernel reads a boot counter from sector 2047, the last sector before the FAT16 partition. It then writes the counter plus one and a byte pattern, checks that a write past the end of the disk fails, and reads the sector back. The second boot must report `boot count 2`, which shows that the first boot's write persisted. The ISO build recreates the disk image, so every test starts from a fresh disk.

`src/kernel/storage/partition.cpp` finds partitions. When the MBR holds a protective `0xEE` entry, it reads the GUID partition table and checks the header and entry-array CRC32s; a damaged table yields no partitions rather than falling back to the MBR. Otherwise it uses the MBR's FAT16 entries. FAT16 mounts the first partition with a valid boot sector.

FAT16 supports subdirectories and VFAT long names, such as `/home/Project Files/Meeting Notes.txt`; `.` and `..` work, and `..` at the root stays at the root. Each path component matches an entry's long name or its 8.3 short name, ignoring case. Names are up to 63 printable ASCII characters without `\ / : * ? " < > |` and may not end in a space or dot. A name that fits 8.3 with a single case in each part is stored as a short entry alone, with lower case recorded in the Windows NT case bits so `notes.txt` lists as `notes.txt`. Any other name gets long-name entries, which `src/kernel/filesystem/fat_names.cpp` encodes and decodes, followed by a generated, unique `BASIS~N` short alias in consecutive free slots; a subdirectory grows when there is no such run. A long name counts only when its sequence is complete and its checksum matches the short entry after it; otherwise the short name is used, and non-ASCII characters read as `?`. Removing a file also marks its long-name entries deleted. `VFS::Create`, `Write`, `Truncate` (shrink only), `Remove`, `MakeDirectory`, and `RemoveDirectory` work on disk paths, while the initramfs under `/system` stays read-only. A directory entry is located by its directory's first cluster (0 for the fixed-size root) and its index. New directories get `.` and `..` entries in a zeroed cluster, which is written before the parent links it, and a full subdirectory grows by one cluster. Only empty directories can be removed, and `Open` and `Remove` refuse directories. Writes may start anywhere up to the end of the file, so files grow only at their end. New clusters are zeroed before use, every FAT copy is updated, and walking a cluster chain is bounded so a looping chain fails instead of hanging. A write that fills the disk stops with the bytes written so far, and the file size covers exactly those bytes. Updates are ordered so an interrupted operation can only leak clusters: data and FAT entries are written before the directory entry that references them, and a directory entry is cleared before its clusters are freed. A `VFS::FileHandle` caches the file's size, so a second handle to the same file does not see the first one's changes, and the filesystem has no lock yet, so only one task may change it at a time. In `STORAGE_TEST`, each boot also checks the previous boot's `PERSIST.TXT`, rewrites it with 200 lines spread over several clusters, and creates and removes `SCRATCH.TMP` to check that its clusters come back. Each `STORAGE_TEST` boot then drives the shell. Boot 1 writes `notes.txt` with `write` and `write -a`, and reads it with `cat` and `ls`. It also reads `Read Me First.txt`, which mtools wrote with a long name, and writes `My Plans.txt`, which needs one. Boot 1 also creates `docs/` with `docs/plan.txt`. Boot 2 must still `cat` all three files after the restart, using a differently cased long name for `My Plans.txt`, checks that `rmdir` refuses the non-empty `docs/`, then removes both files and the directory and checks that `cat`, a second `rm`, and `ls` see them gone. After each boot, the smoke test reads the disk image with mtools: it must see the same `PERSIST.TXT`, no scratch file, `NOTES.TXT`, `DOCS/PLAN.TXT`, and `My Plans.txt` after boot 1 and none of them after boot 2, and matching FAT copies.

## User Programs And Syscalls

The CMake build compiles each program listed in `src/user/CMakeLists.txt` (`src/user/<name>.cpp`) with the matching freestanding cross-compiler and architecture-specific user linker script into `build/<arch>/programs/<name>-<arch>.elf`, and ISO generation packages every program there in the initramfs. x86-64 programs are built with `-mgeneral-regs-only` because context switches do not save SSE state. Each ELF has separate RX, RO, and RW load segments. `src/kernel/process.cpp` loads a program into its own address space, maps a user stack, and enters ring 3 on x86-64 or EL0 on AArch64. A CPU exception in user mode terminates only that program; the kernel reports it with the `ExitStatusFault` or `ExitStatusKernelAccessFault` status from `src/user/syscall_abi.h`.

`src/user/syscall_abi.h` defines the syscall ABI shared by the kernel and programs, and `src/user/syscall.h` wraps it. x86-64 enters through `int 0x80` with the number in `rax` and arguments in `rdi`, `rsi`, and `rdx`; AArch64 uses `svc #0` with the number in `x8` and arguments in `x0`-`x2`. The result returns in `rax` or `x0`. Values at or above `SyscallErrorBase` are errors: as on Linux, the top 4095 values are reserved, which no size, address or process ID reaches. Each error names a cause (`SyscallErrorBadDescriptor`, `SyscallErrorBadAddress`, `SyscallErrorReadOnly`, `SyscallErrorExists`, `SyscallErrorNotDirectory`, and so on), which the C library maps to an `errno` value.

| Number | Syscall | Behavior |
|---|---|---|
| 0 | `Exit(status)` | Ends the program with an exit status |
| 1 | `WriteCharacter(character)` | Writes printable ASCII or `\n` to the console |
| 2 | `ReadLine(buffer, capacity, prompt)` | Shows the prompt and blocks until Enter; only the foreground process may read |
| 3 | `Spawn(command_line, flags, environment)` | Starts the program named by the first word and returns its process ID; `SpawnBackground` leaves the console with the caller, and `SpawnSessionLeader` makes the child a session leader. `environment` is the child's environment block, or null for an empty one |
| 4 | `Wait(process_id, status_pointer, flags)` | Blocks until a child exits, stores its status, and releases it; with `WaitNoHang`, returns `SyscallErrorWouldBlock` while the child runs |
| 5 | `ReadClock()` | Returns the current UTC time in seconds since 1970-01-01 |
| 6 | `ExtendHeap(increment)` | Grows the program break by `increment` bytes and returns the previous break; `0` returns the current one |
| 7 | `Sleep(milliseconds)` | Blocks for at least that long (rounded up to timer ticks, at most one day) without using a CPU |
| 8 | `ReadUptime()` | Returns milliseconds since the kernel's timer started |
| 9 | `CurrentCpu()` | Returns the index of the CPU running the caller; it can change at any preemption |
| 10 | `Open(path, flags)` | Opens a file and returns a descriptor; flags are `OpenRead`, `OpenWrite`, `OpenCreate`, `OpenTruncate`, `OpenAppend` |
| 11 | `Read(descriptor, buffer, length)` | Reads from the descriptor's offset and returns the byte count; `0` at end of file. On descriptors 0–2, returns one console line |
| 12 | `Write(descriptor, buffer, length)` | Writes at the descriptor's offset and returns the byte count, which is short if the disk fills |
| 13 | `Close(descriptor)` | Releases the descriptor |
| 14 | `Seek(descriptor, offset)` | Moves the offset, which may be at most the file size; `SyscallErrorNotSeekable` on the console |
| 15 | `Remove(path)` | Deletes a file on the disk |
| 16 | `ReadDirectory(path, index, entry)` | Fills a `DirectoryEntry` (name, directory flag, size) for the index-th entry of a directory (`/` lists `system` first); `SyscallErrorNotFound` past the last entry and for a missing directory (`Stat` tells them apart), `SyscallErrorNotDirectory` for a file |
| 17 | `MakeDirectory(path)` | Creates a directory on the disk |
| 18 | `RemoveDirectory(path)` | Removes an empty directory |
| 19 | `ChangeDirectory(path)` | Sets the caller's working directory |
| 20 | `CurrentDirectory(buffer, capacity)` | Copies the working directory and returns its length |
| 21 | `Shutdown(action)` | Powers off (`ShutdownPowerOff`) or restarts (`ShutdownRestart`); init and session leaders only, otherwise `SyscallErrorNotPermitted` |
| 22 | `Stat(path, status)` | Fills a `FileStatus`: size, type (regular, directory, or console), and whether it is read-only |
| 23 | `StatDescriptor(descriptor, status)` | The same for an open descriptor |
| 24 | `ProcessId()` | Returns the caller's process ID |
| 25 | `ParentProcessId()` | Returns its parent's process ID, `0` for init |

The kernel resolves every user pointer through the caller's page tables before touching it, so unmapped, read-only, or kernel addresses fail with `SyscallErrorBadAddress` instead of faulting. Blocking syscalls re-enable interrupts and yield to other tasks while they wait.

File descriptors live in a per-process table in `src/kernel/files.cpp`. Numbering starts at 3, and each process may hold 16 at once (`SyscallErrorTooManyOpenFiles` after that). Descriptors 0–2 are the console in every process, and each is readable and writable, like a terminal. Writing prints the bytes as one unit, so another program's output cannot land in the middle; tab prints as a space, bytes above `0x7F` as `?`, and other control bytes are dropped. Reading waits for a line, as `ReadLine` does, and only the foreground process may read (`SyscallErrorNotForeground` otherwise). It returns the line with its `\n`, and keeps what does not fit the buffer for the next read. There is no end-of-file key yet. `Seek` fails with `SyscallErrorNotSeekable`, and `Close` closes one for the rest of the program. Paths are at most 63 characters, and the shell's double quotes keep spaces inside one argument (`cat "my notes.txt"`). `/` is the FAT16 disk, and the read-only initramfs is mounted at `/system`, so `/system/hello.txt` is its `hello.txt`; a disk directory named `system` in the root would be hidden by the mount. Every process has a working directory, which starts as its parent's. init starts in `/home`, or in `/` when the disk has none, so the shell does too. Relative paths in file syscalls are joined to it, and `.`, `..`, and repeated slashes collapse lexically (`src/kernel/filesystem/path.cpp`), so `/home/..` is `/`. `ChangeDirectory` accepts only existing directories, but a directory removed afterwards leaves the old path in place, and calls through it then fail. Listing `/` shows `system/` followed by the disk's root entries, and `/system` lists the initramfs files. `OpenCreate`, `OpenTruncate`, and `OpenAppend` (start at the end of the file) require `OpenWrite`, and opening an initramfs file for writing fails with `SyscallErrorReadOnly`. A disk file may have one writer or any number of readers, and an open file cannot be removed; both cases fail with `SyscallErrorBusy`. These rules exist because each descriptor caches the file's size and clusters. When a process exits, including after a fault or Ctrl+C, the kernel closes its descriptors. `Read` and `Write` check the whole user buffer first and then move data through a 512-byte kernel buffer, and a blocking `Sync::Mutex` (`src/kernel/sync/mutex.h`) keeps filesystem calls from different CPUs from interleaving.

Ctrl+C (serial byte `0x03`, or Ctrl+C on the PS/2 or virtio keyboard) interrupts the console's foreground job: the process a session leader (the shell) started, and all of its descendants. Session leaders, and init, which belongs to no session, are never interrupted; Ctrl+C at its prompt only discards the line. The console echoes `^C` and marks each process in the job, waking any that are sleeping or blocked. A marked program ends with `ExitStatusInterrupted` the next time it would resume in user mode: at its next timer tick, or when its current syscall returns (`ReadLine` returns `SyscallErrorInterrupted` first). The shell prints nothing for that status. Children never outlive their parent: when a process exits, the kernel marks its remaining descendants the same way and reaps its children before the parent's own exit completes.

### Device registers

`Memory::MapDevice(physical, size)` (`src/kernel/memory/device_memory.cpp`) maps device registers uncached into the kernel half at `0xFFFFF00000000000 + physical`, so drivers reach them whichever address space is active. `PageFlags::Device` selects Device-nGnRnE memory on AArch64 and `PCD`/`PWT` on x86-64. The AArch64 UART, PL031 RTC, GICv2, and virtio-mmio devices, and the x86-64 local APIC, all use it. The AArch64 UART uses Limine's identity map only until virtual memory is initialized, so the earliest panics still print.

### Position-independent programs

User programs are built as static position-independent executables (`-fpie -Wl,-pie -Wl,--no-dynamic-linker -Wl,-z,text`), linked at 0 by `src/user/linker_<arch>.ld`. The ELF loader accepts `ET_DYN` images, adds the load bias to every segment, and applies the `RELATIVE` relocations listed under `PT_DYNAMIC` while the segments are still writable, then sets their final permissions. It rejects relocations that need symbols, a PLT, or a write into a non-writable segment. Spawned programs get a random image base; the kernel's own test programs load at `0x400000` (x86-64) or `0x0000400000000000` (AArch64).

**Dynamic-linking policy:** FeatherOS runs only static PIEs. Every program carries its own copy of the C library and `src/user/runtime.h`, and the kernel never resolves symbols. If shared libraries are added later, a user-space dynamic linker will load and relocate them, the way `ld.so` does on other systems; the kernel will keep applying only `RELATIVE` relocations, to the dynamic linker itself.

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

## C Library

`src/libc/` is FeatherOS's own C library, written from scratch with no outside code. The headers in `src/libc/include/` are plain C, wrapped in `extern "C"` for C++ through `__BEGIN_DECLS`. The implementation is freestanding C++, built by CMake as `libfeatheros_c.a` with the same flags as the programs, and every program links it. Only the compiler's freestanding headers come from elsewhere (`stdint.h`, `stddef.h`, `stdarg.h`, `stdbool.h`). Our `limits.h` adds the `long long` limits, which GCC's own leaves to the C library in C++.

A program with a `main` also links `start.cpp`, the C library's `_start`. It copies the arguments to writable memory, since the kernel maps them read-only and C lets a program modify its `argv` strings. It then runs the `.preinit_array` and `.init_array` constructors (C++ static objects and `__attribute__((constructor))` functions) and calls `exit(main(argc, argv))`. `exit` runs `atexit` handlers and static destructors newest first, then the `.fini_array` destructors, and passes the low 8 bits of the status to the kernel, as POSIX does. That also keeps a program's status apart from the kernel's fault statuses. `abort` exits with status 134, what a shell shows for `SIGABRT`, because there are no signals yet. The C++ runtime hooks are `__cxa_atexit`, `__cxa_guard_*` and `__cxa_pure_virtual`; static-local guards take no lock because programs are single-threaded.

Supported so far:
- `string.h`: the `mem*` and `str*` functions, `strtok_r` and `strerror`.
- `ctype.h`: ASCII classification and case mapping.
- `errno.h`: Linux error numbers. `errno` is `*__errno_location()`, so each thread can get its own later.
- `setjmp.h`: written in assembly per architecture in `src/libc/arch/<arch>/setjmp.cpp`.
- `stdlib.h`, integer parts:
  - `exit`, `_Exit`, `abort`, `atexit` (up to 32 handlers)
  - `abs`, `div` and their `long`/`long long` forms
  - `atoi` and the `strtol` family, with base 0, `errno` set to `ERANGE` on overflow and `EINVAL` on a bad base
  - `qsort` (heapsort, `src/libc/heap_sort.h`), `bsearch`
  - `rand`/`srand`
  - `getenv`, `setenv`, `unsetenv`
- `unistd.h`: `read`, `write` and `close` (descriptors 0–2 are the console), `getpid`, `getppid`, `_exit`, `environ`.
- `sys/stat.h`: `stat` and `fstat`. With no users yet, the mode only says whether a file can be written (`0644` or `0444`, and `0755` or `0555` for directories); the console is a character device, and times are 0.
- `sys/types.h`.

A failed call sets `errno` and returns -1. `src/libc/syscall_result.h` maps each syscall error to an `errno` value, for example `SyscallErrorBadAddress` to `EFAULT`; a background program's console read fails with `EIO`, as on Linux.

The environment comes from Spawn: `_start` loads the block the parent passed into a fixed table (`src/libc/environment_table.h`) before constructors run, and `environ` points at its entries. The table is the size Spawn accepts (1 KiB, 32 variables), so anything a program sets can be passed to its children; `setenv` fails with `ENOMEM` beyond that. Programs should not assign `environ` themselves.

The parser, the sort, and the environment table are header-only, so host unit tests cover them. `libctest one "two words"` checks the rest on both architectures, including a console read at its own `> ` prompt. It still prints through `runtime.h` until `stdio.h` exists. `env` prints the environment and uses only the C library.

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

After startup, the kernel spawns `init` (`src/user/init.cpp`), and restarts it if it ever exits. init reads `/system/init.conf` from the initramfs (`initramfs/init.conf`), with one service per line as `<policy> <command line>` and `#` comments. `once` services run to completion in order before the others start. `respawn` services run in the background. A `console` service, at most one, holds the console as a session leader. init polls its services with non-blocking `Wait`s, reports each exit as `init: <command> exited with status N`, and restarts the service; one that exits five times within ten seconds is disabled instead. An `env NAME=value` line adds a variable to the environment every service starts with; the default configuration sets `HOME=/home` and `PATH=/system`, and the shell passes its environment on to every program it runs. Malformed lines are reported and skipped, and a missing `/system/init.conf` falls back to `console shell`. The default configuration prints a startup line and runs the shell as the console service, so the shell's `exit` built-in returns to a fresh shell.

`shutdown` and `reboot` are shell built-ins over the `Shutdown` syscall (`src/kernel/power.cpp`). In order, the kernel:

1. Marks every process except the caller and its ancestors for termination, and waits up to two seconds for them to exit.
2. Takes the filesystem lock for good, so no file operation is half done.
3. Sends a virtio-blk flush when the device supports it.
4. Parks the other CPUs with an IPI.
5. Asks the firmware to act.

AArch64 uses PSCI `SYSTEM_OFF` or `SYSTEM_RESET`, through `hvc` or `smc` as the device tree's `/psci` node says (`src/kernel/arch/aarch64/power.cpp`). x86-64 reads ACPI from Limine's RSDP through the device-memory window, checking table checksums. It powers off with the FADT's PM1 control ports and the `\_S5` sleep types, found by scanning the DSDT's AML instead of interpreting it, and enables ACPI mode first if needed. It restarts through the FADT reset register when present, then the keyboard controller, then a triple fault (`src/kernel/arch/x86_64/power.cpp`). If the firmware does nothing, the kernel panics.

The shell reads lines with a prompt showing its working directory, such as `/home> `, and runs each line's first word as the initramfs program `<name>-<arch>.elf` (names are limited to 32 letters, digits, `_`, or `-`), waits for it, and prints `exit status N` for nonzero statuses. Missing programs report `Unknown command: <name>`; `help` lists the bundled programs:

- `calc [expression]` evaluates an expression such as `4 + 66 - (3 / 4)` with `+ - * /`, parentheses, unary signs, and decimals, using exact fractions (`src/user/calculator.h`) and printing up to six decimal places. Without an expression it reads one per line at a `calc> ` prompt until `exit` or `quit`.
- `date` prints the current UTC time, such as `2026-10-19 03:07:39 UTC`.
- `echo <words...>` prints its arguments.
- `fanout <n>` starts `n` (1–16) `echo` children before waiting for any of them.
- `primes <workers> [limit]` demonstrates multicore scheduling. It counts the primes below `limit` (default 10,000,000) with 1–16 worker processes, each testing every n-th odd number by trial division. Each worker reports its count and the CPUs it ran on through its exit status, and the parent prints one line per worker plus the total time. Compare `primes 1` with `primes 4` and watch the status bar's busy-CPU count.
- `sleep <ms>` sleeps and checks that the wall clock advanced by the whole seconds requested.
- `hardening` runs 46 negative checks, including `Shutdown` from an ordinary program: syscalls with kernel or invalid pointers (including a `Spawn` environment) and oversized arguments, a kernel-half ELF (`badelf`, generated by `tools/create_bad_elf.py`), children that execute the heap, write their code, read kernel memory, or use an invalid syscall (each must be terminated), and the per-process child limit.
- `aslr` prints where its stack, arguments block, and heap landed; the shell test runs it twice and requires different addresses.
- `memtest [overflow|exhaust]` grows its stack and heap and checks them, recurses into the stack guard page to show that the kernel terminates it, or grows its heap until the page limit refuses more (the shell test runs it twice and requires the same count).
- `upper` reads one line as the foreground process and prints it in upper case.
- `cd [dir]` and `pwd` are shell built-ins; `cd` alone returns to `/home`, and the prompt shows the working directory, such as `/home/docs> `.
- `env` prints the environment, one `NAME=value` per line.
- `ls [directory]` lists a directory (the working directory by default) with file sizes, marking subdirectories with `/`. `mkdir <dir>` and `rmdir <dir>` create and remove (empty) directories.
- `cat <file>` prints a file, replacing characters the console cannot show. `write [-a] <file> <text...>` writes the words and a newline to a file, replacing it or, with `-a`, appending. `rm <file>` deletes a file. Names are relative to the working directory, and `cat` also tries `/system` for a missing relative name, so `cat hello.txt` works anywhere.
- `filetest` runs 33 checks of the file syscalls: creation, multi-cluster writes, reads, seeks, appends, the one-writer and no-removing-open-files rules, bad pointers and flags, the read-only initramfs, the 16-descriptor limit, and a child that exits with a file still open.
- `uptime` prints the time since boot as `H:MM:SS`.

Every program except the boot self-test prints its usage and purpose for `<name> -h` or `<name> --help` (`ExitWithHelpIfRequested` in `src/user/runtime.h`), and the shell's `help` points to this. To add a program, create `src/user/<name>.cpp` and add `<name>:<name>` to `src/user/CMakeLists.txt`: to `FEATHEROS_MAIN_PROGRAMS` if it has a C `main(argc, argv)`, or to `FEATHEROS_PROGRAMS` if it defines `_start(const ProgramArguments*)` itself.

Interactive runs accept keys from the QEMU window on both architectures. x86-64 uses PS/2; AArch64 uses virtio-input on QEMU `virt`. The launching terminal's serial console also remains available. The kernel console (`src/kernel/console.cpp`) handles line editing for `ReadLine`: the prompt and the typed line appear inline in the scrolling text with a block cursor, like a normal terminal, and it supports printable ASCII, Backspace (also across a wrapped line), and Enter, echoing input to serial. Program output appears on both serial and the framebuffer console.

The framebuffer's top row is a status bar. Its left side names the foreground program (`kernel` before the shell starts). Its right side shows busy CPUs over online CPUs, then the UTC date and time as `Mon Oct 19 9:36 PM`. A CPU counts as busy when the scheduler's timer ticks found it running a task other than its idle task for at least half of the last second. A high-priority kernel task redraws the bar once a second and when the foreground program changes. Its clock is read from the RTC once and then advanced by timer ticks.

`CALC_TEST=ON` checks a calculator expression, an unknown command, and a blank line; `SHELL_TEST=ON` checks init's startup line and its restart of the shell after `exit`, `filetest`, `cd` and `pwd` from `/home` with relative paths, `/`, and `/system`, `libctest` with a quoted argument and a console read, `env` with the variables from `init.conf`, `--help`/`-h`, Ctrl+C on a CPU-bound `primes`, a `sleep`, `upper` and `calc` waiting for input, and the shell prompt, plus `echo` arguments, foreground input in `upper`, the interactive `calc> ` prompt and its errors, `primes 4 100000` (the count and four worker reports), a nonzero exit status, the `uptime` format, and that `date` is within two minutes of the host clock. `INPUT_TEST=ON` runs the keyboard test, including Ctrl+C at the prompt. Pass each with `./build.sh test --arch x86_64` or `--arch aarch64`.

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

The AArch64 ISO is UEFI-only and uses Limine's `BOOTAA64.EFI`. QEMU runs with the `virt` machine. Interactive runs let the guest restart and power off, so the shell's `reboot` boots again and `shutdown` closes QEMU; set `QEMU_NO_REBOOT=1` to make a reset exit QEMU instead, which helps catch a triple fault during boot. They copy serial output to the launching terminal and to `build/<arch>/serial.log` as it arrives, removing the screen-clearing control sequences the firmware and Limine send on every boot. The log covers the whole session, including the boots after a `reboot`, and each new run starts a fresh log. Set `QEMU_EFI` to an AArch64 UEFI firmware image if the script cannot find one in a common system location.

On macOS, the toolchain builder automatically selects Apple Clang, the active Xcode SDK, GNU89 host flags, and system zlib for Binutils. Build each stage separately:

```sh
./toolchains/build_toolchain.sh --binutils --arch aarch64-elf
./toolchains/build_toolchain.sh --gcc --arch aarch64-elf
```

The builder accepts `HOST_CC`, `HOST_CFLAGS`, and `SDKROOT` overrides if needed.