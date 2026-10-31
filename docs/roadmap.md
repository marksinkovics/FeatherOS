# FeatherOS Roadmap

## Goal

Grow FeatherOS from a Limine-booted kernel into a small operating system that supports both x86-64 and AArch64. The kernel should share architecture-neutral services where practical while keeping CPU entry, exceptions, interrupts, page tables, context switching, and userspace ABI code in separate architecture backends.

This file lists the milestones that are planned or complete. Directions that are not scheduled yet are collected in [ideas.md](ideas.md). An idea moves here, with the next milestone number, when work on it is planned.

## Current Foundation

- Limine boot protocol and x86-64/AArch64 kernel entry
- Freestanding C and C++ builds for x86-64 and AArch64
- Framebuffer drawing, text terminal, and screen logger
- COM1/PL011 serial logging, panic/assert helpers, and QEMU smoke tests for both architectures

## Milestones

### 1. Multi-Architecture Build And Debugging (Complete)

- Keep the existing x86-64 build and add an AArch64 cross-compile target with its own toolchain, entry code, and linker script.
- Select architecture-specific assembly and linker inputs in CMake instead of requiring NASM for every target.
- Keep common kernel services separate from `arch/x86_64` and `arch/aarch64` implementations.
- Add architecture-specific serial backends: COM1 for x86-64 and the platform UART (PL011 on QEMU `virt`) for AArch64.
- Make the build script create the appropriate boot image and launch QEMU for each architecture; use the x86 BIOS/UEFI path and AArch64 UEFI path supported by Limine.
- Keep panic/assert output allocation-free and available before framebuffer setup.
- Add a headless serial smoke test for each architecture.

**Complete:** clean builds produce bootable images for both targets; both boot in QEMU and report readiness over serial; deliberate panic tests report on both targets.

### 2. CPU Exceptions And Interrupts (Complete)

- Define a shared exception-reporting interface and architecture-specific saved-context structures.
- On x86-64, establish the GDT, 64-bit TSS, kernel/IST stacks, 16-byte IDT gates, and assembly exception stubs.
- On AArch64, verify the Limine entry exception level, install the exception vector table through `VBAR_EL1`, and capture the exception syndrome and fault address from `ESR_EL1` and `FAR_EL1`.
- Add fatal exception reporting before enabling hardware interrupts.
- Configure the x86 interrupt controller and the AArch64 Generic Interrupt Controller (GIC) in their respective backends.

**Complete:** x86-64 and AArch64 use architecture-specific saved contexts with a shared fatal exception-reporting interface. QEMU verifies x86 divide errors, AArch64 undefined instructions, and registered/acknowledged x86 PIC/PIT IRQ0 and AArch64 GICv2 SGI0 interrupts.

### 3. Physical And Virtual Memory (Complete)

- Parse Limine's memory map and reserve memory used by the kernel, bootloader, modules, and framebuffer.
- Implement an architecture-neutral physical page-frame allocator with allocation/free tests and accounting.
- Define common virtual-memory operations and permission flags, backed by x86-64 four-level paging and AArch64 translation tables.
- Implement architecture-specific TLB invalidation and memory attributes; begin with 4 KiB pages on both targets.
- Add a kernel heap on top of page allocation only after page allocation and mapping are reliable.
- Track physical addresses separately from virtual addresses using 64-bit types.

**Complete:** Both targets validate the Limine memory map, report architecture and memory-map coverage/usable RAM at startup, allocate and free physical frames with accounting, and create/remove 4 KiB mappings with permission checks and TLB invalidation. Dedicated QEMU tests verify adjacent mappings, read-only and NX protections, unmapped faults, and stale-translation invalidation. A page-backed, coalescing heap provides `malloc`, `calloc`, `free`, and C++ `new`/`delete`, with alignment, zeroing, overflow, and multi-page allocation tests.

### 4. Timers And Input (Complete)

- Provide a shared timer/tick interface with an x86-64 timer backend and the AArch64 Generic Timer backend.
- Add keyboard input through devices available on each QEMU machine, such as PS/2 on x86 and virtio-input on AArch64 `virt`.
- Buffer input in a shared event queue; keep interrupt handlers short and consume events in normal kernel context.
- Add terminal input and basic line editing.

**Complete:** Both targets provide monotonic timer ticks through x86 PIT IRQ0 and the AArch64 Generic Timer PPI 30. Keyboard events reach a shared input queue through x86 PS/2 IRQ1 and a DTB-discovered AArch64 virtio-mmio keyboard/GIC interrupt. The terminal supports printable ASCII, Backspace, and Enter. QEMU tests verify timer behavior and inject keyboard input on both targets; UART input remains available as a fallback.

### 5. Storage, VFS, And Filesystem (Complete)

- Start with a Limine-loaded initramfs so program loading does not depend on a disk driver.
- Define shared block-device and VFS interfaces; use a QEMU-supported virtio block device to reduce duplicated driver work across architectures.
- Add partition discovery and a read-only FAT filesystem as the first disk-backed filesystem.
- Support open, read, seek, stat, and close through the common VFS interface.

**Complete:** Both Limine boot paths load a read-only USTAR initramfs. The shared VFS provides open, read, seek, stat, and close over initramfs files and a read-only FAT16 volume discovered in a primary MBR partition. The shared 512-byte block API uses x86-64 virtio-PCI and AArch64 virtio-MMIO backends. QEMU tests verify known-file reads from both sources on both targets.

### 6. ELF64 And User Mode (Complete)

- Implement ELF64 validation and segment loading for both `EM_X86_64` and `EM_AARCH64` binaries.
- Define isolated user address spaces and a shared virtual address-space policy, with architecture-specific page mappings.
- Enter x86-64 ring 3 using the TSS kernel stack; enter AArch64 EL0 using its exception-level and stack conventions.
- Define one architecture-neutral syscall API with x86-64 and AArch64 entry/return mechanisms.
- Build separate minimal userspace runtimes and test programs for the AMD64 and AArch64 ABIs.

**Complete:** The shared ELF64 parser validates x86-64 and AArch64 executable headers, program-table bounds, load-segment ranges/alignment, and executable entry points. ISO generation cross-compiles target-matched freestanding C++ test programs with RX/RO/RW ELF segments; the kernel loads them into separate address spaces, zeros BSS, and applies final permissions. Both programs use a shared syscall wrapper and dispatcher, enter ring 3/EL0, write a character, exit, and verify kernel-memory isolation. Host tests cover syscall request validation, and QEMU verifies these behaviors on both architectures.

### 7. Processes, Scheduling, And Shell (Complete)

- Define shared process/thread lifecycle and scheduler policy, with architecture-specific context frames and switch code.
- Begin with cooperative task switching on both targets, then add timer-driven preemption after exception and context-switch paths are tested.
- Add wait/exit behavior, process arguments, and foreground input.
- Build a shared shell and compile it for each supported userspace ABI.

**Complete:** A fixed-capacity scheduler switches up to four tasks on both architectures; tasks can be preempted by periodic timer IRQs, return exit statuses, be waited on, and be reaped, and context switches preserve per-task user state (the AArch64 `SP_EL0` and the x86-64 TSS kernel stack). A shared process table spawns initramfs programs into separate address spaces with `argc`/`argv`-style arguments, tracks parents, and gives console input to the foreground process. Spawn, wait, read-line, and newline-capable output syscalls validate every user pointer through the caller's page tables, and a CPU exception in user mode terminates only that program. The kernel starts a shell from the initramfs that runs `calc`, `echo`, and `upper` and reports their exit statuses; host tests cover syscall dispatch and argument parsing, and QEMU shell sessions verify arguments, foreground input in a child process, and exit statuses on both targets.

### 8. SMP And Concurrency (Complete)

- Bring up multiple processors on x86-64 and AArch64, with architecture-specific startup and per-CPU state.
- Add synchronization primitives and inter-processor interrupts (IPIs) for shared kernel state and cross-CPU coordination.
- Extend scheduling to run work across CPUs while preserving the shared process and thread interfaces.
- Exercise concurrency and scheduling under multi-CPU QEMU configurations.
- Make the scheduler, process table, and page allocator SMP-safe with per-CPU run queues and locking.

**Single-CPU groundwork (complete):** An interrupt-saving `Sync::SpinLock` protects the scheduler, input queue, page allocator, heap, and process table, and the scheduler lock is handed across context switches. The boot thread is a schedulable task, an idle task halts the CPU when nothing is ready, and tasks block on wait queues (with optional deadlines) or sleep until a tick instead of busy-yielding; `Wait` and console `ReadLine` use them. QEMU scheduler tests verify blocking and wakeup, timed sleep, and wait deadlines on both targets.

**CPU bring-up (complete):** Both targets start every Limine-reported CPU (up to eight), give each its own exception state (x86-64 GDT/TSS/IST stacks, AArch64 vector base), and park it. QEMU boots with four CPUs, and the normal-boot test requires all of them to come online. Each CPU has a register-based CPU index (x86-64 `GS` base with `SWAPGS` on ring transitions, AArch64 `TPIDR_EL1`), per-CPU current and idle tasks, active page-table roots, and TSS, and serial output is locked. Every CPU takes IPIs and runs its own timer (AArch64 GICv2 SGIs and Generic Timer; x86-64 local APIC with a PIT-calibrated timer, keeping the legacy PIC for device interrupts), verified at boot on both targets.

**Complete:** Every CPU schedules from a shared run queue under the scheduler lock, with per-CPU idle tasks and IPI wakeups of idle CPUs, so tasks and user programs run on any CPU. The framebuffer console and page-table edits are locked, and AArch64 kernel code runs on `SP_EL1` so exception frames stay on each task's stack. QEMU tests verify that tasks run concurrently on three CPUs with an exact shared locked counter, and every shell and user-mode test runs with four CPUs. Follow-on work: per-CPU run queues and load balancing, TLB shootdown once kernel pages are unmapped or address spaces are shared, I/O APIC routing for x86-64 device interrupts, and a lock for `KLogger`.

### 9. Persistent Storage (Complete)

- Extend block-device and VFS support to allow writes as well as reads.
- Add a small writable filesystem and define clear behavior for errors and filesystem inconsistencies.
- Test persistence across kernel and QEMU restarts.
- Extend the FAT driver with GPT partition tables, subdirectories, and long filenames.
- Give the shell a current directory: a `cd` command, relative paths, and the directory shown in the prompt.
- Expose files to user programs through file-descriptor syscalls (open, read, write, close) with a per-process open-file limit (moved from M12).

**Part 1 (complete):** Both virtio-blk drivers write sectors as well as read them. They serialize requests across CPUs and refuse writes to a read-only device. `STORAGE_TEST` boots one disk image twice; the second boot reads the boot counter that the first boot wrote.

**Part 2 (complete):** FAT16 can create, write, shrink, and remove files in its root directory through the VFS. It zeroes new clusters, keeps the FAT copies identical, rejects looping chains, and orders updates so an interruption only leaks clusters. A full disk ends a write cleanly. Unit tests cover multi-cluster writes, overwrites, appends at cluster boundaries, truncation, removal, bad names, a full root directory, a full disk, and looping chains. `STORAGE_TEST` rewrites a multi-cluster file on each boot, and the smoke test checks the image with mtools.

**Part 3 (complete):** The `Open`, `Read`, `Write`, `Close`, `Seek`, and `Remove` syscalls give each process up to 16 descriptors, which is the open-file limit moved here from M12. A disk file has one writer or many readers, an open file cannot be removed, and descriptors close when a process exits. A blocking mutex serializes filesystem access. Unit tests cover descriptor numbering, the limit, flags, access modes, and sharing rules. The shell test's `filetest` runs 33 checks in QEMU on both architectures.

**Part 4 (complete):** The shell has `ls`, `cat`, `write [-a]`, and `rm`, backed by a `ReadDirectory` syscall and an `OpenAppend` flag. `STORAGE_TEST` writes a file with these tools on the first boot and reads it back, removes it, and lists the disk on the second, with mtools checking the image after each boot.

**Part 5 (complete):** Partition discovery reads GUID partition tables, checking both CRC32s, and falls back to the MBR for non-GPT disks. `DISK_LAYOUT=gpt` builds a GPT test disk that macOS also recognizes, unit tests cover a valid table and damaged ones, and matrix cases run the storage test on GPT disks.

**Part 6 (complete):** FAT16 resolves multi-component paths with `.` and `..`, creates directories with their `.` and `..` entries, grows subdirectories by a cluster when full, and removes only empty ones. The new `MakeDirectory` and `RemoveDirectory` syscalls back `mkdir` and `rmdir`, `ls` takes a directory, and open-file sharing rules key on the entry's directory and slot. Unit tests cover nested paths, growth, removal rules, and same-slot files in different directories, and the two-boot storage test now creates and removes a directory, checked by mtools.

**Part 7 (complete):** FAT16 reads and writes VFAT long names of up to 63 ASCII characters, with checksummed, sequence-checked name entries and generated `~N` short aliases. Lower-case 8.3 names keep their case through the NT case bits, names match case-insensitively by long or short name, and program arguments accept double quotes for names with spaces. Unit tests cover name rules, aliases, encoding and decoding, removal, and directory growth for name runs, and the two-boot storage test reads a long name written by mtools and writes one that mtools reads back.

**Part 8 (complete):** Each process has a working directory that children inherit, and file syscalls resolve relative paths against it, collapsing `.` and `..` lexically. `/` is the read-only initramfs and lists `disk/` with the archive's files. The shell has `cd` and `pwd` built-ins and shows the directory in its prompt, and the tools take relative paths. The `ChangeDirectory` and `CurrentDirectory` syscalls back them. Unit tests cover path resolution and the root listing, and the shell test changes directories, reads through relative paths, and lists `/`. The build also stops macOS `tar` from adding `._` AppleDouble entries to the initramfs.

Moved: storage drivers for physical hardware, to M16.

**Complete when:** both targets can create, write, read, and delete files on disk, with data surviving a reboot.

### 10. Networking (Complete)

