# The FeatherOS C Library and Its POSIX Subset

FeatherOS has its own C library, written from scratch under `src/libc/` and linked statically into every program. This page lists what it provides, where it differs from ISO C and POSIX, and what is left out on purpose. Error numbers, `open` flags, `waitpid` statuses and the `reboot` commands use Linux's values, so code and documentation written for Linux read the same here.

The headers are plain C, and C++ sees the same functions with C linkage. The implementation is freestanding C++ in `FeatherOS::LibC`. It uses only the compiler's own `stdint.h`, `stddef.h`, `stdarg.h`, `stdbool.h` and `float.h`, plus a `limits.h` that adds `LLONG_MAX` and similar to the compiler's.

## Writing a program

```cpp
#include <stdio.h>

int main(int argc, char** argv)
{
    printf("hello from %s\n", argv[0]);
    return 0;
}
```

Save it as `src/user/<name>.cpp` and add `<name>:<name>` to `FEATHEROS_PROGRAMS` in `src/user/CMakeLists.txt`. The build places it in `/system` as `/system/<name>`, where the shell finds it by name. A copy anywhere else runs too, by its path (`/tmp/tool`, `./tool`) or by name from a directory in `PATH`, which is `/system:/bin`. Every program in `src/user/` is written this way, except `test_program`, a kernel self-test that defines `_start` itself.

**Startup.** The C library's `_start` does four things before `main`:
- copies the arguments to writable memory, since the kernel maps them read-only
- loads the environment
- runs `.preinit_array` and `.init_array`, which include C++ static constructors and `__attribute__((constructor))` functions
- calls `exit(main(argc, argv))`

`argv[0]` is the path the program was started with, or its name for a program in `/system`.

**Exit.** `exit` runs `atexit` handlers and C++ static destructors, newest first. It then runs `.fini_array`, flushes the stdio streams, and passes the low 8 bits of the status to the kernel. `abort` raises `SIGABRT`, so a handler runs first, and ends the program with it even if the handler returns.

**Threads.** The library is thread-safe where POSIX requires it. Its own locks (`src/libc/lock.cpp`) sleep in the kernel only while contended.
- `errno` is per thread, through TLS behind `__errno_location()`.
- `malloc`, `calloc`, `realloc` and `free` share one lock.
- Every stdio function locks its stream for the whole call, so a `printf` line from one thread never mixes with another's; `flockfile` holds the lock across calls. Opening, closing and `fflush(NULL)` also take a lock over the list of streams. At `exit`, a stream another thread holds is not flushed, since that thread may be blocked reading for good.
- `atexit` and `__cxa_atexit` registration is locked, and a function-local static's initializer runs once while other threads wait (`__cxa_guard_*`).
- `__cxa_thread_atexit` runs `thread_local` destructors in their own thread, newest first, when it ends; `exit` runs the calling thread's before the `atexit` handlers.
- Not thread-safe, as POSIX allows: `strtok` (use `strtok_r`), `rand`, `getenv` against `setenv`/`putenv`, and the time functions that return a static buffer (`gmtime`, `asctime`, `ctime`; use the `_r` forms). A signal handler must not call stdio or `malloc`.

## Limits that shape the API

These come from the kernel and apply everywhere below.

| Limit | Value |
| --- | --- |
| Command line passed to a program | 63 characters, at most 16 words including the name |
| Environment | 1 KiB and at most 32 variables |
| Path | 63 characters |
| Open files per process | 16 descriptors, plus the console's 0, 1 and 2 |
| Running children per process | 16 |
| Memory per process | 1024 pages (4 MiB), including the image and stack |
| Clocks | the wall clock counts whole seconds; the monotonic clock counts milliseconds |

## The file system

`/` is the FAT16 disk. These are mounted on top of it:

| Path | Contents |
| --- | --- |
| `/system` | The read-only initramfs, which holds every bundled program as `/system/<name>` |
| `/dev` | `null`, `zero` and `random`, as on Linux; all take writes. `tty` opens the caller's controlling terminal, or fails with `ENXIO` without one |
| `/tmp` | A RAM filesystem, emptied on restart: at most 128 entries and 4 MiB, with case-sensitive names |
| `/proc` | Read-only: `meminfo`, `stat` (uptime and CPU use), and `<pid>/status` for each process; `/proc/self` is the caller |

`/bin`, an ordinary disk directory, is the second entry of the default `PATH`, for programs copied to the disk. Disk names ignore case and are at most 63 characters, and no file can have holes. `/proc` files follow Linux's `Name:\tvalue` layout, but with fewer fields.

## Headers

### `string.h`

Provides:
- the memory functions: `memcpy`, `memmove`, `memset`, `memcmp`, `memchr`
- the string functions: `strlen`, `strnlen`, `strcpy`, `strncpy`, `strcat`, `strncat`, `strcmp`, `strncmp`, `strchr`, `strrchr`, `strstr`, `strspn`, `strcspn`, `strpbrk`, `strtok`, `strtok_r`
- the allocating copies `strdup` and `strndup`
- `strerror`

There is no locale, so `strcoll` and `strxfrm` are left out. `strerror` knows a message for every value in `errno.h`.

### `ctype.h`

The `is*` classifiers, `tolower` and `toupper`, for ASCII only, since `"C"` is the only locale.

### `errno.h`

`errno`, with these error numbers, at Linux's values:

`EPERM ENOENT ESRCH EINTR EIO ENXIO E2BIG ENOEXEC EBADF ECHILD EAGAIN/EWOULDBLOCK ENOMEM EFAULT EBUSY EEXIST EXDEV ENOTDIR EISDIR EINVAL EMFILE ENOTTY ENOSPC ESPIPE EROFS EPIPE EDOM ERANGE EDEADLK ENAMETOOLONG ENOSYS ENOTEMPTY EOVERFLOW EILSEQ ENOTSOCK EDESTADDRREQ EMSGSIZE ENOPROTOOPT EPROTONOSUPPORT EOPNOTSUPP/ENOTSUP EAFNOSUPPORT EADDRINUSE EADDRNOTAVAIL ENETDOWN ENETUNREACH ECONNRESET ENOBUFS EISCONN ENOTCONN ETIMEDOUT ECONNREFUSED EHOSTUNREACH EALREADY EINPROGRESS`

