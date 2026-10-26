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

Save it as `src/user/<name>.cpp` and add `<name>:<name>` to `FEATHEROS_PROGRAMS` in `src/user/CMakeLists.txt`. The build places it in `/system`, where the shell finds it by name. Every program in `src/user/` is written this way, except `test_program`, a kernel self-test that defines `_start` itself.

**Startup.** The C library's `_start` does four things before `main`:
- copies the arguments to writable memory, since the kernel maps them read-only
- loads the environment
- runs `.preinit_array` and `.init_array`, which include C++ static constructors and `__attribute__((constructor))` functions
- calls `exit(main(argc, argv))`

`argv[0]` is the program's name without `/system/`.

**Exit.** `exit` runs `atexit` handlers and C++ static destructors, newest first. It then runs `.fini_array`, flushes the stdio streams, and passes the low 8 bits of the status to the kernel. `abort` raises `SIGABRT`, so a handler runs first, and ends the program with it even if the handler returns.

**Threads.** `pthread.h` starts and joins threads, but the library itself is not thread-safe yet: `malloc`, stdio streams and `errno` must stay in one thread at a time, and nothing takes a lock. `errno` is already behind `__errno_location()`, so it can become per-thread (see M11 Part 8 in [roadmap.md](roadmap.md)).

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
| `/system` | The read-only initramfs, which holds every program |
| `/dev` | `null`, `zero` and `random`, as on Linux; all take writes |
| `/tmp` | A RAM filesystem, emptied on restart: at most 128 entries and 4 MiB, with case-sensitive names |
| `/proc` | Read-only: `meminfo`, `stat` (uptime and CPU use), and `<pid>/status` for each process; `/proc/self` is the caller |

Disk names ignore case and are at most 63 characters, and no file can have holes. `/proc` files follow Linux's `Name:\tvalue` layout, but with fewer fields.

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

`EPERM ENOENT ESRCH EINTR EIO E2BIG ENOEXEC EBADF ECHILD EAGAIN/EWOULDBLOCK ENOMEM EFAULT EBUSY EEXIST ENOTDIR EISDIR EINVAL EMFILE ENOTTY ENOSPC ESPIPE EROFS EDOM ERANGE ENAMETOOLONG ENOSYS ENOTEMPTY EOVERFLOW EILSEQ`

Each kernel error maps to one of them. A read by a program that is not in the foreground fails with `EIO`, as a background read does on Linux when there are no signals to stop the program.

### `stdlib.h`

- **Process:** `exit`, `_Exit`, `abort` and `atexit`. `atexit` and `__cxa_atexit` share one list of 32 handlers.
- **Memory:** `malloc`, `calloc`, `realloc` and `free`.
  - Blocks are 16-byte aligned and come from a first-fit allocator with address-ordered free lists.
  - The heap grows in 64 KiB steps. Freed memory is reused, and once more than 256 KiB is free at the end of the heap, all but 64 KiB of it goes back to the kernel. Free space below blocks still in use stays, since only the end of the heap can move.
  - Freeing a pointer that `malloc` did not return, or freeing one twice, aborts.
- **Numbers:**
  - `abs`, `labs`, `llabs`, `div`, `ldiv` and `lldiv`.
  - `atoi`, `atol`, `atoll`, `strtol`, `strtoll`, `strtoul` and `strtoull`, with bases 0 and 2–36.
  - `strtod`, `strtof`, `strtold` and `atof`. These are correctly rounded (ties to even), and accept hexadecimal floats, `inf`, `infinity` and `nan(...)`.
- **Sorting:** `qsort`, a heapsort, so it never needs extra memory but is not stable. `bsearch`.
- **Environment:** `getenv`, `setenv` and `unsetenv`. `putenv` is left out, because the table keeps its own copies of the strings.
- **Random numbers:** `rand` and `srand`, a 64-bit linear congruential generator.

Not provided:
- `system`, because there is no `/bin/sh -c`
- `mkstemp`, `realpath`, `mbtowc` and the other multibyte functions

### `stdio.h`

**Streams.** `stdin`, `stdout` and `stderr` are the console. `stdout` is line-buffered and `stderr` unbuffered. Reading the console flushes `stdout` first, so a prompt without a newline still appears.

