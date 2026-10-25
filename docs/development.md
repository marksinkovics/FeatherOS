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

Run `./build.sh unit-test` for native host tests of USTAR, FAT16 reads and writes, VFS, ELF loading, terminal rendering, syscall dispatch, program-argument parsing, calendar conversion, file-syscall error codes and `Stat`, Spawn environment validation, `init.conf` `env` lines, `Seek` origins, and the C library's integer parsing, sort, environment table, allocator, `printf` and `scanf` engines, `strftime`, and `posix_spawn` requests. CMake fetches the pinned Catch2 `v3.7.1` release on the first run, then CTest runs the cases from `build/unit-tests-ninja`. These tests use an in-memory block device; QEMU remains responsible for exercising real PCI/MMIO and boot integration.

## Initramfs

The ISO build packages `initramfs/hello.txt` as `build/<arch>/initramfs.tar` and adds it as a Limine module on both architectures. The kernel mounts the read-only USTAR archive; the shared VFS supports open, read, seek, stat, and close over it. The build also creates `build/<arch>/virtio-disk.img` with an MBR-partitioned FAT16 volume holding a known `BLOCK.TXT`, a long-named `Read Me First.txt`, and an empty `home` directory (`DISK_LAYOUT=gpt` makes a GPT disk with a protective MBR and a backup table instead); x86-64 attaches the disk through virtio-PCI and AArch64 through virtio-MMIO. Normal startup reads and verifies the test file through VFS. Run `./build.sh --arch x86_64 test` or `./build.sh --arch aarch64 test` to check the full boot path.

Both virtio-blk drivers read and write single sectors (`BlockDevice::ReadSector`/`WriteSector`). Each driver serializes requests with a lock, because one request buffer serves every CPU, and writes fail when the device reports itself read-only. `STORAGE_TEST=ON ./build.sh --arch <arch> test` boots the same disk image twice. On each boot the kernel reads a boot counter from sector 2047, the last sector before the FAT16 partition. It then writes the counter plus one and a byte pattern, checks that a write past the end of the disk fails, and reads the sector back. The second boot must report `boot count 2`, which shows that the first boot's write persisted. The ISO build recreates the disk image, so every test starts from a fresh disk.

`src/kernel/storage/partition.cpp` finds partitions. When the MBR holds a protective `0xEE` entry, it reads the GUID partition table and checks the header and entry-array CRC32s; a damaged table yields no partitions rather than falling back to the MBR. Otherwise it uses the MBR's FAT16 entries. FAT16 mounts the first partition with a valid boot sector.

FAT16 supports subdirectories and VFAT long names, such as `/home/Project Files/Meeting Notes.txt`; `.` and `..` work, and `..` at the root stays at the root. Each path component matches an entry's long name or its 8.3 short name, ignoring case. Names are up to 63 printable ASCII characters without `\ / : * ? " < > |` and may not end in a space or dot. A name that fits 8.3 with a single case in each part is stored as a short entry alone, with lower case recorded in the Windows NT case bits so `notes.txt` lists as `notes.txt`. Any other name gets long-name entries, which `src/kernel/filesystem/fat_names.cpp` encodes and decodes, followed by a generated, unique `BASIS~N` short alias in consecutive free slots; a subdirectory grows when there is no such run. A long name counts only when its sequence is complete and its checksum matches the short entry after it; otherwise the short name is used, and non-ASCII characters read as `?`. Removing a file also marks its long-name entries deleted. `VFS::Create`, `Write`, `Truncate` (shrink only), `Remove`, `MakeDirectory`, and `RemoveDirectory` work on disk paths, while the initramfs under `/system` stays read-only. A directory entry is located by its directory's first cluster (0 for the fixed-size root) and its index. New directories get `.` and `..` entries in a zeroed cluster, which is written before the parent links it, and a full subdirectory grows by one cluster. Only empty directories can be removed, and `Open` and `Remove` refuse directories. Writes may start anywhere up to the end of the file, so files grow only at their end. New clusters are zeroed before use, every FAT copy is updated, and walking a cluster chain is bounded so a looping chain fails instead of hanging. A write that fills the disk stops with the bytes written so far, and the file size covers exactly those bytes. Updates are ordered so an interrupted operation can only leak clusters: data and FAT entries are written before the directory entry that references them, and a directory entry is cleared before its clusters are freed. A `VFS::FileHandle` caches the file's size, so a second handle to the same file does not see the first one's changes, and the filesystem has no lock yet, so only one task may change it at a time. In `STORAGE_TEST`, each boot also checks the previous boot's `PERSIST.TXT`, rewrites it with 200 lines spread over several clusters, and creates and removes `SCRATCH.TMP` to check that its clusters come back. Each `STORAGE_TEST` boot then drives the shell. Boot 1 writes `notes.txt` with `write` and `write -a`, and reads it with `cat` and `ls`. It also reads `Read Me First.txt`, which mtools wrote with a long name, and writes `My Plans.txt`, which needs one. Boot 1 also creates `docs/` with `docs/plan.txt`. Boot 2 must still `cat` all three files after the restart, using a differently cased long name for `My Plans.txt`, checks that `rmdir` refuses the non-empty `docs/`, then removes both files and the directory and checks that `cat`, a second `rm`, and `ls` see them gone. After each boot, the smoke test reads the disk image with mtools: it must see the same `PERSIST.TXT`, no scratch file, `NOTES.TXT`, `DOCS/PLAN.TXT`, and `My Plans.txt` after boot 1 and none of them after boot 2, and matching FAT copies.

## User Programs And Syscalls

The CMake build compiles each program listed in `src/user/CMakeLists.txt` (`src/user/<name>.cpp`) with the matching freestanding cross-compiler and architecture-specific user linker script into `build/<arch>/programs/<name>-<arch>.elf`, and ISO generation packages every program there in the initramfs. Programs may use floating point and SIMD. The kernel never touches those registers: x86-64 builds it with `-mgeneral-regs-only` and AArch64 with `+nofp+nosimd`. So the registers keep the running program's values through interrupts and syscalls, and the scheduler only saves them into the outgoing task and loads the incoming task's on a context switch (`src/kernel/arch/floating_point.h`): FXSAVE/FXRSTOR on x86-64, and q0–q31 with FPCR and FPSR on AArch64. Each CPU enables them at boot: CR0/CR4 on x86-64, CPACR_EL1 on AArch64. A new task starts with every exception masked and rounding to nearest. x86-64 programs enter `_start` with `rsp` 8 bytes past a 16-byte boundary, as if called, which SSE code on the stack relies on. Each ELF has separate RX, RO, and RW load segments. `src/kernel/process.cpp` loads a program into its own address space, maps a user stack, and enters ring 3 on x86-64 or EL0 on AArch64. A CPU exception in user mode terminates only that program; the kernel reports it with the `ExitStatusFault` or `ExitStatusKernelAccessFault` status from `src/user/syscall_abi.h`.