Each kernel error maps to one of them. A background read of the controlling terminal that ignores or blocks `SIGTTIN`, or comes from an orphaned group, fails with `EIO`, as on Linux.

### `stdlib.h`

- **Process:** `exit`, `_Exit`, `abort` and `atexit`. `atexit` and `__cxa_atexit` share one list of 32 handlers.
- **Memory:** `malloc`, `calloc`, `realloc`, `free` and `posix_memalign`.
  - Blocks are 16-byte aligned and come from a first-fit allocator with address-ordered free lists.
  - `posix_memalign` takes any power-of-two alignment: a larger one over-allocates and frees the bytes before the aligned block. `realloc` keeps only 16-byte alignment when it moves a block.
  - The heap grows in 64 KiB steps. Freed memory is reused, and once more than 256 KiB is free at the end of the heap, all but 64 KiB of it goes back to the kernel. Free space below blocks still in use stays, since only the end of the heap can move.
  - Freeing a pointer that `malloc` did not return, or freeing one twice, aborts.
- **Randomness:** `arc4random_buf` fills a buffer from `/dev/random`, read 256 bytes at a time. It is only as random as the kernel's source (hardware randomness when the CPU has it, otherwise the cycle counter), so it is not cryptographic everywhere, and it aborts if the device cannot be opened. Swift's hashing and random numbers use it. `arc4random` and `arc4random_uniform` are not provided.
- **Numbers:**
  - `abs`, `labs`, `llabs`, `div`, `ldiv` and `lldiv`.
  - `atoi`, `atol`, `atoll`, `strtol`, `strtoll`, `strtoul` and `strtoull`, with bases 0 and 2–36.
  - `strtod`, `strtof`, `strtold` and `atof`. These are correctly rounded (ties to even), and accept hexadecimal floats, `inf`, `infinity` and `nan(...)`.
- **Sorting:** `qsort`, a heapsort, so it never needs extra memory but is not stable. `bsearch`.
- **Environment:** `getenv`, `setenv` and `unsetenv`. `putenv` is left out, because the table keeps its own copies of the strings.
- **Random numbers:** `rand` and `srand`, a 64-bit linear congruential generator.
- **Pseudo-terminals:** `posix_openpt`, `grantpt`, `unlockpt`, `ptsname` and `ptsname_r`. `posix_openpt` takes `O_RDWR`, optionally with `O_NOCTTY` and `O_CLOEXEC`, and opens `/dev/ptmx`; there are 16 pairs, after which it fails with `ENXIO`. The slave is `/dev/pts/N`, and opening it fails with `EIO` until `unlockpt`. There are no owners or permissions, so `grantpt` only checks that the descriptor is a master. `isatty` is false for a master and true for a slave. `ptsname` returns a static buffer. Closing the master hangs up the slave: the session it was the controlling terminal of loses it and every process there gets `SIGHUP`, reads on the slave return 0, writes fail with `EIO`, and `poll` reports `POLLHUP`. Once every slave descriptor is closed, reads on the master return the rest of the output and then 0 (end of file, as on macOS and the BSDs, where Linux returns `EIO`), with `POLLHUP`. A process's `tcsetattr` settings are restored on every terminal when it exits.

Not provided:
- `system`, because there is no `/bin/sh -c`
- `mkstemp`, `realpath`, `mbtowc` and the other multibyte functions but `uchar.h`'s

### `inttypes.h`

The `PRI*` and `SCN*` format macros for every width, `least`, `fast`, `max` and `ptr` type, and `imaxabs`, `imaxdiv`, `strtoimax` and `strtoumax`. The 64-bit types and `intptr_t` are `long` on both targets. GCC makes the fast 8- and 16-bit types `int`, while Clang makes them `char` and `short`, so their `SCN` macros follow the compiler.

### `uchar.h`

`mbstate_t`, `mbrtoc16`, `c16rtomb`, `mbrtoc32` and `c32rtomb`. Multibyte text is always UTF-8.
- **Decoding:** a character may arrive over several calls, which return `(size_t)-2` until it is complete.
- **Errors:** overlong forms, surrogates, values past U+10FFFF and stray continuation bytes fail with `EILSEQ`.
- **Surrogate pairs:** `mbrtoc16` returns a character past U+FFFF as a pair, the low half from the next call as `(size_t)-3`. `c16rtomb` writes nothing for a high surrogate, and the whole character for the low one that follows.

libc++'s headers take `mbstate_t` from here.

### `stdio.h`

**Streams.** `stdin`, `stdout` and `stderr` are the console. `stdout` is line-buffered and `stderr` unbuffered. Reading the console flushes `stdout` first, so a prompt without a newline still appears.

- **Opening:** `fopen` takes `"r"`, `"w"` and `"a"`, each optionally with `+`; a `b` is ignored. A disk file can have only one writer at a time, so a second open for writing fails with `EBUSY`. `fdopen` is also provided.
- **Buffers and errors:** `fclose`, `fflush`, `setvbuf` and `setbuf`. `setvbuf` uses only the mode: every stream keeps its own 512-byte buffer. `feof`, `ferror`, `clearerr` and `fileno` are also provided.
- **Reading and writing:** `fread`, `fwrite`, `fgetc`, `getc`, `getchar`, `ungetc`, `fgets`, `getline`, `fputc`, `putc`, `putchar`, `fputs` and `puts`.
- **Seeking:** `fseek`, `ftell` and `rewind`. Offsets past the end of a file fail with `EINVAL`, and the console cannot seek (`ESPIPE`).
- **Files:** `remove`, `rename` and `perror`. `rename` works within one filesystem only (`EXDEV` between the disk and `/tmp`), replaces a file but not a directory (`EEXIST`), and refuses open files (`EBUSY`).

**Formatting.** The family is `printf`, `fprintf`, `dprintf`, `sprintf` and `snprintf`, plus their `v` forms. It supports:
- every flag, width and precision (including `*`)
- the length modifiers `hh h l ll j z t L`
- the conversions `d i u o x X c s p %`, and `f F e E g G a A`

Floating-point output is exact: it is computed with big integers, and matches glibc digit for digit. Positional arguments (`%1$d`), `%n` and wide characters (`%lc`, `%ls`) are not supported; `%n` is printed as written.