- **Opening:** `fopen` takes `"r"`, `"w"` and `"a"`, each optionally with `+`; a `b` is ignored. A disk file can have only one writer at a time, so a second open for writing fails with `EBUSY`. `fdopen` is also provided.
- **Buffers and errors:** `fclose`, `fflush`, `setvbuf` and `setbuf`. `setvbuf` uses only the mode: every stream keeps its own 512-byte buffer. `feof`, `ferror`, `clearerr` and `fileno` are also provided.
- **Reading and writing:** `fread`, `fwrite`, `fgetc`, `getc`, `getchar`, `ungetc`, `fgets`, `getline`, `fputc`, `putc`, `putchar`, `fputs` and `puts`.
- **Seeking:** `fseek`, `ftell` and `rewind`. Offsets past the end of a file fail with `EINVAL`, and the console cannot seek (`ESPIPE`).
- **Files:** `remove`, and `perror`.

**Formatting.** The family is `printf`, `fprintf`, `dprintf`, `sprintf` and `snprintf`, plus their `v` forms. It supports:
- every flag, width and precision (including `*`)
- the length modifiers `hh h l ll j z t L`
- the conversions `d i u o x X c s p n %`, and `f F e E g G a A`

Floating-point output is exact: it is computed with big integers, and matches glibc digit for digit. Positional arguments (`%1$d`) and wide characters (`%lc`, `%ls`) are not supported.

**Scanning.** `scanf`, `fscanf` and `sscanf`, plus their `v` forms, read:
- integers, with the same length modifiers as `printf`
- floating-point numbers, through the same parser as `strtod`
- `%c`, `%s`, scansets (`%[...]`), `%n`, `%%`, assignment suppression (`%*d`) and field widths

Not provided:
- `tmpfile`, `tmpnam`, `rename`, `popen` and `freopen`
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
- **Exact, bit for bit the same as glibc:** `fabs copysign floor ceil trunc round fmod frexp ldexp scalbn modf fmin fmax sqrt`
- **Within 1–3 units in the last place of glibc:** `exp exp2 log log2 log10 pow sin cos tan asin acos atan atan2 sinh cosh tanh hypot`

Also provided:
- the classification macros: `fpclassify isnan isinf isfinite isnormal signbit`
- `HUGE_VAL`, `INFINITY` and `NAN`
- the common `M_*` constants

Errors set `errno` (`math_errhandling` is `MATH_ERRNO`):
- `EDOM` outside a function's domain
- `ERANGE` on overflow, underflow or a pole

Differences:
- `sin`, `cos` and `tan` reduce their argument with a three-part π/2, so they lose precision above about 10^6.
- There are no `long double` functions.
- AArch64 programs cannot do `long double` arithmetic, because the compiler runtime routines for 128-bit floats are missing. They can still print, parse and copy `long double` values.
- `fenv.h`, `lround`, `nextafter`, `erf`, `lgamma` and other less common functions are not provided.

### `time.h`

There are no time zones: every time is UTC.

- **Clocks:** `time`, and `clock_gettime` with `CLOCK_REALTIME` (whole seconds) and `CLOCK_MONOTONIC` (milliseconds since boot).
- **Conversions:**
  - `gmtime` and `gmtime_r`; `localtime` and `localtime_r` return the same as gmtime.
  - `mktime` and `timegm`, which normalize out-of-range fields.
  - `asctime` and `ctime`.
  - `strftime` with every C99 conversion, in the C locale.
- **Sleeping:** `nanosleep`, which is never cut short, so `remaining` is always zero.

Differences:
- Dates before 1970 fail with `EOVERFLOW`.
- `clock`, `difftime`, `timer_create` and `CLOCK_PROCESS_CPUTIME_ID` are not provided.

### `unistd.h`, `fcntl.h`, `sys/stat.h`, `sys/types.h`

- **Descriptors:**
  - `read`, `write`, `close` and `lseek`.
  - `open` and `creat`, with `O_RDONLY O_WRONLY O_RDWR O_CREAT O_EXCL O_TRUNC O_APPEND O_CLOEXEC`. The lowest free descriptor is used, out of 32.
  - `dup` and `dup2` share the open file and its offset. `fcntl` supports `F_DUPFD`, `F_DUPFD_CLOEXEC`, `F_GETFD` and `F_SETFD` (`FD_CLOEXEC`); `F_GETFL`, `F_SETFL` and locks are not provided.
  - Programs started with `posix_spawn` inherit every descriptor not marked close-on-exec.
  - `pipe` and `pipe2` (with `O_CLOEXEC`) make a 4 KiB pipe. `PIPE_BUF` is 512: writes up to that size are never split. Reading returns 0 once every write end is closed; writing with no read end left fails with `EPIPE`, since there is no `SIGPIPE` yet. `fstat` reports `S_IFIFO`, and seeking fails with `ESPIPE`. `O_NONBLOCK` is not supported.
  - With `O_APPEND`, every write goes to the end, as POSIX requires.
  - `O_CREAT` and `O_TRUNC` need write access.
  - Offsets past the end of a file fail with `EINVAL`, because files cannot have holes.