`src/user/syscall_abi.h` defines the syscall ABI shared by the kernel and programs, and `src/user/syscall.h` wraps it. x86-64 enters through `int 0x80` with the number in `rax` and arguments in `rdi`, `rsi`, and `rdx`; AArch64 uses `svc #0` with the number in `x8` and arguments in `x0`-`x2`. The result returns in `rax` or `x0`. Values at or above `SyscallErrorBase` are errors: as on Linux, the top 4095 values are reserved, which no size, address or process ID reaches. Each error names a cause (`SyscallErrorBadDescriptor`, `SyscallErrorBadAddress`, `SyscallErrorReadOnly`, `SyscallErrorExists`, `SyscallErrorNotDirectory`, and so on), which the C library maps to an `errno` value.

| Number | Syscall | Behavior |
|---|---|---|
| 0 | `Exit(status)` | Ends the program with an exit status |
| 1 | `WriteCharacter(character)` | Writes printable ASCII or `\n` to the console |
| 2 | `ReadLine(buffer, capacity, prompt)` | Shows the prompt and blocks until Enter; only the foreground process may read |
| 3 | `Spawn(command_line, flags, environment)` | Starts the program named by the first word and returns its process ID; `SpawnBackground` leaves the console with the caller, and `SpawnSessionLeader` makes the child a session leader. `environment` is the child's environment block, or null for an empty one |
| 4 | `Wait(process_id, status_pointer, flags)` | Blocks until a child exits, stores its status, and releases it; with `WaitNoHang`, returns `SyscallErrorWouldBlock` while the child runs |
| 5 | `ReadClock()` | Returns the current UTC time in seconds since 1970-01-01 |
| 6 | `ExtendHeap(increment)` | Moves the program break by `increment` bytes, a signed value, and returns the previous break; `0` returns the current one |
| 7 | `Sleep(milliseconds)` | Blocks for at least that long (rounded up to timer ticks, at most one day) without using a CPU |
| 8 | `ReadUptime()` | Returns milliseconds since the kernel's timer started |
| 9 | `CurrentCpu()` | Returns the index of the CPU running the caller; it can change at any preemption |
| 10 | `Open(path, flags)` | Opens a file and returns a descriptor; flags are `OpenRead`, `OpenWrite`, `OpenCreate`, `OpenTruncate`, `OpenAppend` |
| 11 | `Read(descriptor, buffer, length)` | Reads from the descriptor's offset and returns the byte count; `0` at end of file. On descriptors 0–2, returns one console line |
| 12 | `Write(descriptor, buffer, length)` | Writes at the descriptor's offset and returns the byte count, which is short if the disk fills |
| 13 | `Close(descriptor)` | Releases the descriptor |
| 14 | `Seek(descriptor, offset, whence)` | Moves the offset relative to `SeekFromStart`, `SeekFromCurrent` or `SeekFromEnd` (the offset is signed) and returns the new one, which may be at most the file size; `SyscallErrorNotSeekable` on the console |
| 15 | `Remove(path)` | Deletes a file on the disk |
| 16 | `ReadDirectory(path, index, entry)` | Fills a `DirectoryEntry` (name, directory flag, size) for the index-th entry of a directory (`/` lists `system` first); `SyscallErrorNotFound` past the last entry and for a missing directory (`Stat` tells them apart), `SyscallErrorNotDirectory` for a file |
| 17 | `MakeDirectory(path)` | Creates a directory on the disk |
| 18 | `RemoveDirectory(path)` | Removes an empty directory |
| 19 | `ChangeDirectory(path)` | Sets the caller's working directory |
| 20 | `CurrentDirectory(buffer, capacity)` | Copies the working directory and returns its length |
| 21 | `Shutdown(action)` | Powers off (`ShutdownPowerOff`) or restarts (`ShutdownRestart`); init and session leaders only, otherwise `SyscallErrorNotPermitted` |
| 22 | `Stat(path, status)` | Fills a `FileStatus`: size, type (regular, directory, console, or device), and whether it is read-only |
| 23 | `StatDescriptor(descriptor, status)` | The same for an open descriptor |
| 24 | `ProcessId()` | Returns the caller's process ID |
| 25 | `ParentProcessId()` | Returns its parent's process ID, `0` for init |
| 26 | `Duplicate(descriptor, target, flags)` | Makes another descriptor for the same open file: the lowest free one at or above `target`, or exactly `target` with `DuplicateExact` (closing what it named, as `dup2` does); `DuplicateCloseOnExec` marks the copy |
| 27 | `DescriptorFlags(descriptor, set, flags)` | Returns the descriptor's flags (`DescriptorCloseOnExec`), after setting them to `flags` if `set` is nonzero |
| 28 | `Pipe(descriptors, flags)` | Creates a pipe and stores its read end, then its write end, as two `uint64_t`s; `PipeCloseOnExec` marks both |
| 29 | `ProcessGroup(process_id)` | Returns the process group of the process, or of the caller for 0 |
| 30 | `ForegroundGroup(set, group)` | Returns the group that may read the console, after moving it to `group` if `set` is nonzero; only init and session leaders may move it, to their own group or one holding one of their children |
| 31 | `Kill(target, signal)` | Sends `signal` to a process, the caller's group (0), every process but init and the caller (-1), or group `-target`; signal 0 only checks the target exists. init cannot be signaled (`SyscallErrorNotPermitted`) |
| 32 | `SignalAction(signal, new_action, old_action)` | Reads and sets a `SignalAction`: `SignalDefault`, `SignalIgnore`, or a handler with a mask, flags (`SignalResetHandler`, `SignalNoDefer`) and the restorer it returns to |
| 33 | `SignalMask(how, set)` | Blocks, unblocks or sets the blocked signals, and returns the previous mask; `SIGKILL` cannot be blocked |
| 34 | `SignalReturn()` | Made by the restorer: puts back what a handler interrupted |
| 35 | `Pause()` | Waits for a signal, then returns `SyscallErrorInterrupted` |
| 36 | `TerminalSize()` | Returns the console's text rows in bits 32–63 and columns in bits 0–31: the area below the status bar, or 24×80 without a framebuffer |
| 37 | `TerminalMode(new_settings, old_settings)` | Reads and sets the console's `TerminalSettings`: canonical lines or raw bytes, echo, Ctrl+C as `SIGINT`, Enter as `\n`, and a raw read's minimum and timeout. Only the foreground group may set them (`SyscallErrorNotForeground`); see Shell And Console Input |

The kernel resolves every user pointer through the caller's page tables before touching it, so unmapped, read-only, or kernel addresses fail with `SyscallErrorBadAddress` instead of faulting. Blocking syscalls re-enable interrupts and yield to other tasks while they wait.

