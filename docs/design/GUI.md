# FeatherOS GUI Design

**Status:** design proposal. Nothing here is implemented or scheduled. The clickable companion is [gui/prototype.html](gui/prototype.html), which uses simulated data.

**Scope.** The boot screen, desktop, menu bar, dock, Feather menu, windows, UI elements, motion, System Settings and Task Manager, plus what the OS needs before they can exist.

## 1. Principles

1. **Quiet by default.** The work is the brightest thing on screen. The accent marks focus, selection and the primary action only.
2. **CPU first.** Everything core runs on a software compositor over a plain framebuffer. A GPU adds polish and never adds function.
3. **Familiar layout, own identity.** A top menu bar and a dock come from macOS. The searchable menu, settings grid, workspaces, tiling and configurable panels come from Xfce and Xubuntu. Square-ish controls on the right of the title bar, the feather and the outline-flight animation are FeatherOS's own.
4. **Configurable without being fiddly.** Panels can be rearranged, and the defaults need no setup.
5. **Fully keyboard-operable.** Every surface works without a mouse, which also matters while USB input is being brought up on real hardware.
6. **Honest state.** Settings for hardware without a driver appear disabled with the reason, never as a working control.

## 2. Themes

Three themes share one layout and one token set. **Recommendation: Calm Dusk as the default and Light & Airy as its light mode, with Warm Paper as an optional third theme.** Dusk continues the dark boot screen with no white flash, its amber belongs to the feather, and it stays comfortable next to the dark console. Airy is the most conventional light theme. Paper is the most distinctive, but it is a taste choice and a poor default for a system that also shows terminals and games.

| Token | Role | Calm Dusk | Light & Airy | Warm Paper |
|---|---|---|---|---|
| `desk` | Desktop base | `#141C2B` | `#CFE3F2` | `#D9CDB4` |
| `bar` | Menu bar, dock | `#1A2334` | `#F7F9FB` | `#F3ECDC` |
| `surface` | Window content | `#212C3F` | `#FBFCFD` | `#F8F2E6` |
| `raised` | Focused title, menus, buttons | `#2A374C` | `#FFFFFF` | `#FFFBF2` |
| `sunken` | Fields, sidebars, unfocused title | `#182132` | `#EDF2F6` | `#EADFCB` |
| `line` | Borders, separators | `#3A4860` | `#D3DDE6` | `#CBBDA3` |
| `text` | Primary text | `#E8EDF5` | `#18242E` | `#2A2419` |
| `muted` | Secondary text | `#A7B3C6` | `#526170` | `#5F5441` |
| `accent` | Focus, selection, primary | `#F0A840` | `#1570BD` | `#4E6B3A` |
| `on-accent` | Text on accent | `#2A1A04` | `#FFFFFF` | `#FFFBF2` |
| `accent-soft` | Tinted notices, setting tiles | `#4A3B28` | `#D6E9F8` | `#DDE3C8` |
| `ok` | Success | `#62C48F` | `#1A7F4D` | `#3A7341` |
| `warn` | Warning | `#F0C04A` | `#946000` | `#855C0C` |
| `bad` | Error, destructive | `#F0776C` | `#C33A31` | `#A23A2C` |
| `info` | Information | `#6CB6F0` | `#1570BD` | `#2F6480` |
| `focus` | Keyboard focus ring | `#FFD28A` | `#0B4F8A` | `#2C4220` |

Measured contrast is at least 4.5:1 (WCAG AA) in all three themes for 14 pairs: `text` on `surface`, `bar` and `raised`; `muted` on `surface`, `bar`, `raised` and `sunken`; `on-accent` on `accent`; `accent` on `bar` and `surface`; and `ok`, `warn`, `bad` and `info` on `surface`. The lowest is 4.87:1.

The boot screen is theme-independent (`#10151F`, ivory `#F4EFE2`, amber `#F0A840`) because the kernel draws it before any user setting can be read.

## 3. Foundations

### Type

Fonts are the M34 pre-rendered FFNT files: **Inter** (`sans-N`) for the interface and **JetBrains Mono** (`mono-N`) for terminals, paths, numbers in logs and shortcuts. `N` is the line height in pixels.