**Summary:** Both targets run a TCP/IP stack in the kernel over QEMU's virtio-net, with IPv4 and IPv6 side by side: ARP and neighbor discovery, SLAAC, ICMP and ICMPv6, UDP, and TCP with retransmission, NewReno fast recovery, out-of-order reassembly and window scaling. Programs use BSD sockets from the C library, including non-blocking sockets and `poll`, and a resolver with `/etc/hosts`, `/etc/resolv.conf`, DNS over UDP and TCP, and reverse lookups. The programs on top are `ping`, `dhcp` (a service that holds its lease), `host`, `net`, `nc`, `fetch`, `httpd`, `echod` and `ntp`, which sets the kernel's new wall clock. `net down` and `net up` take the link away and back, while loopback keeps working. The status bar shows a green or red network dot. `NETWORK_TEST` drives all of it against servers the harness starts on the host, with a relay that drops and reorders TCP segments, on both targets. The remote shell moved to `docs/ideas.md` and waits for M23's pseudo-terminals.

- Add the first network driver for QEMU virtio-net, with architecture-appropriate device discovery and DMA handling.
- Build the network stack in stages: Ethernet, IPv4 and IPv6, ICMP, UDP, then TCP.
- Add basic socket interfaces and simple diagnostic and client/server programs.
- Grow the protocols in this order:
  1. `ping` over ICMP, for IPv4 and then IPv6
  2. DHCP (IPv6 takes its address from router advertisements), and DNS for names
  3. TCP
  4. a `fetch` HTTP client
  5. a small HTTP server that serves files from the disk
  6. NTP to set the clock
  7. a remote shell over TCP (moved to `docs/ideas.md`, after M23)