File descriptors live in a per-process table of 32 in `src/kernel/files.cpp` (`SyscallErrorTooManyOpenFiles` past that), and a new descriptor takes the lowest free number. Each names an open file, which keeps the offset and the access mode and closes when its last descriptor does: `Duplicate` and `Spawn` share it, so a copy moves the same offset, as on Unix. A child inherits every descriptor not marked close-on-exec (`OpenCloseOnExec`, `DuplicateCloseOnExec`), so the console reaches it through 0–2 unless the parent put other files there. `OpenAppend` makes every write go to the end, under the filesystem lock. Descriptors 0–2 start as the console, one open file that is readable and writable, like a terminal. A pipe (`src/kernel/pipe.cpp`) is a 4 KiB ring buffer with a read end and a write end. A read waits for data and returns what is there; once every write end is closed, it returns 0. A write waits for room and puts down pieces of up to 512 bytes whole, so writers never interleave within one; with no read end left, it fails with `SyscallErrorBrokenPipe`. Both waits end with `SyscallErrorInterrupted` after Ctrl+C, and neither holds the filesystem lock. Writing prints the bytes as one unit, so another program's output cannot land in the middle; tab prints as a space, bytes above `0x7F` as `?`, and control bytes other than ESC, `\r` and `\b` are dropped. Reading waits for a line, as `ReadLine` does, and only the foreground process may read (`SyscallErrorNotForeground` otherwise). It returns the line with its `\n`, and keeps what does not fit the buffer for the next read. There is no end-of-file key yet. `Seek` on it fails with `SyscallErrorNotSeekable`, and `Duplicate` can put it back on a descriptor that was closed. Paths are at most 63 characters, and the shell's double quotes keep spaces inside one argument (`cat "my notes.txt"`). `/` is the FAT16 disk, and the read-only initramfs is mounted at `/system`, so `/system/hello.txt` is its `hello.txt`. Three virtual filesystems are mounted beside it (see Virtual Files below): `/dev`, `/proc` and `/tmp`. A disk directory with one of these four names in the root would be hidden by the mount. Every process has a working directory, which starts as its parent's. init starts in `/home`, or in `/` when the disk has none, so the shell does too. Relative paths in file syscalls are joined to it, and `.`, `..`, and repeated slashes collapse lexically (`src/kernel/filesystem/path.cpp`), so `/home/..` is `/`. `ChangeDirectory` accepts only existing directories, but a directory removed afterwards leaves the old path in place, and calls through it then fail. Listing `/` shows `system/`, `dev/`, `proc/` and `tmp/`, followed by the disk's root entries, and `/system` lists the initramfs files. `OpenCreate`, `OpenTruncate`, and `OpenAppend` (start at the end of the file) require `OpenWrite`, and opening an initramfs file for writing fails with `SyscallErrorReadOnly`. A disk or `/tmp` file may have one writer or any number of readers, and an open file cannot be removed; both cases fail with `SyscallErrorBusy`. These rules exist because each descriptor caches the file's size and clusters. When a process exits, including after a fault or Ctrl+C, the kernel closes its descriptors. `Read` and `Write` check the whole user buffer first and then move data through a 512-byte kernel buffer, and a blocking `Sync::Mutex` (`src/kernel/sync/mutex.h`) keeps filesystem calls from different CPUs from interleaving.