| Role | Font | Em px | Line px |
|---|---|---|---|
| Body, labels, menus, title bars | `sans-16` | 13 | 16 |
| Section heading | `sans-20` | 17 | 20 |
| Window or dialog title | `sans-24` | 20 | 24 |
| Display, boot wordmark | `sans-32` | 27 | 32 |
| Lock-screen clock | `sans-64` | 53 | 64 |
| Terminal, code | `mono-16` | 13 | 16 |

Only the regular weight exists, so **hierarchy comes from size and color, never bold**. Text is left-aligned, and numbers in columns are monospaced or have tabular alignment. Inter's digits are not guaranteed equal width in FFNT, so the Task Manager's numeric columns should use `mono-16` unless a tabular digit set is generated.

### Spacing, shape and depth

- **Grid:** 4 px base. Common steps are 4, 8, 12, 16 and 24.
- **Radii:** 4 px controls, 6 px windows and menus, 10 px dock and dialogs. Snapped and maximized windows have square corners.
- **Control heights:** 26 px buttons and fields, 24 px menu rows, 28 px menu bar, 32 px title bar.
- **Elevation:** three pre-rendered shadows (unfocused window, focused window or menu, dialog). Each is a 9-slice alpha mask tinted at load, so a window of any size costs only its edge strips.

### Icons

- Drawn on a 24-unit grid with a 1.75-unit round stroke, one color, no gradients.
- App icons are a colored rounded tile with a white symbol. App tile colors stay the same in every theme so that apps remain recognizable.
- Shipped pre-rendered at 16, 20, 24, 32, 48 and 64 px, and at each size again for the UI scales in section 10. Vector rendering at run time is not needed.
- All artwork is original. No third-party logos.

### The feather

One asymmetric vane with two barb notches, crossed by a curved shaft that extends below it as the quill. It works in one color at 16 px, where the shaft becomes a cut-out, and in two colors (ivory vane, amber shaft) at boot size. The wallpaper carries an enlarged, low-contrast feather behind layered ridges.

## 4. Boot and session screens

**Boot.** Limine hands off, and the kernel draws the screen as early as the framebuffer is available:

1. Dark background, the feather centered at 104 px at 1x. The shaft draws, then the vane fades in, then the notches (about 1 s in total).
2. `FeatherOS` in `sans-32`, a 220x3 px progress bar, one status line in `sans-16`: *Starting processors, Mounting the system disk, Starting services, Starting the desktop*.
3. The progress bar advances by boot **stage**, not by a fake timer.
4. The compositor takes over, crossfades in 240 ms, the menu bar slides in and the dock fades in.

**Recovery and debugging.** Esc shows the live kernel log under the logo. Any boot failure leaves the graphical screen and shows the normal console with the error, so a failed boot is never hidden behind a spinner. `boot=verbose` on the kernel command line skips the graphic.

**Lock and login.** The wallpaper at a darkened tint, the feather, a 64-line clock, date, and the password field. **Shutdown** syncs disks, shows "Shutting down" with the feather, then powers off. **Sleep** is listed but disabled until ACPI sleep exists.

## 5. Desktop shell

### Menu bar

28 px, full width, always on top. Opaque on the CPU tier.

- **Left:** Feather button, then the **focused app's name** in the accent color, then its menus (File, Edit, View, app-specific, Window, Help). With no focused window it shows *Desktop*.
- **Right, default order:** workspace switcher, CPU meter, network, sound, battery, notifications, date and time.
- Once one menu is open, moving across the bar switches menus. Left and Right do the same from the keyboard.
- Every right-side item is a **panel plugin**: a small process or in-compositor module with an icon, an optional text value and a popover. Users can reorder, remove and add them.
- When the bar is too narrow, app menus collapse first into one overflow menu, then low-priority items hide. Clock and notifications are last to go.

### Feather menu

Opened with the feather or `Super+Shift+Space`. A 600x430 panel that combines system actions with an app launcher.