- **Files and directories:** `unlink`, `rmdir`, `mkdir`, `chdir`, `getcwd`, `stat`, `fstat`, `access` and `isatty`.
  - There are no users or permissions.
  - `stat` reports owner 0, and mode bits that only show whether a file is writable: `0644` or `0444`, and `0755` or `0555` for directories.
  - Every time in `stat` is 0, because FAT16 timestamps are not read.
  - `access` fails `W_OK` for the read-only initramfs and `/proc`.
  - `isatty` is true only for the console, not for the devices in `/dev`, which are also `S_IFCHR`.
  - `mkdir` ignores its mode.
- **Processes:** `getpid`, `getppid`, `getpgrp`, `getpgid` and `_exit`. `tcgetpgrp` and `tcsetpgrp` work on the console, the only terminal; only init and session leaders (the shell) may move it, to their own group or one holding one of their children, and others get `EPERM`. `setpgid`, `setsid` and `getsid` are not provided: groups and sessions are chosen when `posix_spawn` starts the child.
- **Sleeping:** `sleep` and `usleep`.
- **`environ`.**
- **`sysconf`:** `_SC_NPROCESSORS_ONLN` and `_SC_NPROCESSORS_CONF` (the same, from `/proc/stat`), `_SC_PAGESIZE` and `_SC_OPEN_MAX`; other names fail with `EINVAL`.
- **`sbrk`:** moves the end of the heap that `malloc` also uses. A negative increment returns the whole pages above the new end to the kernel, and going below the heap's start fails with `EINVAL`. `malloc` never lowers the break below memory a program got from `sbrk` itself. `brk` is not provided.

Not provided:
- `fork` and the `exec*` family, because processes start with `posix_spawn`
- `link`, `symlink` and `rename`
- `chmod` and `chown`
- `getuid` and the other user and group functions
- `ftruncate`, `fsync` and `fcntl`

### `dirent.h`

`opendir`, `readdir`, `rewinddir` and `closedir`.

- **Entries:** each has `d_ino`, `d_type` (`DT_DIR` or `DT_REG`) and `d_name`. `d_ino` numbers the entries from 1 within their directory.
- **Paths:** `opendir` makes its path absolute, so a later `chdir` does not change which directory the stream reads.
- **Left out:** listings do not include `.` and `..`. `scandir`, `telldir`, `seekdir` and `fdopendir` are not provided.

### `spawn.h` and `sys/wait.h`

**`posix_spawn` and `posix_spawnp`.** The path names a program in `/system`, either as `/system/ls` or as `ls`; programs elsewhere fail with `ENOENT`.
- **Arguments:** the kernel always makes the program's name its `argv[0]`.
- **Errors:** an argument containing a double quote fails with `EINVAL`. A command line or environment over the limits fails with `E2BIG`.
- **Console:** by default the child takes the console until it exits.
- **PATH:** `posix_spawnp` does not search `PATH`, since every program is in `/system`.

**Attributes.** The supported flags are:

| Flag | Effect on FeatherOS |
| --- | --- |
| `POSIX_SPAWN_SETSID` | The child leads a new session: Ctrl+C stops at it, and it may call `reboot`. init starts the shell this way. |
| `POSIX_SPAWN_SETPGROUP` | The child joins the attributes' process group: 0 for a new one it leads, or an existing group that is the caller's or holds one of the caller's children. Others fail with `EINVAL`. |
| `POSIX_SPAWN_BACKGROUND` | FeatherOS: the child's group does not take the console. With a new group, that makes a background job, whose console reads fail with `EIO`. Without this flag, the child's group becomes the foreground group, as if the caller had called `tcsetpgrp`. |

Other flags fail with `EINVAL`.