Ctrl+C (serial byte `0x03`, or Ctrl+C on the PS/2 or virtio keyboard) interrupts the console's foreground group. Every process belongs to a process group: a child joins its parent's group unless `Spawn` gets `SpawnSetGroup`, which names another group (the caller's, or one holding one of its children) or, with group 0, starts a new one that the child leads; a session leader always leads a new group. A child's group takes the console unless it is spawned with `SpawnBackground`, and only processes in the foreground group may read it (`SyscallErrorNotForeground` otherwise). When the foreground group's last process is reaped, the console returns to the group of the process that waited. `ForegroundGroup` lets init and session leaders move it, as `tcsetpgrp` does. Ctrl+C marks every process in the foreground group except session leaders and init, so Ctrl+C at the shell's own prompt only discards the line. The console echoes `^C` and sends `SIGINT` to each process in the group (see Signals below). The shell prints nothing for a command `SIGINT` ended. Children never outlive their parent: when a process exits, the kernel sends `SIGKILL` to its remaining descendants and reaps its children before the parent's own exit completes.

### Signals

Each program has pending and blocked signal sets and an action per signal (`src/kernel/signals.cpp`), with Linux's numbers; the stop signals 19-22 do not exist, since processes cannot be stopped. A signal is sent by `Kill`, by Ctrl+C (`SIGINT`), by a dying parent (`SIGKILL`), by a write to a pipe with no reader (`SIGPIPE`, besides `SyscallErrorBrokenPipe`), and by a child's exit (`SIGCHLD` to its parent). An ignored signal, or one whose default is to be ignored (`SIGCHLD`, `SIGCONT` and two others), is dropped when sent; the others stay pending until the program returns to user mode, after a syscall or when a timer tick preempts it. Blocking calls (`Sleep`, `Wait`, `Pause`, console and pipe reads, pipe writes) give up with `SyscallErrorInterrupted` once a signal they would take is pending. The program then takes the lowest pending signal it does not block. The default action ends it with exit status `ExitStatusSignalBase + signal`, which `waitpid` reports as `WIFSIGNALED`. A handler runs on the program's own stack: the kernel saves the interrupted registers, its floating-point state (FXSAVE, or q0-q31 with FPCR and FPSR) and the signal mask in a frame below the stack pointer, past a 128-byte red zone, blocks the signal and the action's mask, and enters the handler with the signal number as its argument, returning to the restorer the C library registered. The restorer makes `SignalReturn`, which puts everything back, refusing kernel addresses, flags outside the user-changeable ones, and MXCSR or FPCR/FPSR bits that would fault or are undefined; a frame that cannot be written or read ends the program with `SIGSEGV`. CPU exceptions still end a program as `SIGSEGV` without running a handler. Children start with every handler back at its default action, while ignored signals and the blocked mask carry over, as across exec.

### Virtual Files

The VFS (`src/kernel/filesystem/vfs.cpp`) sends each path to one of five filesystems by its first component, and each open file records which kind it is: archive, disk, device, memory or generated.

- **`/dev`** (`devices.cpp`) holds `null`, whose reads end at once; `zero`, which reads as zero bytes; and `random`, which reads from `Random::Next`, mixing `RDRAND`/`RNDR` with the cycle counter. All three take and discard writes, report size 0 and file type `Device` (`S_IFCHR`), and accept any seek. Nothing else can be created in `/dev`.
- **`/tmp`** (`tmpfs.cpp`) is a RAM filesystem with nested directories and case-sensitive names, up to 63 characters. It holds at most 128 files and directories and 4 MiB of data, on the kernel heap, and a write past that is short. Its contents last until restart. Open files follow the disk's rules (one writer or many readers, and no removing an open file), and `rmdir /tmp` fails with `SyscallErrorBusy`, like `/`.
- **`/proc`** (`procfs.cpp`) is read-only. It lists `meminfo`, `stat`, and one directory per process, by ascending ID, each holding `status`. A file's text is made when it is opened, so a reader sees one snapshot.
  - `status` gives `Name`, `State` (`R`, `S` or `Z`, as on Linux), `Pid`, `PPid`, `Pgid`, `Pages` mapped, `PageLimit`, `OpenFiles`, `CpuTimeMs` (the timer ticks the process ran for, on any CPU), `LastCpu` and `StartTimeMs` (uptime when it was spawned).
  - `stat` gives `UptimeMs`, `Cpus`, and each CPU's busy time as `Cpu<n>BusyMs`, the ticks it ran a task rather than idling.
  - `meminfo` gives `MemTotal` and `MemFree` from the page allocator, the kernel heap's mapped size (`KernelHeap`) and the memory `/tmp` holds (`Tmp`), in kB.
  - The file system takes its data from `process.cpp` through callbacks (`Procfs::SetSource`), so the host tests can supply their own.
  - `/proc/self` is rewritten to `/proc/<pid>` when a syscall resolves a path (`Path::ReplaceProcSelf`).

### Device registers

`Memory::MapDevice(physical, size)` (`src/kernel/memory/device_memory.cpp`) maps device registers uncached into the kernel half at `0xFFFFF00000000000 + physical`, so drivers reach them whichever address space is active. `PageFlags::Device` selects Device-nGnRnE memory on AArch64 and `PCD`/`PWT` on x86-64. The AArch64 UART, PL031 RTC, GICv2, and virtio-mmio devices, and the x86-64 local APIC, all use it. The AArch64 UART uses Limine's identity map only until virtual memory is initialized, so the earliest panics still print.

### Position-independent programs

User programs are built as static position-independent executables (`-fpie -Wl,-pie -Wl,--no-dynamic-linker -Wl,-z,text`), linked at 0 by `src/user/linker_<arch>.ld`. The ELF loader accepts `ET_DYN` images, adds the load bias to every segment, and applies the `RELATIVE` relocations listed under `PT_DYNAMIC` while the segments are still writable, then sets their final permissions. It rejects relocations that need symbols, a PLT, or a write into a non-writable segment. Spawned programs get a random image base; the kernel's own test programs load at `0x400000` (x86-64) or `0x0000400000000000` (AArch64).

**Dynamic-linking policy:** FeatherOS runs only static PIEs. Every program carries its own copy of the C library, and the kernel never resolves symbols. If shared libraries are added later, a user-space dynamic linker will load and relocate them, the way `ld.so` does on other systems; the kernel will keep applying only `RELATIVE` relocations, to the dynamic linker itself.

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

User memory grows on demand within the per-process page limit. The stack occupies a 64-page region ending at `Process::UserStackTop`: one page is mapped at load, and a not-present user page fault inside the region maps another zeroed page and resumes the program. The region's lowest page is never mapped, so a stack overflow faults there and terminates only that program. The heap starts at the page after the highest ELF segment; `ExtendHeap` maps whole pages eagerly up to the stack region's guard page. A negative increment lowers the break, no further than the heap's start. It unmaps every whole page above the new break and frees its frame, so the pages count against the limit again; the page holding the new break keeps its contents. Only the current CPU's TLB is flushed. That is enough because a process runs on one CPU at a time, and every switch reloads the page-table root, which drops the old entries: CR3 on x86-64, where user pages are not global and PCIDs are unused, and `TTBR0_EL1` with `tlbi vmalle1` on AArch64. Threads sharing an address space will need a TLB shootdown.

At entry, each program receives a pointer to a read-only `FeatherOS::User::ProgramArguments` block containing its process ID, the full command line, and `argument_count`/`argument_values` (the line split at spaces, at most 16 arguments in 63 characters). After a program exits, `Memory::DestroyAddressSpace` frees its user-mapped frames, private page-table frames, and root frame; it refuses to destroy an active address space and preserves shared kernel mappings. QEMU memory tests verify that free-frame accounting returns to its pre-test value on both architectures.

Normal boot first runs `src/user/test_program.cpp` twice as scheduled tasks to verify console output, exit statuses, and that user mode cannot read a mapped kernel address.

## C Library

`src/libc/` is FeatherOS's own C library, written from scratch with no outside code. The headers in `src/libc/include/` are plain C, wrapped in `extern "C"` for C++ through `__BEGIN_DECLS`. The implementation is freestanding C++, built by CMake as `libfeatheros_c.a` with the same flags as the programs, and every program links it. Only the compiler's freestanding headers come from elsewhere (`stdint.h`, `stddef.h`, `stdarg.h`, `stdbool.h`). Our `limits.h` adds the `long long` limits, which GCC's own leaves to the C library in C++. Every program in `src/user/` is written against it, except the kernel self-test `test_program`. [posix.md](posix.md) lists every function, how it differs from POSIX, and what is left out on purpose; this section covers how it works.

A program with a `main` also links `start.cpp`, the C library's `_start`. It copies the arguments to writable memory, since the kernel maps them read-only and C lets a program modify its `argv` strings. It then runs the `.preinit_array` and `.init_array` constructors (C++ static objects and `__attribute__((constructor))` functions) and calls `exit(main(argc, argv))`. `exit` runs `atexit` handlers and static destructors newest first, then the `.fini_array` destructors, and passes the low 8 bits of the status to the kernel, as POSIX does. That also keeps a program's status apart from the kernel's fault statuses. `abort` exits with status 134, what a shell shows for `SIGABRT`, because there are no signals yet. The C++ runtime hooks are `__cxa_atexit`, `__cxa_guard_*` and `__cxa_pure_virtual`; static-local guards take no lock because programs are single-threaded.

Supported so far:
- `string.h`: the `mem*` and `str*` functions, `strtok_r`, `strdup`/`strndup` and `strerror`.
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
  - `malloc`, `calloc`, `realloc`, `free`
- `unistd.h`:
  - `read`, `write`, `close` and `lseek` (descriptors 0–2 are the console)
  - `unlink`, `rmdir`, `chdir`, `getcwd`, `access`, `isatty`
  - `sleep`, `usleep`, `getpid`, `getppid`, `_exit`, `environ`
- `fcntl.h`: `open` with `O_RDONLY`/`O_WRONLY`/`O_RDWR`, `O_CREAT`, `O_EXCL`, `O_TRUNC`, `O_APPEND` (every write goes to the end) and an ignored `O_CLOEXEC`; `creat`.
- `dirent.h`: `opendir`, `readdir`, `rewinddir`, `closedir`. Listings have no `.` and `..`. A stream keeps reading the same directory after a `chdir`.
- `time.h`:
  - `time`, `gmtime`/`localtime` and their `_r` forms (UTC only, from 1970 on)
  - `mktime`/`timegm`, which normalize out-of-range fields
  - `strftime` (the C locale, header-only in `src/libc/time_format.h`), `asctime`, `ctime`
  - `clock_gettime` (`CLOCK_REALTIME` to the second, `CLOCK_MONOTONIC` to the millisecond), `nanosleep`
- `spawn.h` and `sys/wait.h`:
  - `posix_spawn`/`posix_spawnp` build the kernel's one-line command from `path` and `argv`, so the program must be in `/system` and an argument cannot hold a double quote; `argv[0]` becomes the program's name.
  - File actions and attributes can be initialized but hold nothing.
  - `waitpid`/`wait` use the Linux status encoding, with a CPU exception reported as signal 11 and Ctrl+C as signal 2.
  - `waitpid(-1, …)` polls the children `posix_spawn` started every 10 ms.
- `sys/stat.h`: `stat`, `fstat`, and `mkdir`. With no users yet, the mode only says whether a file can be written (`0644` or `0444`, and `0755` or `0555` for directories); the console is a character device, and times are 0.
- `sys/types.h`.
- `stdio.h`:
  - formatting: `printf`, `fprintf`, `dprintf`, `sprintf`, `snprintf` and their `v` forms
  - streams: `fopen` (modes `r`, `w`, `a`, with `+` and an ignored `b`), `fdopen`, `fclose`, `fflush`, `setvbuf`/`setbuf` (mode only)
  - reading and writing: `fread`/`fwrite`, `fgetc`/`getc`/`getchar`, `ungetc` (one character), `fgets`, `getline`, `fputc`/`putc`/`putchar`, `fputs`, `puts`
  - position and state: `fseek`/`ftell`/`rewind`, `feof`/`ferror`/`clearerr`, `fileno`
  - also `remove` and `perror`
  - scanning: `scanf`, `fscanf`, `sscanf` and their `v` forms (`src/libc/scan.h`), with integers and the `printf` length modifiers, `%e`/`%f`/`%g`/`%a` (`float`, with `l` `double`, with `L` `long double`), `%c`, `%s`, `%[...]` sets, `%n`, `%*` and widths.
- `math.h` (`src/libc/math_functions.h`):
  - exact: `fabs`, `copysign`, `floor`, `ceil`, `trunc`, `round`, `fmod`, `frexp`, `ldexp`/`scalbn`, `modf`, `fmin`, `fmax`, and `sqrt` (the hardware instruction)
  - within 1–3 ulps of the host's results: `exp`, `exp2`, `log`, `log2`, `log10`, `pow`, `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2`, `sinh`, `cosh`, `tanh`, `hypot`
  - `float` versions of each (computed in `double`), the classification macros, `HUGE_VAL`/`INFINITY`/`NAN` and the `M_` constants
  - Each function reduces its argument to a small interval and sums a Taylor series there. `pow` keeps `y ln x` in two parts so large results stay accurate.
  - sin, cos and tan reduce by pi/2 in three exact parts and lose precision above about 10^6.
  - Errors set `errno` (`EDOM`, `ERANGE`). There are no `long double` versions.
- `assert.h`: a failed `assert` prints `file:line: function: Assertion `expression' failed.` to stderr and aborts; `NDEBUG` turns it off.

One `printf` engine (`src/libc/format.h`) serves every formatting function through a sink: a stream, a buffer, or a descriptor. It supports `d i u o x X c s p f F e E g G a A %`, the flags `- + space # 0`, width and precision (including `*`), and the length modifiers `hh h l ll j z t L`. A null string prints `(null)` and a null pointer `(nil)`, as in glibc. `%n` is not supported, since it writes through a pointer the format supplies.

Floating-point output is exact (`src/libc/float_format.h`). A binary float is an integer times a power of two, so a big integer (`src/libc/big_integer.h`) produces every decimal digit, and rounding sees all of them, with ties going to even as in glibc. The host tests compare the output with the host's `printf` digit for digit. Digits past precision 1100 print as zeros, which is exact for every `double`. `%a` rounds half to even too, and a carry may make the leading digit 2, as in glibc. `strtod`, `strtof`, `strtold` and `atof` (`src/libc/float_parse.h`) are correctly rounded the same way. They keep 800 significant digits, plus a sticky bit for later ones that still settles ties, and accept hexadecimal floats, `inf`, `infinity` and `nan(...)`. `long double` is the x87 80-bit format on x86-64 and the 128-bit IEEE format on AArch64. Both print and parse exactly. On AArch64, `long double` arithmetic and comparisons need compiler runtime routines that are not provided, so programs can only move such values, print them and parse them.

Each stream has a 512-byte buffer. stdout is line-buffered, and stderr is unbuffered, though one call's output is still written at once. Files are fully buffered. Reading from the console first flushes stdout, so a prompt without a newline appears. `exit` flushes every stream after the `atexit` handlers and destructors; `_exit` and `abort` do not. Streams in `a` mode write at the end of the file even after an `fseek`. Seeking past the end fails with `EINVAL`, and the console cannot seek (`ESPIPE`). There are 16 file streams besides the three console ones, matching the descriptor limit.

A failed call sets `errno` and returns -1. `src/libc/syscall_result.h` maps each syscall error to an `errno` value, for example `SyscallErrorBadAddress` to `EFAULT`; a background program's console read fails with `EIO`, as on Linux.

`malloc` (`src/libc/heap_allocator.h`) is a first-fit allocator over `ExtendHeap`. Each block has a 16-byte header, so payloads are 16-byte aligned. Free blocks are kept in address order, which lets `free` merge a block with free neighbors on both sides; `realloc` shrinks in place or grows into a free neighbor before it moves the data. The heap grows in 64 KiB steps, and only by what the free block at its end lacks. When a `free` or `realloc` leaves more than 256 KiB free at the end of the heap, the allocator returns all but 64 KiB of it, in whole 64 KiB steps, with a negative `ExtendHeap`; the cushion keeps a loop of large allocations from making a syscall each time. It first checks that the break is still where its memory ends, so it never takes back memory a program got with its own `sbrk`. Free space below blocks still in use stays in the heap, since only the end can move; `mmap` would lift that, as noted in [ideas.md](ideas.md). Each region ends in a marker block that is never freed, so a block never takes memory past its region for a neighbor. A header tag catches frees of pointers that did not come from `malloc`, and double frees: `free` and `realloc` then print `free: invalid pointer` and abort. C++ `new` and `delete`, including the array and sized forms, use `malloc`. With exceptions disabled, a failed `new` prints a message and aborts instead of throwing. Programs are single-threaded, so the allocator takes no lock. `ExtendHeap` checks the page limit before it maps anything, so a request that is too large fails cleanly and the heap can still grow afterwards.

The environment comes from Spawn: `_start` loads the block the parent passed into a fixed table (`src/libc/environment_table.h`) before constructors run, and `environ` points at its entries. The table is the size Spawn accepts (1 KiB, 32 variables), so anything a program sets can be passed to its children; `setenv` fails with `ENOMEM` beyond that. Programs should not assign `environ` themselves.

The parser, the sort, the environment table, the allocator, the `printf` and `scanf` engines, `strftime`, the `posix_spawn` request builder, float conversion, and the math functions are header-only, so host unit tests cover them, most against the host's own C library and `libm`. The allocator runs over an in-memory arena, including a randomized run, and the engine's output is compared with the host's `snprintf` for every flag combination. `libctest one "two words"` checks the rest on both architectures: it prints through stdio and reads three typed lines at its `> ` prompts, with `read`, `fgets(stdin)`, and `scanf`. It also covers descriptors, directories, `/dev`, `/tmp` and `/proc`, time, and `posix_spawn`, starting `echo`, `env`, a failing child, a faulting one, and children with `POSIX_SPAWN_SETSID` and `POSIX_SPAWN_SETPGROUP`. It also runs a child whose `assert` fails, and checks `sbrk`, `sched_getcpu`, that `reboot` is refused, and that `longjmp` restores AArch64's callee-saved `d8`–`d15`. Host tests also check that a line split by the shell reaches a program as the same words after `posix_spawn` joins it and the kernel splits it again. `env` prints the environment and uses only the C library.

## Scheduler And Processes

Tasks are heap-allocated, so their number is limited only by memory. Each has a 16 KiB kernel stack from `src/kernel/memory/kernel_stack.cpp`: stacks live at `0xFFFFFC0000000000` with an unmapped guard page below each, so an overflow faults instead of corrupting a neighbour. Reaped stacks return to a pool still mapped, so reuse never unmaps a kernel page and needs no TLB shootdown. On x86-64 every kernel-half PML4 entry is created at boot, so kernel mappings added later appear in every address space. Each task also keeps its own user exception state; context switches also preserve the AArch64 user stack pointer (`SP_EL0`), and x86-64 restores the TSS kernel stack after blocking syscalls. `Scheduler::Initialize` adopts the boot thread as a task and creates an idle task that halts the CPU (`hlt`/`wfi`) whenever no other task is ready. Tasks have a priority (`Low`, `Normal`, or `High`; set with `Scheduler::SetPriority`). Each CPU runs the highest-priority ready task, rotating among tasks of equal priority, and a yielding task keeps running if no ready task outranks it. Lower priorities can starve while higher ones stay ready. Tasks yield cooperatively or are preempted by periodic timer interrupts when preemption is enabled; the kernel can wait for exit statuses and reap completed tasks, which frees them.

Waiting tasks do not run. `Scheduler::Wait` blocks until the task exits, `SleepUntil` sleeps until a timer tick, and `WaitOn`/`WakeAll` block on a `WaitQueue` with an optional deadline tick. `ReadLine` uses this to block until a keyboard or UART event arrives; the timer interrupt moves UART input into the event queue. `Sync::SpinLock` (`src/kernel/sync/spinlock.h`) protects the scheduler, input queue, page allocator, heap, and process table; `LockIrqSave` and `SpinLockGuard` also mask interrupts. The scheduler lock is held across context switches: the switched-in task releases it and then restores its own interrupt state. Code that waits on a condition holds its own lock and passes it to `WaitOn`, which releases it while the task waits; subsystem locks are always taken before the scheduler lock. Process records in `src/kernel/process.cpp` are heap-allocated too; each has a parent, its own address space, and a page limit (1024 pages by default) covering its ELF segments, stack, and arguments block. Console input belongs to a foreground process group (see Ctrl+C below). Run `SCHEDULER_TEST=ON ./build.sh --arch x86_64 test` or `SCHEDULER_TEST=ON ./build.sh --arch aarch64 test` for kernel-task lifecycle checks. Spawned programs are time-sliced: they run in user mode with interrupts unmasked, and every timer tick on their CPU may switch to a higher-priority or other ready task. Kernel tasks are switched only when they block or yield, except in the scheduler test. Run `USER_PREEMPTION_TEST=ON ./build.sh --arch x86_64 test` or `USER_PREEMPTION_TEST=ON ./build.sh --arch aarch64 test` to verify timer preemption while user code is executing. The scheduler test also covers wait-queue blocking and wakeup, timed sleep, wait deadlines, and three rounds of 12 concurrent tasks without leaking page frames, priority ordering; the memory test checks that a kernel stack's guard page faults.

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

After startup, the kernel spawns `init` (`src/user/init.cpp`), and restarts it if it ever exits. init reads `/system/init.conf` from the initramfs (`initramfs/init.conf`), with one service per line as `<policy> <command line>` and `#` comments. `once` services run to completion in order before the others start. `respawn` services run in the background. A `console` service, at most one, holds the console as a session leader. init starts them with `posix_spawn`: the console service with `POSIX_SPAWN_SETSID`, and respawned services with `POSIX_SPAWN_SETPGROUP`, which FeatherOS treats as starting in the background. init polls its services with `waitpid(..., WNOHANG)`, reports each exit as `init: <command> exited with status N` (or `ended by signal N` after a fault or Ctrl+C), and restarts the service; one that exits five times within ten seconds is disabled instead. An `env NAME=value` line adds a variable to the environment every service starts with; the default configuration sets `HOME=/home` and `PATH=/system`, and the shell passes its environment on to every program it runs. Malformed lines are reported and skipped, and a missing `/system/init.conf` falls back to `console shell`. The default configuration prints a startup line and runs the shell as the console service, so the shell's `exit` built-in returns to a fresh shell.

`shutdown` and `reboot` are shell built-ins that call `reboot(RB_POWER_OFF)` and `reboot(RB_AUTOBOOT)`, over the `Shutdown` syscall (`src/kernel/power.cpp`). In order, the kernel:

1. Marks every process except the caller and its ancestors for termination, and waits up to two seconds for them to exit.
2. Takes the filesystem lock for good, so no file operation is half done.
3. Sends a virtio-blk flush when the device supports it.
4. Parks the other CPUs with an IPI.
5. Asks the firmware to act.

AArch64 uses PSCI `SYSTEM_OFF` or `SYSTEM_RESET`, through `hvc` or `smc` as the device tree's `/psci` node says (`src/kernel/arch/aarch64/power.cpp`). x86-64 reads ACPI from Limine's RSDP through the device-memory window, checking table checksums. It powers off with the FADT's PM1 control ports and the `\_S5` sleep types, found by scanning the DSDT's AML instead of interpreting it, and enables ACPI mode first if needed. It restarts through the FADT reset register when present, then the keyboard controller, then a triple fault (`src/kernel/arch/x86_64/power.cpp`). If the firmware does nothing, the kernel panics.

The shell reads lines with `featheros_read_line` and a prompt showing its working directory, such as `/home> `. It splits each line into words by the kernel's rules (`SplitCommandLine` in `src/user/command_line.h`, where double quotes group words), runs the first word as the initramfs program `<name>-<arch>.elf` with `posix_spawnp` (names are limited to 32 letters, digits, `_`, or `-`), waits for it with `waitpid`, and prints `exit status N` for nonzero statuses. Missing programs report `Unknown command: <name>`. A command's input and output can be redirected with `< file`, `> file`, `>> file`, `2> file` and `2>&1`, as its own word or joined to the file name (`>out.txt`); the shell opens the files and passes them as `posix_spawn` file actions, and a quoted `">"` is an ordinary word. Built-ins cannot be redirected. `a | b` runs up to four commands with each one's output feeding the next one's input, as in other shells, and redirections still apply to each command; the last command's status is reported. Each command line, pipeline or not, runs as one process group led by its first command, so any of them may read the console and Ctrl+C stops them all. A trailing `&` (`sleep 900 &`, or `sleep 900&`) starts it in the background instead: the shell prints `[n] pid` and returns to the prompt, and the job cannot read the console. Before each prompt the shell reaps finished jobs and reports `[n] Done`, `[n] Exit N` or `[n] Signal N` with the command. `jobs` lists running jobs, `fg [n]` gives job `n` (or the newest) the console and waits for it, and `wait` waits for every job. At most 8 jobs run at once. `help` lists the bundled programs:

- `calc [expression]` evaluates an expression such as `4 + 66 - (3 / 4)` with `+ - * /`, parentheses, unary signs, and decimals, using exact fractions (`src/user/calculator.h`) and printing up to six decimal places. Without an expression it reads one per line at a `calc> ` prompt until `exit` or `quit`.
- `date` prints the current UTC time, such as `2026-10-19 03:07:39 UTC`.
- `echo <words...>` prints its arguments.
- `fanout <n>` starts `n` (1–16) `echo` children before waiting for any of them.
- `primes <workers> [limit]` demonstrates multicore scheduling. It counts the primes below `limit` (default 10,000,000) with 1–16 worker processes, each testing every n-th odd number by trial division. Each worker reports its count and the CPUs it ran on in one line through a pipe of its own, and the parent prints one line per worker plus the total time. Compare `primes 1` with `primes 4` and watch the status bar's busy-CPU count.
- `sleep <ms>` sleeps and checks that the wall clock advanced by the whole seconds requested.
- `fptest [workers]` starts 8 workers by default, more than there are CPUs. Each fills every vector register with its own pattern, spins while the others run, and checks the pattern is intact; it fails if context switches lose floating-point state.
- `hardening` runs 48 negative checks, including `Shutdown` from an ordinary program: syscalls with kernel or invalid pointers (including a `Spawn` environment) and oversized arguments, a kernel-half ELF (`badelf`, generated by `tools/create_bad_elf.py`), children that execute the heap, touch a heap page after shrinking the heap below it, write their code, read kernel memory, or use an invalid syscall (each must be terminated), and the per-process child limit.
- `aslr` prints where its image, stack, arguments page (`featheros_arguments_page`), and heap (`sbrk(0)`) landed; the shell test runs it twice and requires different addresses.
- `memtest [overflow|exhaust]` grows its stack and heap and checks them, recurses into the stack guard page to show that the kernel terminates it, or grows its heap until the page limit refuses more, shrinks it back to its start, and checks that it grows as far again (the shell test runs it twice and requires the same count).
- `upper` reads one line as the foreground process and prints it in upper case; when its input is a file, it converts all of it.
- `cd [dir]` and `pwd` are shell built-ins; `cd` alone returns to `/home`, and the prompt shows the working directory, such as `/home/docs> `.
- `env` prints the environment, one `NAME=value` per line.
- `ls [directory]` lists a directory (the working directory by default) with file sizes, marking subdirectories with `/`. `mkdir <dir>` and `rmdir <dir>` create and remove (empty) directories.
- `cat [file]` prints a file, or its input without one; the console shows tabs as spaces and drops other control characters. `write [-a] <file> <text...>` writes the words and a newline to a file, replacing it or, with `-a`, appending. `rm <file>` deletes a file. Names are relative to the working directory, and `cat` also tries `/system` for a missing relative name, so `cat hello.txt` works anywhere.
- `filetest` runs 33 checks of the file syscalls: creation, multi-cluster writes, reads, seeks, appends, the one-writer and no-removing-open-files rules, bad pointers and flags, the read-only initramfs, the 16-descriptor limit, and a child that exits with a file still open.
- `uptime` prints the time since boot as `H:MM:SS`.
- `kill [-signal] <pid>...` sends `SIGTERM`, or the named or numbered signal, to each process, or to a group as `-group`; `kill -l` lists the signals. In the shell, `kill %n` names job `n`.
- `keys` switches the console to raw input and prints the name of each key pressed (`up`, `ctrl-a`, `page-down`, `escape`, …), Ctrl+C included, until `q`. It decodes keys with `src/user/key_decoder.h`, which also reads the SS3 and `ESC [ 1 ~` forms some terminals send, and waits a tenth of a second to tell a lone Escape from the start of a sequence.
- `ps` lists every process from `/proc`: ID, parent, group, state, the CPU it last ran on, its CPU time, and its name.
- `top [-n count] [-d milliseconds]` samples `/proc` every second (or `-d`, at least 100 ms) and prints the processes busiest first, with `%CPU` of one CPU and the whole system's use, until Ctrl+C or `count` refreshes. Each refresh prints a new table, since the console cannot redraw yet.

Every program except the boot self-test prints its usage and purpose for `<name> -h` or `<name> --help` (`HelpRequested` in `src/user/command_line.h`), and the shell's `help` points to this. To add a program, create `src/user/<name>.cpp` with a `main(argc, argv)` and add `<name>:<name>` to `FEATHEROS_PROGRAMS` in `src/user/CMakeLists.txt`. `FEATHEROS_START_PROGRAMS` is for programs that define `_start(const ProgramArguments*)` themselves and skip the C library's startup, which only `test_program` does. `filetest` and `hardening` still call `src/user/syscall.h` directly where they test the kernel's exact errors.

Interactive runs accept keys from the QEMU window on both architectures. x86-64 uses PS/2; AArch64 uses virtio-input on QEMU `virt`. The launching terminal's serial console also remains available. Program output appears on both serial and the framebuffer console.

Input reaches the console as one byte stream (`src/kernel/input/input.cpp`), in the form a VT100 terminal sends it. Serial bytes arrive as typed, with `\r\n` counted once. The keyboard drivers only turn scancodes into key codes (PS/2 set 1, including its `E0` keys, or virtio-input codes, which match for the main keys). `src/kernel/input/keyboard.cpp` then maps them for both architectures, with the US layout, Shift, Caps Lock and Ctrl:

| Key | Bytes |
| --- | --- |
| Enter, Backspace, Tab, Escape | `\r`, `0x7F`, `\t`, `ESC` |
| Ctrl+A to Ctrl+Z, Ctrl+[ \ ] | 1 to 26, 27 to 29 |
| Up, Down, Right, Left | `ESC [ A`, `B`, `C`, `D` |
| Home, End | `ESC [ H`, `ESC [ F` |
| Insert, Delete, Page Up, Page Down | `ESC [ 2 ~`, `3 ~`, `5 ~`, `6 ~` |
| Shift+Tab | `ESC [ Z` |

Ctrl+C (byte 3) goes to the console's interrupt handler first, in order with the bytes around it, and is queued only if the handler declines it. The console has terminal settings (`TerminalMode`, behind `tcgetattr` and `tcsetattr`), which start as canonical input with echo, signals, and Enter as `\n`:
- **Canonical:** `ReadLine` and console reads edit a line: the prompt and the typed line appear inline in the scrolling text with a block cursor, like a normal terminal. Printable ASCII is inserted, Backspace erases (also across a wrapped line), and Enter ends the line. Escape sequences are skipped, so the cursor keys do nothing yet. Input is echoed to serial and the screen unless echo is off.
- **Raw** (canonical off): a read returns bytes as they arrive, without editing, once `minimum` bytes are there. `timeout`, in tenths of a second, limits the wait for the first byte when `minimum` is 0, and the wait between bytes otherwise; with both 0 a read returns at once. Echo, when on, prints what arrives.
- **Signals:** with them off, Ctrl+C arrives as byte 3 instead of sending `SIGINT`.

Only the foreground group may change the settings. The process that changed them owns them: when it exits, even after a fault or `SIGKILL`, the console returns to the defaults and the terminal to normal video with the cursor shown, so a crashed full-screen program cannot leave the shell in raw mode.

The framebuffer's top row is a status bar. Its left side names the foreground program (`kernel` before the shell starts). Its right side shows busy CPUs over online CPUs, then the UTC date and time as `Mon Oct 19 9:36:05 PM`. A CPU counts as busy when the scheduler's timer ticks found it running a task other than its idle task for at least half of the last second. A high-priority kernel task redraws the bar once a second and when the foreground program changes. Its clock is read from the RTC once and then advanced by timer ticks.

Below the bar, the console understands a VT100 subset, so programs can draw full screens (`src/kernel/framebuffer/term.cpp`). Row 1 of `CSI row;column H` is the first row below the bar, and moves stop at the screen's edges.

| Sequence | Effect |
| --- | --- |
| `CSI n A`, `B`, `C`, `D` | Cursor up, down, right, left by `n` (default 1) |
| `CSI n G`, `CSI n d` | Cursor to column `n`, to row `n` |
| `CSI row;column H` or `f` | Cursor to a position (default 1;1) |
| `CSI n J` | Erase below the cursor (0), above it (1), or the whole text area (2) |
| `CSI n K` | Erase to the end of the line (0), to its start (1), or the whole line (2) |
| `CSI 7 m`, `CSI 27 m`, `CSI 0 m` | Reverse video on, off, off |
| `CSI s`, `CSI u`, `ESC 7`, `ESC 8` | Save and restore the cursor |
| `CSI ?25 l`, `CSI ?25 h` | Hide and show the cursor |
| `\r`, `\b` | Column 0, one column left |

Other sequences are read and ignored. Serial output carries the same bytes, so a terminal emulator on the serial port follows them too. `ioctl(TIOCGWINSZ)` reports the text area's size.

`CALC_TEST=ON` checks a calculator expression, an unknown command, and a blank line; `SHELL_TEST=ON` checks init's startup line and its restart of the shell after `exit`, `filetest`, `cd` and `pwd` from `/home` with relative paths, `/`, and `/system`, `ls /dev`, a file written to `/tmp`, `/proc/self/status` and `/proc/meminfo`, `fptest 8`, `libctest` with a quoted argument and a console read, `env` with the variables from `init.conf`, `--help`/`-h`, Ctrl+C on a CPU-bound `primes`, a `sleep`, `upper` and `calc` waiting for input, and the shell prompt, plus `echo` arguments, foreground input in `upper`, the interactive `calc> ` prompt and its errors, `primes 4 100000` and `primes 16 20000` (the counts and one report per worker, through pipes), a nonzero exit status, the `uptime` format, and that `date` is within two minutes of the host clock. `INPUT_TEST=ON` runs the keyboard test through QEMU's `sendkey`, which exercises PS/2 on x86-64 and virtio-input on AArch64: Ctrl+C at the prompt, typing with Backspace and Enter, and then special keys in `keys` (`up`, `home`, `delete`, `esc`, `ctrl-a`, `shift-tab`). The shell test also runs `keys` over serial with every special key and Ctrl+C as a byte. Pass each with `./build.sh test --arch x86_64` or `--arch aarch64`.

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

The AArch64 ISO is UEFI-only and uses Limine's `BOOTAA64.EFI`. QEMU runs with the `virt` machine. Interactive runs let the guest restart and power off, so the shell's `reboot` boots again and `shutdown` closes QEMU; set `QEMU_NO_REBOOT=1` to make a reset exit QEMU instead, which helps catch a triple fault during boot. The window shows the guest's 1024×768 screen pixel for pixel, which is small on a Retina display; set `QEMU_ZOOM=1` to scale it with the window instead (`-display cocoa,zoom-to-fit=on` on macOS, GTK elsewhere), then resize the window or use View → Zoom To Fit. They copy serial output to the launching terminal and to `build/<arch>/serial.log` as it arrives, removing the screen-clearing control sequences the firmware and Limine send on every boot. The log covers the whole session, including the boots after a `reboot`, and each new run starts a fresh log. Set `QEMU_EFI` to an AArch64 UEFI firmware image if the script cannot find one in a common system location.

On macOS, the toolchain builder automatically selects Apple Clang, the active Xcode SDK, GNU89 host flags, and system zlib for Binutils. Build each stage separately:

```sh
./toolchains/build_toolchain.sh --binutils --arch aarch64-elf
./toolchains/build_toolchain.sh --gcc --arch aarch64-elf
```

The builder accepts `HOST_CC`, `HOST_CFLAGS`, and `SDKROOT` overrides if needed.