**Scanning.** `scanf`, `fscanf` and `sscanf`, plus their `v` forms, read:
- integers, with the same length modifiers as `printf`
- floating-point numbers, through the same parser as `strtod`
- `%c`, `%s`, scansets (`%[...]`), `%n`, `%%`, assignment suppression (`%*d`) and field widths

Not provided:
- `tmpfile`, `tmpnam`, `popen` and `freopen`
- the wide-character functions

Use `remove` and `fopen` in place of the file functions.

### `assert.h`

`assert` prints `Assertion `expr' failed.` with the file, line and function, then calls `abort`. Defining `NDEBUG` turns it off.

### `setjmp.h`

`setjmp` and `longjmp`, in assembly for each architecture:
- **x86-64:** saves the callee-saved general registers, the stack pointer and the return address.
- **AArch64:** saves the same, plus `d8`–`d15`, the callee-saved vector registers.

There are no signals, so `sigsetjmp` and `siglongjmp` are not provided.

### `math.h`

Every function exists for `double` and, with an `f` suffix, `float`:
- **Exact, bit for bit the same as glibc:** `fabs copysign floor ceil trunc round fmod frexp ldexp scalbn modf fmin fmax sqrt fma`
- **Within 1–3 units in the last place of glibc:** `exp exp2 log log2 log10 pow sin cos tan asin acos atan atan2 sinh cosh tanh hypot`

Also provided:
- the classification macros: `fpclassify isnan isinf isfinite isnormal signbit`
- `HUGE_VAL`, `INFINITY` and `NAN`
- the common `M_*` constants

`fma` and `fmaf` round `x * y + z` once. They work on the exact product in 128-bit integers, so they need no fused multiply-add instruction. `fmaf` rounds straight to `float`, since going through `double` would round twice.

Errors set `errno` (`math_errhandling` is `MATH_ERRNO`):
- `EDOM` outside a function's domain
- `ERANGE` on overflow, underflow or a pole

Differences:
- `sin`, `cos` and `tan` reduce their argument with a three-part π/2, so they lose precision above about 10^6.
- There are no `long double` functions.
- `fma` sets `ERANGE` only on overflow, not when the result underflows.
- AArch64 programs cannot do `long double` arithmetic, because the compiler runtime routines for 128-bit floats are missing. They can still print, parse and copy `long double` values. Of libgcc's integer routines, the C library has only `__lshrti3` (`src/libc/compiler_runtime.cpp`), which AArch64 code at `-O2` calls to shift `unsigned __int128` values.
- `fenv.h`, `lround`, `nextafter`, `erf`, `lgamma` and other less common functions are not provided.

### `time.h`

There are no time zones: every time is UTC.

- **Clocks:** `time`, `clock_gettime` with `CLOCK_REALTIME` (10 ms timer ticks, in nanoseconds) and `CLOCK_MONOTONIC` (milliseconds since boot), `clock_getres`, which gives 10 ms for both, and `clock_settime` for `CLOCK_REALTIME`, which any process may call.
- **Conversions:**
  - `gmtime` and `gmtime_r`; `localtime` and `localtime_r` return the same as gmtime.
  - `mktime` and `timegm`, which normalize out-of-range fields.
  - `asctime` and `ctime`.
  - `strftime` with every C99 conversion, in the C locale.
- **Sleeping:** `nanosleep`, in whole milliseconds; a signal cuts it short with `EINTR` and the time left in `remaining`.

Differences:
- Dates before 1970 fail with `EOVERFLOW`.
- `clock`, `difftime`, `timer_create` and `CLOCK_PROCESS_CPUTIME_ID` are not provided.

### `unistd.h`, `fcntl.h`, `sys/stat.h`, `sys/types.h`

- **Descriptors:**
  - `read`, `write`, `close` and `lseek`.
  - `open` and `creat`, with `O_RDONLY O_WRONLY O_RDWR O_CREAT O_EXCL O_TRUNC O_APPEND O_CLOEXEC O_NONBLOCK O_NOCTTY`. The lowest free descriptor is used, out of 32. A session leader without a controlling terminal takes the first terminal it opens unless it passes `O_NOCTTY`.
  - `openat` does what `open` does for an absolute path, or a relative one with `AT_FDCWD`. Directories cannot be opened, so a relative path with any other descriptor fails with `ENOTDIR`, or `EBADF` when the descriptor is not open.
  - `dup` and `dup2` share the open file, its offset and its `O_APPEND` and `O_NONBLOCK` flags. `fcntl` supports `F_DUPFD`, `F_DUPFD_CLOEXEC`, `F_GETFD` and `F_SETFD` (`FD_CLOEXEC`), and `F_GETFL` and `F_SETFL`, which reports the access mode and changes `O_APPEND` and `O_NONBLOCK`, ignoring other flags as Linux does. Locks are not provided.
  - `O_NONBLOCK` makes a pipe or socket call that would wait fail with `EAGAIN` instead; on files it changes nothing. A terminal's flags cannot change (`EINVAL`), since every process shares the console's open file; raw mode with `VMIN` 0 reads it without waiting.
  - Programs started with `posix_spawn` inherit every descriptor not marked close-on-exec.
  - `pipe` and `pipe2` (with `O_CLOEXEC` and `O_NONBLOCK`) make a 4 KiB pipe. `PIPE_BUF` is 512: writes up to that size are never split. Reading returns 0 once every write end is closed; writing with no read end left raises `SIGPIPE` and fails with `EPIPE`. A non-blocking write writes the 512-byte pieces that fit, and fails with `EAGAIN` when not even the first does. `fstat` reports `S_IFIFO`, and seeking fails with `ESPIPE`.
  - With `O_APPEND`, every write goes to the end, as POSIX requires.
  - `O_CREAT` and `O_TRUNC` need write access.
  - Offsets past the end of a file fail with `EINVAL`, because files cannot have holes.
- **Files and directories:** `unlink`, `rmdir`, `mkdir`, `chdir`, `getcwd`, `stat`, `fstat`, `access` and `isatty`.
  - There are no users or permissions.
  - `stat` reports owner 0, and mode bits that only show whether a file is writable: `0644` or `0444`, and `0755` or `0555` for directories.
  - Every time in `stat` is 0, because FAT16 timestamps are not read.
  - `access` fails `W_OK` for the read-only initramfs and `/proc`.
  - `isatty` is true for terminals (the console, `/dev/tty` and pseudo-terminal slaves), not for the other devices in `/dev`, which are also `S_IFCHR`.
  - `mkdir` ignores its mode.