**File actions.** `posix_spawn_file_actions_adddup2`, `addopen` and `addclose` run in order, at most 8 per spawn. The C library applies them to the caller's own descriptors around the `Spawn` syscall and puts those back afterwards. Another thread using those descriptors meanwhile would see the child's, so spawn with file actions from one thread at a time. A failing action makes `posix_spawn` return its error without starting the child. `addopen` paths are at most 63 characters.

**`waitpid` and `wait`.** They use the Linux status encoding with `WIFEXITED`, `WEXITSTATUS`, `WIFSIGNALED` and `WTERMSIG`, plus `WNOHANG`. There are no signals yet, but two outcomes are reported as if signals ended the child:
- a CPU exception reports signal 11 (`SIGSEGV`)
- Ctrl+C reports signal 2 (`SIGINT`)

A `pid` of -1 waits for any child `posix_spawn` started, 0 for one in the caller's process group, and -n for one in group n, by polling every 10 ms.

### `signal.h`

`sigaction`, `signal`, `kill`, `raise`, `sigprocmask`, `sigemptyset`, `sigfillset`, `sigaddset`, `sigdelset` and `sigismember`, with `pause` in `unistd.h` and `strsignal` in `string.h`. Signals have Linux's numbers: `SIGHUP`, `SIGINT`, `SIGQUIT`, `SIGILL`, `SIGTRAP`, `SIGABRT`, `SIGBUS`, `SIGFPE`, `SIGKILL`, `SIGUSR1`, `SIGSEGV`, `SIGUSR2`, `SIGPIPE`, `SIGALRM`, `SIGTERM`, `SIGCHLD` and `SIGCONT`.

- **Delivery:** a handler runs when the program next returns from the kernel, after a syscall or when a timer tick preempts it, so `raise` returns after its handler has run. The handler gets only the signal number. While it runs, the signal and `sa_mask` are blocked, unless `SA_NODEFER`; `SA_RESETHAND` puts the default action back first. `signal` installs a handler that stays, as glibc's does.
- **Interrupted calls:** `read` and `write` on the console or a pipe, `waitpid`, `sleep`, `nanosleep` and `pause` fail with `EINTR` once a signal arrives (`sleep` returns the seconds left, and `nanosleep` the time left). `SA_RESTART` is accepted, but calls are not restarted.
- **Defaults:** most signals end the program; `SIGCHLD` and `SIGCONT` are ignored. `SIGKILL` cannot be caught, ignored or blocked. A write to a pipe without a reader raises `SIGPIPE`, so a program sees `EPIPE` only if it ignores or handles it.
- **Faults:** a CPU exception ends the program with `SIGSEGV` and runs no handler.
- **Processes:** a child starts with handlers back at their defaults, while ignored signals and the blocked mask carry over. `kill(1, ...)` fails with `EPERM`, since init cannot be signaled.
- **Not provided:** `SA_SIGINFO` and `siginfo_t`, `sigsuspend`, `sigpending`, `sigaltstack`, `alarm`, and the stop signals (`SIGSTOP`, `SIGTSTP`, `SIGTTIN`, `SIGTTOU`), since processes cannot be stopped.

### `strings.h`

`strcasecmp` and `strncasecmp`, for ASCII.

### `sched.h`

`sched_getcpu`, the glibc extension, returns the CPU the caller was running on. That may change as soon as it returns. `sched_yield` lets other ready threads run first.

### `pthread.h`

