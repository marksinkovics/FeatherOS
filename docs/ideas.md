# FeatherOS Ideas

These are directions that are not scheduled yet. The planned milestones are in [roadmap.md](roadmap.md). An idea moves to the roadmap, with the next milestone number, when work on it is planned. As with the roadmap, everything is written from scratch: Limine is the only outside code.

## Long-Term Goals

- **Develop FeatherOS on FeatherOS.** This builds on the M17 toolchain plus our own replacements for each host build tool:
  - a `make`-style build tool, in place of CMake, Ninja and the Python scripts
  - a disk and ISO image tool, in place of mtools and xorriso
  - a version-control tool
  - a way to install a newly built kernel and reboot into it
- **Claude on FeatherOS:** a `claude` chat program that calls the Claude API over HTTPS. Later, an agent that reads files and runs shell tools on the OS. Needs M10, TLS, an HTTP client and a JSON parser.
- **A windowed GUI,** a basic browser, and 3D graphics (see the sections below).

## Networking Applications

- **TLS 1.3:** our own SHA-256, HMAC/HKDF, ChaCha20-Poly1305 and AES-GCM, X25519, and certificate-chain checks with a built-in root-certificate list. Fine for a hobby OS, but not trusted with real secrets.
- **HTTP/1.1 client and JSON parser,** shared by `fetch`, the Claude client and the browser.
- **Browser, in stages:**
  1. a text browser like lynx (HTML to text, links, forms)
  2. a graphical browser with an HTML subset, CSS block and inline layout, and images
  3. JavaScript, building on the M18 interpreter work

## 3D Graphics

- **Software rasterizer:** a small OpenGL-like API ("FeatherGL") running on the CPU, with SIMD and work split across CPUs. It needs the floating-point state saved in M11 Part 3.
- **virtio-gpu with 3D:** QEMU hands 3D commands to the host GPU (virgl, or Venus for Vulkan). We implement the protocol, not import code.
- **Shaders:** a shader language compiler, reusing the M17 compiler work.
- **Not planned:** drivers for real NVIDIA, AMD or Intel GPUs. They need closed firmware blobs, which break the no-outside-code rule, and the hardware is enormous.

## Other Languages

- **Swift (Embedded Swift):** usable for experimental host-built apps through its C++ interop.
  - Embedded Swift compiles Swift's standard library into each binary, which is outside code.
  - The Swift compiler is built on LLVM, so Swift code could never be built on FeatherOS itself.
  - So keep the kernel and core programs in C++, and use Swift only as a deliberate exception.

## Executable Formats

- **Loaders behind one interface,** like filesystems behind the VFS, each producing the same in-memory process image.
- **PE (Windows `.exe`) and Mach-O:** load our own programs built in these formats. Clang can emit both with `-nostdlib`, for both architectures.
  - The useful work is the relocation formats: PE base relocations, and Mach-O rebase opcodes or chained fixups.
  - Running real Windows or macOS programs is not a goal. Both systems hide their kernel behind dynamic system libraries (`kernel32.dll`/`ntdll.dll`, `libSystem`), and their syscall numbers change between releases. Running those programs means re-implementing those libraries, which is Wine-sized work.
- **Mac-built programs (Mach-O):** write a console program on the Mac, build it with Apple's clang, and run it on FeatherOS. Revisit once M11 Part 3 (the C library) is complete.
  - **Build:** `clang -arch arm64|x86_64 -static -nostdlib -e _start`, linked against the FeatherOS libc built as Mach-O objects. macOS has no static `libSystem`, so the Mac's own C library cannot be used.
  - **What the binary contains:** a static Mach-O executable with no dynamic loader. Its segments are `__PAGEZERO`, `__TEXT`, `__DATA` and `__LINKEDIT`, and an `LC_UNIXTHREAD` command gives the entry point. Without `-pie` it is fixed at `0x100000000`. With `-Wl,-pie` it slides, using classic local relocations.
  - **Loader:**
    - map the `LC_SEGMENT_64` segments with their protections, leaving `__PAGEZERO` unmapped
    - take the entry point from the thread state
    - apply the local relocations for ASLR
    - ignore the code signature, UUID and build-version commands
  - **ABI:** Darwin's calling convention differs from the standard one (arm64 variadic arguments go on the stack, `x18` is reserved, symbols have a leading `_`). This only matters inside the binary, so the libc must be compiled with the same Apple target. The kernel's syscall boundary is unaffected.
  - **Limits:** no Apple frameworks and no C++ standard library (Apple's `libc++` is dynamic). Programs use only the FeatherOS libc and C++ runtime.
  - **Tests:** LLVM's `ld64.lld` links Mach-O on Linux too, so the matrix test does not require a Mac.
- **Linux compatibility:** the realistic way to run outside programs. Linux has a stable syscall interface, and statically linked Linux ELF programs exist. A Linux-syscall personality for a subset of calls could run static Linux tools unmodified. They are programs run on the OS, not code built into it.

## Platforms

- **RISC-V port:** add `riscv64` as a third architecture (Limine supports it). Building it would test how well the architecture boundaries hold up.
- **Real hardware:** boot a Raspberry Pi 4/5 and a real PC from a USB stick. This builds on M14 (USB) and M16 (physical storage) and needs drivers for each board's interrupt controller, timer and UART.
- **Broader hardware support:** devices beyond the QEMU targets, such as network adapters other than virtio-net, behind shared driver interfaces.

## Debugging And Observability

- **Stack traces with symbol names on panic:** walk frame pointers and look addresses up in a symbol table built into the kernel image.
- **`dmesg`:** keep the kernel log in a ring buffer that user programs can read.
- **GDB stub over serial:** our own implementation of GDB's remote protocol, for breakpoints and memory inspection on both architectures.
- **Sampling profiler:** record the interrupted address on timer ticks and report the busiest functions, like `perf top`.

## Storage

- **File timestamps** set from the RTC, and shown by `ls`.
- **FeatherFS:** a filesystem of our own design with journaling, so files survive a crash in the middle of a write.
- **Page cache:** keep recently read disk blocks in memory so repeated reads don't go to the disk.

## Graphics And Sound

- **Graphical desktop:** windowing and user-facing graphics on top of the framebuffer and input systems.
- **Framebuffer API for user programs,** with double buffering and a mouse cursor, once a USB or PS/2 mouse is supported.
- **Image viewer:** BMP first, then PNG with our own DEFLATE decompressor.
- **Games:** snake, tetris, and a Wolfenstein-style raycaster.
- **Sound:** Intel HDA or virtio-sound, a WAV player, and a `beep` command.

## Security

- **Users and permissions:**
  - `login` and file ownership
  - permission checks in the VFS
  - a `sudo`-like rule for administrative commands such as `shutdown`

## Scalability

These remove the known limits listed in `CLAUDE.md`:
- per-CPU run queues with load balancing
- TLB shootdown
- `mmap` and shared memory between processes
- swapping pages out to disk

## Project Infrastructure

- **Continuous integration:** a GitHub Actions job that builds both architectures, runs lint and unit tests, and runs `./build.sh test-all` on every push.
- **Installation and updates:** produce an installable image and define a reliable system update and recovery workflow.