- **Processes:** `getpid`, `getppid`, `getpgrp`, `getpgid`, `setsid`, `getsid` and `_exit`. The kernel starts init in a session of its own without a controlling terminal; a child joins its parent's session and controlling terminal unless it is started with `POSIX_SPAWN_SETSID`. `setsid` starts a session without a controlling terminal and fails with `EPERM` for a group leader, which every command the shell starts is. `tcgetpgrp` works only on the caller's controlling terminal (`ENOTTY` on others). `tcsetpgrp` only names a group of the caller's session there, otherwise `EPERM`. On the controlling terminal a background group's read gets `SIGTTIN`, and its `tcsetattr` or `tcsetpgrp` gets `SIGTTOU`, both of which stop the group by default; the call runs again once the group continues. A read that ignores or blocks `SIGTTIN` fails with `EIO`; a change that ignores or blocks `SIGTTOU` goes ahead, which is how a shell takes the terminal back. An orphaned group gets `EIO` instead of the signal. Writes from the background are allowed, as with `stty -tostop`. On another terminal there is no such check. Besides the console, pseudo-terminal slaves are terminals (see `stdlib.h`). `setpgid` is not provided: groups are chosen when `posix_spawn` starts the child.
- **Sleeping:** `sleep` and `usleep`.
- **`environ`.**
- **`sysconf`:** `_SC_NPROCESSORS_ONLN` and `_SC_NPROCESSORS_CONF` (the same, from `/proc/stat`), `_SC_PAGESIZE` and `_SC_OPEN_MAX`; other names fail with `EINVAL`.
- **`sbrk`:** moves the end of the heap that `malloc` also uses. A negative increment returns the whole pages above the new end to the kernel, and going below the heap's start fails with `EINVAL`. `malloc` never lowers the break below memory a program got from `sbrk` itself. `brk` is not provided.

Not provided:
- `fork` and the `exec*` family, because processes start with `posix_spawn`
- `link` and `symlink`
- `chmod` and `chown`
- `getuid` and the other user and group functions
- `ftruncate` and `fsync`

### `dirent.h`

`opendir`, `readdir`, `rewinddir` and `closedir`.

- **Entries:** each has `d_ino`, `d_type` (`DT_DIR` or `DT_REG`) and `d_name`. `d_ino` numbers the entries from 1 within their directory.
- **Paths:** `opendir` makes its path absolute, so a later `chdir` does not change which directory the stream reads.
- **Left out:** listings do not include `.` and `..`. `scandir`, `telldir`, `seekdir` and `fdopendir` are not provided.

### `spawn.h` and `sys/wait.h`

**`posix_spawn` and `posix_spawnp`.** The path names a program file anywhere the VFS can read: the disk, `/tmp`, or `/system`. A relative path such as `./tool` starts at the working directory. FeatherOS difference: a name without a `/` is looked up in `/system`, so `posix_spawn("ls", …)` runs `/system/ls`, where POSIX would look in the working directory.
- **Programs:** FAT16 has no execute bit, so any regular file up to 1 MiB may be tried. The kernel reads it whole and checks it like a `/system` program; one that is not a valid program fails with `ENOEXEC`, as do devices, a larger file fails with `ENOMEM`, a directory with `EISDIR`, and a missing file with `ENOENT`.
- **Arguments:** the kernel always makes the path its `argv[0]`, or the bare name for a program in `/system`.
- **Errors:** the path or an argument containing a double quote fails with `EINVAL`. A command line or environment over the limits fails with `E2BIG`; the path counts toward the 63-character command line, and must resolve to at most 63 characters (`ENAMETOOLONG`).
- **Console:** by default the child takes the console until it exits.
- **PATH:** `posix_spawnp` runs the first regular file it finds in the directories of the caller's `PATH`, or `/system:/bin` when `PATH` is unset; empty entries are skipped rather than meaning the working directory. A file name holding a `/` is used as it is. Unlike `execvp`, a file that is not a program is not run as a shell script; `posix_spawnp` returns `ENOEXEC`.

**Attributes.** The supported flags are:

| Flag | Effect on FeatherOS |
| --- | --- |
| `POSIX_SPAWN_SETSID` | The child leads a new session: Ctrl+C stops at it, and it may call `reboot`. Since the file actions run in the parent, the child takes the terminal at its descriptor 0 as its controlling terminal if no session has it, as if it had opened it. init starts the shell this way. |
| `POSIX_SPAWN_SETPGROUP` | The child joins the attributes' process group: 0 for a new one it leads, or an existing group that is the caller's or holds one of the caller's children. Others fail with `EINVAL`. |
| `POSIX_SPAWN_BACKGROUND` | FeatherOS: the child's group does not take the console. With a new group, that makes a background job, which a console read stops with `SIGTTIN`. Without this flag, the child's group becomes the foreground group, as if the caller had called `tcsetpgrp`. |

Other flags fail with `EINVAL`.

**File actions.** `posix_spawn_file_actions_adddup2`, `addopen` and `addclose` run in order, at most 8 per spawn. The C library applies them to the caller's own descriptors around the `Spawn` syscall and puts those back afterwards. Another thread using those descriptors meanwhile would see the child's, so spawn with file actions from one thread at a time. A failing action makes `posix_spawn` return its error without starting the child. `addopen` paths are at most 63 characters, and opens with `O_NOCTTY`, so the caller never takes the terminal meant for the child.

**`waitpid` and `wait`.** They use the Linux status encoding with `WIFEXITED`, `WEXITSTATUS`, `WIFSIGNALED`, `WTERMSIG`, `WIFSTOPPED`, `WSTOPSIG` and `WIFCONTINUED`, and the options `WNOHANG`, `WUNTRACED` (also report a child that stopped) and `WCONTINUED` (also report one that `SIGCONT` continued). Each stop or continue is reported once, and a newer one replaces one not yet reported. A CPU exception reports signal 11 (`SIGSEGV`).

A `pid` of -1 waits for any child `posix_spawn` started, 0 for one in the caller's process group, and -n for one in group n, by polling every 10 ms.

### `signal.h`

