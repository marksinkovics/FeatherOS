---
name: add-program
description: Checklist for adding a FeatherOS user program (C++, Embedded Swift, Swift with concurrency or C++ interop, or full Swift) so that it builds, lands in the initramfs, is tested on both architectures and is documented. Use when creating a new program in src/user/.
---

# Adding a user program

## 1. Source and build list (`src/user/CMakeLists.txt`)

Entries are `<program name>:<source file>`; every built program goes into the initramfs as `/system/<name>`.

| Kind | Source | List |
|---|---|---|
| C++ | `src/user/<name>.cpp` with `main` | `FEATHEROS_PROGRAMS` |
| Defines `_start` itself (kernel ABI tests only) | `.cpp` | `FEATHEROS_START_PROGRAMS` |
| Embedded Swift | `src/user/<name>.swift` (top-level code) or a directory `src/user/<name>/` of several files | `FEATHEROS_SWIFT_PROGRAMS` |
| Embedded Swift with concurrency | `.swift`, top-level code runs on `MainActor` | `FEATHEROS_SWIFT_CONCURRENCY_PROGRAMS` |
| Embedded Swift importing C++ | `.swift`; own C++ files in `FEATHEROS_SWIFT_CXX_SOURCES_<name>`, modules in `swift_cxx/module.modulemap` | `FEATHEROS_SWIFT_CXX_PROGRAMS` |
| Full Swift (runtime type information) | `.swift`; cannot import `FeatherSupport` or other Embedded modules | `FEATHEROS_SWIFT_FULL_PROGRAMS` |

Swift sources borrowed from another program's directory go in `FEATHEROS_SWIFT_SHARED_SOURCES_<name>`.

## 2. Code rules

- Use libc (C++) or `FeatherSupport` (Embedded Swift), not raw syscalls, unless the program tests the kernel ABI itself.
- Parse arguments with `src/user/command_line.h` (C++) or `FeatherSupport`'s `Arguments`/`CommandLine`. Give it `-h` with a `usage:` line, errors as `<name>: <path>: <strerror>` on stderr, and a non-zero exit status on failure.
- A missing POSIX call goes into `src/libc/` (declared in `src/libc/include/`), not into the program, and is documented in `docs/posix.md`.
- Code shared by several programs goes in a header with declarations plus a `.cpp` in `featheros_user_support`. Logic that can be tested on the host also goes in `tests/unit/CMakeLists.txt` with a Catch2 test (Swift: a CTest under `tests/unit/swift/`).
- Headers only declare (templates excepted), comments only where the code cannot explain itself, `FeatherOS::` namespaces with PascalCase functions in shared C++ code.
- Size: initramfs programs are stripped; keep an eye on the 64 MiB per-process page limit and the 63-page user stack.

## 3. Tests on both architectures

- `tools/smoke/cases/shell/`: add the program's input (the `b"..."` byte strings sent to the shell) and the exact output lines to the part of the shell session it belongs to, or a new part file listed in `PARTS` in `shell/__init__.py` (order matters: later parts use files earlier ones made). Cover normal use, an error path (a missing file) and `-h`. A part whose programs the build may skip (a Swift toolchain part) names one of them in `requires=` with a `missing_note=`; follow `swift_full.py` or `swift.py`.
- Programs that need a feature test mode (network, storage, input) go in that mode's session instead, and in `CASES` in `tools/matrix.py` if the mode needs a new matrix case.
- Check expected output against `build/<arch>/smoke-output.txt` from a real run; mind the testing gotchas (`cat` adds a newline, prompt matching).

## 4. Discoverability and docs

- Add the command to the shell's built-in help list in `src/user/shell.cpp` (the `"wc, grep <text>, sort, "` string).
- Describe it in `docs/development.md` in the list of programs (one bullet: synopsis with options, then what it does), plus a section if it has a protocol, format or design worth explaining.
- Programs that need data files put them under `initramfs/` (`system/` and `etc/` are copied in).

## 5. Verify

Build both architectures, `bash tools/quality.sh --fix` then `check`, then `./build.sh unit-test` and `SHELL_TEST=ON` (or the relevant mode) on x86-64, then on AArch64. The `test-runner` agent can do the runs.
