# FeatherOS desktop GUI: implementation plan

**Status:** plan, written 2026-11-25. Nothing is implemented. The milestones are on the roadmap as M35 to M46.

**Inputs:** the prototype, `HANDOFF.md` and the pointer artwork in `docs/design/gui/` (the target look and behaviour), `docs/design/GUI.md` (the older spec), and the code on `main` at `8b9ba14`. Every statement in section 1 was checked against the code, not the docs.

**How to use it:** this is the master plan. When a milestone starts it gets its roadmap entry, a design document `docs/design/M<n>.md` (decisions, files, invariants, tests) and an autopilot plan `docs/plans/m<n>.md`, as M16 did. Sizes: **S** is about one working session, **M** two to four, **L** five or more (split it when the milestone is planned in detail).

## 0. Before the first milestone

1. **Done on 2026-11-25:** the approved design is in the repo. `docs/design/gui/prototype.html` is the current prototype (the handoff calls it `featheros-prototype.html`), `docs/design/gui/HANDOFF.md` is its brief, and `docs/design/gui/cursors/` holds the 13 pointers as SVG sources, PNG renders at 24, 32 and 48 px, and `cursors.json` with the hotspots.
2. **Done on 2026-11-25:** the prototype's three known issues are fixed in the repo copy (`HANDOFF.md`, "Known issues"). The overview overflow came from thumbnail sizes rounded up by half a pixel; M40 A7 must size its grid in whole pixels too.
3. **Done on 2026-11-25:** `docs/design/GUI.md` matches the prototype and carries the corrections from section 1.5.

## 1. Current state

### 1.1 Kernel: what exists

| Piece | Where | State |
|---|---|---|
| Framebuffer device | `/dev/fb0`, `framebuffer.h`, `graphics_mode.*`, `console.*` | One owner per virtual console. `FB_ACQUIRE` blanks the console, `FB_PRESENT` copies `0x00RRGGBB` rectangles, `FB_RELEASE` repaints the console. Never inherited by a child. |
| Present path | `PresentFramebuffer` in `process.cpp`, `Console::PresentRow` | Per row: a page-walk copy from user memory into a bounce row, a conversion into the kernel's back buffer (a plain copy when the screen is XRGB8888), then a copy of the **whole row** to the screen, however narrow the rectangle. |
| Screen size | `config.h`, `framebuffer.h`, `term.h` | **Fixed at 1024x768 at compile time.** The back buffer is a static array, the console grid is a constant, `Framebuffer::init` refuses a larger mode, and both Limine configs ask for 1024x768. |
| Key events | `fb_event` on the fb0 descriptor | Press, repeat, release with Shift, Control, Alt and Caps Lock bits; `Shown` and `Hidden` on console switches; `poll` works. Queue of 64 (62 keys). `Alt+F1` to `Alt+F4` switch consoles in the kernel. `Ctrl+C` also sends `SIGINT` to the console's foreground group. |
| Pointer | `/dev/mouse`, `input/pointer.*`, `usb/hid/` | USB HID mice only. Relative motion, wheel and buttons as Linux `input_event` records, a 256-record ring, any number of readers, `poll` works. |
| Time | `time/timer.*` | A 100 Hz tick. Sleeps, `poll` timeouts and `CLOCK_MONOTONIC` all move in 10 ms steps. |
| Memory | `process.h`, `memory/` | A process has an image, a heap that grows by `ExtendHeap`, and thread stacks. There is no list of mapped regions. 16384 pages (64 MiB) per process. Frames come from a bitmap with no reference counts. `MapPage`, `UnmapUserPage`, `ProtectPage`, `TranslateUserPage` and `ShootDownUserMappings` exist on both architectures. |
| IPC | `pipe.*`, `network/socket.*` | Pipes (4 KiB), and sockets for IPv4 and IPv6 only (TCP, UDP, ICMP, with loopback). `Spawn` passes descriptors to a child. |
| Processes | `process.*`, `procfs.*` | 16 threads per process, futexes, sessions, 16 pseudo-terminal pairs. `/proc/<pid>/status` already has pages, CPU milliseconds, threads and state; `/proc/stat` has per-CPU busy time; `/proc/meminfo` exists. |
| Services | `init.cpp`, `init.conf` | `once`, `respawn` and `console` services, with `/etc/init.d/` and `/etc/init.disabled`. A shell runs on each of `tty1` to `tty4`. |
| Tests | `tools/smoke/`, `tools/matrix.py` | QMP `screendump`, key events and pointer events are already used (`GRAPHICS`, `FONTS`, `USB`). QEMU runs with TCG, 2 GiB and 4 CPUs. |

### 1.2 Kernel: gaps