- **Left column:** user, *About FeatherOS*, *System Settings*, *Task Manager*, *Customize Panels*; then *Lock Screen*, *Log Out*; then *Sleep*, *Restart*, *Shut Down*.
- **Right column:** search field (focused on open), category chips (Favorites, Recent, All, Accessories, Games, System, Settings), results.
- Typing searches apps **and individual settings panes**. Arrow keys move, Enter opens. Restart, Shut Down and Log Out ask for confirmation.

### Quill command center

Opened with `Super+Space`, the search item in the menu bar, or the Feather menu. A centered 700 px panel over a light scrim: input, grouped results, a detail pane with the available actions, and a key hint row. Below 640 px the detail pane is hidden.

- **Empty state:** running windows, suggested apps, quick actions and the five prefixes.
- **Plain text** searches windows, apps, commands, settings panes and files, with a *Top hit*. Arithmetic shows an answer first, and any text can be run in Terminal.
- **Prefixes:** `>` commands, `=` calculate (with hex and binary for non-negative integers), `$` run in Terminal, `/` files, `@` windows.
- **Commands** act on the focused window (snap, maximize, minimize, close, move to a workspace) and the system (workspaces, theme, rendering tier, window buttons, animations, panels, lock, sleep, log out, restart, shut down). Log Out, Restart and Shut Down still ask for confirmation.
- **Keys:** `↑↓` move, `Enter` runs, `Shift+Enter` runs the secondary action (Close Window on a window), `Tab` completes, `Esc` clears the text and then closes. Focus returns to where it was.
- **Motion and rendering:** 120 ms fade with a 6 px drop on the CPU tier, a small scale and blur only on the GPU tier. Only the panel's rectangle is damaged while typing.
- **Implementation note:** commands should come from one registry shared with menus and shortcuts, so a new action appears everywhere at once.

### Dock

- Default at the bottom center, 44 px icons. **Position** left, bottom or right; **size** 32-64 px; optional **auto-hide**.
- Pinned apps first, then a separator and running unpinned apps.
- Indicators: a dot means running, a wider accent bar means focused, a hollow dot means minimized.
- Click opens, focuses or minimizes. The context menu has Show, Keep in Dock, Quit and Dock Settings.
- Hover lifts an icon 2 px on the CPU tier. Magnification exists only on the GPU tier.

### Desktop, workspaces and customization

- Right-clicking the desktop opens an Xfce-style root menu. Desktop icons (Home, System, Trash) are optional.
- **Four workspaces by default, 1-9 configurable.** The switcher shows which contain windows.
- **Customize mode** outlines every panel item. Drag to reorder, × to remove, an add list for hidden items, Done to leave.

## 6. Windows

**Anatomy:** 1 px border, 32 px title bar, content. The title bar has two layouts, chosen in Settings > Appearance > Window buttons. **Left** (default): Close, Minimize, Maximize, centered title, then the window menu icon. **Right:** window menu icon, left-aligned title, then Minimize, Maximize, Close. Keyboard focus follows the visual order, and the change applies to open windows at once.

| State | Treatment |
|---|---|
| Focused | `raised` title, `text` title color, large shadow |
| Unfocused | `sunken` title, `muted` title color, small shadow |
| Snapped or maximized | Square corners, no gap to the work area |
| Close hover | `bad` fill |

**Behavior**

- Drag the title to move; double-click it to maximize. All edges and corners resize (minimum 240x160).
- **Snapping.** Dragging within 10 px of an edge previews the zone: left or right half, top to maximize, corners for quarters. Release to apply. Dragging a snapped window off restores its previous size under the pointer.
- Windows always keep at least 90 px reachable, so none can be lost off-screen.
- The work area excludes the menu bar and a non-hidden dock.
- Modal dialogs dim the parent or the screen and trap focus.

## 7. UI elements

Buttons (primary, default, quiet, destructive, disabled), text fields (normal, focused, error with a message), checkboxes, radios, switches, segmented controls, sliders, determinate and indeterminate progress, tabs, lists, menus, tooltips, dialogs, notifications and status badges. The prototype's **UI Elements** app shows each one in all themes.