`sigaction`, `signal`, `kill`, `raise`, `sigprocmask`, `sigemptyset`, `sigfillset`, `sigaddset`, `sigdelset` and `sigismember`, with `pause` in `unistd.h` and `strsignal` in `string.h`. Signals have Linux's numbers: `SIGHUP`, `SIGINT`, `SIGQUIT`, `SIGILL`, `SIGTRAP`, `SIGABRT`, `SIGBUS`, `SIGFPE`, `SIGKILL`, `SIGUSR1`, `SIGSEGV`, `SIGUSR2`, `SIGPIPE`, `SIGALRM`, `SIGTERM`, `SIGCHLD`, `SIGCONT`, `SIGSTOP`, `SIGTSTP`, `SIGTTIN`, `SIGTTOU` and `SIGWINCH`.

- **Delivery:** a handler runs when the program next returns from the kernel, after a syscall or when a timer tick preempts it, so `raise` returns after its handler has run. The handler gets only the signal number. While it runs, the signal and `sa_mask` are blocked, unless `SA_NODEFER`; `SA_RESETHAND` puts the default action back first. `signal` installs a handler that stays, as glibc's does.
- **Interrupted calls:** `read` and `write` on the console or a pipe, `waitpid`, `sleep`, `nanosleep` and `pause` fail with `EINTR` once a signal arrives (`sleep` returns the seconds left, and `nanosleep` the time left). `SA_RESTART` is accepted, but calls are not restarted. A call that only a stop interrupted starts again once the program continues, and a sleep only for the time that was left.
- **Defaults:** most signals end the program; `SIGCHLD`, `SIGCONT` and `SIGWINCH` are ignored. `SIGSTOP`, `SIGTSTP`, `SIGTTIN` and `SIGTTOU` stop every thread of the program until `SIGCONT`, which continues it whatever its action; a signal sent meanwhile waits, except `SIGKILL`. `SIGTSTP`, `SIGTTIN` and `SIGTTOU` do not stop a process whose group is orphaned, one in which no process has a parent in another group of the same session. Sending a stop signal drops a pending `SIGCONT`, and the reverse. `SIGKILL` and `SIGSTOP` cannot be caught, ignored or blocked. A write to a pipe without a reader, or to a TCP connection that can no longer send, raises `SIGPIPE`, so a program sees `EPIPE` only if it ignores or handles it.
- **Faults:** a CPU exception ends the program with `SIGSEGV` and runs no handler.
- **Processes:** a child starts with handlers back at their defaults, while ignored signals and the blocked mask carry over. `kill(1, ...)` fails with `EPERM`, since init cannot be signaled.
- **Not provided:** `SA_SIGINFO` and `siginfo_t`, `SA_NOCLDSTOP`, `sigsuspend`, `sigpending`, `sigaltstack` and `alarm`.

### `strings.h`

`strcasecmp` and `strncasecmp`, for ASCII.

### `sched.h`

`sched_getcpu`, the glibc extension, returns the CPU the caller was running on. That may change as soon as it returns. `sched_yield` lets other ready threads run first.

### `pthread.h`

`pthread_create`, `pthread_join`, `pthread_exit`, `pthread_self`, `pthread_equal`, `pthread_attr_init` and `pthread_attr_destroy`. They return an error number rather than setting `errno`.
- **Limits:** at most 16 threads per process, the main one included (`EAGAIN` past that). Each gets a kernel-managed stack of up to 63 pages, growing on demand behind a guard page, so attributes have nothing to set and stack sizes cannot be chosen.
- **Joining:** `pthread_join` refuses the caller itself (`EDEADLK`), the main thread, unknown or already joined threads (`ESRCH`) and detached ones (`EINVAL`). `pthread_detach`, or `PTHREAD_CREATE_DETACHED` in `pthread_attr_setdetachstate`, frees a thread's slot once it ends.
- **Mutexes:** `pthread_mutex_init`, `lock`, `trylock`, `unlock` and `destroy`, with `PTHREAD_MUTEX_INITIALIZER` and the `NORMAL`, `ERRORCHECK` and `RECURSIVE` types through `pthread_mutexattr_settype`. A contended mutex sleeps in the kernel (`FutexWait`); an uncontended lock and unlock make no syscall. `ERRORCHECK` returns `EDEADLK` for a relock by its owner and `EPERM` for an unlock by another thread; `RECURSIVE` counts its owner's locks. There is no `pthread_mutex_timedlock`, robust or priority-inheritance mutexes.
- **Condition variables:** `pthread_cond_wait`, `timedwait`, `signal` and `broadcast`, with `PTHREAD_COND_INITIALIZER` and `pthread_condattr_setclock`. Wakeups may be spurious, as POSIX allows, so wait in a loop. The deadline is on `CLOCK_REALTIME` unless the attributes chose `CLOCK_MONOTONIC`.
- **`pthread_once`:** the first caller runs the routine while later ones sleep until it returns.
- **Keys:** `pthread_key_create`, `pthread_key_delete`, `pthread_getspecific` and `pthread_setspecific`.
  - There are at most `PTHREAD_KEYS_MAX` (128) keys at a time; past that, `pthread_key_create` fails with `EAGAIN`.
  - A thread that ends through `pthread_exit` or by returning calls each key's destructor on its non-null values. It does so for up to `PTHREAD_DESTRUCTOR_ITERATIONS` (4) rounds while destructors set values again. `exit` calls none, as POSIX says.
  - A deleted key's values read as null, even once the key is created again.
  - The values take 2 KiB of each thread's thread-local storage, but only in programs that create a key.