**Design:** the stack lives in the kernel under `src/kernel/network/`, with its packet formats, checksums, caches and the TCP state machine host-tested. Programs use BSD sockets from the C library, backed by socket syscalls, and a socket is a file descriptor. QEMU runs user-mode networking (slirp) on both targets: the guest is `10.0.2.15/24`, the gateway `10.0.2.2` (which also reaches the host's loopback), and the name server `10.0.2.3`; for IPv6 the prefix is `fec0::/64`, the gateway `fec0::2` and the name server `fec0::3`. Tests stay offline: the gateway answers ARP and ping, servers the test harness starts on the host's loopback are reached at `10.0.2.2`, and `hostfwd` forwards host ports to servers in the guest.

**IPv4 and IPv6:** each IP version gets its own part, IPv4 first, and everything above them is written once for both. From Part 2 on, sockets, routing and checksums take an address that holds either version, and the transport checksum is computed over a pseudo-header the IP layer supplies. Parts 4 to 11 then support both versions from the start and test each over both, except DHCP, which is IPv4-only.

**Part 1 (complete):** x86-64 PCI and AArch64 device-tree virtio-mmio discovery are shared helpers, which the virtio-blk and keyboard drivers now use too, and the page allocator hands out physically contiguous frames. A shared split virtqueue backs a virtio-net driver over legacy PCI (x86-64) or virtio 1.0 MMIO (AArch64), whose receive interrupt wakes a high-priority network task. Ethernet frames and ARP, with a 16-entry cache, replies for our address and a resolver with three retries, run at the fixed address `10.0.2.15/24` via `10.0.2.2`. Every QEMU run attaches the NIC on user-mode networking. Host tests cover addresses, frames, ARP packets and the cache, and `NETWORK_TEST` (both targets in the matrix) resolves the gateway to slirp's `52:55:0a:00:02:02`, the second time from the cache.

**Part 2 (complete):** IPv4 input and output (`src/kernel/network/ip.cpp`) checks header checksums, drops fragments, routes through the gateway off the local network, and loops our own address and `127.0.0.0/8` back inside the kernel. Echo requests to our address are answered straight to the sender's MAC, so the receive task never waits on ARP. Sockets are descriptors, starting with ICMP datagram sockets like Linux's ping sockets: the kernel sets each request's identifier and checksum and queues matching replies. The `Socket`, `Bind`, `SendTo`, `ReceiveFrom` and `SetSocketOption` syscalls back libc's `socket`, `bind`, `sendto`, `recvfrom` and `setsockopt` (`SO_RCVTIMEO`), with `inet_pton`/`inet_ntop` for IPv4. Above the IP layer addresses are `Network::IpAddress`, which holds either version, and the syscall ABI's `SocketAddress` has room for IPv6. `ping` sends an echo request a second and prints round-trip times and totals. Host tests cover the checksum, IPv4 and ICMP formats, the socket layer and address text, and the shell test pings the gateway, our own address and loopback on both targets.

**Part 3 (complete):** The interface takes a link-local address from its MAC, solicits a router and takes a global address from the advertised `/64` prefix (SLAAC); new addresses are optimistic while duplicate address detection runs in the background, so boot waits only for the advertisement. Neighbor discovery (`src/kernel/network/neighbor_discovery.cpp`) answers solicitations, learns from advertisements and resolves next hops into one neighbor cache shared with ARP; the receive filter lets IPv6 multicast through. IPv6 input and output skip extension headers, drop fragments, check ICMPv6 checksums over the pseudo-header, select the source address, route off-link destinations through the router, answer echo requests and loop `::1` and our own addresses back. `SendIp` now fills in a transport checksum at a given offset for the source it picks, ready for UDP and TCP. `AF_INET6` ICMP sockets, `inet_pton`/`inet_ntop` for IPv6 (RFC 5952 output), `netinet/icmp6.h` and `in6addr_any`/`in6addr_loopback` complete the user side, and `ping` takes `-6` or any address with a colon. Host tests cover the IPv6 and ICMPv6 formats, the pseudo-header checksum, address building and text, the neighbor cache and IPv6 sockets; `NETWORK_TEST` resolves `fec0::2` through neighbor discovery and the shell test pings `fec0::2`, `::1` and our global address on both targets.

**Part 4 (complete):** UDP sockets work over both IP versions (`src/kernel/network/udp.cpp`, `socket.cpp`): ports from 49152 up, binding to a specific address, checksums over the pseudo-header, and IPv4 broadcasts. The kernel now boots without an IPv4 address. init's new `once dhcp` service runs a user-space DHCP client (`src/user/dhcp.cpp`), which leases slirp's `10.0.2.15/24`, its gateway and name server. It configures them through a new `NetworkSettings` syscall (`network/settings.cpp`), which libc exposes as `featheros_network_settings`; before configuration, the IP layer accepts what a server sends to the address it offers. Router advertisements add their RFC 8106 DNS server, slirp's `fec0::3`, after the configured ones (`network/name_servers.cpp`). The C library resolves names with `getaddrinfo` (`src/libc/netdb.cpp`, messages in `dns_message.cpp`). It sends A and AAAA queries over UDP to the kernel's name servers over either IP version, follows CNAMEs, answers `localhost` itself, and refuses all-numeric names. `freeaddrinfo`, `gai_strerror` and `netdb.h` come with it. New programs: `net` shows the settings and sets name servers (`address#port`) or IPv4 by hand, `host` looks up names, and `ping` takes names. Host tests cover UDP headers and sockets, the DNS option in router advertisements, the name server list, DHCP messages and DNS queries and responses. `NETWORK_TEST` now also runs a shell session against a DNS server the harness starts on the host's loopback, reached as `10.0.2.2` and `fec0::2`. It checks the lease, both name servers, lookups (CNAME, missing name, `localhost`) and pings by name over both versions. Lease renewal, ICMP port-unreachable, `/etc/hosts`, `/etc/resolv.conf`, `getnameinfo` and DNS over TCP moved to Part 10.

**Part 5 (complete):** TCP over both IP versions. The segment format (`src/kernel/network/tcp.cpp`) reads the MSS option and skips others. The connection state machine (`tcp_connection.cpp`, RFC 9293) has no addresses, locks or clock of its own, so host tests drive it with scripted segments and times: both opens (simultaneous ones too), 16 KiB send and receive buffers whose free space is the window, MSS-sized segments, RFC 6298 retransmission with Karn's rule and backoff (go-back-N, 5 SYN retries, 8 data retries then a reset), RFC 5681 slow start, congestion avoidance and fast retransmit, zero-window probes, window updates after reads, every close sequence with a 60 s TIME-WAIT, and RFC 5961 challenge ACKs. Out-of-order segments are dropped. Sockets (`socket.cpp`) add stream sockets with `connect`, `listen` (a backlog of up to 16), `accept`, `send`, `recv`, `shutdown`, `getsockname` and `getpeername`, binding as if with `SO_REUSEADDR`, and resets for segments nobody takes. One high-priority TCP task sends every segment and runs the timers, since only the receive task completes address resolution; closed sockets linger until their connections finish, and closing with unread data resets. `read`, `write` and `connect` now work on every socket, and a connected UDP socket filters its peer. `poll` (`src/kernel/poll.cpp`, libc `poll.h`) covers files, pipes, sockets and the console, with `SIGPIPE` and `MSG_NOSIGNAL` for broken connections. The new `nc` program connects or listens and relays standard input and output. `NETWORK_TEST` runs `nc` against an upper-casing echo server on the host's loopback over both versions, against a closed port, through a `hostfwd` port to a guest server (QEMU has no IPv6 forwarding), between two guest programs over `127.0.0.1` and `::1`, and streams a 225 KiB program through the echo server and back.

**Part 6 (complete):** `fetch [-4 | -6] [-o file] url` gets an `http://` URL with HTTP/1.1 and prints the body or saves it to a file, created only once a 2xx response arrives. URLs, requests and responses live in `src/user/http_message.cpp` for the server parts to reuse: URLs take IPv6 literals in brackets, default to port 80 and to `http://` when the scheme is missing, and relative `Location`s resolve with RFC 3986's dot-segment removal. The response parser takes bytes as they arrive and moves body bytes to the front of each buffer, framed by `Content-Length`, chunked coding (other codings are refused) or the connection's close; it skips 1xx responses, keeps the start of overlong header lines, and refuses conflicting lengths. `fetch` follows up to 5 redirects and reports other statuses outside 2xx as failures. `nc` and `fetch` share `src/user/tcp_client.cpp`. Host tests cover URLs, references, requests and responses; `NETWORK_TEST` fetches from an HTTP server on the host's loopback over both versions: a `Content-Length` body, a chunked one, a relative and an absolute redirect, a 404, and 10000 lines ending with the connection saved with `-o`.

**Part 7 (complete):** `httpd [-4 | -6] [-p port] [directory]` serves a directory, the current one by default, on port 80 over both IP versions, one listener each, with a thread per connection and at most 8 at once. It answers GET and HEAD with `Content-Length`, a `Content-Type` from the extension, `Date` and `Server`, and other methods with 501. A directory redirects to its name with a trailing `/`, then sends its `index.html` or an HTML listing sorted by name, with links percent-encoded and names HTML-escaped. `src/user/http_message.cpp` gained an incremental request parser that stops at the end of each head, so pipelined requests stay in order. It applies the version's keep-alive default and the `Connection` header, refuses malformed heads with 400, 414, 431 or 505 (a missing or repeated `Host` included), and notes bodies, after which the server closes since it never reads them. Request paths are percent-decoded before their dot segments are removed, and a decoded `?` no longer shields what follows it, so no target climbs above the directory; the absolute form is accepted. Connections close after 10 s idle and drain what the client still sends, so the close never resets the response. Each request is logged as `client method target status bytes`. Host tests cover request heads, paths, link encoding and content types. In `NETWORK_TEST` the guest's `fetch` gets files, a redirect, a listing, a 404 and a 225 KiB program over `127.0.0.1` and `::1`. Through a second `hostfwd` port, a harness client checks pipelined GET and HEAD, escapes with `..` and `%2e%2e`, POST, HTTP/1.0, a missing `Host`, HTTP/2.0, the redirect's `Location`, the absolute form, and the program byte for byte. The test then checks the log.

**Part 8 (complete):** `ntp [-4 | -6] [-n] [-p port] [server]` sets the clock from an NTP server, `pool.ntp.org` unless named, over either IP version. The kernel's wall clock (`src/kernel/time/wall_clock.cpp`) reads the RTC once and then advances by timer ticks, so the status bar, `time` and `clock_gettime(CLOCK_REALTIME)` share one clock with 10 ms resolution; `ReadClock` now returns nanoseconds, and a new `SetClock` syscall backs libc's `clock_settime`. The RTC itself is never written, so a set time lasts until the next boot. SNTP version 4 packets live in `src/user/ntp_message.cpp`: timestamps that wrap in 2036 into era 1, requests whose transmit timestamp the reply must echo as its origin, kiss-o'-death codes, unsynchronized servers, and the offset and round-trip delay from the four timestamps (the delay floored at 0, since a trip can fit inside one tick). `ntp` sends each address up to 3 requests, waits 2 s for each, steps the clock by the offset and prints it with the new time, or with `-n` only measures; a kiss or an unsynchronized server moves on to the next address. init does not run it at boot, since QEMU's RTC already follows the host. Host tests cover the wall clock, timestamps, requests, replies and samples, and `libctest` (411 checks) sets the clock and puts it back. `NETWORK_TEST` runs `ntp` against an NTP server on the host's loopback whose clock starts in 2031: it measures over IPv4, sets the clock by name over IPv6, `date` must show 2031 and a second measurement an offset under a second, and a second port's `RATE` kiss must be reported.

**Part 10 (complete):** Part 4's follow-ups. `dhcp` is a `respawn` service that keeps its lease: it renews at T1 by unicast, rebinds at T2 by broadcast and drops the address when the lease ends (timing host-tested in `src/user/dhcp_lease.cpp`), and `dhcp -r` renews at once. UDP datagrams to unbound ports get rate-limited ICMP or ICMPv6 port unreachable errors, and those errors make connected UDP sockets fail with `ECONNREFUSED` (`nc -u`). `/etc` comes from the initramfs as a RAM copy beside `/tmp`, with `hosts`, which `getaddrinfo` reads before `localhost` and DNS, and `resolv.conf`, whose name servers and `timeout:`/`attempts:` options replace the kernel's list. `getnameinfo` names addresses from `/etc/hosts` or PTR records under `in-addr.arpa` and `ip6.arpa`, so `host` takes an address. A response with the truncation bit is asked again of the same server over TCP with the two-byte length prefix (RFC 7766). Host tests cover the lease timing, the error formats and rate limit, both file parsers, reverse names, PTR answers and truncated responses; `NETWORK_TEST` checks each over both IP versions, with the harness's DNS server truncating UDP answers over 512 bytes and answering them over TCP on the same port.

**Part 11 (complete):** Part 5's TCP follow-ups. `O_NONBLOCK` belongs to the open file, so `dup` and inheritance share it; `fcntl(F_SETFL)`, `SOCK_NONBLOCK`, `accept4` and `MSG_DONTWAIT` set it, pipes follow the same rules, and a non-blocking `connect` reports `EINPROGRESS` and later its outcome through `poll` and `getsockopt(SO_ERROR)`. `echod` serves several clients from one thread with them. Out-of-order segments are written straight to their place in the receive ring, up to 4 ranges, each with a duplicate ACK, and our sending side recovers with NewReno (RFC 6582) instead of go-back-N; the harness's `LossyRelay` drops and swaps segments in both directions to prove it. Windows scale (RFC 7323) when both SYNs carry the option: ours is a shift of 3, enough for 256 KiB, and rounding to its steps holds the right edge rather than pulling it back. Each direction's ring is on the heap, starting empty and doubling from 4 KiB up to 256 KiB as data needs it, so idle connections stay small; without scaling the receive window stays at most 64 KiB. Host tests cover each piece; `NETWORK_TEST` runs `echod` with a held and an echoed client, the lossy echoes over both versions, and about 2 MB through `echod` over `127.0.0.1` and `::1`, where both ends scale.

**Part 12 (complete):** Kernel and tool follow-ups. A zero-length user range is valid at any address, so `read`, `write`, `send`, `recv` and `poll` with nothing to copy no longer need guards around the checks; `hardening` covers each on the console, pipes, files and a loopback UDP socket. The kernel has freestanding `memcpy`, `memmove`, `memset` and `memcmp` (`src/kernel/string/memory_functions.cpp`), which GCC calls for struct copies and initializers on AArch64, so the structs once filled field by field use designated initializers. `sleep` takes seconds with an optional fraction (`ParseSeconds`, host-tested), and the shell help, `hardening`, `libctest` and smoke tests use it so.

**Part 13 (complete):** A network kill switch. `net down` resets the virtio-net device (status 0, so no DMA, interrupts or frames), drops the IPv4 configuration, the IPv6 addresses, the router and its name server and the neighbor cache, and closes every TCP connection to a peer outside `127.0.0.0/8` and `::1` with a new `ENETDOWN`, sending nothing more. While the link is down, `SendIp` refuses every destination that is not local with `SendStatus::NetworkDown`, a TCP `connect` without a source address fails the same way, `SendFrame` and address resolution refuse at once, and IPv4 cannot be set; loopback servers and clients keep working. A mutex the receive task holds around each frame keeps a frame from before the change from being handled after it. `net up` negotiates with the device again over emptied queues and restarts SLAAC with a router solicitation; IPv4 waits for `dhcp`. Its waits look at the link and its address each second, so it reports the address gone right after `net down` and leases again as soon as the link is up. It keeps its discovery backoff, without messages, while sends fail with `ENETDOWN`. The settings syscall gained `NetworkChangeLink` and a `link_up` field, which `net` shows as `link up` or `link down`. Host tests cover the neighbor cache's clearing, a dropped TCP connection, the new send status, connections the link going down ends or keeps, and connects while it is down; `NETWORK_TEST` holds a connection through the lossy relay, which `net down` must end with `Network is down`, then checks that `ping`, `nc`, `nc -u` and `fetch` fail with it over both versions while `ping 127.0.0.1` and a loopback `nc` pair work and the harness's servers see nothing, and after `net up` that the lease comes back by itself and `nc` reaches the host over both versions, on both targets.

**After Part 13 (complete):** Two fixes and one addition from trying the kill switch by hand.
- **The prompt came first:** since `dhcp` became a service, its lease message and the kernel's IPv4 log printed after the shell's first prompt. The shell's raw reads now set `FEATHEROS_WAKE_ON_OUTPUT` (`TerminalWakeOnOutput`). Output from other programs or the kernel ends such a read once it pauses for a tenth of a second, the kernel moves the cursor to a new row, and `LineEditor::Redraw` draws the prompt and the half-typed line again below it.
- **No IPv4 after `net up`:** `dhcp` had only checked its address before each renewal, half a day away. Its waits now look at the link and the address each second, as described above.
- **The network dot:** a dot between the CPU count and the clock in the status bar, green while `Network::HasRoute` finds the link up with an IPv4 gateway or an IPv6 router, red otherwise. `Term` draws status marker bytes 1 and 2 as filled dots, which a host test checks.

**Parts, in order:**
1. **virtio-net and ARP** (complete): device discovery shared with the other virtio drivers (PCI on x86-64, device-tree virtio-mmio on AArch64), a shared split virtqueue, and a virtio-net driver (legacy PCI on x86-64, virtio 1.0 MMIO on AArch64) whose receive interrupt wakes a network task. Ethernet framing and ARP, with a cache, answers to requests for our address, and a resolver with retries. The address is fixed at `10.0.2.15/24` via `10.0.2.2` until DHCP. Tests: host tests for frames and ARP, and `NETWORK_TEST` resolves the gateway on both targets.
2. **IPv4, ICMP and `ping`** (complete): IPv4 input and output with the Internet checksum and routing through the gateway (fragments are dropped), echo replies to incoming pings, and socket descriptors with `socket`, `bind`, `sendto`, `recvfrom` and `close`, starting with ICMP datagram sockets like Linux and macOS ping sockets. A `ping` program; the shell test pings the gateway.
3. **IPv6, ICMPv6 and `ping -6`** (complete): a link-local address from the MAC, neighbor discovery in place of ARP (solicitations, advertisements and a neighbor cache, with the solicited-node multicast frames let through the receive filter), duplicate address detection, router solicitation and a global address from the router's advertised prefix (SLAAC), IPv6 input and output with extension headers skipped and fragments dropped, ICMPv6 echo replies, and `AF_INET6` ping sockets. `ping -6` (or an IPv6 address) pings over IPv6; the shell test pings `fec0::2`. Host tests cover the IPv6 and ICMPv6 formats, the pseudo-header checksum, address building and the neighbor cache.
4. **UDP, DHCP and DNS** (complete): UDP sockets over both versions with receive timeouts, a DHCP client that init runs to lease the IPv4 address, gateway and name server, and a resolver in the C library (`getaddrinfo`, with A and AAAA records and both kinds of addresses). Tests answer from a DNS server in the test harness.
5. **TCP** (complete): the connection state machine, retransmission, windows and `connect`, `listen`, `accept`, `send`, `recv`, `shutdown` and `poll`. The state machine is host-tested against scripted segments; QEMU tests connect to a host echo server and accept a connection through `hostfwd`.
6. **`fetch`** (complete): an HTTP/1.1 client that prints or saves a page, tested against a host HTTP server.
7. **`httpd`** (complete): a small HTTP server for files on the disk, tested through `hostfwd`.
8. **NTP** (complete): a client that sets the clock, tested against a host NTP responder.
9. **Remote shell** (moved to `docs/ideas.md`): a shell over a socket is not a terminal, so line editing, `vi`, Ctrl+C and jobs would not work in it. It waits for M23's pseudo-terminals, after which it is a small relay between a TCP connection and a pty.
10. **Part 4 follow-ups** (complete): what Part 4 left out, in this order:
    - **DHCP lease renewal** (done; timing in `src/user/dhcp_lease.cpp`, discovery retried with backoff from 1 s to 64 s, `dhcp -r` sends SIGUSR1 to the running client, and the smoke harness waits for the client's lease before its network session): `dhcp` stays running as a `respawn` service instead of `once`. It renews at T1 by unicasting a request to the server, rebinds at T2 with a broadcast, and drops the address through `NetworkSettings` if the lease runs out. The timer logic is host-tested. slirp's leases last a day, so the QEMU test asks a running client to renew at once and checks that slirp acknowledges.
    - **ICMP port unreachable** (done; formats in `icmp.cpp` and `icmpv6.cpp`, a shared token bucket of 10 errors then one per 100 ms, errors sent straight to the sender's MAC, `nc -u` for UDP, and the harness echo server answering datagrams too): a UDP datagram to a port nobody has bound gets ICMP destination unreachable (type 3, code 3) or ICMPv6 type 1, code 4. Replies are rate-limited and never sent for broadcast or multicast datagrams. An incoming error for a UDP socket made with `connect` (Part 5) makes the next call fail with `ECONNREFUSED`. Host tests cover the error format; the shell test sends to a closed port on both versions.
    - **`/etc/hosts`** (done; the initramfs now holds `system/` and `etc/`, each mounted read-only, the parser in `src/libc/hosts_file.cpp` is host-tested, and a name listed only for the other family goes on to DNS): an `/etc` directory, shipped in the initramfs, holds a `hosts` file that `getaddrinfo` reads before DNS, with `localhost` listed in it.
    - **`/etc/resolv.conf`** (done; `/etc` is now the RAM filesystem's second root beside `/tmp`, copied from the initramfs at boot so both files can be edited until the next boot; the parser in `src/libc/resolv_conf.cpp` is host-tested, and the shell test writes the file to reach the harness's server over both versions, waits out a closed port with `timeout:1`, and falls back to the kernel's list): `nameserver` lines (with an optional port), plus `options timeout:` and `attempts:`. When the file has no name servers, the resolver falls back to the kernel's list, which DHCP and router advertisements fill.
    - **`getnameinfo`** (done; reverse names and PTR answers in `src/libc/dns_message.cpp`, host-tested, numeric output and `/etc/hosts` names checked by `libctest`, and the harness's DNS server answering PTR queries for both versions): numeric output with `NI_NUMERICHOST`, `NI_NUMERICSERV` and `NI_NAMEREQD`, names from `/etc/hosts`, and PTR lookups under `in-addr.arpa` and `ip6.arpa`. With it, `host` takes an address too.
    - **DNS over TCP** (done; `Dns::Outcome::Truncated` from the parser, `AskOverTcp` in `src/libc/netdb.cpp`, and the harness's server listening for TCP on its UDP port and truncating UDP answers over 512 bytes to the question, as servers without EDNS do): an answer with the truncation (TC) bit set is asked again over TCP, where each message has a two-byte length prefix (RFC 7766). The harness DNS server truncates one name over UDP and answers it over TCP; `host` looks it up over each version.

    Host tests cover the file parsers and the PTR names; the shell test exercises each item over both IP versions.
11. **TCP follow-ups:** what Part 5 left out, in this order:
    - **Non-blocking sockets** (done; `OpenNonBlocking` on the open file in `files.cpp`, `FileStatusFlags` and `GetSocketOption` syscalls, the nonblocking paths in `pipe.cpp` and `network/socket.cpp`, libc `F_GETFL`/`F_SETFL`, `SOCK_NONBLOCK`, `MSG_DONTWAIT`, `getsockopt` and `EINPROGRESS`, `libctest` over pipes and loopback, and `echod` in `NETWORK_TEST`): `O_NONBLOCK` becomes a flag of the open file, so `dup` and inheritance share it, set with `fcntl(F_SETFL)`, `SOCK_NONBLOCK` on `socket` and `accept4`, or per call with `MSG_DONTWAIT`. Each call that would wait fails with `EAGAIN` instead; pipes follow the same rule. A non-blocking `connect` fails with a new `EINPROGRESS`, and a program waits for `POLLOUT`, then reads the outcome with `getsockopt(SO_ERROR)`, through a new `GetSocketOption` syscall. Host tests cover each call without waiting; the shell test runs a small single-threaded server that serves two clients at once.
    - **Out-of-order segments** (done; ranges and NewReno in `network/tcp_connection.cpp`, whose advertised window now grows only by whole segments so duplicate ACKs keep their window; host tests script reordered, lost, overlapping and repeated segments, and `LossyRelay` in `tools/smoke_test.py` carries the guest's frames through QEMU `filter-redirector` objects, dropping and swapping segments each way on a second echo server's connections over both versions): a segment inside the receive window always has room in the receive ring, so it is written straight to its offset from `rcv_nxt`, and up to 4 out-of-order ranges are tracked; when the missing data arrives, the ranges merge and `rcv_nxt` jumps past them. Each out-of-order segment still gets a duplicate ACK, so the peer retransmits quickly. Our sending side moves from go-back-N to NewReno fast recovery (RFC 6582), and SACK (RFC 2018) may follow. Host tests script reordered, lost and overlapping segments; QEMU never loses packets, so the harness drops or reorders a few segments through a TCP relay on the host.
    - **Window scaling** (done; the option in `network/tcp.cpp`, negotiation, scaled windows and rings that grow in `network/tcp_connection.cpp`, freed by `TcpConnection::Release`; host tests cover the option format, both opens with and without the peer's option, scaled windows each way, rounding that keeps the right edge and both rings growing; `NETWORK_TEST` sends about 2 MB through `echod` over loopback on both versions): the window scale option (RFC 7323) is offered in SYN and SYN-ACK and used only when both sides send it. Advertised windows are shifted down and received ones up, and rounding never shrinks the right edge already advertised. Buffers move out of the connection and grow on demand up to 256 KiB each way, so idle connections stay small. Host tests cover the negotiation and scaled windows; the bulk transfer in `NETWORK_TEST` grows to a few megabytes.

    Each item lands with host tests and runs over both IP versions.
12. **Kernel and tool follow-ups** (done; zero-length ranges in `process.cpp`, checked by `hardening`; `memcpy`/`memmove`/`memset`/`memcmp` in `src/kernel/string/memory_functions.cpp`; `ParseSeconds` in `src/user/command_line.cpp`, host-tested): workarounds Part 5 ran into, in this order:
    - **Zero-length user ranges:** `UserRangeWritable` and `UserRangeReadable` in `process.cpp` treat a size of 0 as invalid, so a zero-length copy fails with `SyscallErrorBadAddress`. They accept it instead, and the guards Part 5 added around them (end-of-stream reads, empty sends, `poll` with no entries) go away. `hardening` covers zero-length reads, writes, `poll` and datagrams from address 0 on the console, pipes, files and loopback UDP, since `process.cpp` is not built for the host tests.
    - **`memset` and `memcpy` in the kernel:** the AArch64 kernel lacks them, so structs are filled field by field wherever a designated initializer or `= {}` might make the compiler call `memset` (`ip.cpp`, `tcp_connection.cpp`, `socket.cpp`, `files.cpp`, `tmpfs.cpp`). Freestanding versions in `src/kernel/string/` for both architectures remove those workarounds and the link errors they guard against.
    - **`sleep` in seconds:** `sleep` takes milliseconds today, unlike POSIX. It takes seconds, with fractions such as `0.5`, and the shell and smoke tests that call it change with it.
13. **Network kill switch** (complete; `Network::SetLink` in `interface.cpp`, `Device::Stop`/`Restart`, `Sockets::DropExternalConnections`, `net down`/`net up`, and `dhcp` leasing again once its address is gone): `net down` cuts every external connection, down to Ethernet, while `localhost` keeps working; `net up` brings the interface back.
    - **Down:** the virtio-net device is reset (status 0), so it stops DMA and interrupts and takes no frames at all, and `SendFrame` and the receive task refuse anything that would reach the driver. The internet, the QEMU host at `10.0.2.2`, DNS, DHCP, ARP and neighbor discovery are all cut off.
    - **Loopback stays:** `127.0.0.0/8` and `::1` never touch the device, so local servers and clients keep working.
    - **State:** the IPv4 address, the IPv6 addresses and the ARP and neighbor caches are dropped. Sends to anything else fail at once with a new `ENETDOWN`, and open TCP connections to outside hosts end with that error instead of retrying.
    - **Up:** the driver starts the device again, SLAAC begins with a fresh router solicitation, and IPv4 waits for `dhcp` or an address set by hand.
    - **Interface:** a `NetworkChangeLink` change in the `NetworkSettings` syscall with a field for the state, shown and set by `net`.

    Host tests cover the down state's refusals and cleared state; `NETWORK_TEST` runs `net down`, checks that `ping`, `nc` and `fetch` fail with `ENETDOWN` and that the harness's servers see nothing, that loopback still works, then `net up` and `dhcp` and checks the network works again, on both targets. Booting with the network off (a kernel command-line option) and honoring QEMU's link status (`set_link`) can follow later.

**Complete when:** both targets can exchange packets in QEMU over IPv4 and IPv6, pass basic ICMP/ICMPv6/UDP/TCP tests over both, and run a small network client and server.

### 11. System Services And Compatibility (Complete)

**File system layout:** The disk is mounted at `/` and the read-only initramfs at `/system`, as on Linux, and init and the shell start in `/home`. This replaces the earlier `/disk` mount point and initramfs root from M9.

**Part 1 (complete):** The kernel starts `init`, which runs the `once`, `respawn`, and `console` services listed in `/system/init.conf`, restarts services that exit, and disables one that exits five times within ten seconds. `Spawn` gained background and session-leader flags, `Wait` gained a non-blocking mode, and Ctrl+C jobs now end at the session leader. Unit tests cover the configuration format and restart limit, and the shell test checks that init restarts the shell after `exit`.

**Part 2 (complete):** The shell's `shutdown` and `reboot` built-ins call a `Shutdown` syscall limited to init and session leaders. The kernel stops other processes, quiesces the filesystem, flushes virtio-blk, parks the other CPUs, and uses PSCI on AArch64 or ACPI S5 and the reset register (with keyboard-controller and triple-fault fallbacks) on x86-64. Matrix cases check that QEMU exits after `shutdown` and boots again after `reboot` on both architectures, and `hardening` checks that ordinary programs are refused.

- Add an init process and define service startup and shutdown behavior.
- Add `shutdown` and `reboot`: PSCI `SYSTEM_OFF`/`SYSTEM_RESET` on AArch64; on x86-64, ACPI S5 through the FADT's PM1a control port (`\_S5` sleep type found by scanning the DSDT) and the ACPI reset register with the keyboard-controller reset as a fallback. Restrict them to the session leader, stop the other CPUs, flush the disk (virtio-blk flush), and halt if power-off fails. Test that QEMU exits on `shutdown` and boots again after `reboot`.
- Expand the userspace runtime and libc around a documented, deliberately scoped POSIX subset (Part 3 below).
- Add a core set of shell utilities, and run programs from the disk as well as the initramfs (Parts 9 and 10). Packaging programs moved to [ideas.md](ideas.md).
- Add a full-screen text editor (`vi`, a small vi), needed to write code on the OS for M17 and M18.
- Add `ps`, `top` and `kill`, with per-process CPU time from the scheduler's tick accounting.
- Add virtual files:
  - `/dev/null` and `/dev/zero`
  - `/dev/random`, backed by `RDRAND`/`RNDR`
  - `/proc/<pid>/status` and `/proc/meminfo`
- Add a RAM-backed `/tmp`.
- ~~Add a `cron`-style service that init runs, starting programs on a schedule.~~ Moved to [ideas.md](ideas.md).
- Broaden syscall coverage and the userspace library, and add pipes, background jobs, and process signals.
- Improve the shell with line editing beyond Backspace, command history and a documented command set. A more capable `calc` moved to [ideas.md](ideas.md).
- ~~Add kill/abort for a stuck child and reclaim a child's resources when its parent exits first.~~ Done early (see M15): Ctrl+C interrupts the foreground job, and an exiting parent terminates and reaps its children.
- Add user threads with thread-local storage (`PT_TLS`, moved from M13). The C library must become thread-safe at the same time:
  - a lock in `malloc`, with a futex-style syscall so a waiting thread sleeps instead of spinning
  - a per-thread `errno` behind `__errno_location()`
  - locked `__cxa_guard_*` for function-local statics
  - locked stdio streams
  - a TLB shootdown when the heap shrinks, since `UnmapUserPage` flushes only the current CPU, which is enough only while each address space runs on one CPU at a time
- If a `fork`-style API is adopted, build it on copy-on-write and demand paging (moved from M13); processes are currently created with `Spawn`.

**Remaining parts, in order** (agreed after Part 3):
4. **Virtual files** (complete): `/dev/null`, `/dev/zero`, `/dev/random`, a RAM-backed `/tmp`, `/proc/<pid>/status` and `/proc/meminfo`. This adds file kinds other than disk files, which pipes reuse, and `ps` reads `/proc`.
5. **Signals, pipes and jobs** (complete), in five steps: (1, complete) one descriptor table with shared open files, `dup`/`dup2`/`fcntl`, `O_CLOEXEC`, inheritance through `Spawn`, `posix_spawn` file actions, and the shell's `<`, `>`, `>>`, `2>` and `2>&1`; (2, complete) `pipe` and the shell's `|`; (3, complete) process groups, so a pipeline is one Ctrl+C job, and `&` with `jobs`; (4, complete) signals; (5, complete) `primes` over pipes. In detail: signals and `kill`, `pipe` and `dup2`, and the shell's `|`, `>`, `<` and `&`. `primes` can then return results through a pipe instead of its exit status.
6. **Process tools** (complete): `ps` and `top`, with per-process CPU time from the scheduler's tick accounting.
7. **Terminal** (complete), in five steps: (1, complete) VT100 cursor control and the terminal size; (2, complete) raw keyboard input and termios; (3, complete) shell line editing and history; (4, complete) `vi`; (5, complete) `top` redrawing in place (`clear` came with step 3).
8. **Threads** (complete), in five steps:
   1. **Threads in a process** (complete). Each thread is a scheduler task running the process's program with its own execution context (kernel stack, signal mask, thread ID). `ThreadCreate`, `ThreadExit`, `ThreadJoin` and `ThreadId` syscalls, and `pthread_create`, `pthread_join`, `pthread_exit`, `pthread_self` and `pthread_equal` in a new `pthread.h`. At most 16 threads per process. Stacks are kernel-managed slots of 63 demand-grown pages with a guard page, stacked below the main stack, and stay mapped for reuse, so nothing is unmapped before step 5. `exit`, a fault or a terminating signal in any thread ends the whole process; `pthread_exit` ends one thread, and the process ends with its last. Signal actions are shared, masks are per thread, and process-directed signals go to the main thread (or the first one alive). A per-process memory lock guards page faults and the heap, and the heap gives memory back only while the process has a single thread.
   2. **Thread-local storage** (complete): `PT_TLS` in the ELF loader, a per-thread pointer (`FS` base on x86-64, `TPIDR_EL0` on AArch64) saved on each switch, and `__thread` in programs.
   3. **Waiting** (complete): a futex-style wait/wake syscall, then `pthread_mutex_*`, `pthread_cond_*`, `pthread_once` and `pthread_detach`.
   4. **A thread-safe C library** (complete): a lock in `malloc`, `errno` per thread through TLS, locked `__cxa_guard_*`, and locked stdio streams. Also `__cxa_thread_atexit`, so `thread_local` objects with destructors work: a per-thread list in TLS that a thread's end and `exit` (for the main thread, before the `atexit` handlers) run newest first, tested for running in the right thread and in reverse order.
   5. **TLB shootdown** (complete): unmapping in an address space that other CPUs may be running sends them an IPI to flush it, so the heap can shrink again with threads, and thread stacks can be freed.

9. **Core shell utilities** (complete; step 1: `cp`, `touch`, `head`, `tail`, `wc`, `grep` and `sort` over `src/user/text_tools.cpp`, host-tested, with the shell test running each, and the test's time limit raised to 90 seconds; step 2: the `Rename` syscall (45) with renaming in FAT16 and tmpfs, `rename` in libc with `EXDEV`, and `mv`, which shares `src/user/file_copy.cpp` with `cp`): `cp`, `mv`, `touch`, `head`, `tail`, `wc`, `grep` and `sort`, each with `--help`, working on files and on standard input so they fit in pipelines. `mv` needs a `Rename` syscall and renaming in FAT16 (in one directory and across directories, long names included); across filesystems it copies and removes. `grep` matches plain text first, with `-i`, `-v`, `-n` and `-c`; `sort` sorts lines, with `-r`, `-n` and `-u`. Host tests cover the parts that are logic (line splitting, matching, sorting, FAT16 renaming), and the shell test runs each tool in a pipeline on both targets. These come first, since Part 10 tests itself with `cp`.
10. **Programs from the disk** (complete): `Spawn` starts an ELF from any path the VFS can read (the disk or `/tmp`), not only `<name>-<arch>.elf` from the initramfs. The kernel reads the file into its own memory, up to a size limit, and checks it as it checks initramfs programs; FAT16 has no execute bit, so any regular file may be tried, and one that is not a valid program fails with `ENOEXEC`. `posix_spawnp` and the shell search `PATH`, which becomes `/system:/bin`, and a name with a `/` runs that file. Tests copy a program to `/bin` and to `/tmp` with `cp` and run it from both, check that a missing or invalid file fails cleanly, and check the program limits (page limit, arguments, ASLR) for disk programs too.

`cron`, packaging programs and a more capable `calc` moved to [ideas.md](ideas.md). `fork` stays optional.

**Part 10 (complete): programs from the disk.** This completes M11: init supervises services, and the shell runs a documented set of utilities, plus programs copied to the disk, on both targets.
- **Kernel:** `Spawn` takes the program from the command line's first word, after quotes are removed: a name without a `/` is a file in `/system`, and anything else a path resolved against the parent's working directory. `Files::ProgramFile` reads it under the filesystem lock, using an initramfs file in place and copying any other into a kernel buffer of at most `MaximumProgramSize` (1 MiB), and the same `ELF::Parse` and loader checks follow. A missing file fails with `SyscallErrorNotFound`, a directory with `SyscallErrorIsDirectory`, a device, an empty file or anything that is not a valid program with `SyscallErrorNotExecutable`, a larger file with `SyscallErrorNoMemory`, and a path too long once resolved with `SyscallErrorNameTooLong`. A process is named after the last part of its path.
- **Build:** the initramfs holds each program as `/system/<name>` instead of `<name>-<arch>.elf`, so `cp /system/echo /bin/say` copies a program, and the test disk has an empty `/bin`.
- **C library:** `posix_spawn` passes paths through (a path in `/system` still goes as the bare name, which leaves room on the 63-character line), and `posix_spawnp` searches `PATH`, which `init.conf` sets to `/system:/bin` and which is also the default when unset. The shell reports `Unknown command` only for a name, and `shell: <word>: <error>` otherwise.
- **Tests:** host tests for the command line, the `PATH` search and `ProgramFile` limits; `libctest` (407 checks) runs a copy of `echo` in `/tmp` by path, relative path and `PATH`, and checks the errors; `hardening` (61 checks) checks the exact `Spawn` errors, a truncated program and the kernel-half ELF copied to `/tmp`, and a file over the limit; the shell test copies programs to `/bin` and `/tmp` with `cp`, runs them by name and path, checks their names, arguments, ASLR (`aslr`) and the page limit (`memtest exhaust`), and runs a missing file, a text file and a directory.

**Part 9 (complete): core shell utilities.**
- **Programs:** `cp`, `mv`, `touch`, `head`, `tail`, `wc`, `grep` (`-ivnc`) and `sort` (`-rnu`), each with `--help`, reading their files or standard input so they work in pipelines. Their shared logic is in `src/user/text_tools.cpp` (counting, matching, numeric ordering, option parsing, the last N lines) and `src/user/file_copy.cpp` (target paths and copying, for `cp` and `mv`).
- **Kernel:** a `Rename` syscall (45). FAT16 writes the new entry before clearing the old one, so a crash leaves both names rather than neither; it moves a directory's `..` with it, refuses to put a directory inside itself (checked by walking `..`, so another case of the path is caught too), and handles a new case for the same name. tmpfs moves the node, so its index stays valid. `Files::Rename` replaces a file at the target, and refuses open files (`SyscallErrorBusy`), directories in the way, mount points, read-only paths, and other filesystems with a new `SyscallErrorCrossDevice`.
- **C library:** `rename`, and `EXDEV`. `mv` falls back to copying and removing a file between the disk and `/tmp`.
- **Tests:** host tests for the text tools, FAT16 and tmpfs renames (data, cluster counts, `..`, loops, long names, case changes) and the `Rename` errors; the shell test runs each tool, several through pipes, and moves files and directories within `/tmp` and from `/tmp` to the disk. The shell test's time limit went up to 90 seconds.

**Part 8, step 5 (complete): TLB shootdown.** `Memory::ShootDownUserMappings` returns once no CPU can still use a translation for a page unmapped from an address space, and unmapped frames are freed only after it. On x86-64 it sends an IPI to each other CPU whose active root is the address space and waits for it to reload `CR3`, serving its own requests while it waits, and `ActivateAddressSpace` records the root before loading it so a CPU switching in cannot be missed. On AArch64 the `tlbi vaae1is` that unmapping already used is broadcast in hardware, so nothing more is sent. The heap now shrinks in processes with threads, unmapping under the memory lock and freeing after it, and a joined thread's stack goes back. `hardening` (51 checks) runs a child whose thread keeps reading a heap page on another CPU while the main thread returns it: the thread must fault, and it did not when the x86-64 shootdown was disabled. `libctest` (396 checks) checks that a joined thread's pages and a threaded process's heap tail go back. This completes Part 8, and so every part agreed for M11.

**Part 8, step 4 (complete): a thread-safe C library.** The library's own locks (`src/libc/lock.cpp`) sleep on futexes only while contended, and `pthread.cpp` moved onto them. `errno` is per thread in TLS; `malloc`, `realloc` and `free` share one lock; each stdio stream has a recursive lock that every call holds, with `flockfile`, `ftrylockfile` and `funlockfile`, and a lock over the list of streams; `exit` skips streams another thread holds rather than waiting on a blocked reader. `atexit` registration is locked, `__cxa_guard_*` makes other threads sleep while one runs a function-local static's initializer, and `__cxa_thread_atexit` keeps a per-thread list in TLS that a thread's end and `exit` run newest first. `libctest` (394 checks) checks per-thread `errno`, `malloc` and `realloc` hammered from eight threads, whole `fprintf` lines from four, a slow static built once, `thread_local` destructors in their thread in reverse order, `flockfile`, and `exit` running `thread_local` destructors before `atexit` handlers.

**Part 8, step 3 (complete): waiting.**
- **Kernel:** `FutexWait` and `FutexWake` (`src/kernel/futex.cpp`, host-tested against shared scheduler stubs): the word is checked and the waiter queued under the lock every wake takes, waiters are keyed by process and address and woken oldest first, and waits end on a signal or a time limit with a new `SyscallErrorTimedOut`. `ThreadDetach` marks a thread whose slot `ThreadCreate` reclaims once it has ended.
- **C library:** mutexes (normal, error-checking and recursive; an uncontended lock makes no syscall), condition variables with `CLOCK_REALTIME` or `CLOCK_MONOTONIC` deadlines, `pthread_once`, `pthread_detach` and the detach-state attribute, `ETIMEDOUT`, and a `pthread_self` that reads the thread's ID from TLS instead of making a syscall.
- **Tests:** `libctest` (387 checks): a mutex keeping a counter exact across eight threads, `trylock`, error-checking and recursive mutexes, a producer/consumer queue on condition variables, a broadcast, a timed wait that gives up holding the mutex, `pthread_once` under a race, and a detached thread that cannot be joined and gives its slot back.

**Part 8, step 2 (complete): thread-local storage.** The ELF loader accepts one `PT_TLS` segment (at most 16 KiB, its template inside a load segment), which the user linker scripts now emit from `.tdata` and `.tbss`. The kernel builds each thread's block at the top of its stack slot from the template (`src/kernel/thread_local_storage.cpp`, host-tested for both TLS variants): below the `FS` base, which points to itself, on x86-64, and after a 16-byte control block at `TPIDR_EL0` on AArch64. The scheduler saves and loads the thread pointer on each switch through `src/kernel/arch/thread_pointer.h`. `libctest` (377 checks) checks that the main thread and each new thread start from the template with their own aligned copies, that changes stay with their thread, that a reused stack slot starts clean, and `thread_local` in a function.

**Part 8, step 1 (complete): threads in a process.**
- **Kernel:** a process has up to 16 threads, each a scheduler task with its own execution context (kernel stack, signal mask, thread ID) over the shared program. `ThreadCreate`, `ThreadExit`, `ThreadJoin` and `ThreadId` syscalls; the architecture code tells `ThreadExit` from `Exit`. Stacks are kernel-managed slots below the main stack (`src/kernel/thread_stacks.cpp`, host-tested), demand-grown and reused but never unmapped. `Exit`, a fault or a terminating signal in any thread ends the process, which sends `SIGKILL` to the rest and waits for them before closing its files. Signal actions are shared, masks per thread, and process-directed signals go to the main thread or the first live one. A per-process memory lock covers page faults and the heap, which keeps its pages once there are threads. `/proc/<pid>/status` adds `Threads` and sums their CPU time. `Sleep(0)` now yields.
- **C library:** `pthread.h` (`pthread_create`, `pthread_join`, `pthread_exit`, `pthread_self`, `pthread_equal`, attributes), a start routine that reads the thread's function from its stack, `sched_yield`, and `EDEADLK`.
- **Programs:** `threads` counts primes with threads of one process, sharing `src/user/prime_count.cpp` with `primes`.
- **Tests:** host tests for the stack slots and the new syscalls; `libctest` (372 checks) creates and joins threads, checks their values and IDs, join errors, the 16-thread limit, `/proc`'s thread count, a stack that grows, and children for `exit` and a fault in a thread, `pthread_exit` in the main thread, and `SIGTERM` with every thread blocked; `hardening` (50) refuses a thread entry in the kernel and a kernel `ThreadJoin` value; the shell test runs `threads` beside `primes`.

**Part 7, step 5 (complete): `top` in place.** On the console, without `-n`, `top` takes the alternate screen, redraws its table over the last one, cut to the screen's height, and quits on `q`, Escape or Ctrl+C. It reads keys in raw mode with a timeout (`RawTerminal::ReadKeyWithin`), so the wait between refreshes answers a key at once, and the kernel restores the screen if `top` dies. With `-n`, or output that is not the console, it prints tables one after another as before. The shell test quits an interactive `top` with `q`. This completes Part 7: the console is a VT100-style terminal with raw input, the shell edits lines with history, and `vi` and `top` run full screen.

**Part 7, step 4 (complete): `vi`.** A small vi replaces the planned nano-style `edit`. The engine (`src/user/vi_editor.cpp`, host-tested) has normal, insert, `:` and `/` modes:
- **Moving:** `h j k l` and the arrows, `w b e`, `0 ^ $`, `gg G`, `+ -`, Enter, paging with Ctrl+F, Ctrl+B, Ctrl+D and Ctrl+U, and counts.
- **Editing:** `i a I A o O`, `x X D dd J r`, `yy Y p P` with a character or line register, and multi-step `u` and Ctrl+R over whole-buffer snapshots (at most 100 steps and 512 KiB).
- **Commands:** `:w [file]`, `:q`, `:q!`, `:wq`, `:x`, `ZZ`, `ZQ`, `:N` and `:$`, plus `/` and `?` searches with `n` and `N` that wrap around.

The screen redraws only the rows that changed, expands tabs to 8 columns, and scrolls long lines sideways together, as vim does without wrapping. `src/user/vi.cpp` adds files, the terminal size and raw input, and draws on the alternate screen (`CSI ?1049 h`, new in the kernel terminal), so leaving vi shows the shell as it was. Host tests cover each command, undo and redo, writing and quitting, search, and the screen as the kernel's terminal draws it. The shell test creates and edits a file in two sessions, which the smoke harness drives with a new `TypeWhen` step that types once a program shows some text.

**Part 7, step 3 (complete): shell line editing and history.**
- **Programs:** the shell edits its line in raw mode with `src/user/line_editor.h` (host-tested): the cursor keys, Home, End, Delete, the usual Ctrl keys (A, E, B, F, D, H, U, K, W, L, P, N), Ctrl+C to discard, and Up and Down through the last 32 lines, with a `history` built-in. Lines that wrap redraw with relative cursor moves at the console's width. `src/user/raw_terminal.h` switches to raw input and reads keys, telling a lone Escape from a sequence, for the shell and `keys`. A `clear` command, the shortcuts in the readme and the shell's `help`, and QEMU's window scaling by default (`QEMU_ZOOM=0` turns it off) came with it.
- **Tests:** host tests for editing, history and output, including one that renders the editor's output with the kernel's own terminal and checks the screen and cursor while a line wraps, erases back across the wrap, recalls a long line on the last row and clears the screen; the shell test edits lines over serial, and the input test edits one with the keyboard's Left and Up.

**Part 7, step 2 (complete): raw keyboard input.**
- **Kernel:** input is one byte stream in the form a VT100 terminal sends. A shared key map (`src/kernel/input/keyboard.cpp`, host-tested) turns key codes into bytes for both keyboards, adding the cursor keys, Home, End, Insert, Delete, Page Up and Down, Tab, Shift+Tab, Escape and every Ctrl+letter. The PS/2 driver now maps its `E0` keys, and the two drivers no longer keep copies of the layout. A `TerminalMode` syscall sets canonical or raw input, echo, Ctrl+C as a signal or a byte, `\r` to `\n`, and a raw read's minimum and timeout; the settings return to the defaults when the process that changed them exits. Line editing now skips escape sequences, and a serial Ctrl+C keeps its place among the bytes around it, even when two CPUs poll the port.
- **C library:** `termios.h` with `tcgetattr`, `tcsetattr` and `cfmakeraw`.
- **Programs:** `keys` prints each key pressed, using `src/user/key_decoder.h`, which the shell and `vi` reuse.
- **Tests:** host tests for the key map, the decoder (split, unknown and overlong sequences, a lone Escape) and the byte queue; `libctest` (359 checks) covers the settings, raw reads with a timeout and the reset when a child exits in raw mode; the shell test types every special key into `keys` over serial, and the input test sends them through `sendkey` on both keyboards.

**Part 7, step 1 (complete): terminal output.**
- **Kernel:** the framebuffer console (`src/kernel/framebuffer/term.cpp`) understands a VT100 subset: `CSI A/B/C/D/G/d/H/f` move the cursor, `CSI J` and `CSI K` erase, `CSI 7 m`, `27 m` and `0 m` switch reverse video, `CSI s/u` and `ESC 7/8` save and restore the cursor, `CSI ?25 l/h` hide and show it, and `\r` and `\b` move within the line. Unknown sequences are swallowed. The console passes ESC, `\r` and `\b` through, and a `TerminalSize` syscall reports the text area below the status bar.
- **C library:** `sys/ioctl.h` with `ioctl(TIOCGWINSZ)`.
- **Tests:** a host test for the escape parser, including sequences split across writes; `libctest` (351 checks) reads the console size and checks `ENOTTY` and `EINVAL`.

**Part 6 (complete): process tools.**
- **Kernel:** the scheduler counts each task's timer ticks and remembers the CPU it last ran on, and a process records when it was spawned. `/proc/<pid>/status` gains `CpuTimeMs`, `LastCpu` and `StartTimeMs`, and a new `/proc/stat` gives the uptime, the CPU count and each CPU's busy time.
- **Programs:** `ps` lists every process; `top` samples `/proc` and shows each process's share of a CPU and the system's, busiest first, until Ctrl+C or `-n` refreshes. Both parse `/proc` with `src/user/process_table.h`. The shell's `kill %n` names job `n`.
- **C library:** `sysconf` for the CPU count, page size and descriptor limit.
- **Tests:** host tests parse what the kernel's own `/proc` code writes; `libctest` (348 checks) checks `sysconf` and that CPU time grows while a program spins; the shell test runs `ps`, then `top` while a background `primes` worker spins, which must show at least 50%, then `kill %1`.

**Part 5, step 5 (complete): `primes` over pipes.** Each worker now writes its count and CPU mask as a line to a pipe of its own, which `primes` reads with `fdopen` and `fscanf` before reaping the worker, so `primes` uses only the C library. The shell test adds `primes 16 20000`, which holds sixteen pipes open at once, and its time limit rose to 60 seconds for the longer AArch64 session. This completes Part 5.

**Part 5, step 4 (complete): signals.**
- **Kernel:** each program has pending and blocked signals and an action per signal (`src/kernel/signals.cpp`, host-tested), delivered on every return to user mode. Handlers run on the user stack behind a frame that saves the registers and floating-point state on both architectures; `SignalReturn` puts them back after checking the addresses, flags and floating-point control bits. Ctrl+C now sends `SIGINT`, a dying parent `SIGKILL`, a broken pipe `SIGPIPE` and a child's exit `SIGCHLD`. `Kill`, `SignalAction`, `SignalMask`, `SignalReturn` and `Pause` syscalls; `Sleep`, `Wait` and console and pipe I/O give up with `EINTR` when a signal arrives. A signal's exit status is `ExitStatusSignalBase + n`, replacing `ExitStatusInterrupted`.
- **C library:** `signal.h` (`sigaction`, `signal`, `kill`, `raise`, `sigprocmask`, the set functions), a per-architecture restorer, `pause`, `strsignal`, `strings.h`, interrupted `sleep` and `nanosleep`, and `abort` through `SIGABRT`.
- **Programs:** a `kill` command (`kill -l`, names or numbers, groups as `-group`); the shell names the signal that ended a command or job.
- **Tests:** host tests for the signal core; `libctest` (344 checks) runs handlers, nested handlers, masks and `SA_RESETHAND`, checks that a handler keeps the interrupted code's floating-point values, and covers `SIGTERM`, `SIGPIPE`, and `SIGCHLD` interrupting `pause` and `nanosleep`, on both architectures. The smoke-test harness now ends a session only on output after its last typed line, since the end marker can appear earlier.
- **Not yet:** faults still end a program without running a `SIGSEGV` handler; no `SA_SIGINFO`, `sigsuspend` or `alarm`.

**Part 5, step 3 (complete): process groups and jobs.**
- **Kernel:** every process has a process group, which children join unless `Spawn`'s `SpawnSetGroup` names another or starts a new one. The console belongs to a foreground group instead of one process: any member may read it, Ctrl+C marks every member but session leaders and init, and reaping the group's last process hands it back to the waiter's group. `ProcessGroup` and `ForegroundGroup` syscalls give `getpgid` and `tcsetpgrp`; `/proc/<pid>/status` shows `Pgid`.
- **C library:** `getpgrp`, `getpgid`, `tcgetpgrp`, `tcsetpgrp`, `posix_spawnattr_setpgroup` for any joinable group, a FeatherOS `POSIX_SPAWN_BACKGROUND` flag (which `POSIX_SPAWN_SETPGROUP` used to stand for), and `waitpid(0)` and `waitpid(-group)`. init starts respawned services with both.
- **Shell:** each command line is one group led by its first command, so a pipeline is one Ctrl+C job and its first command can read the console. A trailing `&` runs it in the background; `jobs`, `fg [n]` and `wait` manage up to 8 jobs, and finished ones are reported before each prompt.
- **Tests:** a host test for `&`; `libctest` (327 checks) covers groups, `waitpid` by group and the console's owner, and the shell test runs `upper | upper`, three background jobs with `jobs`, `wait` and `fg`, and Ctrl+C on `sleep | sleep`.

**Part 5, step 2 (complete): pipes.**
- **Kernel:** a `Pipe` syscall makes a 4 KiB ring buffer with a read end and a write end (`src/kernel/pipe.cpp`), as a new kind of open file. Reads wait for data and return 0 once every writer is gone; writes wait for room, keep pieces of up to 512 bytes whole, and fail with a new `SyscallErrorBrokenPipe` without readers. Both give up on Ctrl+C, and neither holds the filesystem lock while waiting.
- **C library:** `pipe`, `pipe2`, `EPIPE`, `S_IFIFO`, and `PIPE_BUF`, `OPEN_MAX` and `PATH_MAX` in `limits.h`.
- **Shell:** `|` joins up to four commands (`SeparatePipes` and `SplitPipeline` in `src/user/command_line.h`), started last to first so the first one has the console. Until process groups (step 3), Ctrl+C stops only that first command, and the rest then see their input end.
- **Tests:** host tests for pipe order, end of input, broken pipes, wrap-around, atomic pieces, close-on-exec and pipeline parsing; `libctest` (316 checks) and the shell test use pipes on both architectures, including Ctrl+C on `primes | upper`.

**Part 5, step 1 (complete): descriptors and redirection.**
- **Kernel:** each process has 32 descriptors naming reference-counted open files, with the console as one shared open file behind 0–2. `Duplicate` and `DescriptorFlags` syscalls give `dup`, `dup2` and `fcntl`, `OpenCloseOnExec` gives `O_CLOEXEC`, and `Spawn` passes every other descriptor to the child. `O_APPEND` moved into the kernel, so writes through any copy land at the end.
- **C library:** `posix_spawn_file_actions_adddup2`/`addopen`/`addclose`, applied to the caller's descriptors around the spawn and put back afterwards.
- **Shell:** `<`, `>`, `>>`, `2>` and `2>&1`, parsed by `ExtractRedirects` in `src/user/command_line.h`; a quoted `">"` stays a word. `cat` reads its input without a file, and `upper` converts all of it when it is not the console.
- **Tests:** host tests for sharing, `dup2`, close-on-exec, inheritance, append and redirection parsing; `libctest` (306 checks) and the shell test redirect on both architectures. The shell test's time limit rose from 30 to 45 seconds for the longer AArch64 session.

**Part 4 (complete): virtual files.**
- **VFS:** `/dev`, `/proc` and `/tmp` are mounted beside `/system`. Each open file records its kind (archive, disk, device, memory or generated), which replaces the disk-only flag.
- **`/dev`:** `null`, `zero` and `random` (`Random::Next`, from `RDRAND`/`RNDR` when present). They have a new `Device` file type, which the C library reports as `S_IFCHR`; `isatty` now checks for the console itself.
- **`/tmp`:** a RAM filesystem on the kernel heap with nested, case-sensitive names, limited to 128 entries and 4 MiB, and following the disk's one-writer and no-removing-open-files rules.
- **`/proc`:** `meminfo` and `<pid>/status`, generated at open from the process table and memory counters through callbacks. `/proc/self` resolves to the caller.
- **Tests:** host tests for each filesystem (with stand-ins for the process table, the kernel heap and the random source) and for `/proc/self`. `libctest` (295 checks) and the shell test use all three on both architectures.

**Part 3 (complete): C library.** A libc written from scratch under `src/libc/`, built as a static library and linked into every program. The public headers are plain C (wrapped in `extern "C"` for C++), so C and C++ programs both use them. The implementation is freestanding C++ in `FeatherOS::LibC`, with the same style and lint setup as the kernel. No outside code: only the compiler's freestanding headers (`stdint.h`, `stddef.h`, `stdarg.h`, `stdbool.h`, `limits.h`). Each step lands with host unit tests and a QEMU case on both architectures.

1. **(Complete) Startup and basics:** `crt0` builds `argc`/`argv`, runs global constructors, and calls `main`, whose return value goes to `exit` (which runs `atexit` handlers). Also the C++ runtime hooks the implementation needs: `__cxa_atexit`, `__cxa_guard_*`, `__cxa_pure_virtual`, and `new`/`delete` once `malloc` exists. Also `string.h`, `ctype.h`, `errno`, `setjmp`/`longjmp`, and the integer parts of `stdlib.h` (`strtol`, `atoi`, `abs`, `qsort`, `bsearch`).
2. **(Complete) Kernel support:** the console as descriptors 0, 1 and 2; syscall errors mapped to `errno` values; `getpid`/`getppid`; a `Stat` syscall; and an environment block passed by `Spawn` (`environ`, `getenv`, `setenv`).
3. **(Complete) Memory:** `malloc`/`free`/`calloc`/`realloc` as a free-list allocator over `ExtendHeap`.
4. **(Complete) stdio:** the `printf` family built on one `vsnprintf`, and buffered `FILE*` streams (`stdin`/`stdout`/`stderr`, `fopen`, `fread`/`fwrite`, `fgets`/`fputs`, `fflush`, `fclose`). The console is line-buffered.
5. **(Complete) POSIX calls:**
   - the rest of `unistd.h` (`lseek`, `unlink`, `rmdir`, `chdir`, `getcwd`, `sleep`), `fcntl.h` (`open` flags), `mkdir` in `sys/stat.h`, and `dirent.h`
   - `time.h`: `time`, `gmtime`, `strftime`, `clock_gettime`, `nanosleep`
   - `spawn.h` (`posix_spawn`) with `sys/wait.h` (`waitpid`, `WNOHANG`, `WIFEXITED`)
   - `scanf`, `fscanf`, `sscanf` and their `v` forms: integers with the same length modifiers as `printf`, `%s`, `%c`, scansets (`%[...]`), `%n`, assignment suppression (`%*d`), and field widths, sharing one header-only engine with host tests against the host's `sscanf`
6. **(Complete) Floating point:** save FPU/SIMD state per task on both architectures and drop `-mgeneral-regs-only`. Then add the floating-point conversions to `printf` and `scanf`, `strtod`/`atof`, and a small `math.h`.
7. **(Complete) Adoption:**
   - Move every program in `src/user/` onto libc and retire `runtime.h`.
   - Add a `libctest` conformance program to the matrix.
   - Document the subset in `docs/posix.md`: each function, how it differs from POSIX, and what is left out on purpose.

**Step 7 summary:**
- **Programs:** every program in `src/user/` now starts at `main` and uses the C library, and `runtime.h` is gone. The one exception is the kernel self-test `test_program`, which reads the raw arguments block. `src/user/command_line.h` holds the shared `-h`/`--help` check, a strict number parser, and the shell's command-line splitter. A host test checks that a split line reaches the program as the same words after `posix_spawn` and the kernel.
- **Syscalls still used directly:** `filetest` and `hardening` call `syscall.h` where they test the kernel's exact errors. The `primes` workers do too, because they return 64-bit results through their exit status.
- **New in the C library**, so programs need no raw syscalls:
  - `posix_spawnattr_setflags`, where `POSIX_SPAWN_SETSID` makes a session leader and `POSIX_SPAWN_SETPGROUP` starts a background child; init uses both
  - `sbrk`, `sched_getcpu`, and `reboot`
  - `<featheros.h>`: `featheros_read_line`, a prompted line read that redraws the prompt after Ctrl+C, and `featheros_arguments_page`
- **Fixes:** AArch64 `setjmp`/`longjmp` now save `d8`–`d15`, which step 6 had left out. `libctest` (278 checks) catches its absence.
- **Docs:** [posix.md](posix.md) documents every header and function, how each differs from POSIX, and what is left out on purpose.
- **Image size:** programs are larger with the C library linked in, about 20 pages at `-O0`, so `memtest exhaust` reaches about 1000 heap pages instead of 1020.

**Step 6 summary:**
- **Kernel:** the x86-64 kernel is built with `-mgeneral-regs-only`, so the kernel never uses FP/SIMD registers on either architecture. The scheduler saves and loads them on each context switch: FXSAVE on x86-64, q0–q31 with FPCR/FPSR on AArch64. Each CPU enables them at boot, and x86-64 programs start with the ABI's stack alignment. User programs drop `-mgeneral-regs-only` and `+nofp+nosimd`. `fptest` fills every vector register in 8 workers and fails if a switch loses them; with saving disabled, it does fail.
- **Conversions:** `printf` gains `%f %e %g %a` (and `L`), exact via big integers and matching the host's `printf` digit for digit. `strtod`/`strtof`/`strtold`/`atof` are correctly rounded, and `scanf` reads floats.
- **`math.h`:** the exact functions match the host bit for bit. `exp`, `log`, `pow`, the trigonometric and hyperbolic functions, and `hypot` are within 1–3 ulps of the host's `libm`, and set `errno`.
- **Tests:** `libctest` (264 checks) checks all of this on both architectures, including x87 and 128-bit `long double` output.

**Step 5 summary:**
- **Files:** `open`/`creat` with Linux flag values, where `O_APPEND` puts every write at the end. Also `lseek` (the kernel's `Seek` now takes an origin and returns the new offset), `unlink`, `mkdir`, `rmdir`, `chdir`, `getcwd`, `access`, `isatty`, and `dirent.h`.
- **Time:** `time.h`, built on the shared `calendar.h`, with UTC conversions, `mktime` normalization, a C-locale `strftime`, `clock_gettime` and `nanosleep`, plus `sleep`/`usleep`.
- **Processes:** `posix_spawn`/`posix_spawnp` turn `argv` and `envp` into the kernel's command line and environment block. `waitpid`/`wait` use the Linux status encoding, and `-1` polls the spawned children.
- **Scanning:** `scanf`, `fscanf` and `sscanf` share one header-only engine.
- **Tests:** host tests compare `scanf` and `strftime` with the host's own and cover the spawn requests. `libctest` (240 checks) exercises all of it on both architectures, including `scanf(stdin)` and spawning `echo`, `env`, a failing child and a faulting one.

**Step 4 summary:** One header-only `printf` engine writes through a sink: a stream, a buffer, or a descriptor. It handles the integer, character, string and pointer conversions with every flag, width, precision and length modifier, and host tests compare it with the host's `snprintf`. `FILE*` streams have 512-byte buffers: stdout is line-buffered, stderr unbuffered, and a console read flushes stdout first. They cover `fopen` modes, `fread`/`fwrite`, `fgets`, `getline`, `ungetc`, `fseek`/`ftell`, append mode, and switching between reading and writing. `exit` flushes all streams last. `assert.h`, `perror` and `remove` are also added. `libctest` (187 checks) now prints through stdio, reads a line with `fgets(stdin)`, round-trips a disk file, and checks that a failed `assert` aborts. Not yet: floating-point conversions (step 6) and `scanf` (added in step 5).

**Step 3 summary:** `malloc`, `calloc`, `realloc` and `free` use a first-fit allocator with address-ordered free lists. Free neighbors merge on both sides, `realloc` grows in place when it can, and the heap grows in 64 KiB steps by only the shortfall. Invalid and double frees are caught. C++ `new`/`delete` and `strdup`/`strndup` build on it. `ExtendHeap` now checks the page limit before mapping, so a failed request no longer leaves pages mapped that later growth would trip over. Host tests cover the allocator over an in-memory arena; `libctest` (152 checks) covers allocation, `new`/`delete`, reuse, and growing after a failed request.

**Step 2 summary:** Descriptors 0–2 are the console in every process: writes print as one unit, and reads return a line at a time (foreground only), keeping what does not fit for the next read. Syscall errors now name their cause (bad descriptor or address, read-only, exists, not a directory, not empty, and others), with the error range widened to 4095 values as on Linux, and the C library maps each to `errno`. New syscalls are `Stat`, `StatDescriptor`, `ProcessId` and `ParentProcessId`. `Spawn` takes an environment block that the kernel validates and maps with the arguments. init sets variables from `env` lines in `init.conf`, and the shell passes its environment on. The C library adds `read`/`write`/`close`, `getpid`/`getppid`, `stat`/`fstat`, `environ`, `getenv`/`setenv`/`unsetenv`, and a new `env` program uses only the C library. Unit tests cover the error codes, `Stat`, environment validation, `env` lines, and the environment table; `libctest` grows to 104 checks, including a console read.

**Step 1 summary:** `src/libc/` builds as `libfeatheros_c.a` in CMake, which now also builds the user programs into `build/<arch>/programs`. A program with a `main` links the C library's `_start`, which copies `argv` to writable memory, runs constructors and calls `exit(main(...))`. Also included: `string.h`, `ctype.h`, `errno.h`, `setjmp.h` in assembly for each architecture, the integer parts of `stdlib.h`, and the C++ runtime hooks. Host unit tests cover the `strtol` parser and heapsort, and the shell test runs `libctest` (75 checks plus exit-handler order) on both architectures.

**Follow-up (complete): returning freed heap memory.** `ExtendHeap` takes a negative increment: it unmaps the whole pages above the new break, frees their frames, and lowers the page count, through a new `UnmapUserPage` on both architectures. `sbrk` passes negative increments through. `malloc` gives back all but a 64 KiB cushion, in 64 KiB steps, when more than 256 KiB is free at the end of the heap, and only while the break is still where its memory ends.
- **Tests:**
  - Host tests cover the release, its threshold and cushion, refusing when the end has moved, and a randomized run while shrinking.
  - `memtest exhaust` now grows to the page limit, shrinks back, and must grow as far again.
  - `hardening` checks that a page the heap gave back faults, and that the heap cannot shrink below its start (48 checks).
  - `libctest` (282 checks) checks that `free` returns memory and leaves a program's own `sbrk` memory alone.
- **Still open:** only the top of the heap can be returned. `mmap`/`munmap` (in [ideas.md](ideas.md) under Scalability), with large allocations in their own mappings, would return any freed block at once.

Out of scope for Part 3:
- `fork`/`exec`: `posix_spawn` covers process creation until copy-on-write exists.
- signals and pipes: covered by the later bullet.
- pthreads, sockets, users and permissions, `mmap`, `termios`, locales, and wide characters.

**Complete when:** init starts and supervises basic services, and users can run a documented set of utilities and programs through the shell on both targets, including programs installed on the disk.

### 12. Security And Hardening (Complete)

**Part 1 (complete):** The security invariants are documented in `docs/development.md`. The loader checks that segments lie in user space, AArch64 user pages are privileged-execute-never, and each process may have at most 16 live children. The shell test's `hardening` program runs 44 negative checks: bad syscall pointers and arguments, a kernel-half ELF, heap execution, code writes, kernel reads, an invalid syscall, and the child limit.

**Part 2 (complete):** AArch64 device registers (UART, RTC, GIC, virtio-mmio) are mapped in the kernel half through `Memory::MapDevice`, as is the x86-64 local APIC, and AArch64 user page tables no longer copy the kernel's low `TTBR0` mappings. The memory test maps a user page at the UART's identity address to prove the slot is free.

**Part 3 (complete):** Spawned processes get a randomized stack, arguments page, and heap start, using a cycle-counter-seeded generator. The shell test runs `aslr` twice and requires different addresses. AArch64 user space now ends at `2^47` to match its canonical-address check.

**Part 4 (complete):** Spawned programs load as static PIEs at a random image base (see M13). The kernel enables x86-64 SMEP/SMAP and AArch64 PAN when the CPU has them; the loader's copies into user memory open a short user-access window, and interrupt entry closes it again. `RDRAND`/`RNDR` are mixed into the random source when present. Matrix cases run the shell session on QEMU's `max` CPU and require these protections. Moved: the open-file limit to M9, with file syscalls.

- Audit syscall argument validation, ELF loading, address-space isolation, and resource cleanup.
- Enforce writable/executable memory policy and tighten privilege transitions.
- Add negative tests for invalid syscalls, malformed binaries, and attempts to access protected memory.
- Remove kernel low-address mappings (such as the UART identity map) from AArch64 user roots by moving them to the higher half.
- Add user stack guard pages, per-process resource limits (pages, tasks, open files), and address-space layout randomization. Randomizing the program image depends on PIE support in M13.

**Complete when:** security invariants are documented and automated tests demonstrate that malformed inputs and user programs cannot access or corrupt protected kernel state.

### 13. Memory Management And Process Scalability (Complete)

- Replace fixed limits with dynamic ones: a growable task table, heap-allocated kernel stacks, and a per-process page limit instead of the fixed ELF page cap.
- Give user programs a growable stack, a heap (`brk` or `mmap`-style syscall), and optional demand paging with copy-on-write.
- Extend the ELF loader beyond static executables: PIE/relocations (which also enables image ASLR for M12), `PT_TLS`, and a documented policy for dynamic linking.
- Add scheduler policy beyond round-robin, such as priorities; blocked/sleeping states, timed sleep, and an idle task already exist.

**Part 1 (complete):** Tasks and process records are heap-allocated. Kernel stacks come from a guard-paged pool, and a per-process page limit replaces the fixed ELF page cap. QEMU tests run 12 concurrent tasks for three rounds without leaking frames, check that a guard page faults, and have the shell's `fanout 6` run six children at once.

**Part 2 (complete):** User stacks grow on demand through a 64-page region whose lowest page is a guard, and the `ExtendHeap` syscall grows a program break above the ELF image. The shell test's `memtest` checks deep recursion and heap reads and writes, and `memtest overflow` is terminated at the guard page.

**Part 3 (complete):** Tasks have Low, Normal, or High priority, and the scheduler runs the highest-priority ready task, round-robin within a level. The `Sleep` syscall blocks a program without using a CPU. A scheduler test checks priority order, and the shell test runs `sleep 1100`.

**Part 4 (complete):** User programs are built as static PIEs. The ELF loader accepts `ET_DYN`, applies `R_X86_64_RELATIVE`/`R_AARCH64_RELATIVE` relocations before setting final permissions, and rejects symbol, PLT, and text relocations; unit tests cover each case. `memtest exhaust` checks that hitting the page limit fails cleanly and that pages are reclaimed. The dynamic-linking policy is documented in `docs/development.md`. Moved: `PT_TLS` and copy-on-write to M11.

**Complete when:** more than four processes can run with independently growing memory on both targets, sleeping tasks do not consume CPU, and QEMU tests cover allocation failure and resource reclamation under load.

### 14. USB Host And HID Input

- Add xHCI host-controller support and enumerate USB devices on both architectures.
- Implement USB HID keyboard reports and route key events through the shared input queue from M4.
- Add QEMU xHCI keyboard devices to both machine configurations for repeatable tests.

**Complete when:** both targets enumerate a QEMU USB keyboard and deliver key events through the shared input path.

### 15. Terminal Layout And Status Bar (Complete)

**Summary:** The framebuffer terminal has a status bar on its top row. It shows the foreground program on the left and, on the right, busy/online CPUs and the UTC time. Busy CPUs are counted from scheduler timer ticks. The prompt (`> `) and input are edited inline with a cursor. The `ReadUptime` syscall backs a new `uptime` command, and the once-a-second uptime log is gone. Unit tests cover the bar layout, inline erase across wraps, incremental-versus-full rendering, and the short date format. The SMP test checks busy-CPU accounting, and the shell test runs `uptime`. Follow-ups: `calc` evaluates full expressions with exact fractions, interactively at a `calc> ` prompt or from its arguments (the argument limit rose to 16). The `primes <workers>` demo splits work across processes and reports each worker's CPUs through the new `CurrentCpu` syscall. Spawned programs are now time-sliced on every timer tick on both architectures, so a CPU-bound program no longer keeps its core until it blocks. Ctrl+C interrupts the shell's foreground job and its descendants, and children are terminated and reaped when their parent exits.

- Add a one-row status bar at the top of the framebuffer terminal, above the scrolling text.
- Show the foreground program's name on the left of the bar, and on the right the busy CPU count over the online CPU count (for example `2/4`) followed by the current RTC date and time as `Mon Oct 19 9:36 PM`.
- Remove the dedicated input row at the bottom: the prompt and the line being typed appear inline in the scrolling text, like a normal terminal, with Backspace editing in place.
- Change the shell prompt to `> `.
- Remove the on-screen uptime and add an `uptime` command that prints it, backed by a new syscall that reports time since boot.
- Stop the once-a-second uptime log on the terminal and serial port; the smoke test checks the `uptime` command instead.

**Complete when:** on both targets the bar shows the right program, CPU count, and time as programs start and exit, input is edited inline, and QEMU tests cover the `uptime` command and the new prompt.

### 16. Physical Storage Drivers

- Add NVMe first, since its paired submission and completion queues are close to the virtio-blk driver, then AHCI for SATA disks (moved from M9).
- Develop and test each driver on QEMU's emulated controllers (`-device nvme`, `-device ahci`) with the same matrix tests as virtio-blk, before trying real hardware.
- Share one block-device interface so the filesystem does not depend on the controller.
- Later, SD/eMMC for ARM boards. USB mass storage is part of M19.

**Complete when:** both targets mount and write the FAT test disk through NVMe and AHCI in QEMU, passing the same storage tests as virtio-blk.

### 17. Native Toolchain

A compiler toolchain written from scratch that runs on FeatherOS. Porting GCC or Clang is out of scope, both because the project uses no outside code and because of their size. Each tool is written in C++, developed and unit-tested on the host first, and then built as a FeatherOS program. Needs: the M11 C library, running programs from disk, per-process memory limits well above 4 MiB, and a way to edit source files.

- **Stage 1: Assembler and linker.**
  - Assemble x86-64 and AArch64 text assembly into object files.
  - Link them into static-PIE ELF programs that the existing loader accepts.
- **Stage 2: C compiler.**
  - Cover a C11 subset, with one code generator per architecture.
  - It should build the libc and simple tools on FeatherOS.
- **Stage 3: C++ subset.** Namespaces, classes, references, overloading, `constexpr`, and simple templates: the subset the FeatherOS code style uses. It should build the user programs in `src/user/`. Full C++ (general templates, exceptions, RTTI, newer standards) is not a goal.
- **Stretch:** compile the toolchain itself (self-hosting), then the kernel.

**Complete when:** on both targets, FeatherOS compiles a C++ user program from source on its own disk and runs the result, and the matrix tests this.

**Open decision: port TCC first.** Not decided yet. An alternative is to start with a port of TCC (Tiny C Compiler), an exception to the no-outside-code rule, and keep the stages above as a longer-term goal that TCC can be compared against.

- **Why TCC:** it is small (about 1 MB), has x86-64 and AArch64 backends, writes ELF, and needs little beyond stdio, `malloc` and files. GCC and Clang need `mmap`, hundreds of MB per process and much of POSIX, and there is no small C++ compiler.
- **Work:** build TCC against the FeatherOS libc and close the gaps it finds; make its output something the loader accepts (static-PIE, or teach the loader `ET_EXEC`); build its `libtcc1` runtime; and install the libc headers and objects on the disk.
- **Gives:** a C compiler on FeatherOS on both targets, but no C++. The from-scratch C++ subset would still be needed for Stage 3.
- **Complete when (for this part):** on both targets, TCC on FeatherOS compiles a C program from the disk and runs it, and the matrix tests this.

### 18. Scripting Languages

Interpreters written from scratch, like the M17 toolchain.

- **Part 1: Shell scripts.** Extend the shell into an interpreter for a documented subset of POSIX `sh`, not full Bash:
  - variables, quoting, `$?`
  - `if`/`for`/`while`/`case`, functions
  - `$(...)` command substitution, `#!` script files
  - Builds on the M11 pipes, signals and background jobs.
- **Part 2: Python subset.**
  - A bytecode interpreter for a documented core: integers, floats, strings, lists, dicts, functions, classes, exceptions, and a small built-in library (`print`, file I/O, `os`-style calls over the M11 C library).
  - Memory is managed by reference counting with a cycle collector, or by a tracing garbage collector.
  - Needs floating point from M11 Part 3.

**Complete when:** on both targets, the matrix runs a shell script and a Python script from the disk and checks their output.

### 19. Removable Drives And Foreign Filesystems

Scheduled right after M14, whose xHCI stack it uses. The goal is to plug in a USB drive formatted as FAT, exFAT, ext4, NTFS or APFS, and read and write it. The mount table, the filesystem driver interface and ext2/ext4 come from M22, so this milestone adds USB, hot-plug and the remaining filesystems.

**Groundwork:**
- **USB mass storage** on the M14 xHCI stack: the Bulk-Only Transport protocol carrying SCSI commands, with UAS later.
- **Hot-plug:** detect the partitions on a new drive (the GPT/MBR code exists), mount each one at `/media/<label>`, and provide `mount`, `umount` and `eject`. Unmounting flushes everything first, so pulling the drive out afterwards is safe.

**Safety rules:** a filesystem bug here corrupts a real drive, so:
- Mount new filesystem drivers read-only until their write path passes the matrix.
- Mount read-only whenever the volume reports it was not cleanly unmounted:
  - an ext4 journal that needs recovery, until we replay the journal ourselves
  - NTFS left hibernated by Windows "Fast Startup", or flagged dirty
  - an APFS container whose latest checkpoint is not valid
- After the OS writes to a test image, check it on the host with that filesystem's own repair tool. This extends the `STORAGE_TEST` pattern, which already cross-checks FAT with mtools.

| Filesystem | Specification | Write difficulty | Host tools for test images and checks |
|---|---|---|---|
| FAT32 | Microsoft FAT specification | Low: extends the FAT16 driver | `mkfs.fat`, `fsck.fat`, mtools |
| exFAT | Microsoft's published exFAT specification | Low to medium: allocation bitmap, checksummed directory entry sets, UTF-16 names. The usual format of large USB drives | `mkfs.exfat`, `fsck.exfat` |
| NTFS | Reverse-engineered documentation (Linux ntfs3 and NTFS-3G docs) | High: MFT records and attributes, data runs, B+ tree directory indexes, `$Bitmap`, update sequence arrays. Mark the volume dirty while mounted. Compressed and encrypted files stay read-only | `mkntfs`, `ntfsfix -n` (NTFS-3G) |
| APFS | Apple's "Apple File System Reference" | Highest: copy-on-write B-trees, object map, space manager, checkpoints and transaction IDs, Fletcher-64 checksums. FileVault-encrypted volumes also need AES-XTS and the keybag | `hdiutil create -fs APFS`, `fsck_apfs -n` on macOS |

ext4 drives work through the M22 driver once USB mass storage is in place.

**Order:** FAT32 → exFAT → NTFS → APFS. Each filesystem is read-only first, then gets writes.

**Complete when:** on both targets, a QEMU USB drive with each filesystem is detected, mounted under `/media`, written and cleanly ejected, and the host repair tool finds no errors.

### 20. C++23 And Sanitizers (Complete)

**Summary:** The kernel, libc, programs and host tests all build as `gnu++23`. The host tests run under AddressSanitizer and UndefinedBehaviorSanitizer, which found one font bug. Every syscall handler table uses designated initializers, and the global locks and wait queues are `constinit`. Along the way, the AArch64 shell smoke test's intermittent failure turned out not to be a guest hang. The scripted session had grown to about 90 s on AArch64, which was its whole time limit. The limit is now 180 s, and a failure that stopped at the limit says so.

Scheduled right after M11, before the next feature milestone, so later work starts on a stricter base. Each part lands on its own, with the full matrix on both targets.

**Part 1 (complete):** The kernel, libc, programs and host tests all build as `gnu++23` (CMake 3.20 or newer). Besides the six warnings from the trial build, four more `++` on a `volatile` sat in `kernel_test.cpp` code that only builds with a test toggle on. Each became `x = x + 1`. The standard brought no new clang-tidy findings.

**Part 2 (complete):** `./build.sh unit-test` builds the Catch2 tests, and Catch2 itself, with `-fsanitize=address,undefined -fno-sanitize-recover=all`, and `SANITIZE=OFF` turns that off. On macOS it uses Homebrew LLVM, because Apple clang 17's AddressSanitizer runtime deadlocks before `main` on macOS 26. The sanitizers found one bug: `indexForCharacter` scanned 96 bytes of a 95-byte map. It returned a glyph past the end of the font for a NUL cell, and drew the apostrophe with the backtick's glyph. It now computes the index from the character.

**Part 3 (complete):** `ProcessSyscallHandlers`, the serial tables in both `arch/*/exceptions.cpp` and the unit tests' tables name each slot with a designated initializer. Every `Syscall::Handlers` member defaults to `nullptr`, so a table lists only the syscalls it serves, and a new syscall no longer touches the architecture files. The kernel's 22 spinlocks, its file-system mutex and its 5 wait queues defined at namespace scope are `constinit`, and so are libc's three locks (`LibC::Lock` gained a `= 0` default so clang accepts it).

- **Part 1: one language standard.** The kernel builds as `gnu++11`, libc and the programs as `gnu++17`, and the host tests with the compiler's default. Move all of them to `gnu++23`, which GCC 14 and Apple clang 17 both support. A trial build as C++23 found no errors and six warnings, all `++` on a `volatile` (deprecated since C++20), in `arch/aarch64/keyboard.cpp`, `scheduler.cpp`, `time/timer.cpp`, `libctest.cpp` and `test_program.cpp`. C++23 rather than C++20, because C++23 allows `|=` and `&=` on `volatile` again, which the device-register code relies on. Also clear whatever new clang-tidy findings the standard brings.
- **Part 2: sanitizers for the host tests.** Build and run the Catch2 tests with AddressSanitizer and UndefinedBehaviorSanitizer (`-fsanitize=address,undefined`), as a `build.sh` option or the default for `unit-test`, and fix what they find. Leak checking is not available on macOS arm64, so it covers out-of-bounds access, use after free and undefined behaviour.
- **Part 3: safer tables and globals.** Fill the syscall handler tables (`process.cpp`, both `arch/*/exceptions.cpp`, and the tests' tables) with C++20 designated initializers, so each entry names its slot and a new syscall no longer edits the architecture files. Mark the global spinlocks, mutexes and wait queues `constinit`, so the compiler checks that they need no constructor, which early boot code relies on.

**Complete when:** every target builds as `gnu++23` without warnings, the host tests pass under both sanitizers, and the syscall tables use designated initializers.

### 21. Embedded Swift For User Programs

User programs written in Embedded Swift, built on the host with the swift.org toolchain (Swift 6 or newer; Xcode's toolchain does not reliably target ELF) and linked against the FeatherOS libc like the C++ programs. The kernel stays C++.

- **Part 1: toolchain and build.** Find the toolchain in `build.sh` and CMake (Swift with Ninja), compile with `-enable-experimental-feature Embedded -target x86_64-unknown-none-elf` or `aarch64-none-none-elf`, and link the objects into the same static-PIE programs. A missing toolchain skips the Swift programs and says so, without failing the build.
- **Part 2: C interop.** A module map over the libc headers (`stdio.h`, `stdlib.h`, `string.h`, `unistd.h`, `fcntl.h`, `pthread.h`, ...), and the symbols the Embedded Swift runtime expects: `posix_memalign`/`free` for ARC allocations, `memcpy`/`memset`/`memmove`, `arc4random_buf` for hashing, and `putchar` for `print`. Any that are missing go in `src/libc/` and `docs/posix.md`.
- **Part 3: first programs.** A `hello` and a Swift version of a small existing tool (for example `wc` or `upper`), using structs, classes with ARC, generics and protocols. A program is registered like a C++ one, with a `.swift` source.
- **Part 4: shared Swift code.** A small Swift support library over the libc (arguments, file reading, string output), so programs do not repeat C interop.
- Document what Embedded Swift leaves out (reflection, `Codable`, Foundation, some existential types) in `docs/development.md`.

**Complete when:** on both targets, Swift programs build from `src/user/`, run from the shell, and the matrix checks their output.

### 22. Mount Table And ext2/ext4

The filesystem half of M19, split out because it needs no USB: it is built and tested on the existing virtio-blk disk. It moves beyond the FAT16 limits (ASCII names of at most 63 characters, no permissions or links).

**Feather Disk core:** the ext2/ext4 code lives in Feather Disk, a separate repo by the same author (dual MIT OR Apache-2.0) that also ships a Mac app mounting these filesystems through FSKit. FeatherOS pulls in only its `core/` (freestanding C++, no exceptions, RTTI or standard library) as a git subtree under `src/fs/`, and keeps its own VFS adapter and block-device glue. The core's driver interface covers probe, mount/unmount/sync, lookup, directory reads, attributes, read/write/truncate, create/remove/rename/mkdir/link/symlink and capabilities, with opaque 64-bit node IDs, plain error codes, and every allocation, lock, log and clock call going through its `host.h` hooks. Each part below lands in the core first, with unit tests over image files there, so it reaches the Mac app as soon as it lands.

Feather Disk repo layout (proposed); only `core/` comes into FeatherOS:

```
core/          freestanding C++: no exceptions, RTTI or std library
  include/fs/  block_device.h, host.h (alloc/lock/log/time hooks), filesystem.h
  common/      block cache, bitmaps, CRC32C, UTF-8/16, little-endian helpers
  ext/ ntfs/ … one directory per filesystem
  probe.cpp    filesystem detection from superblocks
partition/     MBR + GPT
tests/         unit tests over image files; image scripts (mke2fs, e2fsck, …)
tools/fscli/   mtools-style CLI (ls, cp in/out); built before the FSKit module
mac/App/, mac/FSExtension/   SwiftUI app + thin Swift FSKit layer over core
```

- **Part 1: mount table.** A filesystem driver interface and a mount table in the VFS, with an adapter from the Feather Disk core's interface and its `host.h` hooks to the kernel's heap, locks, `KLogger` and clock, replacing the hard-coded `/` (FAT16) and `/system` (initramfs). FAT16, the initramfs, `/dev`, `/tmp` and `/proc` become drivers behind it. Add `mount` and `umount`, and `/proc/mounts`. Unmounting flushes everything first.
- **Part 2: ext2 read.** Superblock, block groups, inodes, direct and indirect blocks, directories, symlinks. The test disk gets a second partition made with `mke2fs -d`.
- **Part 3: ext2 write.** Block and inode bitmaps, create, write, truncate, delete, rename, `mkdir`/`rmdir`, symlinks and hard links. Permissions and owners are stored and shown by `ls`, though not yet enforced (FeatherOS has no users). After the OS writes, the host checks the image with `e2fsck -fn`, extending the `STORAGE_TEST` pattern.
- **Part 3b: ext2 as root.** The test disk becomes a single ext2 partition mounted at `/`, built with `mke2fs -d` from Homebrew `e2fsprogs` (keg-only, so `build.sh` finds it), and `STORAGE_TEST` checks persistence with `e2fsck` instead of mtools. FAT16 moves to a second partition so its tests keep running; its driver stays for USB drives (M19) and a future EFI system partition.
- **Part 4: ext4 read.** Extents, htree directories, CRC32C metadata checksums, 64-bit block numbers, flexible block groups. A volume whose journal needs recovery mounts read-only.
- **Part 5: ext4 write.** Extent allocation and splitting, checksum updates, and jbd2 journal transactions, so a crash leaves a volume the host can recover.
- **Part 5b: ext4 as root.** Switch the root partition to ext4.
- **Safety:** each driver mounts read-only until its write path passes the matrix, as in M19.

**Complete when:** on both targets, the root filesystem is ext4 from the Feather Disk core, ext2 and ext4 partitions on the virtio-blk disk are mounted, written and unmounted, the data survives a reboot, and `e2fsck -fn` finds no errors.

### 23. Pseudo-Terminals And Virtual Consoles

The kernel has exactly one terminal today, and all of its state is global: the mode (`Console::Settings`), the size (`Console::Size`), the foreground process group (`foreground_group` in `process.cpp`), and Ctrl+C (`InterruptForegroundJob` in `console.cpp`). This milestone turns the terminal into an object, adds pseudo-terminals (ptys), and runs several consoles with a shell each. Later, every terminal window in a GUI holds a pty, and a remote shell (`docs/ideas.md`) is a relay between a socket and a pty.

**Terms:** a terminal has two sides. Programs (the shell, `vi`) read and write the *slave* side and see an ordinary terminal. The *master* side belongs to whatever plays the screen and keyboard: it reads what programs print and writes what the user types. Between the two, the kernel's *line discipline* applies the terminal's mode: echo, line editing in canonical mode, and Ctrl+C becoming `SIGINT` for the foreground group. For the console, the kernel itself is the master, drawing with `term.cpp` and reading the keyboard. For a pty, a user program is.

**End state, as on macOS:** terminal emulation leaves the kernel. macOS has no virtual consoles. Its kernel provides ptys and the line discipline, and Terminal.app, an ordinary program, parses escape sequences, draws the text and turns key presses into bytes. FeatherOS goes the same way with the GUI, where each terminal window is a program holding a pty master. It is safer: the VT100 parser reads bytes that any program can print (`cat` of a crafted file is enough), so a bug in it crashes one window instead of the kernel, and less code runs with full privileges. The kernel keeps a minimal text console for boot messages and panics, which cannot depend on user programs. M23 stops short of that move. Its virtual consoles still draw with `term.cpp` in the kernel, because drawing from user space needs a framebuffer and raw-input interface for programs, which only the GUI needs.

- **Part 1: the terminal object** (no change in behavior). The mode, size, foreground group, input queue and line discipline move out of `console.cpp` and `process.cpp` into a `Terminal` in `src/kernel/terminal/`, with an output sink and an input feed. The console becomes the first terminal, fed by the keyboard and serial and drawing through `term.cpp`. Each process gets a session and a controlling terminal, both inherited on spawn: `setsid` starts a session without a terminal, and the first terminal its leader opens becomes the controlling one. `TerminalMode`, `TerminalSize` and `ForegroundGroup` take a descriptor and act on the terminal behind it, so `tcsetattr`, `ioctl(TIOCGWINSZ)`, `tcsetpgrp` and `isatty` work on any terminal and fail on anything else. `/dev/tty` opens the caller's controlling terminal. Host tests cover the line discipline with scripted input and the session and foreground rules, and the matrix passes unchanged.
- **Part 2: pseudo-terminals.** Opening `/dev/ptmx` creates a pair and returns its master, and the slave is `/dev/pts/N` (16 pairs at most). libc adds `posix_openpt`, `grantpt`, `unlockpt` and `ptsname`. Bytes written to the master are the slave's input, through the same line discipline as the keyboard, and what programs write to the slave is read from the master. `ioctl(TIOCSWINSZ)` on the master sets the size and sends a new `SIGWINCH` to the foreground group; the line editor, `vi` and `top` redraw on it. Closing the master sends `SIGHUP` to the session and ends reads on the slave, and once every slave descriptor is closed, master reads return end of file. Both sides work with `poll`. Host tests cover the data flow both ways, size changes and hangup, and `libctest` checks a pair from both ends.
- **Part 3: `script`.** `script [-c command] [file]` runs the shell, or a command, on a new pty. It puts its own terminal in raw mode, relays it to the master and saves the session to a file. Its relay loop is the same as a GUI terminal window's or a remote shell's, minus the drawing. The shell test runs `script` on both targets and drives line editing, `vi`, Ctrl+C, `&`/`fg` and a size change inside it, then checks that the console's own jobs stayed separate and that the file holds the session.
- **Part 4: virtual consoles.** Four kernel terminals, each with its own `term.cpp` screen (text, cursor, alternate screen) and its own input. Alt+F1 to Alt+F4 switch which one is shown and gets the keys; a hidden console updates its buffer without drawing. Serial stays with the first console. init's `console` services name a terminal (`/dev/tty1` to `/dev/tty4`) and start a shell on each, each in its own session. The status bar shows the shown console's number and foreground program. The QEMU test switches consoles with the monitor's `sendkey`, runs a job on each, and checks through a kernel test hook which console is shown and that each one's foreground program is its own.

**After M23, with the GUI:** `term.cpp`'s VT100 code moves into a user-space terminal library, each terminal window holds a pty master, and the kernel's virtual consoles shrink to the boot and panic console. Ctrl+Z and the stop signals (`SIGTSTP`, `SIGTTIN`, `SIGTTOU`), left out in M11, fit naturally once terminals are objects, and can join any of these parts.

**Complete when:** on both targets, four consoles each run their own shell and jobs, a program on a pty behaves as on the console (line editing, `vi`, Ctrl+C, the window size), and the matrix covers both.

## Architecture Boundaries

- Keep framebuffer terminal, logging API, panic/assert API, VFS, syscall definitions, and scheduler policy architecture-neutral when possible.
- Keep port I/O, UART drivers, exception entry, interrupt controllers, page-table manipulation, context switching, and syscall entry under the matching `arch` backend.
- Do not share x86 GDT/TSS/IDT structures with AArch64; AArch64 uses exception levels, vector tables, and the GIC instead.
- Avoid fixed physical addresses. Obtain usable memory from Limine's memory map and use explicit 64-bit address types.
- Keep early logging independent of dynamic allocation so it remains usable during allocator failures.