Every interactive control has these states: normal, hover, pressed, focused (2 px `focus` ring), disabled. Selected rows use `accent` with `on-accent`. Status is never carried by color alone: text or an icon goes with it. Tooltips appear after 500 ms and never hold essential information. Notifications appear top right for 5 s, stay in the notification list and respect Do Not Disturb.

## 8. Motion

Motion explains where something came from, and it is short. A CPU-tier effect must touch few pixels or use cheap operations.

| Event | Duration | Easing | CPU tier | GPU tier |
|---|---|---|---|---|
| Hover, press | 80 ms | ease-out | Color change, 2 px lift | Same, dock magnify |
| Menu or popover open | 80 ms | ease-out | Fade + 3 px move | Same, with blur |
| Window open | 120 / 180 ms | ease-out | Fade + 6 px rise | Scale from 96% + fade |
| Window close | 80 ms | linear | Fade | Fade + slight scale |
| Minimize, restore | 140 / 220 ms | ease-in-out | **Outline flight:** a 2 px rectangle travels to the dock icon | The window itself scales into the icon |
| Snap preview | 80 ms | ease-out | Outline + flat tint | Same |
| Workspace switch | 120 / 200 ms | ease-out | Crossfade + 2% shift | Full slide |
| Notification | 140 ms in, 120 ms out | ease-out | Slide + fade | Same |
| Boot to desktop | 240 ms | linear | Crossfade | Same |

Easing is `cubic-bezier(.2,.7,.2,1)` unless noted. **Rules:** an animation never delays input; a new action cancels the running one and jumps to its end state. If a frame misses its budget twice in a row, the animation skips to its last frame. *Reduce motion* (setting, also forced off under a low frame rate) replaces all of it with immediate changes.

The outline flight is the key CPU-tier idea: it reads like a minimize animation while redrawing only four thin strips each frame.

## 9. System apps

### System Settings

Opens on a searchable **category grid**, then a pane with a back button:

- **Personal:** Appearance, Desktop, Dock, Menu Bar, Workspaces, Notifications
- **Hardware:** Display, Keyboard, Mouse, Sound, Power, Disks
- **Network:** Network, Sharing
- **System:** Users, Date & Time, Services, About

Panes are label-and-control rows with one column, at most 560 px wide. Changes apply at once; only destructive or risky ones ask (a display change would keep the old mode unless confirmed within 15 s). Services maps to `/etc/init.d/` and `/etc/init.disabled`, and Disks to the mount table.

### Task Manager

- **Processes:** sortable name, PID, user, CPU, memory, state; End Task with confirmation. App windows are named by app.
- **Performance:** per-CPU graphs, memory, disk and network with 40-sample history.
- **Services:** init services with state and Restart.

It needs kernel counters per process and per CPU, read through `/proc`. Graphs update at 1 Hz and only while visible, so the monitor does not become the load it is measuring.

## 10. Screens and scaling

- **Minimum: 1024x768**, the current guest resolution. All default windows fit it, the Feather menu fits, and the dock leaves a work area of 1024x678.
- Below 720 px wide (compact mode), windows open maximized, app menus collapse and the dock scrolls. The prototype shows this at phone width.

The existing font sizes already give the body text an exact size at each UI scale:

| Scale | 100% | 125% | 150% | 200% | 300% | 400% |
|---|---|---|---|---|---|---|
| Body font | `sans-16` | `sans-20` | `sans-24` | `sans-32` | `sans-48` | `sans-64` |
| Mono font | `mono-16` | `mono-20` | `mono-24` | `mono-32` | missing | missing |

Scale is per-display and applies to layout, icons, shadows and fonts together. Apps draw at the real pixel size, and nothing is bitmap-stretched. Larger roles do not always have an exact file: a 20-line heading needs `sans-25` at 125% and `sans-60` at 300%, and terminals need `mono-48` and `mono-64`. Those sizes are a font-generation task. Until then, a role uses the nearest smaller file. Scales other than these six are not offered.

## 11. Accessibility