- **Ending:** `exit`, a fault or a terminating signal in any thread ends the whole process. `pthread_exit` ends one thread; in the last thread running it exits as `exit(0)` does.
- **Signals:** actions are shared; masks are per thread, but there is no `pthread_sigmask` yet (`sigprocmask` sets the calling thread's). Signals sent to the process go to the main thread, or to the first live one after it has ended.
- **Thread-local storage:** `__thread` and `thread_local` work for variables with constant initializers, at most 16 KiB per thread; each thread's copy starts from the program's initial values. `thread_local` objects with constructors and destructors work too.
- **Missing:** read-write locks, barriers, spinlocks, `pthread_cancel`, `pthread_kill` and `pthread_sigmask`.

### `sys/ioctl.h`

Only `ioctl(fd, TIOCGWINSZ, &size)` and `ioctl(fd, TIOCSWINSZ, &size)`, which do what `tcgetwinsize` and `tcsetwinsize` in `termios.h` do, and include `termios.h` for `struct winsize`. On a terminal, other requests fail with `EINVAL`; other descriptors fail with `ENOTTY`. The console understands the VT100 sequences listed in `docs/development.md`.

### `termios.h`

`tcgetattr`, `tcsetattr` and `cfmakeraw` on any terminal; other descriptors fail with `ENOTTY`. The constants have FeatherOS's own values.
- **Window size:** `tcgetwinsize` and `tcsetwinsize` from POSIX 2024, with `struct winsize`, on a terminal or a pseudo-terminal master, which stands for its slave. Getting fills `ws_row` and `ws_col` and leaves the pixel sizes 0; the console's size is the text area below the status bar (80×24 without a framebuffer), and a new pty's is 80×24. Setting ignores the pixel sizes, works from either end of a pty and from any process, as on Linux, and a change sends `SIGWINCH` to the terminal's foreground group. Other descriptors fail with `ENOTTY`.
- **What has an effect:** `ICANON`, `ECHO` and `ISIG` in `c_lflag`, `ICRNL` in `c_iflag`, and `c_cc[VMIN]` and `c_cc[VTIME]` for non-canonical reads, with the POSIX meanings. The console's settings start with all four flags on, `VMIN` 1 and `VTIME` 0.
- **Accepted without effect:** the other flags that `cfmakeraw` clears (`BRKINT`, `IXON`, `OPOST`, `IEXTEN`, …). The console never changes output, and `tcgetattr` always reports `OPOST` and `CS8`. `VINTR`, `VSUSP`, `VERASE` and `VEOF` report Ctrl+C, Ctrl+Z, Backspace and Ctrl+D but cannot be changed, and there is no end-of-file key yet.
- **FeatherOS's own flag:** with `FEATHEROS_WAKE_ON_OUTPUT` in `c_lflag`, a non-canonical read with `VMIN` above 0 that has no byte yet returns 0 once output from other programs or the kernel reached the console and paused for a tenth of a second, with the cursor moved to the start of a row. The shell's line editor uses it to draw its prompt again below a service's message.
- **Actions:** `TCSANOW`, `TCSADRAIN` and `TCSAFLUSH` all apply at once; `TCSAFLUSH` keeps input already typed.
- **Who may change them:** on the controlling terminal, the foreground group; a background group gets `SIGTTOU` (see Processes). When the program that changed them exits, the console returns to the defaults, so a program that crashes in raw mode does not leave the shell there.
- **Missing:** `cfgetispeed` and the other speed functions, `tcflush`, `tcdrain` and `tcsendbreak`.

### `sys/reboot.h`

`reboot(RB_POWER_OFF)` and `reboot(RB_AUTOBOOT)`, with Linux's values. Only init and session leaders may call it. The kernel then stops every process, flushes the disk, and powers off or restarts. Anyone else gets `EPERM`.

### `sys/socket.h`, `netinet/in.h`, `arpa/inet.h`, `netinet/ip_icmp.h`, `netinet/icmp6.h`, `sys/time.h`

Sockets are descriptors: `read`, `write`, `close`, `dup`, `fstat` (`S_IFSOCK`), `poll` and inheritance work on them as on files.
- **Calls:** `socket`, `bind`, `connect`, `listen`, `accept`, `accept4` (with `SOCK_CLOEXEC` and `SOCK_NONBLOCK`), `send`, `sendto`, `recv`, `recvfrom`, `shutdown` (`SHUT_RD`, `SHUT_WR`, `SHUT_RDWR`), `getsockname`, `getpeername`, `setsockopt` and `getsockopt`, with Linux's constants (`AF_INET`, `AF_INET6`, `SOCK_STREAM`, `SOCK_DGRAM`, `SOCK_CLOEXEC`, `SOCK_NONBLOCK`, `IPPROTO_*`, `SOL_SOCKET`, `SOMAXCONN`). Receive `flags` may only hold `MSG_DONTWAIT`, and send `flags` `MSG_DONTWAIT` and `MSG_NOSIGNAL`. `recvfrom` truncates a datagram that does not fit and drops the rest, without `MSG_TRUNC`.
- **TCP:** `socket(AF_INET or AF_INET6, SOCK_STREAM, 0 or IPPROTO_TCP)`. `connect` blocks until the connection is open or fails (`ECONNREFUSED`, `ETIMEDOUT` after about a minute, `ENETUNREACH`), and Ctrl+C abandons it with `EINTR`. `listen` keeps up to 16 connections for `accept`, a larger backlog being cut down. `send` and `write` block until all the data is buffered; on a connection that can no longer send they fail with `EPIPE` and raise `SIGPIPE`, unless `MSG_NOSIGNAL` is given, or with `ECONNRESET` after a reset. `recv` and `read` return what has arrived, up to 1480 bytes a call, and 0 at the end of the stream. `close` sends what is buffered and then FIN in the background, or a reset when received data was never read. Ports are bound as if `SO_REUSEADDR` were set. There is no urgent data, `SO_LINGER`, `TCP_NODELAY` (there is no Nagle algorithm to turn off) or keep-alive.
- **Datagrams:** UDP sockets, `socket(AF_INET or AF_INET6, SOCK_DGRAM, 0 or IPPROTO_UDP)`, and Linux's unprivileged ping sockets, `socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP)` and `socket(AF_INET6, SOCK_DGRAM, IPPROTO_ICMPV6)`. A UDP socket is bound to a port from 49152 up on its first `sendto` unless `bind` chose one; an `AF_INET6` socket is IPv6-only, as with `IPV6_V6ONLY`. Broadcasts need no `SO_BROADCAST`, and a datagram to port 0 fails with `EINVAL`. Each socket queues up to 8 datagrams of up to 1480 bytes. For a ping socket, a program sends an ICMP echo request (`struct icmphdr` from `netinet/ip_icmp.h`, type `ICMP_ECHO`, or `struct icmp6_hdr` from `netinet/icmp6.h`, type `ICMP6_ECHO_REQUEST`), and the kernel fills in the identifier and checksum; `recvfrom` returns the replies to it, ICMP header included, without the IP header. `connect` on a datagram socket picks the peer that `send` and `write` go to and the only one it receives from; after an ICMP port unreachable error from that peer, the next `send` or `recv` fails with `ECONNREFUSED` and `poll` reports `POLLERR` until then. Unconnected sockets get no ICMP errors (there is no `IP_RECVERR`). Other families and types fail with `EAFNOSUPPORT` or `EPROTONOSUPPORT`.
- **Non-blocking:** with `O_NONBLOCK` on the socket (`SOCK_NONBLOCK`, `accept4` or `fcntl`) or `MSG_DONTWAIT` on one call, `accept`, `recv` and `send` fail with `EAGAIN` where they would wait; a stream `send` buffers what fits and returns its length. A non-blocking TCP `connect` fails with `EINPROGRESS` while the handshake goes on in the background, then `EALREADY` until it ends; `poll` reports `POLLOUT` once it has, with `POLLERR` too if it failed, and `getsockopt(SO_ERROR)` (or another `connect`) tells why. A datagram `send` never waits for queue space, but can still wait a few seconds for address resolution.
- **Options:** `SO_RCVTIMEO`, which takes a `struct timeval` from `sys/time.h`; a receive or `accept` that waits that long fails with `EAGAIN`. `SO_REUSEADDR` is accepted and changes nothing. `getsockopt` reads `SO_RCVTIMEO` and `SO_ERROR`: a failed TCP connection's error, which stays, or a connected UDP socket's ICMP error, which reading clears. Others fail with `ENOPROTOOPT`.
- **Addresses:** `sockaddr_in`, `sockaddr_in6` and `sockaddr_storage` have Linux's layouts. `htons`, `htonl`, `ntohs` and `ntohl`. `inet_pton` and `inet_ntop` handle `AF_INET` and `AF_INET6`. For IPv4, `inet_pton` takes exactly four decimal parts without leading zeros, as POSIX asks; for IPv6 it takes `::` compression and a trailing dotted IPv4 part but no `%zone`, and `inet_ntop` writes the RFC 5952 form, without the dotted form for IPv4-mapped addresses. `in6addr_any`, `in6addr_loopback`, `IN6ADDR_ANY_INIT` and `IN6ADDR_LOOPBACK_INIT` exist; `sin6_scope_id` is ignored, since there is one interface.
- **Missing:** `sendmsg`, `recvmsg`, `socketpair`, Unix-domain sockets, and `getsockopt` options other than the two above.

### `poll.h`

`poll` with Linux's `struct pollfd` and `POLLIN`, `POLLOUT`, `POLLERR`, `POLLHUP` and `POLLNVAL`, for at most 64 descriptors; a negative timeout waits until one is ready, and a signal ends the wait with `EINTR`. Files are always ready; pipes, sockets and the console report as described in [development.md](development.md). Console input counts as soon as a key is waiting, so a canonical read may still wait for the end of the line. `select`, `ppoll` and `epoll` are not provided.

### `netdb.h`

`getaddrinfo`, `freeaddrinfo`, `getnameinfo` and `gai_strerror`, with Linux's `AI_*` and `NI_*` flags and `EAI_*` codes and glibc's messages.
- **Hosts:** a numeric IPv4 or IPv6 address of the asked family (another family gives `EAI_ADDRFAMILY`); no node, meaning the loopback addresses, or the wildcard ones with `AI_PASSIVE`; names that `/etc/hosts` (`_PATH_HOSTS`) lists with addresses of the asked family; then `localhost` and names under it, which are the loopback addresses (RFC 6761); and other names, looked up over DNS. A name of only digits and dots is refused with `EAI_NONAME` rather than looked up, as is any non-numeric host with `AI_NUMERICHOST`.
- **`/etc/hosts`:** the file starts as the initramfs's copy and can be changed until the next boot. Each line is an address and its names, the canonical one first, with `#` starting a comment; names compare without regard to case, and a final dot is ignored. A lookup takes every matching address of the family in the file's order, so a name listed only with IPv6 still goes to DNS for `AF_INET`. Lines with a bad address or no name are skipped, as are names past the eighth on a line and files over 64 KiB.
- **Lookups:** A records for `AF_INET`, AAAA records for `AF_INET6`, both for `AF_UNSPEC`, with IPv4 addresses first. The resolver asks the name servers in order over UDP, waiting 2 seconds for each and going round them twice, and follows CNAMEs that the server answers with. An answer with the truncation bit set is asked again of the same server over TCP (RFC 7766), one connection per query, with the same timeout for each read; connecting itself can take as long as TCP's SYN retries. If that fails too, the next server is asked.
- **`/etc/resolv.conf`** (`_PATH_RESCONF`, in `netdb.h` rather than glibc's `resolv.h`): read at every lookup. `nameserver address` lines, with an optional `#port` as `net dns` takes it, name up to 3 servers that replace the kernel's; without any, the resolver asks the ones from `featheros_network_settings`, which DHCP, router advertisements and `net dns` set. `options timeout:n` (seconds per answer, 1-30) and `attempts:n` (rounds, 1-5) apply to either list. Lines starting with `#` or `;`, other keywords and options, and malformed lines are ignored, as are files over 16 KiB. The shipped file sets only the defaults. A missing name gives `EAI_NONAME`, a name without addresses `EAI_NODATA`, and no answer or no name server `EAI_AGAIN`. At most 8 addresses of each type are kept.
- **Services:** only port numbers, since there is no services database; a name gives `EAI_SERVICE`, or `EAI_NONAME` with `AI_NUMERICSERV`.
- **Results:** one entry per address and socket type, `SOCK_STREAM` and `SOCK_DGRAM` when `ai_socktype` is 0, with `IPPROTO_TCP` or `IPPROTO_UDP` unless `ai_protocol` says otherwise. `AI_CANONNAME` puts the canonical name from `/etc/hosts`, or otherwise the node, into the first entry's `ai_canonname`. `AI_V4MAPPED`, `AI_ALL` and `AI_ADDRCONFIG` are accepted and have no effect.
- **`getnameinfo`:** takes a `sockaddr_in` or `sockaddr_in6` (another family or a shorter length gives `EAI_FAMILY`) and fills whichever of `host` and `service` has room; with neither, it fails with `EAI_NONAME`. The host is the canonical name of the first `/etc/hosts` line with the address, or else the name in the address's PTR record under `in-addr.arpa` or `ip6.arpa`, asked of the same name servers as `getaddrinfo`, CNAMEs followed. Without a name, the host is the address as `inet_ntop` writes it, or `EAI_NONAME` with `NI_NAMEREQD`; as in glibc, a lookup that no name server answered fails with `EAI_AGAIN` instead. `NI_NUMERICHOST` skips the lookup. A PTR name with characters other than letters, digits, `-` and `_` counts as none. The service is always the port number, so `NI_NUMERICSERV` and `NI_DGRAM` change nothing, and neither does `NI_NOFQDN`, since there is no local domain. A result that does not fit gives `EAI_OVERFLOW`. `sin6_scope_id` is never written as a `%zone`, and an IPv4-mapped address is looked up under `ip6.arpa`.
- **Missing:** EDNS (so UDP answers stay within 512 bytes), search domains (`search`, `domain` and `options ndots:` are ignored), `RES_OPTIONS`, a services database (`getservbyname`), and the obsolete `gethostbyname` and `gethostbyaddr` families.

### `featheros.h`

FeatherOS calls with no POSIX equivalent:
- **`featheros_read_line(prompt, buffer, size)`** reads one console line with editing, and returns its length without the newline. It shows the prompt again after Ctrl+C discards a half-typed line, which printing a prompt and calling `fgets` cannot do. The shell, `calc` and `upper` use it. It flushes `stdout` first, but does not see input already buffered in `stdin`.
- **`featheros_arguments_page()`** returns the read-only page where the kernel placed the arguments and environment. `aslr` uses it to show that the page moves between runs.
- **`featheros_argument_count()`** and **`featheros_argument_values()`** return the `argc` and `argv` that `main` receives, for code that does not see `main`'s parameters: a Swift program's top-level code.
- **`featheros_open`, `featheros_openat`, `featheros_fcntl`** and **`featheros_ioctl`** are `open`, `openat`, `fcntl` and `ioctl` with fixed parameters: a `mode_t` mode, an `intptr_t` argument for `fcntl` and a pointer for `ioctl`. Swift does not import variadic C functions, so `FeatherSupport` calls these.
- **`featheros_format(buffer, size, format, arguments, count)`** is `snprintf` with its arguments in an array of `struct featheros_format_argument`, each tagged `FEATHEROS_FORMAT_SIGNED`, `_UNSIGNED`, `_DOUBLE`, `_STRING` or `_POINTER`, through the same engine as `printf`. An integer of either type suits `d i u o x X c` and `*`, and keeps all 64 bits unless `hh` or `h` asks for fewer, so `%d` prints any `Int`; a double suits the floating-point conversions whatever the length modifier, a string `%s` and a pointer `%p`. A conversion whose argument is missing or of another type is written as it appears in `format` and still takes its argument, so the ones after it keep theirs. `String(format:)` in `FeatherSupport` uses it.
- **`featheros_network_settings(new_settings, old_settings, changes)`** reads and changes the network interface's settings in a `struct featheros_network`: the MAC, the IPv4 address, prefix length and gateway (all zero until DHCP or `net ipv4` sets them), the IPv6 link-local and global addresses and router, and up to `FEATHEROS_NAME_SERVERS` name servers as `sockaddr_in` or `sockaddr_in6` in `sockaddr_storage`. It stores the current settings in `old_settings` unless that is null, then applies the parts of `new_settings` that `changes` names: `FEATHEROS_NETWORK_IPV4`, `FEATHEROS_NETWORK_NAME_SERVERS` and `FEATHEROS_NETWORK_LINK`; a name server's port 0 means 53. The name servers set this way come first, then one a router advertised. IPv6 cannot be changed. `link_up` 0 with `FEATHEROS_NETWORK_LINK` takes the link down: the device is reset, the IPv4 and IPv6 addresses, the router's name server and the neighbor caches are dropped, TCP connections to anything but `127.0.0.0/8` and `::1` end with `ENETDOWN`, and sends and connects to anything else fail with `ENETDOWN` while loopback keeps working. `link_up` 1 starts the device and IPv6 autoconfiguration again; IPv4 waits for DHCP (`dhcp -r`) or `net ipv4`. Errors: `ENETUNREACH` without a network device, `EINVAL`, `EAFNOSUPPORT`, `ENETDOWN` for IPv4 while the link is down, `EIO` when the device does not start. `dhcp`, `net` and `getaddrinfo` use it.

### C++ support

The runtime hooks `__cxa_atexit`, `__cxa_guard_acquire`/`release`/`abort`, `__cxa_pure_virtual` and `__dso_handle` are provided. So are `operator new` and `operator delete`, in their plain, array, sized and aligned forms (`std::align_val_t`, defined here since there is no `<new>`). They are built on `malloc` and `posix_memalign`, and a failed `new` aborts; the `nothrow` forms are not provided yet. Programs are built with `-fno-exceptions -fno-rtti`, and there is no C++ standard library. For Swift programs that use libc++'s header-only types, the library also defines `std::__libcpp_verbose_abort`, which libc++'s compiled library would otherwise provide: it prints its message to standard error and aborts.

## Left out on purpose

These need kernel features that do not exist yet. Each one is planned in [roadmap.md](roadmap.md) or listed in [ideas.md](ideas.md).

| Missing | Why | Planned |
| --- | --- | --- |
| `SA_SIGINFO`, `sigsuspend`, `alarm`, real-time signals | Handlers get only the signal number; no timers send signals | Not scheduled |
| `fork`, `exec*` | Processes start with `Spawn`; `fork` needs copy-on-write | M11, if adopted |
| C11 `threads.h`, read-write locks, barriers | Not needed by any program yet | Not scheduled |
| `mmap`, `munmap` | The heap is the only growable mapping, so free space below used blocks cannot be returned | [ideas.md](ideas.md) |
| `locale.h`, `wchar.h`, multibyte functions but `uchar.h`'s | ASCII and the `"C"` locale only; `uchar.h` converts UTF-8 | Not scheduled |
| Time zones | Everything is UTC | Not scheduled |
| Users, permissions, `getuid` | No user accounts | Not scheduled |
| `select` | `poll` does the same job | Not scheduled |
| Dynamic linking, `dlopen` | Static PIEs only | [ideas.md](ideas.md) |