- **No `mmap`, no shared memory, no memory-object descriptor.** `ExtendHeap` is the only way to get memory.
- **No Unix-domain sockets and no descriptor passing.** `SocketMessage` has no control data.
- **No Super modifier.** `fb_event` has no bit for it and the header has no `KEY_LEFTMETA`. USB keyboards report codes 125 and 126; the PS/2 path (`arch/x86_64/keyboard.cpp`) drops `E0 5B` and `E0 5C`, and that is the keyboard QEMU's x86-64 tests use.
- **No raw mode for the screen owner:** `Ctrl+C` would signal the compositor's own session.
- **Acquiring the screen blanks it,** so a boot picture cannot stay up while the desktop starts.
- **No absolute pointer** (`EV_ABS`), so QEMU's `usb-tablet` does nothing. No PS/2 mouse, no I2C touchpad.
- **No clock finer than 10 ms,** so frame times cannot be measured and frames cannot be paced at 60 Hz.
- **No users.** There is no `getuid`; file owners are stored but not enforced.
- **No disk or network rate counters** in `/proc` (Task Manager's Performance tab needs them).
- **`init` has no run-time control:** a service cannot be started, stopped or restarted on request.
- **No setting for key repeat** (the USB `Repeater` uses fixed values).
- **No ACPI battery, sleep, lid or power button; no sound; no Wi-Fi; no mode setting.**
- **No SHA-256** anywhere in the tree (the login password needs a hash).

### 1.3 User space: what exists

- **`src/user/graphics/`** (`FeatherOS::Graphics`, also the Swift module `FeatherGraphics`):
  - `Surface`: opaque XRGB pixels; fill, rectangle outline, blit, colour-keyed blit, text.
  - `Font`: FFNT files with 4-bit coverage, blended over the pixel already there.
  - `DirtyRegion`: at most 8 rectangles, merged into bounding boxes.
  - `Display`: one program owns the whole screen and reads keys.
  - `TileSheet`: tiles drawn as text.
- **Fonts** in `/system/fonts/`: `sans-16`, `-20`, `-24`, `-32`, `-48`, `-64` and `mono-16`, `-20`, `-24`, `-32`. Regular weight only. Made by `tools/fonts/`, committed, with `SHA256SUMS` and a CTest check.
- **Reusable program code:** `calculator.*`, `calendar.*`, `process_table.*` (used by `ps` and `top`), `line_editor.*`, `key_decoder.*`, `vi_editor.*`, `text/utf8.*`, and the kernel's VT100 code in `framebuffer/term.cpp`, which already has host tests.
- **Full-screen programs:** `featherfall`, `wickdeep`, `fontview`, `fbdemo`.

### 1.4 User space: gaps against the prototype

Everything the prototype shows is missing. Grouped by layer:

- **Pixels:** alpha surfaces and blending, a clip stack, rounded rectangles, anti-aliased shapes (wallpaper, graphs), 9-slice shadows, tinted icon masks, a real region type with subtraction.
- **Assets:** design tokens in code, the icon set (about 50 glyphs plus the feather), app tiles with the fold, shadows, cursors, the wallpaper.
- **Window system:** a compositor, a window protocol, a client library, decorations, move and resize, snapping, workspaces, switcher, overview, the animation rules.
- **Toolkit:** widgets, layout, focus and keyboard paths, text input, menus and popups, dialogs, themes at run time.
- **Shell:** menu bar with status items and global app menus, dock, Feather menu, Quill, notifications, desktop icons, trash, customize mode.
- **Session:** boot animation, login and lock, log out, restart and shut-down screens.
- **Apps:** Terminal, Files, Settings, Task Manager, Notes, Calculator, UI Elements, and the two games in windows.
- **Design work not in the prototype:** the error states and the first-run flow listed in `HANDOFF.md`'s backlog. Pointers and scrollbars are designed (`HANDOFF.md`, "Pointers (cursors) and scrollbars").

### 1.5 Corrections to `GUI.md` (applied on 2026-11-25)

What the earlier spec had wrong, kept here as a record:

- Section 13 says per-process accounting "must be checked": it exists (`/proc/<pid>/status`, `/proc/stat`).
- Section 14 asks for a queue larger than 64 because pointer motion would overflow it: pointer events never enter that queue. They have their own 256-record ring behind `/dev/mouse`.
- Section 13 calls `FB_PRESENT` "an extra full copy": it is three passes in the kernel, and the last one copies whole rows to the screen whatever the rectangle's width.
- Section 10 calls 1024x768 "the current guest resolution": it is a compile-time constant.
- Section 12's 60 Hz budget cannot be paced or measured with a 100 Hz tick.
- Section 2's contrast measurements covered three themes. All eight themes and every accent colour are now measured and pass 4.5:1; the lowest is 4.82:1.

## 2. Decisions

The first four were decided by the owner on 2026-11-25. The rest are recommendations to confirm in each milestone's design document.

### D1. Process model: kernel first (decided)

- **Options:** (a) shared memory and local sockets first, every app a separate process from the start; (b) apps inside the compositor process first, moved out later; (c) one process for good.
- **Trade-offs:** (a) puts the riskiest kernel work first and shows no windows until it is done, but nothing is built twice and a crashing app never takes the desktop down. (b) shows windows sooner. (c) cannot run the existing games in windows.
- **Decision:** (a). To keep something visible meanwhile, the pixel library, the assets and the boot animation run in parallel with the kernel work (section 3).

### D2. Language: C++ throughout (decided)

Compositor, toolkit, shell and system apps are C++, host-tested with Catch2. Swift programs reach the toolkit through C++ interop, as they reach `FeatherGraphics` today.

### D3. Screen target: any resolution early (decided)

The kernel takes the screen size from Limine at run time in the first milestone. From the first compositor step, tests run at 1024x768 and 1920x1080 in QEMU on both architectures. Real hardware is not a gate; it is a step in M45. 4K is out of scope until the per-process memory limit is raised (D8).

### D4. Login: one real password (decided)

- One user. A salted PBKDF2-HMAC-SHA256 hash in a file under `/etc`, checked by the login and lock screens. Automatic login is a setting. No password set means automatic login.
- **Consequence:** the lock screen is only real if the text consoles ask for the password too, because `Alt+F2` reaches a shell. So a small `login` runs on the virtual consoles once a password is set (M43).
- **Honest limits, to be shown in Settings:** without file permissions, any program run after login can read or replace the hash. The password guards the keyboard, not the files. The user picker and the guest account from the prototype are hidden until users exist.

### D5. Shared memory on both architectures

- **Options:**
  - (a) POSIX-style: anonymous `mmap`/`munmap`, plus a memory-object descriptor (`memfd_create`, `ftruncate`, `mmap(MAP_SHARED)`).
  - (b) a special "surface" object made only for the compositor.
  - (c) named shared memory in a filesystem (`shm_open`).
- **Trade-offs:** (b) is the least code but helps nothing else. (c) needs names, cleanup after crashes and file-backed mapping rules. (a) is a little more work, removes the "only the top of the heap can be returned" limit, and ties a surface's life to descriptors, so the kernel cleans up after a crashed client.
- **Recommendation: (a), written architecture-neutral.**
  - **Regions:** each process gets a list of its mapped regions (anonymous and object) under its `memory_lock`, in an address window of their own. The image, heap and stacks keep their existing code. New code goes in `src/kernel/memory/` (`regions.*`, `memory_object.*`), not in the 3700-line `process.cpp`.
  - **Objects:** a memory object owns its frames. It is sized once by `ftruncate` before the first mapping and never shrinks, so there is no fault-on-truncate case. Frames are allocated and zeroed then, so "out of memory" has one clean failure point. Descriptors and mappings each hold a reference; the frames are freed when the last one goes.
  - **Mapping:** every mapping installs its pages at `mmap` time, anonymous ones too, because the kernel's user-copy code does not fault pages in. Mappings are never executable.
  - **Teardown:** unmapping an object region removes the pages, waits for the TLB shootdown, then drops the reference without freeing frames. At exit, object regions are removed this way before `DestroyAddressSpace` frees what is left.
  - **Architecture code:** none expected beyond what exists. To check in the design: AArch64 needs break-before-make when a live entry changes, and both backends must map these pages non-executable.
  - **Accounting:** an object's pages count against the process that created it, never against a process that only maps it, so the compositor can map every client's buffers. A system-wide cap guards RAM. Both show in `/proc`.
  - **Left out:** `fork`, copy-on-write, file-backed mappings, `mremap`, `MAP_FIXED` over existing regions.

### D6. Local IPC and the window protocol

- **Options:** (a) `AF_UNIX` stream sockets with `SCM_RIGHTS`; (b) TCP over loopback plus named shared memory; (c) a custom kernel message port.
- **Trade-offs:** (b) needs no new socket code but depends on (c)-style names from D5 and gives no crash cleanup. (c) is non-standard and helps no other program. (a) is the Wayland model and reuses the existing socket syscalls and the pipe code's ring buffers.
- **Recommendation: (a).** `socketpair`, and `bind`/`listen`/`accept`/`connect` on abstract names first (a kernel name table, so no socket nodes in the filesystems). `sendmsg`/`recvmsg` carry at most 8 descriptors per message. Passing a local socket over a local socket is refused at first, which rules out reference cycles.
- **Protocol:** our own small binary protocol, versioned, specified in `docs/design/M39.md`. A client creates surfaces, attaches shared buffers, reports damage and commits; it sets the pointer shape by name (Text over a text field, Link, Working); it receives configure, close, pointer, key, frame and theme events. Each window has two buffers, so the compositor never reads a half-drawn frame. Later extensions add popups, the shell's privileged requests, app menus, the lock role, the clipboard, and drag and drop.
- **Rule:** the compositor never trusts a client. It checks buffer sizes with `fstat`, never blocks on a client's socket, and disconnects a client that sends garbage or stops reading.

### D7. Who draws what

- **Decorations:** drawn by the compositor, as `GUI.md` recommends, so every window matches and a hung app can still be moved and closed.
- **Processes:**
  - `compositor`: scene, input, window management, decorations, animations, cursor, wallpaper, switcher and overview (they need every window's pixels).
  - `shell`: menu bar, dock, Feather menu, Quill, notifications, desktop icons, customize mode. A privileged client using layer surfaces. If it crashes, windows survive and it is restarted.
  - `greeter`: login and lock. A client with a session-lock role: while locked, only its surface shows and gets input. If it dies, the screen stays locked and it is restarted.
  - `desktop`: the session leader that `init` starts on `tty1`; it starts and supervises the three above. `tty2` to `tty4` stay text consoles, which helps debugging.
- **Global menus:** the menu bar shows the focused app's menus, so an app exports its menu tree through the compositor and receives "item chosen" events.
- **Alternative:** putting the shell inside the compositor saves the shell protocol (about one M step) but one shell bug then ends the session.

### D8. Damage and memory budget

One full-screen 32-bit surface is **3.0 MiB at 1024x768** and **7.9 MiB at 1920x1080**.

| Item | 1024x768 | 1920x1080 |
|---|---|---|
| Kernel back buffer (exists today) | 3.0 MiB | 7.9 MiB |
| Compositor back buffer (none if it maps the kernel's, D9) | 3.0 MiB | 7.9 MiB |
| Wallpaper, drawn once at screen size | 3.0 MiB | 7.9 MiB |
| Icons, shadows, cursors, thumbnails | under 1 MiB | under 1 MiB |
| **Compositor total** | **about 7 MiB** | **about 17 MiB** |
| A maximized window, two buffers | 5.3 MiB | 14.5 MiB |
| The prototype's start-up session (Terminal, Task Manager, Settings), two buffers each | 5.9 MiB | 5.9 MiB |

- With D5's accounting both sizes fit the 64 MiB limit: the compositor stays near 17 MiB, and an app with a maximized 1080p window uses 14.5 MiB of buffers plus its heap. At 4K a maximized window alone is about 60 MiB, which is why 4K waits.
- **Damage rules:**
  - No damage, no frame; an idle desktop uses no CPU.
  - Damage is a banded rectangle region with union, subtract and intersect (the 8-rectangle `DirtyRegion` stays for simple programs).
  - Each frame walks the windows front to back and subtracts opaque areas, so hidden pixels are never drawn.
  - Only shadows, rounded corners and fades are blended; everything else is copied.
  - The cursor bypasses the scene: restore the saved pixels, draw, flush two small rectangles.
- **Cost per full redraw today:** four passes over the pixels (compose, bounce, back buffer, screen): 12 MiB moved at 1024x768 and 32 MiB at 1920x1080. At 50 frames a second that is 0.6 and 1.6 GiB/s, too much under TCG. So full-screen effects are rare and short, and the present path is measured and shortened first (D9).
- `GUI.md`'s millisecond targets are for real hardware. QEMU targets are set from the first measurements (M35 Part C, M39 Part D).

### D9. Output path and frame pacing

- **Output options:** (a) keep `FB_PRESENT` as it is; (b) shorten `FB_PRESENT`: copy only the presented span to the screen instead of whole rows, and drop the bounce row if measurement shows it matters (`docs/design/M35.md`, D9); (c) the compositor maps the kernel's back buffer (`FB_MAP`) and asks for rectangles to be flushed (`FB_FLUSH`).
- **Recommendation:** do (b) early, since it is small and helps every program. Decide (c) from the measurements at the end of M39; it removes two more passes but needs D5.
- **Pacing options:** (a) frames every 20 ms on the existing tick (50 Hz); (b) timeouts finer than a tick for `poll`; (c) a 1000 Hz tick.
- **Recommendation:** start with (a). The prototype's animations last 80 to 220 ms, which is 4 to 11 frames. Add (b) only if 50 Hz looks wrong on real hardware. (c) costs interrupts under TCG and changes scheduling for everything.
- A sub-millisecond `CLOCK_MONOTONIC` (TSC or the AArch64 counter) is needed in any case, to measure frames.
- Desktop binaries may have to be built `-O2` even in Debug builds. Decide from the same measurements.

### D10. Input path

- The compositor reads keys from its fb0 descriptor and the pointer from `/dev/mouse`, in one `poll`. Both work today.
- Kernel changes: a Super modifier bit and the Meta key codes on every keyboard path; an acquire flag for raw keys (no `SIGINT` on `Ctrl+C`); an acquire flag that keeps the screen until the first present; absolute pointer events for `usb-tablet`.
- **Why absolute pointers:** the QEMU window then needs no mouse grab, and a test can say "move to (x, y)" exactly.
- Text input (key code to character) is done in the toolkit from a keymap. US first; the structure allows more layouts.
- `Alt+F1` to `Alt+F4` stay with the kernel, so the desktop never uses them.
- **Development caveat:** on a Mac host, QEMU's window takes some Command shortcuts (`Cmd+Q` quits QEMU). The compositor gets a setting that makes `Ctrl+Alt` act as Super, as the prototype does in the browser.

### D11. The boot animation, exactly as approved

- **Who draws it:** the kernel, because it runs before any program.
- **Options for the frames:**
  - (a) capture them from headless Chrome and ship those;
  - (b) generate them on the host with our own code, and check them against Chrome;
  - (c) rasterize the vector feather in the kernel at run time.
- **Trade-offs:** (a) is exact by construction but cannot be regenerated byte for byte, since anti-aliasing changes between Chrome versions. (c) puts the outline renderer in the kernel. (b) is deterministic and follows the fonts: generated files are committed with checksums.
- **Recommendation: (b).**
  - `tools/gui/boot.py` evaluates the three curves from `HANDOFF.md` (`bx`, `by`, `br`, each segment with its own cubic-bezier easing), the fade-in, the shadow and the wordmark fade. It samples every 20 ms, which is every second timer tick, and writes one transform per sample.
  - The frames are then drawn by `guirender`, the host tool built from FeatherOS's own outline renderer (M37), from the feather's outline at each sample's transform. So there is one renderer, and no rasterizer in Python.
  - The frames are stored as cropped, run-length-encoded coverage planes (vane with the barb notches cut out, and shaft), with the wordmark and the four stage lines pre-rendered from the committed fonts. Expected size is a few hundred KiB; measure it in the first step.
  - **Check 1, exact:** a developer script loads the prototype in headless Chrome with Playwright, sets the animation time for each sample, and reads the three computed transform matrices. They must match the generator's within 0.01 px and 0.01 degrees. This checks the motion itself, free of anti-aliasing.
  - **Check 2, visual:** the same script screenshots each sample and compares it with the generated frame within a small tolerance, and traces the quill tip and the feather's centre, as `HANDOFF.md` advises.
  - Both checks run by hand when the generator changes; their result table is committed. The normal build and the matrix need neither Chrome nor Playwright.
- **In the kernel:** the timer tick on CPU 0 draws each frame, without allocating. Log lines stay in the `dmesg` ring and on serial but off the screen. The progress bar follows real boot stages. `boot=verbose` skips the picture, Esc shows the log, and any failure returns to the console.
- **Timing (assumption to confirm):** the animation always plays to its end, so the desktop appears no earlier than about 2.3 s after the timer starts, even when the boot is faster. Tests boot with `boot=verbose`, so the matrix does not slow down.

### D12. Artwork is drawn live (decided)

"Host" means the development machine (the Mac that builds FeatherOS). "Host tools" are scripts that run there, never inside FeatherOS.

**Decided by the owner on 2026-11-26: FeatherOS draws its artwork itself.** Nothing on the desktop is shipped as a picture. That includes icons, app tiles, pointers and shadows, which the handoff had planned as pre-rendered bitmaps.

- **Why the change:** the prototype draws its 46 glyph icons at about 20 sizes between 12 and 64 px, before any UI scaling. Bitmaps for every size would be about 0.5 MiB per scale, need a second renderer in Python, and need regenerating for every new size.
- **What ships:** one small file of outlines (`/system/gui/outlines.fvo`): the glyphs, the feather, the pointers and the wallpaper as polylines of integer points.
- **What draws:** one outline renderer in the C++ graphics library (fills and round strokes, with a cache). It draws at any size, so UI scaling later needs no new files. Shadows are computed at load as a blurred rounded rectangle.
- **What the host does:** `tools/gui/outlines.py` flattens the SVG sources into that file, using only Python's standard library, and `tools/gui/tokens.py` generates the colour tables. Both outputs are committed with checks, as the fonts are. **Building FeatherOS needs no new tools.**
- **The one exception:** the boot animation's frames are pre-rendered (D11), because the kernel draws them before any program runs.
- **A free check for the pointers:** the PNGs in `docs/design/gui/cursors/png/` are the design tool's own renders. FeatherOS's renderer, run on the host, must match them within a small tolerance.
- **Playwright and Chrome** are used only by a developer, for the two boot-animation checks in D11 and for review screenshots of the prototype.
- **Cost:** a few milliseconds per app at start-up to draw its icons into the cache. Measured in M37 step D1, with a budget.

The full design is `docs/design/M37.md`.

### D13. Design tokens and themes

- One source, `tools/gui/tokens.json`, extracted once from the prototype's theme blocks: 8 themes, the accent table (a light and a dark shade for each colour), radii, control heights, durations and easing.
- A generator writes a committed C++ table. A check proves three things: the table matches the JSON, the prototype's CSS still matches the JSON, and every theme and accent passes the 14 contrast pairs at 4.5:1.
- If a pair fails, the owner decides; the generator never changes a colour.
- Themes are compiled in. The chosen family, mode, accent and the other desktop settings live in one settings file. The compositor owns it and tells every client when it changes.

### D14. How GUI pixels are tested

- All drawing uses integer arithmetic with fixed rounding, so the host build and both guests produce the same bytes. No floating point in pixel paths.
- **Host:** Catch2 tests under ASan and UBSan. Each drawing operation is compared with a naive per-pixel reference on random input. Widgets are rendered off screen and compared with pinned checksums; the images can be written out for review beside prototype screenshots.
- **QEMU:** a host tool renders the expected picture with the same C++ code, and the smoke test compares the QMP `screendump` with it byte for byte, as `FONTS` does with `ffnt.py`. Geometry is also checked numerically through a state dump from the compositor.
- Every step lands with a matrix case on both architectures. Cases at 1920x1080 are added only where the size matters, to keep the matrix short.

### D15. Terminal emulation

The roadmap plans to move the VT100 code out of the kernel. **Recommendation:** share it instead. Extract the parser and screen model from `term.cpp` into a library that both the kernel console and the Terminal app compile, as `src/user/text/` is shared today. The text consoles keep working, and the existing host tests keep covering it.

### D16. Honest state on missing hardware

The prototype's default menu bar shows battery and sound items, and its network item has a Wi-Fi switch. FeatherOS has no driver for any of the three. **Recommendation:** leave the battery and sound items out of the default bar until a driver exists, let the network item show Ethernet only, and show the Sound, Power and Wi-Fi settings disabled with the reason, as `GUI.md`'s principle 6 says. Sleep is listed and disabled.

## 3. Milestones

### Order and parallel work

| # | Milestone | Kind | Needs | First visible result |
|---|---|---|---|---|
| M35 | Screen and input foundations | kernel | – | The console at 1920x1080 |
| M36 | Shared memory and local sockets | kernel, libc | – | – |
| M37 | Pixels, design tokens and artwork | user space, host | – | A test card with the icons, shadows and themes |
| M38 | Boot splash | kernel | M35 A, M37 B4 and C1 | The falling feather at boot |
| M39 | Compositor and window protocol | user space | M35, M36, M37 | Wallpaper, a cursor, two test windows dragged |
| M40 | Window management | user space | M39 | Decorated windows that snap, workspaces, switcher |
| M41 | Toolkit and UI Elements | user space | M37 (M39 to show windows) | The UI Elements gallery in all themes |
| M42 | Desktop shell | user space | M40, M41 | The prototype's default desktop |
| M43 | Session: login, lock, power | user space, small kernel | M42, M38 | Boot, feather, login, desktop |
| M44 | Core apps | user space, small kernel | M41, M42 | Terminal, Files, Settings, Task Manager |
| M45 | Polish, scaling and real hardware | all | M44 | – |
| M46 | GPU tier | kernel, user space | M45 | Blur and scaled transitions |

Three tracks do not depend on each other and can be worked in any order or side by side:

- **Kernel track:** M35, then M36.
- **Pixel track:** M37, then the host-testable parts of M41 (A1 to A4), which need no compositor.
- **Boot track:** M38, as soon as M35 Part A and M37 steps B4 and C1 are done.

They meet at M39. After M39, M40 and M41 are independent. M44's parts are independent of each other.

Each milestone ends with the full matrix, and each part with its affected smoke modes on both architectures, as `CLAUDE.md` asks. "Both" in the tables means x86-64 and AArch64.

### M35. Screen And Input Foundations

Design: `docs/design/M35.md`. Plan: `docs/plans/m35.md`.

Goal: the screen size comes from the bootloader, the framebuffer device gives a compositor the input it needs, and the cost of putting pixels on the screen is known.

| Step | Delivers | Needs | Tests | Size |
|---|---|---|---|---|
| A1 | The framebuffer and console take their size from Limine. The back buffer is sized at start-up without the heap, the console grid is computed up to a static maximum (a larger screen centres it), the present bounce row follows the screen. `Config::Width` and `Height` go away. | – | Host: `framebuffer` and `term` tests at 800x600, 1024x768, 1920x1080 and an odd size. QEMU: every existing mode still passes at 1024x768, both. | M |
| A2 | `SCREEN=WxH` in `build.sh` writes the Limine resolution and the harness picks a matching QEMU display. | A1 | QEMU: new cases `screen_1080p`, both: console text and status bar by `screendump`, `fbdemo`'s pattern, `fontview --card` against its reference at 1920x1080. | S |
| B1 | A Super modifier bit and `KEY_LEFTMETA`/`KEY_RIGHTMETA` in `fb_event`, on the PS/2, USB and virtio keyboard paths. | – | Host: `keyboard` and `graphics_mode` tests. QEMU: `GRAPHICS` sends `meta_l` by QMP and reads it back, both. | S |
| B2 | Two `FB_ACQUIRE` flags: raw keys (`Ctrl+C` is only queued) and keep-screen (no blanking before the first present). | – | Host: `Owners` tests. QEMU: `fbtest` in raw mode gets `Ctrl+C` as a key and no signal; the screen is unchanged after a keep-screen acquire, both. | S |
| B3 | Absolute pointers: the HID parser reads absolute X and Y, and `/dev/mouse` reports `EV_ABS` scaled to 0..32767. | – | Host: HID report tests with the tablet's descriptor. QEMU: the `USB` mode with `usb-tablet` and QMP absolute events, read by `mousetest`, both. | M |
| C1 | A sub-millisecond `CLOCK_MONOTONIC` from the TSC or the AArch64 counter (code under `arch/`). Sleeps stay tick-based. | – | Host: conversion arithmetic. QEMU: `TIMER` checks that it never goes back and resolves under 1 ms, both. | S–M |
| C2 | `fbbench`: times full-screen and small `FB_PRESENT` calls. The numbers for Debug and Release, both sizes, both architectures go in `docs/design/M35.md`. | A2, C1 | QEMU: a shell part runs it and checks that it reports, both. | S |
| C3 | The present copies only its span to the screen; the bounce row goes only if `fbbench` shows it matters. | C2 | Host: the span copy leaves the same pixels as the whole-row copy for random rectangles. QEMU: `GRAPHICS`, `FONTS` and `DUNGEON` unchanged; `fbbench` shows the gain. | S |

**Complete when:** both targets boot and run the console at 1024x768 and 1920x1080, a full-screen program receives Super and a raw `Ctrl+C`, `usb-tablet` moves the pointer, and the present cost is written down.

### M36. Shared Memory And Local Sockets

Design: `docs/design/M36.md`. Plan: `docs/plans/m36.md`.

Goal: two unrelated processes can share pixel memory and exchange messages and descriptors. The design also closes a small existing race: a frame could be freed while another thread's syscall was still copying through it.

| Step | Delivers | Needs | Tests | Size |
|---|---|---|---|---|
| A1 | The region list for a mapping window of its own, and `/proc/<pid>/maps`. No behaviour change. | – | Host: region tests (insert, find, split, overlap) with random operations against a model. QEMU: `SHELL`, `MEMORY` and the `hardening` tests unchanged, both. | M |
| A2 | The user-copy drain, and anonymous `mmap` and `munmap` (private, zeroed and populated at once, in an ASLR window, counted in the page limit) with `<sys/mman.h>`. | A1 | Host: region tests. QEMU: a `MEMORY` part: map, touch, unmap, reuse; the limit gives `ENOMEM`; access after unmap gives `SIGSEGV`; threads on 4 CPUs unmap while others touch, both. | M |
| A3 | Optional: `malloc` takes large blocks from `mmap`, which removes the "only the top of the heap" limit. | A2 | Host: allocator tests. QEMU: `libctest` and `memtest`, both. | S |
| B1 | Memory objects: `memfd_create`, `ftruncate` (once), `fstat`; frames owned and reference-counted; accounting and caps. | A1 | Host: object lifetime and limit tests, and a `[concurrency]` test under ThreadSanitizer. | M |
| B2 | `mmap(MAP_SHARED)` of an object in two processes (the descriptor inherited through `Spawn` for now), with unmap and exit ordering. | B1, A2 | QEMU: `shmtest`: parent and child share a counter and an 8 MiB checksummed buffer; a child is killed mid-write; 100 create, map and unmap cycles leave `MemFree` unchanged, both. | M–L |
| C1 | `AF_UNIX` stream sockets: `socketpair`, and bind, listen, accept and connect on abstract names; non-blocking, `poll`, shutdown, hang-up on close. | – | Host: the buffer and accept-queue state machine. QEMU: `unixtest`, both. | M |
| C2 | `sendmsg` and `recvmsg` with `SCM_RIGHTS`; a descriptor never received is closed. | C1 | Host: in-flight lifetime tests. QEMU: one process passes a memory object to an unrelated one, both map it and agree on a checksum; a receiver exits without receiving and `MemFree` returns, both. | M |
| C3 | libc (`<sys/un.h>`, socket additions), `docs/posix.md`, the syscall table, and the `IPC_TEST` mode. | B2, C2 | QEMU: matrix cases `ipc`, both, plus one Release case. | S |
| D1 | Conditional on M39's measurements: `FB_MAP` and `FB_FLUSH` (D9). | B2, M35 | Host: flush clipping. QEMU: `fbdemo --mapped` gives the same `screendump` as the present path, both. | M |

**Complete when:** `shmtest` and `unixtest` pass on both targets in Debug and Release, no frames leak after crashes, and `docs/posix.md` lists the new calls and what they leave out.

### M37. Pixels, Design Tokens And Artwork

Design: `docs/design/M37.md`. Plan: `docs/plans/m37.md`.

Goal: everything needed to draw the prototype's look into memory, with its colours and artwork, and a first picture of it on a real screen. Needs no kernel work. Artwork is drawn live (D12).

| Step | Delivers | Needs | Tests | Size |
|---|---|---|---|---|
| A1 | `tools/gui/tokens.json`, the generator, the committed C++ tokens (D13). | Step 0 | CTest `gui.tokens`: regeneration is identical, the prototype's CSS matches, contrast passes for all 8 themes and every accent. | S–M |
| B1 | `Graphics::Region`: banded rectangles with union, subtract, intersect and translate. | – | Host: random operations against a bitmap model. | M |
| B2 | A surface format for premultiplied ARGB, coverage masks, a `Canvas` with a clip stack and origin, and the blend rules. | – | Host: each operation against a naive reference on random input, with pinned checksums. | M |
| B3 | Rounded rectangles (fill and stroke), pills, circles and straight lines. | B2 | Host: against a brute-force reference within 2 levels; symmetry checks. | S–M |
| B4 | The outlines file and its loader, the outline renderer (fills and round strokes on an 8x8 sample grid, integer arithmetic only) and its cache. | B2 | Host: loader refusals and damaged files; fills and strokes against a brute-force reference; the cache gives the same pixels. | M–L |
| B5 | Shadows computed at load and drawn as 9-slices, area-average scaling for thumbnails, text through the canvas with an ellipsis. | B2 | Host: shadow symmetry and coverage; stretch tests; text equal to today's. | M |
| C1 | The art sources (`tools/gui/art/`), `outlines.py`, the committed `outlines.fvo`, and `DrawGlyph`. | B4 | CTests `gui.files`, `gui.reproducible`, `gui.prototype`. Host: every glyph present and inside its box. | M |
| C2 | App tiles composed live: rounded square, fold, glyph; the small and plain styles; the high-contrast form. | C1, B3, A1 | Host: fold, corners, glyph placement, the styles. | S–M |
| C3 | The 13 pointers from `docs/design/gui/cursors/` at 24, 32 and 48 px, with hotspots, the accent as a paint slot and 8 spinner frames. | C1 | CTest `gui.pointers`: each pointer matches the design's PNG within the measured limits. Host: hotspots, accent, frames. | M |
| C4 | The wallpaper drawn at the screen's size in the theme's colours, and the shadows by role. | C1, B5, A1 | Host: full coverage and pinned checksums at three sizes. | S |
| D1 | `uicard`: a test card on `/dev/fb0` with the shapes, glyphs, tiles, pointers, shadows, wallpaper and themes. The host tool `guirender` draws the same card with the same code. | B, C | QEMU: `PIXELS_TEST`: the `screendump` equals the host render byte for byte at 1024x768 and (once M35 A2 exists) 1920x1080, both. Drawing times recorded against a budget. | M |
| D2 | Docs. | D1 | — | S |

**Complete when:** the card matches on both targets at both sizes, the tokens and outlines regenerate byte for byte, and the drawing times are recorded.

### M38. Boot Splash

Goal: the approved falling feather at boot. Design per D11.

| Step | Delivers | Needs | Tests | Size |
|---|---|---|---|---|
| A1 | `tools/gui/boot.py` (the transform per sample), a `guirender` subcommand that draws the frames with the M37 renderer, and the committed frame file. | M37 B4, C1 | CTest: identical regeneration. Host: the trace test (the end pose is the logo, the quill tip lands once and does not rise again, the keyframe values from `HANDOFF.md` are hit). | M |
| A2 | The developer check against headless Chrome: transform matrices and frames (D11). The result table goes in `docs/design/M38.md`. | A1, Playwright on the developer's machine | Run by hand; not in the matrix. | S–M |
| A3 | The kernel player: splash mode, the frame decoder and blender, drawing from the timer tick, the stage-driven progress bar, `boot=verbose`, Esc for the log, fallback to the console on failure. | A1, M35 A1 | Host: decoder, blender and timeline tests. QEMU: `SPLASH_TEST`, both: a test hook holds the splash at chosen frames and the `screendump` equals the generated frame; `boot=verbose` shows the console; a forced panic shows the console. One case at 1920x1080. | M |
| A4 | The end of the splash: the last frame stays until a program takes the screen with keep-screen, or the console returns once the animation is over. | A3, M35 B2 | QEMU: the shell prompt appears after the animation; `fbdemo` started with keep-screen replaces the picture without a blank frame, both. | S |

**Complete when:** both targets show the animation at the right place and time at both screen sizes, and a failed boot is never hidden behind it.

### M39. Compositor And Window Protocol

Goal: `GUI.md`'s first proof point: a cursor and two overlapping windows from separate processes, dragged smoothly, with measured frame times.

| Step | Delivers | Needs | Tests | Size |
|---|---|---|---|---|
| A1 | The protocol specification and the wire library `src/user/desktop/wire/`: framing, typed messages, attached descriptors, a version handshake. | – | Host: round trips; truncated, oversized and random input never crashes under ASan. | M |
| B1 | The scene as a pure library `src/user/desktop/scene/`: layers, surfaces with opaque regions, damage, front-to-back drawing into a `Canvas`. | M37 B | Host property test: after random changes, the incremental picture equals a full redraw byte for byte, and no pixel outside the damage is touched. | M–L |
| B2 | The `compositor` program: takes the screen (raw, keep-screen), draws the wallpaper, runs the frame loop (no damage, no frame), draws the software pointer (the Arrow at the size in the settings), reads keys and pointer in one `poll`, handles `Hidden` and `Shown`. **First desktop pixels: wallpaper and a moving cursor.** | B1, M35, M37 C3–C4 | QEMU: `COMPOSITOR_TEST`, both, both sizes: after QMP pointer moves, the `screendump` equals the host-rendered wallpaper with the cursor at that place; `Alt+F2` and back repaints. | M |
| B3 | The server side: the listening socket, clients, surfaces, buffer checks, attach, damage, commit, frame events, pointer and key focus, and the pointer shape a client asks for. | A1, B2, M36 | Covered by C1 and C2. | M |
| C1 | The client library `src/user/desktop/client/` (two-buffer pool, event loop) and `wintest`, which draws a pattern and prints its events. | B3 | QEMU: two `wintest` windows overlap as the host render says; pointer and key events reach the right client, both. | S–M |
| C2 | Robustness: a client that exits, is killed, hangs, lies about its buffer size or floods the socket; 32 clients at once. | C1 | QEMU: `wintest --crash`, `--lie-size`, `--flood`; the compositor keeps running and its page count returns to the start value, both. | M |
| D1 | Frame-time measurement (`compstat`) and the proof run: a scripted drag of two overlapping windows at both sizes, Debug and Release, both architectures. The numbers go in `docs/design/M39.md`. | C1 | The run itself. It decides M36 D1 (`FB_MAP`), finer `poll` timeouts, and whether desktop binaries are always built `-O2`. | S–M |

**Complete when:** the proof run passes its screendump checks on both targets at both sizes, a misbehaving client cannot stop or corrupt the compositor, and the frame times are recorded with the decisions they led to.

### M40. Window Management

Goal: the prototype's window behaviour. The policy is a pure library, `src/user/desktop/wm/`, transcribed from the prototype's own functions (`area`, `zoneRect`, `detectZone`, `clampWin`, the focus order), so the host tests can use the prototype's numbers.

| Step | Delivers | Needs | Tests | Size |
|---|---|---|---|---|
| A1 | Decorations and focus: the 32 px title bar in both button layouts, button states, the window menu, focused and unfocused styles, shadows, rounded corners, click to focus and raise, close requests. | M39 | Host: decoration rendering in 2 themes and both layouts. QEMU: `WM_TEST`: pixel compare of two decorated windows, focus change by click, both. | M |
| A2 | Move and resize: title drag, eight handles with the four resize pointers, the 240x160 minimum, resize in step with the client's redraw, the 90 px reachable rule, double-click to maximize. | A1 | Host: policy tests. QEMU: scripted drags; geometry from the state dump and a pixel compare, both. | M |
| A3 | Snapping: zones within 10 px of an edge, the preview, halves, quarters and maximize, restoring the size on drag-off, the work area. | A2 | Host: zone tests with the prototype's numbers at both screen sizes. QEMU: drag to each zone, both. | S–M |
| A4 | The animation engine (timeline, fixed-point cubic-bezier easing, cancel and jump to the end, skip on a missed budget, reduce motion) with window open and close, the outline flight for minimize and restore, and the Working pointer for 0.5 s after an app opens (its spinner steps every 90 ms). | A1 | Host: easing values against the CSS curve; the cancel rules. QEMU: with animations held at chosen frames, pixel compare, both. | M |
| A5 | Workspaces: 4 by default, 1 to 9, move a window, the switch animation. | A1 | Host: policy tests. QEMU: windows on two workspaces, switch and move, both. | S–M |
| A6 | The global shortcut table from `HANDOFF.md` and the window switcher with live thumbnails (hold, step, release to switch). | A5, M37 B5 | Host: switcher order and state machine. QEMU: `Super+Tab` twice then release focuses the right window, both. | M |
| A7 | The workspace overview: strip with mini layouts and Add, thumbnail grid sized to fit (the prototype's overflow bug must not be copied), click, drag to a workspace, close. Modal dialogs dim their parent and trap focus. | A6 | Host: grid layout tests, including a tall window. QEMU: open the overview, drag a window to workspace 2, both. | M |

**Complete when:** every row of the window behaviour in `GUI.md` section 6 and every shortcut in `HANDOFF.md` works on both targets, checked by scripted input.

### M41. Toolkit And UI Elements

Goal: every control the prototype's UI Elements app shows, in all 8 themes, usable from the keyboard. This is the largest user-space piece. Steps A1 to A4 need only M37 and can start before the compositor exists.

| Step | Delivers | Needs | Tests | Size |
|---|---|---|---|---|
| A1 | The core in `src/user/ui/` (`FeatherOS::UI`): widget tree, layout on the 4 px grid, painting with clip and per-widget damage, pointer and key events, focus traversal, live theme changes, and an off-screen backend for tests. | M37 | Host: layout and event-routing tests; off-screen rendering with pinned checksums. | L |
| A2 | Text: the keymap (US), the text field's caret and selection, the type roles from the tokens. | A1 | Host: editing tests reusing the UTF-8 boundary rules; rendering per state. | M |
| A3 | Controls, first batch: the five buttons, checkbox, radio, switch, segmented control, slider, both progress bars, badge, label, separator. Every state: normal, hover, pressed, focused, disabled. | A1 | Host: one render per control, state and theme. | M |
| A4 | Controls, second batch: tabs, list, sortable table, icon grid, text area, and the scroll view with the scrollbar from `HANDOFF.md` (12 px gutter, 6 px thumb, 8 px on hover, accent while dragged, a solid thumb on a track in the high-contrast themes). | A2 | Host: as A3, plus scrolling and keyboard tests. | L |
| A5 | The window backend, and popups: menu (checks, shortcuts, headers, separators, disabled items, full keyboard use), context menu, tooltip after 500 ms, dialog and alert with a focus trap. | A3, M39 C1 (popup surfaces added to the protocol) | Host: menu model and keyboard tests. QEMU: `ELEMENTS_TEST` opens a menu and a dialog by keyboard, both. | M |
| A6 | The `elements` app: the UI Elements gallery, with the pointer tiles and the scrolling sample. A developer page puts its renders beside prototype screenshots for review. | A4, A5 | QEMU: the gallery window equals the host render in 2 themes at both sizes; a keyboard walk reaches every control, both. | M |

**Complete when:** the gallery matches the prototype's by eye in all 8 themes, and every control can be reached and operated without a pointer.

### M42. Desktop Shell

Goal: the prototype's default desktop: menu bar, dock, Feather menu, Quill, notifications, desktop icons and trash. Logic that the prototype already has as functions (search scoring, the command list, the calculator, the dock and menu models) is ported and host-tested with the prototype's cases.

| Step | Delivers | Needs | Tests | Size |
|---|---|---|---|---|
| A1 | The shell's part of the protocol: layer surfaces with reserved zones, the window list and its controls, workspace state, exported app menus, shortcut registration, notification requests. The app registry: one small file per app in `/system/apps/` (id, name, icon, colour, category, command). | M40 | Host: wire tests; registry parser tests. | M |
| A2 | The menu bar: Feather button, app name, global menus with open-tracking across the bar, and the status items for workspaces, CPU, network, notifications and clock with calendar. Overflow rules. | A1, M41 | Host: menu-bar model. QEMU: `SHELL_UI_TEST`: the bar equals the host render; a File menu opens and tracks to Edit, both. | M–L |
| A3 | The dock: pinned and running apps, the three indicators, the three positions, sizes 32 to 64, auto-hide, context menu, tooltips. It reserves the work area and gives the minimize flight its target. | A1, M41 | Host: dock model. QEMU: click to launch, click to minimize, move the dock to the left, both. | M |
| A4 | The Feather menu: system actions, the launcher with categories and search, confirmations for power actions. | A2 | Host: search and ordering. QEMU: `Super+Shift+Space`, type, Enter launches, both. | M |
| A5 | Quill and the command registry shared with menus and shortcuts: windows, apps, commands, settings panes, files (a directory walk, no index), the calculator (reusing `calculator.*`), run in Terminal; the five prefixes; the full keyboard model. | A2 | Host: scoring, grouping and prefix tests ported from the prototype. QEMU: `=2+2` shows 4; a command snaps the focused window, both. | L |
| A6 | Notifications: toasts for 5 s, the list, Do Not Disturb. | A2 | Host: queue and timing. QEMU: a test client posts one; it shows, then is in the list, both. | S–M |
| A7 | The desktop: icons, the root context menu, and the trash (a trash directory with origin records, Put Back, Empty with confirmation, the full icon in the dock). | A3 | Host: trash model. QEMU: trash a file from a test client, put it back, both. | M |
| A8 | Customize mode for the bar and dock, and the settings file that keeps the layout. | A3 | Host: reorder model, settings round trip. QEMU: remove an item, restart the shell, it stays removed, both. | M |

**Complete when:** a boot into the desktop shows the prototype's default screen in the default theme, the shell survives being killed, and every shell shortcut works.

### M43. Session: Login, Lock, Power

| Step | Delivers | Needs | Tests | Size |
|---|---|---|---|---|
| A1 | SHA-256 and PBKDF2-HMAC-SHA256 in a small support library, the `passwd` program, the hash file. | – | Host: the published test vectors. QEMU: a shell part sets and checks a password, both. | S–M |
| A2 | `login` on the virtual consoles when a password is set (D4). | A1 | QEMU: with a password, `tty2` asks for it and refuses a wrong one; without one, nothing changes, both. | S–M |
| A3 | The `greeter` and the session-lock role: the login and lock screens as in the prototype (clock, avatar, password field, the shake on an empty or wrong password, restart and shut down), `Super+L`, automatic login. | A1, M42 | Host: lock state machine. QEMU: `SESSION_TEST`: wrong password refused, right one enters; while locked no key reaches a client; killing the greeter leaves the screen locked, both. | M |
| A4 | The `desktop` session program and its `init` entry; log out (ask apps to close, then stop them), restart and shut down with their screens; Sleep disabled with its reason. The desktop becomes the default on `tty1` for interactive builds; tests keep the text console unless they ask for the desktop. | A3 | QEMU: log out returns to login; shut down from the Feather menu powers off cleanly, both. | M |
| A5 | The handover from the splash: the compositor's first frame is the splash's last, then the 240 ms crossfade, the bar slides in and the dock fades in. | A4, M38 | QEMU: no blank frame between splash and login, checked by screendumps at held frames, both. | S |

**Complete when:** a cold boot plays the feather, shows login, and reaches the desktop on both targets; locking cannot be bypassed from the keyboard, including through the text consoles.

### M44. Core Apps

The parts are independent. Each app is its own program with an entry in `/system/apps/`.

| Step | Delivers | Needs | Tests | Size |
|---|---|---|---|---|
| A1 | The shared VT core, extracted from `term.cpp` and used by the kernel console unchanged (D15). | – | Host: the existing terminal tests still pass. QEMU: `SHELL` and `SCREEN` unchanged, both. | M |
| A2 | Terminal: a pty master, the `mono-16` grid with damage, resize, scrollback, the clipboard with `Ctrl+Shift+C`. | A1, M41 | Host: grid renderer. QEMU: `vi` and `top` run in a window; the cells match the expected text; two terminals at once, both. | M–L |
| B1 | Files: places, the path bar, the icon view, open, the context menu, Move to Trash by menu and Delete. Dragging onto the trash waits for drag and drop in M45. | M41, M42 A7 | Host: directory model. QEMU: browse to `/system/fonts`, trash and restore a file, both. | M–L |
| C1 | Settings: the category grid, search and the pane frame; the Personal panes, all real (Appearance with family, mode, Auto from 07:00 to 19:00, accent, animations, window buttons; Desktop; Dock; Menu Bar; Workspaces; Notifications). | M41, M42 A8 | Host: settings model. QEMU: change the theme and the dock position; both apply at once and survive a restart, both. | L |
| C2 | The other panes, each real or disabled with its reason: Display (read-only), Keyboard (repeat needs a small kernel setting; one layout), Mouse (with the pointer size: 24, 32 or 48), Disks (the mount table), Network, Sharing, Users (password, automatic login), Date & Time, Services (needs a control channel in `init`), About. Sound and Power show as unavailable. | C1 | Host: `init` control protocol. QEMU: stop and start `httpd` from Services; set the password in Users, both. | L |
| D1 | Disk and network counters in `/proc` (kernel). | – | Host: procfs formatting. QEMU: a shell part reads them, both. | S |
| D2 | Task Manager: Processes (sortable, End Task with confirmation; no User column until users exist), Performance (per-CPU, memory, disk, network; 40 samples; updates only while visible), Services. | D1, M41 A4 | Host: sampling and sorting, reusing `process_table.*`. QEMU: a busy test program rises to the top by CPU; End Task stops it, both. | M |
| E1 | Notes (a plain-text editor with open and save dialogs) and Calculator (the prototype's keypad over `calculator.*`). | M41 | Host: document model. QEMU: type, save, reopen, both. | M |
| F1 | Games in windows: a windowed backend for `Graphics::Display`, so `featherfall`, `wickdeep` and `fontview` run unchanged, in a full-screen window first. Layouts that fit a small window are a later step. | M39 C1 | QEMU: `GRAPHICS` and `DUNGEON` pass again with the games started from the dock, both. | M |

**Complete when:** each app does on both targets what its prototype counterpart shows, with real data.

### M45. Polish, Scaling And Real Hardware (outline)

Planned in detail when M44 is done. In likely order:

1. Drag and drop between apps (Files onto the trash), and clipboard polish.
2. A motion audit against `GUI.md`'s table, reduce motion, and the budget rule.
3. The error states and first-run flow from `HANDOFF.md`'s backlog (an app quit unexpectedly, disk full, boot failure; choose a theme, the window-button side, set a password). They need designs first.
4. Accessibility: the high-contrast themes checked control by control, scripted keyboard-only paths, a role and label on every widget.
5. UI scale from 125% to 400%: `mono-48` and `mono-64` fonts, assets per scale, a scale in the protocol.
6. More keyboard layouts and time zones.
7. Compact mode below 720 px, and a check at 800x600.
8. The laptop run: the checklist in `docs/hardware.md`, the framebuffer's memory type on x86-64, frame times at the panel's size, and the memory limit if the panel is larger than 1920x1080.

### M46. GPU Tier (outline)

virtio-gpu first, for a real flush, a hardware cursor and page flips; then blur, scaled transitions and dock magnification on the same scene. The CPU tier stays the fallback.

## 4. Risks, and what to measure first

| Risk | Why it matters | First action |
|---|---|---|
| **Present cost under TCG, in `-O0` builds, at 1920x1080** | Four passes over 7.9 MiB per full redraw could take over 100 ms. It decides the frame rate and D9. | `fbbench` (M35 C2) before any compositor code. Then the fast path, then the M39 proof run. |
| **1920x1080 in QEMU on AArch64** | The mode comes from the firmware's list for `ramfb`. It may not offer 1920x1080. | Check with the current `QEMU_EFI` before M35 A2. Fall back to the nearest listed size for the AArch64 cases. |
| **Shared-memory teardown on 4 CPUs** | A frame freed while another CPU can still reach it corrupts memory silently. | Write D5's invariants in `docs/design/M36.md` first; ThreadSanitizer host tests; the kill-mid-write and leak cases in B2. |
| **Descriptors in flight** | Leaks or use-after-close in the kernel. | A host model of the queue before the syscalls exist (M36 C2). |
| **Host and guest pictures must be identical** | D14's whole test method depends on it. | The integer-only rule from M37 B2 on; `uicard` (M37 D1) proves it on both targets early. |
| **Toolkit scope** | The largest piece; it can grow without end. | Build only what the gallery shows. |
| **50 Hz frames from the 100 Hz tick** | Animations may look uneven. | Judge it in the M39 proof run and on hardware in M45; D9 names the fix. |
| **Splash details** | Frame data size in the kernel image; drawing from the timer tick; boots shorter or longer than the animation. | Measure the size in M38 A1; the timeline tests in A3; confirm the timing rule (section 5). |
| **Super inside a QEMU window on macOS** | The host takes Command shortcuts. | The `Ctrl+Alt` setting from D10, from M39 on. |
| **Matrix time** | Each new mode runs on two architectures, some at two sizes. | One mode per milestone where possible; 1080p cases only where size matters. |
| **Real hardware** | An uncached framebuffer mapping could be very slow; laptop touchpads are usually I2C, which has no driver; panels above 1080p hit the memory limit. | Not a gate (D3). Collected in M45 step 8. A USB mouse is needed on a laptop. |
| **Drawing artwork live under TCG** | Icons and the wallpaper are rasterized at start-up, in Debug builds under emulation. | A budget in M37 D1 (all glyphs under 2 s, the wallpaper at 1920x1080 under 2 s in Debug); a smaller sample grid for large drawings if it is missed. |

## 5. Decisions taken, and assumptions to confirm

**Decided on 2026-11-25:** kernel first with every app a separate process (D1), C++ throughout (D2), any resolution early with tests at 1024x768 and 1920x1080 (D3), one real password (D4).

**Assumed here; say so if any is wrong:**

1. **Splash timing:** the animation always plays to the end, so the desktop appears no earlier than about 2.3 s after the timer starts (D11).
2. **Artwork is drawn live (D12), decided 2026-11-26.** No longer an assumption: FeatherOS ships outlines and draws icons, tiles, pointers and shadows itself; only the boot frames are pre-rendered.
3. **New design work happens in the prototype first:** the error states and the first-run flow. (Pointers and scrollbars arrived with the handoff of 2026-11-25.)
4. **The battery and sound items** leave the default menu bar, and the network item shows Ethernet only, until drivers exist (D16).
5. **The user picker and the guest account are hidden** while there is one user (D4).
6. **Text consoles ask for the password** once one is set; the serial console stays open in test builds (D4).
7. **Sharing the VT code** with the kernel instead of moving it out (D15).
8. **The shell and the greeter are separate processes** from the compositor (D7).
9. **Abstract socket names** instead of socket files, at first (D6).
10. **Drag and drop between apps waits for M45;** until then Files uses the menu and Delete for the trash.
11. **Games run in a full-screen window first;** layouts for small windows come later.
12. **One keyboard layout (US) and no time-zone choice until M45.** The prototype lists UK and Hungarian layouts; they show as unavailable until then.
13. **The desktop becomes the default on `tty1` only after M43;** tests keep the text console unless they ask for the desktop.
14. **No pointer during the kernel's boot animation.** The prototype shows the Working pointer on the boot screen; in the OS it appears with the compositor's first frame.
15. **Numbering and branches:** M35 to M46, each on its own `feat/<topic>` branch cut from `main`, with `feat/gui` carrying only the design documents and this plan.