- AA contrast in all themes, plus a future high-contrast theme.
- Complete keyboard paths and a visible focus ring.
- Reduce motion. Status never by color alone. Pointer targets are at least 24 px, and dock icons at least 32 px.
- Interface scale up to 400%. A screen reader is out of scope, but the toolkit should keep a role and label on every widget so that one can be added.

### Keyboard

| Shortcut | Action |
|---|---|
| `Super+Space` | Quill command center |
| `Super+Shift+Space` | Feather menu |
| `Super+Tab` | Switch window |
| `Super+←/→/↑/↓` | Snap left, right, maximize, restore or minimize |
| `Super+1..9` | Go to a workspace |
| `Super+Shift+1..9` | Move window to a workspace |
| `Super+M` / `Super+W` / `Super+Q` | Minimize / close window / quit app |
| `Super+D` | Show desktop |
| `Super+L` | Lock |
| `Super+Esc` | Task Manager |

Two choices avoid clashes with what the OS already does. **`Alt+F4` does not close windows**, because `Alt+F1` to `Alt+F4` switch virtual consoles in the kernel. The terminal's copy is `Ctrl+Shift+C`, because `Ctrl+C` must stay an interrupt there.

## 12. Rendering architecture and budget

### Model

A user-space **compositor** owns `/dev/fb0` on one virtual console, with a window surface per client. Each frame it:

1. collects damage from clients, moved windows, the cursor and animations;
2. walks the windows front to back, subtracting opaque areas, so hidden pixels are never drawn;
3. copies the visible parts to a back buffer, blending only shadows, rounded corners and fades;
4. presents each damaged rectangle.

The hardware cursor is unavailable on a plain framebuffer, so the cursor is a small software sprite with its own damage. It should bypass the full scene: restore the saved pixels, blit, present two small rectangles.

### Budget

These are **targets, to be measured on real hardware**. QEMU without acceleration will be slower and may run at 30 Hz.

| Item | Target at 1920x1080, 60 Hz |
|---|---|
| Frame budget | 16.6 ms |
| Compositor work, typical (cursor, typing, one small area) | under 2 ms |
| Window drag at full size | under 8 ms |
| Worst case, full-screen fade | under 14 ms, else skip to the end |
| Idle | no frames and about 0% CPU |

Rules: **no damage, no frame**. Copy opaque spans rather than blending. Pre-render shadows, gradients and the wallpaper at the screen size. Dither the wallpaper so its gradients do not band. Live blur is GPU-tier only. On the CPU tier the bar, dock and menus are opaque.

### Memory

A 32-bit surface takes about **7.9 MiB at 1920x1080** and **31.6 MiB at 3840x2160**. The current limit of 64 MiB per process cannot hold a 4K back buffer plus two full-size window surfaces. Window surfaces must live in shared memory outside that accounting, or the limit must rise.

### GPU tier

Later, through virtio-gpu and then real drivers. It adds live blur, scaled transitions and dock magnification. It uses the same scene description, and if GPU setup fails the system falls back to the CPU tier without a user-visible difference in function.

## 13. Mapping to what exists

| Existing piece | State today | Use in the GUI | Gap |
|---|---|---|---|
| `/dev/fb0` (`FB_ACQUIRE`, `FB_PRESENT`) | One owner per console, copies `0x00RRGGBB` rectangles | Compositor output | No vsync or page flip, so tearing is possible. `FB_PRESENT` is an extra full copy |
| `Graphics::Surface` | Opaque fill, blit, color-keyed blit, text | Back buffer, client drawing | **No alpha channel**, blending, 9-slice or clipping rectangle stack |
| `Graphics::DirtyRegion` | 8 rectangles, merges touching ones, whole screen past half area | Damage per output | Probably too coarse for many windows. Needs region subtract |
| `Graphics::Display` | Open, present, key events | Basis of the compositor's output | No pointer events. Single-program model |
| `Graphics::Font` and FFNT | Anti-aliased regular text in UTF-8 | All interface text | No bold, and no sizes for high scales. Glyph blending works on an opaque background, so text on alpha needs care |
| `/dev/mouse` (M14) | Relative `EV_REL` and `EV_KEY` records, evdev layout | Pointer | No `EV_ABS`, so QEMU's absolute tablet is not usable yet |
| Pseudo-terminals (M23) | 16 pairs | Terminal windows | VT100 code still lives in the kernel's `term.cpp`. The roadmap already plans moving it out |
| `init` services, `/proc` | Service files, process data | Settings, Task Manager | Per-process CPU and memory accounting must be checked |
| Embedded Swift and C++ | `FeatherGraphics` module | Apps and toolkit bindings | Toolkit language is an open decision |