`pthread_create`, `pthread_join`, `pthread_exit`, `pthread_self`, `pthread_equal`, `pthread_attr_init` and `pthread_attr_destroy`. They return an error number rather than setting `errno`.
- **Limits:** at most 16 threads per process, the main one included (`EAGAIN` past that). Each gets a kernel-managed stack of up to 63 pages, growing on demand behind a guard page, so attributes have nothing to set and stack sizes cannot be chosen.
- **Joining:** `pthread_join` refuses the caller itself (`EDEADLK`), the main thread, and unknown or already joined threads (`ESRCH`). Every thread is joinable; `pthread_detach` comes with step 3 of M11 Part 8.
- **Ending:** `exit`, a fault or a terminating signal in any thread ends the whole process. `pthread_exit` ends one thread; in the last thread running it exits as `exit(0)` does.
- **Signals:** actions are shared; masks are per thread, but there is no `pthread_sigmask` yet (`sigprocmask` sets the calling thread's). Signals sent to the process go to the main thread, or to the first live one after it has ended.
- **Missing:** mutexes, condition variables, `pthread_once`, `pthread_detach`, thread-local storage and `pthread_kill` (M11 Part 8, steps 2–4).

### `sys/ioctl.h`

Only `ioctl(fd, TIOCGWINSZ, &size)` on the console, which fills `ws_row` and `ws_col` with the text area below the status bar (80×24 without a framebuffer) and leaves the pixel sizes 0. Other descriptors fail with `ENOTTY`, and other requests with `EINVAL`. The console understands the VT100 sequences listed in `docs/development.md`.

### `termios.h`

`tcgetattr`, `tcsetattr` and `cfmakeraw` on the console; other descriptors fail with `ENOTTY`. The constants have FeatherOS's own values.
- **What has an effect:** `ICANON`, `ECHO` and `ISIG` in `c_lflag`, `ICRNL` in `c_iflag`, and `c_cc[VMIN]` and `c_cc[VTIME]` for non-canonical reads, with the POSIX meanings. The console's settings start with all four flags on, `VMIN` 1 and `VTIME` 0.
- **Accepted without effect:** the other flags that `cfmakeraw` clears (`BRKINT`, `IXON`, `OPOST`, `IEXTEN`, …). The console never changes output, and `tcgetattr` always reports `OPOST` and `CS8`. `VINTR`, `VERASE` and `VEOF` report Ctrl+C, Backspace and Ctrl+D but cannot be changed, and there is no end-of-file key yet.
- **Actions:** `TCSANOW`, `TCSADRAIN` and `TCSAFLUSH` all apply at once; `TCSAFLUSH` keeps input already typed.
- **Who may change them:** only the foreground group (`EIO` otherwise, as for a background read). When the program that changed them exits, the console returns to the defaults, so a program that crashes in raw mode does not leave the shell there.
- **Missing:** `cfgetispeed` and the other speed functions, `tcflush`, `tcdrain` and `tcsendbreak`.

### `sys/reboot.h`

`reboot(RB_POWER_OFF)` and `reboot(RB_AUTOBOOT)`, with Linux's values. Only init and session leaders may call it. The kernel then stops every process, flushes the disk, and powers off or restarts. Anyone else gets `EPERM`.

### `featheros.h`

FeatherOS calls with no POSIX equivalent:
- **`featheros_read_line(prompt, buffer, size)`** reads one console line with editing, and returns its length without the newline. It shows the prompt again after Ctrl+C discards a half-typed line, which printing a prompt and calling `fgets` cannot do. The shell, `calc` and `upper` use it. It flushes `stdout` first, but does not see input already buffered in `stdin`.
- **`featheros_arguments_page()`** returns the read-only page where the kernel placed the arguments and environment. `aslr` uses it to show that the page moves between runs.

### C++ support

The runtime hooks `__cxa_atexit`, `__cxa_guard_acquire`/`release`/`abort`, `__cxa_pure_virtual` and `__dso_handle` are provided. So are `operator new` and `operator delete`, in their plain, array and sized forms. They are built on `malloc`, and a failed `new` aborts; the aligned and `nothrow` forms are not provided yet. Programs are built with `-fno-exceptions -fno-rtti`, and there is no C++ standard library.

## Left out on purpose

These need kernel features that do not exist yet. Each one is planned in [roadmap.md](roadmap.md) or listed in [ideas.md](ideas.md).

| Missing | Why | Planned |
| --- | --- | --- |
| `SA_SIGINFO`, `sigsuspend`, `alarm`, real-time signals | Handlers get only the signal number; no timers send signals | Not scheduled |
| Stopping and continuing processes (`SIGSTOP`, `SIGTSTP`, job control stops) | Processes cannot be stopped | Not scheduled |
| `fork`, `exec*` | Processes start with `Spawn`; `fork` needs copy-on-write | M11, if adopted |
| Mutexes, condition variables, TLS, C11 `threads.h` | Threads exist, but there is no way to wait on a lock yet | M11 Part 8, steps 2–4, which also make the library thread-safe |
| `mmap`, `munmap` | The heap is the only growable mapping, so free space below used blocks cannot be returned | [ideas.md](ideas.md) |
| `locale.h`, `wchar.h`, multibyte | ASCII and the `"C"` locale only | Not scheduled |
| Time zones | Everything is UTC | Not scheduled |
| Users, permissions, `getuid` | No user accounts | Not scheduled |
| Sockets, `select`, `poll` | No networking | M10 |
| Dynamic linking, `dlopen` | Static PIEs only | [ideas.md](ideas.md) |