## 14. OS prerequisites

Required before a usable desktop:

1. **USB HID on real hardware:** run the laptop checklist in `docs/hardware.md`, which has no runs yet. PS/2 pointer support is a useful fallback.
2. **Shared memory:** `mmap` with anonymous and shared mappings, or a dedicated surface object. FeatherOS has **no `mmap` today**, and the per-process page limit must allow it.
3. **IPC:** Unix-domain sockets and `sendmsg`/`recvmsg` with descriptor passing are all missing. Clients need them to connect to the compositor and hand over surfaces.
4. **A compositor input path:**
   - pointer and keyboard read together, multiplexed with `poll`;
   - **a `Super` modifier**, since `fb_event` only reports Shift, Control, Alt and Caps Lock;
   - **a raw mode for the owner of `/dev/fb0`**, since today `Ctrl+C` sends `SIGINT` to the console's foreground group even with `ISIG` off, so a Ctrl+C meant for an app would interrupt the compositor;
   - a larger queue than 64 events, since pointer motion would overflow it.
5. **Display timing:** a page flip or vblank signal where hardware allows it. A timer-paced frame loop works until then.
6. **Real-hardware display:** use the mode Limine and the firmware provide, and read EDID for size and DPI. Native mode-setting comes with GPU drivers.
7. **Power and hardware state:** ACPI for battery, sleep, the lid and the power button. Wi-Fi and sound are separate drivers.
8. **Per-process accounting** for the Task Manager, and users with permissions before the login screen means anything.

## 15. Open decisions

1. **Default theme.** Recommended: Dusk, with Airy as light mode.
2. **Compositor model.** Recommended: client-drawn shared-memory surfaces, like Wayland in concept, with our own small protocol. Server-side drawing is simpler at first but limits apps.
3. **Window decorations.** Recommended: drawn by the compositor, so every window is consistent and a hung app can still be moved and closed.
4. **Toolkit language.** C++ core with Swift bindings fits the codebase. Swift-only would be more distinctive, but Embedded Swift limits it.
5. **Quill's name and reach.** "Quill" is a working name. Decide later whether file search needs a persistent index and whether apps can register their own commands.
6. **Bold.** A second generated weight would improve hierarchy for about 1.5 MB more font data.
7. **Name and details of the feather mark,** once the direction is chosen.

## 16. Suggested phases

These are outlines, not assigned milestones.

| Phase | Result | Depends on |
|---|---|---|
| G0 Foundations | `mmap`/shared memory, Unix sockets with descriptor passing, input path changes | - |
| G1 Pixels | Alpha surfaces, 9-slice, clipping, regions, damage tests on the host | - |
| G2 Compositor | Full-screen scene, cursor, one test client, frame timing measured on real hardware | G0, G1 |
| G3 Windows | Move, resize, focus, decorations, snapping, workspaces | G2 |
| G4 Toolkit | Tokens, themes, all elements in section 7, a gallery app | G1 |
| G5 Shell | Menu bar, Feather menu, dock, notifications | G3, G4 |
| G6 Boot and session | Boot graphic, handover, lock screen, shutdown | G5 |
| G7 Core apps | Terminal, Files, Settings, Task Manager; Featherfall and Wickdeep in windows | G5 |
| G8 Polish | Motion, panel customization, HiDPI, accessibility | G7 |
| G9 GPU tier | virtio-gpu, blur and scaled transitions | G8, GPU work |

**First proof point (end of G2):** a cursor and two overlapping windows dragged smoothly on the real target machine. It validates the budget in section 12 before any toolkit work.
