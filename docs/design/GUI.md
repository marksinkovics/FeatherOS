# FeatherOS GUI Design

**Status:** design, matched to the prototype of 2026-11-25. Nothing here is implemented. The implementation order is in [../plans/gui-plan.md](../plans/gui-plan.md) (milestones M35 to M46).

**Companions:**
- [gui/prototype.html](gui/prototype.html): the clickable prototype, with simulated data. Where this document and the prototype disagree, the prototype wins.
- [gui/HANDOFF.md](gui/HANDOFF.md): the prototype's brief, with the decisions already made.
- [gui/cursors/](gui/cursors/): the pointer artwork (SVG sources, PNG renders, hotspots).

**Scope.** The boot screen, login and lock, desktop, menu bar, dock, Feather menu, Quill, windows, UI elements, pointers, motion and the system apps, plus what the OS needs before they can exist.

## 1. Principles

1. **Quiet by default.** The work is the brightest thing on screen. The accent marks focus, selection and the primary action only.
2. **CPU first.** Everything core runs on a software compositor over a plain framebuffer. A GPU adds polish and never adds function.
3. **Familiar layout, own identity.** A top menu bar and a dock come from macOS. The searchable menu, settings grid, workspaces, tiling and configurable panels come from Xfce and Xubuntu. The feather, the diagonal fold on app icons and pointers, and the outline-flight animation are FeatherOS's own.
4. **Configurable without being fiddly.** Panels can be rearranged, and the defaults need no setup.
5. **Fully keyboard-operable.** Every surface works without a mouse, which also matters while USB input is being brought up on real hardware.
6. **Honest state.** Settings for hardware without a driver appear disabled with the reason, never as a working control.

## 2. Themes

Eight themes in four families share one layout and one token set. Each family is a dark and a light theme. **Calm Dusk is the default:** it continues the dark boot screen with no white flash, and its amber belongs to the feather.

| Family | Dark | Light |
|---|---|---|
| Dusk and Airy | `dusk`, Calm Dusk: blue slate and amber | `airy`, Light & Airy: off-white and sky blue |
| Paper and Ink | `ink`, Ink: warm charcoal, cream text, moss | `paper`, Warm Paper: cream, ink and moss |
| Ember and Terracotta | `ember`, Ember: oxblood and coral | `terra`, Terracotta: cream and burnt orange |
| High contrast | `hcd`: black, white outlines, yellow accent | `hcl`: black on white, blue accent |

- **Mode:** Dark, Light or Auto. Auto is light from 07:00 to 19:00.
- **High contrast:** windows, buttons and fields get 2 px borders, the focused window gets an accent border, and shadows are off.

| Token | Role | Dusk | Airy | Ink | Paper | Ember | Terracotta | Contrast Dark | Contrast Light |
|---|---|---|---|---|---|---|---|---|---|
| `desk` | Desktop base | `#141C2B` | `#CFE3F2` | `#24211B` | `#D9CDB4` | `#2B1714` | `#E8C9B5` | `#000000` | `#FFFFFF` |
| `bar` | Menu bar, dock | `#1A2334` | `#F7F9FB` | `#2B2720` | `#F3ECDC` | `#331C18` | `#FBF4EE` | `#000000` | `#FFFFFF` |
| `surface` | Window content | `#212C3F` | `#FBFCFD` | `#312C24` | `#F8F2E6` | `#3A211C` | `#FDF8F4` | `#000000` | `#FFFFFF` |
| `raised` | Focused title, menus, buttons | `#2A374C` | `#FFFFFF` | `#3A342B` | `#FFFBF2` | `#462823` | `#FFFFFF` | `#0A0A0A` | `#FFFFFF` |
| `sunken` | Fields, sidebars, unfocused title | `#182132` | `#EDF2F6` | `#2A261F` | `#EADFCB` | `#2F1A16` | `#F5E9E0` | `#000000` | `#F2F2F2` |
| `line` | Borders, separators | `#3A4860` | `#D3DDE6` | `#4D463A` | `#CBBDA3` | `#5C3A32` | `#E3CFC1` | `#FFFFFF` | `#000000` |
| `text` | Primary text | `#E8EDF5` | `#18242E` | `#EFE6D3` | `#2A2419` | `#F6E9E4` | `#2B1A12` | `#FFFFFF` | `#000000` |
| `muted` | Secondary text | `#A7B3C6` | `#526170` | `#B8AB93` | `#5F5441` | `#C9A89E` | `#6B5144` | `#E6E6E6` | `#262626` |
| `accent` | Focus, selection, primary | `#F0A840` | `#1570BD` | `#9DBB78` | `#4E6B3A` | `#FF7A4D` | `#B8482A` | `#FFD400` | `#0040C0` |
| `on-accent` | Text on accent | `#2A1A04` | `#FFFFFF` | `#1C2412` | `#FFFBF2` | `#2A0C04` | `#FFFFFF` | `#000000` | `#FFFFFF` |
| `accent-soft` | Tinted notices, setting tiles | `#4A3B28` | `#D6E9F8` | `#3D4630` | `#DDE3C8` | `#5A2F24` | `#F6DCCF` | `#3D3300` | `#D6E2FF` |
| `ok` | Success | `#62C48F` | `#1A7F4D` | `#8CC58F` | `#3A7341` | `#7CC98F` | `#2F7A45` | `#5CFF8A` | `#006B2D` |
| `warn` | Warning | `#F0C04A` | `#946000` | `#E0B45A` | `#855C0C` | `#F2C14E` | `#8A5A00` | `#FFD400` | `#7A4D00` |
| `bad` | Error, destructive | `#F0776C` | `#C33A31` | `#E58A7B` | `#A23A2C` | `#FF6B6B` | `#B3261E` | `#FF7070` | `#B00020` |
| `info` | Information | `#6CB6F0` | `#1570BD` | `#86B6CF` | `#2F6480` | `#7FB8E6` | `#2B6A9A` | `#7CC8FF` | `#0040C0` |
| `focus` | Keyboard focus ring | `#FFD28A` | `#0B4F8A` | `#C8DCA8` | `#2C4220` | `#FFB08F` | `#7A2A14` | `#00E5FF` | `#C00060` |

Each theme also has three ridge colours and a feather tint for the wallpaper, and a shadow colour. Their values are in the prototype's theme blocks.

**Accent colour.** A picker with eight options: the theme's own accent, blue, teal, green, amber, coral, pink and violet. Each colour has a shade for dark themes and one for light themes, with its own text colour. The choice replaces `accent`, `on-accent` and `focus`, and derives `accent-soft` by mixing the accent into `surface` (30% on dark themes, 18% on light).

| Accent | On dark themes | Text on it | On light themes | Text on it |
|---|---|---|---|---|
| Blue | `#6CB6F0` | `#06223A` | `#1565B0` | `#FFFFFF` |
| Teal | `#4FC4B0` | `#04261F` | `#0F7364` | `#FFFFFF` |
| Green | `#8CC66A` | `#142407` | `#3B7326` | `#FFFFFF` |
| Amber | `#F0A840` | `#2A1A04` | `#945800` | `#FFFFFF` |
| Coral | `#FF7A4D` | `#2A0C04` | `#B3452A` | `#FFFFFF` |
| Pink | `#F07AA8` | `#2E0716` | `#AD2F66` | `#FFFFFF` |
| Violet | `#A98CF0` | `#1A0F36` | `#6644C0` | `#FFFFFF` |

**Contrast,** measured on 2026-11-25 from the prototype's values: all eight themes pass 4.5:1 (WCAG AA) for 14 pairs: `text` on `surface`, `bar` and `raised`; `muted` on `surface`, `bar`, `raised` and `sunken`; `on-accent` on `accent`; `accent` on `bar` and `surface`; and `ok`, `warn`, `bad` and `info` on `surface`. The lowest is 4.82:1 (Terracotta, `accent` on `bar`). Every accent colour also passes on every theme for its three pairs.

The boot screen is theme-independent (`#10151F`, ivory `#F4EFE2`, amber `#F0A840`) because the kernel draws it before any user setting can be read.

## 3. Foundations

### Type

Fonts are the M34 pre-rendered FFNT files: **Inter** (`sans-N`) for the interface and **JetBrains Mono** (`mono-N`) for terminals, paths, numbers in logs and shortcuts. `N` is the line height in pixels, and the em is five sixths of it.

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
- **Elevation:** pre-rendered shadows for unfocused windows, focused windows, menus, the dock, notifications and dialogs. Each is a 9-slice alpha mask tinted with the theme's shadow colour, so a window of any size costs only its edge strips.

### Icons

- **Glyphs:** drawn on a 24-unit grid with a 1.75-unit round stroke, one color, no gradients. They take the color of the text around them.
- **App icons** are one family:
  - **Shape:** a rounded square with a 23% radius.
  - **Fold:** the lower-right half is about 16% darker, split along the bottom-left to top-right diagonal, with a faint light hairline. It echoes the feather's shaft.
  - **Glyph:** white, in the same stroke style as the UI glyphs.
  - **Colour by category,** the same in every theme so that apps stay recognizable:

    | Category | Apps |
    |---|---|
    | System | Files `#3D78B0`, Terminal `#34465C`, Task Manager `#2F6F8F`, Trash `#4A5563` |
    | Accessories | Notes `#B0832A`, Calculator `#9A7B4F`, UI Elements `#A86A3C` |
    | Games | Featherfall `#6F55A8`, Wickdeep `#8F4A6E` |
    | Settings | System Settings `#555D68` |

  - **Size ladder:** at 16 px only the glyph is shown (menus). Small tiles (Quill rows, the Feather menu, switcher labels) drop the fold and use thicker strokes. The dock and other large tiles get the full design.
  - **High contrast:** tiles become black-and-white outlines.
- Icons ship pre-rendered at the sizes the interface uses. Vector rendering at run time is not needed.
- All artwork is original. No third-party logos.
- **Open:** the Settings panes still use generic tiles; they need icons of their own.

### Pointers

Thirteen pointers, drawn on a 24x24 grid. The sources and hotspots are in [gui/cursors/](gui/cursors/).

- **Style:** dark fill `#141A23` with a white outline about 1.5 px wide and rounded joins, readable on every theme. The Arrow's right half is 14% lighter, split along the same diagonal as the app icons' fold.
- **Accent details** appear only where they add meaning, and follow the live accent colour: the dot on Link, the spinner on Working and Busy, the centre of Move, Moving and Precise.
- **Size:** Small 24, Medium 32 or Large 48 px (Settings > Mouse > Pointer size). Hotspots scale with the size.

| Pointer | Hotspot at 24 px | Used |
|---|---|---|
| Arrow | 3,2 | Everywhere else |
| Link | 3,2 | Links |
| Working | 3,2 | The boot screen, and the first 0.5 s after an app opens |
| Text | 12,12 | Text fields and the Terminal |
| Busy | 12,12 | Not used in the prototype yet |
| Move | 12,12 | Items in Customize Panels |
| Moving | 12,12 | Dragging a file to the Trash, or a window in the overview |
| Resize (4: vertical, horizontal, two diagonals) | 12,12 | Window edges and corners |
| Not allowed | 12,12 | Not used in the prototype yet |
| Precise | 12,12 | Not used in the prototype yet |

The spinner on Working and Busy turns by swapping 8 frames, one every 90 ms.

### The feather

One asymmetric vane with two barb notches, crossed by a curved shaft that extends below it as the quill. It works in one color at 16 px, where the shaft becomes a cut-out, and in two colors (ivory vane, amber shaft) at boot size. Its resting pose, with the quill tip at the bottom left, is the logo. The wallpaper carries an enlarged, low-contrast feather behind layered ridges.

## 4. Boot and session screens

### Boot

The animation is **final and approved. Do not change it.** A feather falls in one swing and lands on its quill tip, with no bounce.

- Dark background `#10151F`. The feather is 104 px at 1x.
- Three motions run together for 2.1 s, each on its own smooth curve. Each segment eases in and out (`cubic-bezier(.42,0,.58,1)`) unless noted:
  - **Horizontal:** from 90 px left, to 46 px right at 52%, to rest.
  - **Vertical:** from 150 px up, to 84 px up at 27% (the bottom of the swing), to 100 px up at 50% (a small rise at the far end), to rest. The last segment uses `cubic-bezier(.5,0,.4,1)`.
  - **Tilt** around the point 48%, 52% of the feather: from 64 degrees clockwise, to 12 degrees at 52%, to upright.
- The feather fades in over the first 0.35 s. A soft shadow under the quill tip fades in from 55% of the fall.
- `FeatherOS` in `sans-32` fades in at 1.9 s over 0.4 s. Below it are a 220x3 px progress bar and one status line in `sans-16`: *Starting processors, Mounting the system disk, Starting services, Starting the desktop*.
- **In the OS** the progress bar advances by real boot stage, not by a timer. The compositor then takes over with a 240 ms crossfade; the menu bar slides in and the dock fades in. In the prototype the whole screen lasts 3.1 s.

**Recovery and debugging.** Esc shows the live kernel log under the logo. Any boot failure leaves the graphical screen and shows the normal console with the error, so a failed boot is never hidden behind a spinner. `boot=verbose` on the kernel command line skips the graphic.

### Login and lock

One screen serves both. Boot leads to login unless *Automatic login* is on (Settings > Users). `Super+L` locks.

- The wallpaper, a 64-line clock with the date, and a card with the avatar, the name and role, a password field and a submit button.
- An empty or wrong password shakes the card and shows an error in the `bad` color.
- Sleep, Restart and Shut Down buttons sit at the bottom right. Restart and Shut Down ask first.
- **Prototype against implementation:** the prototype accepts any non-empty password and shows a user picker (feather and guest). The OS has no users yet, so the implementation has one user with one real password and hides the picker (plan D4).

**Shutdown** syncs disks, shows "Shutting down" with the feather, then powers off. **Sleep** is listed but disabled until ACPI sleep exists. **Log Out** closes open programs and returns to login.

## 5. Desktop shell

### Menu bar

28 px, full width, always on top. Opaque on the CPU tier.

- **Left:** Feather button, then the **focused app's name** in the accent color, then its menus (File, Edit, View, app-specific, Window, Help). With no focused window it shows *Desktop*.
- **Right, default order:** Quill search, workspace switcher, CPU meter, network, sound, battery, notifications, date and time.
  - The implementation plan proposes leaving battery and sound out of the default bar until drivers exist (plan D16).
  - The workspace switcher shows which workspaces contain windows. A click opens the overview; a right-click lists the workspaces with their window counts.
- Once one menu is open, moving across the bar switches menus. Left and Right do the same from the keyboard.
- Every right-side item is a **panel plugin** with an icon, an optional text value and a popover. Users can reorder, remove and add them.
- When the bar is too narrow, app menus collapse first, then low-priority items hide. Quill, notifications and the clock are last to go.

### Feather menu

Opened with the feather or `Super+Shift+Space`. A 600x430 panel that combines system actions with an app launcher.

- **Left column:** user, *About FeatherOS*, *Quill Command Center*, *System Settings*, *Task Manager*, *Customize Panels*; then *Lock Screen*, *Log Out*; then *Sleep*, *Restart*, *Shut Down*.
- **Right column:** search field (focused on open), category chips (Favorites, Recent, All, Accessories, Games, System, Settings), results.
- Typing searches apps **and individual settings panes**. Arrow keys move, Enter opens. Restart, Shut Down and Log Out ask for confirmation.

### Quill command center

Opened with `Super+Space`, the search item in the menu bar, or the Feather menu. A centered 700 px panel over a light scrim: input, grouped results, a detail pane with the available actions, and a key hint row. Below 640 px the detail pane is hidden.

- **Empty state:** running windows, suggested apps, quick actions and the five prefixes.
- **Plain text** searches windows, apps, commands, settings panes and files, with a *Top hit*. Arithmetic shows an answer first, and any text can be run in Terminal.
- **Prefixes:** `>` commands, `=` calculate (with hex and binary for non-negative integers), `$` run in Terminal, `/` files, `@` windows.
- **Commands** act on the focused window (snap, maximize, minimize, close, move to a workspace) and the system (workspaces, overview, switch window, theme family, mode, rendering tier, window buttons, animations, panels, empty trash, lock, sleep, log out, restart, shut down). Log Out, Restart and Shut Down still ask for confirmation.
- **Keys:** `↑↓` move, `Enter` runs, `Shift+Enter` runs the secondary action (Close Window on a window), `Tab` completes, `Esc` clears the text and then closes. Focus returns to where it was.
- **Motion and rendering:** 120 ms fade with a 6 px drop on the CPU tier, a small scale and blur only on the GPU tier. Only the panel's rectangle is damaged while typing.
- **Implementation note:** commands should come from one registry shared with menus and shortcuts, so a new action appears everywhere at once.

### Dock

- Default at the bottom center, 44 px icons. **Position** left, bottom or right; **size** 32-64 px; optional **auto-hide**.
- Pinned apps first, then a separator and running unpinned apps, then a separator and the **Trash**.
- Indicators: a dot means running, a wider accent bar means focused, a hollow dot means minimized.
- Click opens, focuses or minimizes. The context menu has Show, Keep in Dock, Quit and Dock Settings.
- Hover lifts an icon 2 px on the CPU tier. Magnification exists only on the GPU tier.

### Trash

- The dock's Trash shows a full icon when it holds items. Its context menu has Open and Empty Trash.
- A file reaches it by a drag from Files onto the dock's Trash or the desktop's Trash icon, by *Move to Trash* in the context menu, or by Delete.
- The Trash folder in Files offers *Put Back* (to where the item came from) and *Empty Trash*, which asks first.

### Desktop, workspaces and customization

- Right-clicking the desktop opens an Xfce-style root menu. Desktop icons (Home, System, Trash) are optional.
- **Four workspaces by default, 1-9 configurable.**
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

- Drag the title to move; double-click it to maximize. All edges and corners resize (minimum 240x160) and show the resize pointers.
- **Snapping.** Dragging within 10 px of an edge previews the zone: left or right half, top to maximize, corners for quarters. Release to apply. Dragging a snapped window off restores its previous size under the pointer.
- Windows always keep at least 90 px reachable, so none can be lost off-screen.
- The work area excludes the menu bar and a non-hidden dock.
- The window menu has Minimize, Maximize or Restore, the six snap positions, *Move to* each workspace, and Close.
- Modal dialogs dim the parent or the screen and trap focus.

**Window switcher.** `Super+Tab`, or ``Super+` ``, shows the current workspace's windows as live thumbnails, most recent first. Each further press steps on (Shift steps back); releasing the modifiers switches. Arrow keys, Enter and Esc also work. Minimized windows appear dimmed and are restored when chosen.

**Workspace overview.** `Super+O`, or a click on the workspace switcher.

- A strip at the top shows each workspace as a small layout of its windows, with an *Add* button up to nine workspaces. A click goes to that workspace.
- Below it, the current workspace's windows are a grid of thumbnails sized to fit. A click goes to a window, × closes it, and dragging it onto a workspace in the strip moves it there.
- Esc or a click on the background closes the overview.

## 7. UI elements

Buttons (primary, default, quiet, destructive, disabled), text fields (normal, focused, error with a message), checkboxes, radios, switches, segmented controls, sliders, determinate and indeterminate progress, tabs, lists, menus, tooltips, dialogs, notifications, status badges and scrollbars. The prototype's **UI Elements** app shows each one in all themes, with every pointer and a scrolling sample.

Every interactive control has these states: normal, hover, pressed, focused (2 px `focus` ring), disabled. Selected rows use `accent` with `on-accent`. Status is never carried by color alone: text or an icon goes with it. Tooltips appear after 500 ms and never hold essential information. Notifications appear top right for 5 s, stay in the notification list and respect Do Not Disturb.

**Scrollbars**

- At rest: a 12 px gutter with no track, and a 6 px rounded thumb in `text` at 30%.
- On hover the thumb grows to 8 px, in `text` at 48%. While dragged it takes the accent color.
- The thumb is never shorter than 32 px.
- High-contrast themes use a solid `text` thumb on a `sunken` track.

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
| Quill, dialog open | 120 ms | ease-out | Fade + 6 px move | Small scale, with blur |
| Window switcher open | 90 ms | ease-out | Fade + slight scale | Same, with blur |
| Overview open, close | 140 / 100 ms | linear | Fade | Same, with blur |
| Notification | 140 ms in, 120 ms out | ease-out | Slide + fade | Same |
| Lock, unlock | 200 ms | linear | Fade | Same |
| Dock auto-hide | 180 ms | ease-out | Slide | Same |
| Boot animation | 2.1 s | per section 4 | The falling feather | Same |
| Boot to desktop | 240 ms | linear | Crossfade | Same |

Easing is `cubic-bezier(.2,.7,.2,1)` unless noted. **Rules:** an animation never delays input; a new action cancels the running one and jumps to its end state. If a frame misses its budget twice in a row, the animation skips to its last frame. *Reduce motion* (the Animations setting, also forced off under a low frame rate) replaces all of it with immediate changes.

The outline flight is the key CPU-tier idea: it reads like a minimize animation while redrawing only four thin strips each frame.

## 9. System apps

### System Settings

Opens on a searchable **category grid**, then a pane with a back button:

- **Personal:** Appearance, Desktop, Dock, Menu Bar, Workspaces, Notifications
- **Hardware:** Display, Keyboard, Mouse, Sound, Power, Disks
- **Network:** Network, Sharing
- **System:** Users, Date & Time, Services, About

Panes are label-and-control rows with one column, at most 560 px wide. Changes apply at once; only destructive or risky ones ask (a display change would keep the old mode unless confirmed within 15 s). What each pane holds in the prototype:

| Pane | Rows |
|---|---|
| Appearance | Theme family, mode (Dark, Light, Auto), accent colour, rendering tier, animations, window buttons |
| Desktop | Desktop icons, wallpaper, right-click root menu |
| Dock | Position, icon size, hide automatically, customize |
| Menu Bar | A switch for each status item, customize |
| Workspaces | Number of workspaces, snapping, wrap at the last workspace |
| Notifications | Do Not Disturb, show on lock screen, banner time |
| Display | Resolution (read-only until a display driver exists), interface scale, night tint |
| Keyboard | Layout, repeat delay, repeat rate |
| Mouse | Pointer size, pointer speed, natural scrolling, swap buttons |
| Sound, Power | Shown with the reason they are unavailable |
| Disks | The partitions with their filesystem and mount state |
| Network | Ethernet state, DHCP, DNS server, host name |
| Sharing | Web server, remote shell (planned) |
| Users | The user, automatic login |
| Date & Time | Set automatically, time zone, 24-hour clock |
| Services | The init services, from `/etc/init.d/` and `/etc/init.disabled` |
| About | Architecture, processors, memory, root filesystem, display |

### Task Manager

- **Processes:** sortable name, PID, user, CPU, memory, state; End Task with confirmation. App windows are named by app.
- **Performance:** per-CPU graphs, memory, disk and network with 40-sample history.
- **Services:** init services with state and Restart.

Per-process CPU time, pages and state, and per-CPU busy time, already exist in `/proc`. Disk and network rates do not. Graphs update at about 1 Hz and only while visible, so the monitor does not become the load it is measuring.

### Other apps

- **Files:** places (Home, Root, System, Programs, Temporary, Trash), a path bar, an icon view, a context menu, and the Trash (section 5).
- **Terminal:** a shell on a pseudo-terminal, in `mono-16`. Copy is `Ctrl+Shift+C`.
- **Notes:** a plain-text editor. **Calculator:** a keypad calculator.
- **UI Elements:** the component gallery for this design.
- **Featherfall** and **Wickdeep:** the existing games, each in a window.

## 10. Screens and scaling

- **Minimum: 1024x768.** All default windows fit it, the Feather menu fits, and the dock leaves a work area of 1024x678. Today the kernel's screen is fixed at this size at compile time; the first milestone makes it follow the bootloader's mode.
- Below 720 px wide (compact mode), windows open maximized, app menus collapse and the dock scrolls. The prototype shows this at phone width.

The existing font sizes already give the body text an exact size at each UI scale:

| Scale | 100% | 125% | 150% | 200% | 300% | 400% |
|---|---|---|---|---|---|---|
| Body font | `sans-16` | `sans-20` | `sans-24` | `sans-32` | `sans-48` | `sans-64` |
| Mono font | `mono-16` | `mono-20` | `mono-24` | `mono-32` | missing | missing |

Scale is per-display and applies to layout, icons, pointers, shadows and fonts together. Apps draw at the real pixel size, and nothing is bitmap-stretched. Larger roles do not always have an exact file: a 20-line heading needs `sans-25` at 125% and `sans-60` at 300%, and terminals need `mono-48` and `mono-64`. Those sizes are a font-generation task. Until then, a role uses the nearest smaller file. Scales other than these six are not offered.

## 11. Accessibility

- AA contrast in all eight themes and every accent colour (section 2), and two high-contrast themes.
- Complete keyboard paths and a visible focus ring.
- Reduce motion. Status never by color alone. Pointer targets are at least 24 px, and dock icons at least 32 px.
- Three pointer sizes. Interface scale up to 400%.
- A screen reader is out of scope, but the toolkit should keep a role and label on every widget so that one can be added.

### Keyboard

| Shortcut | Action |
|---|---|
| `Super+Space` | Quill command center |
| `Super+Shift+Space` | Feather menu |
| `Super+Tab`, or ``Super+` `` | Window switcher; release the modifiers to switch |
| `Super+O` | Workspace overview |
| `Super+L` | Lock |
| `Super+←/→/↑/↓` | Snap left, right, maximize; down restores or minimizes |
| `Super+1..9` | Go to a workspace |
| `Super+Shift+1..9` | Move the window to a workspace |
| `Super+M` / `Super+W` / `Super+D` | Minimize / close window / show desktop |

- `Super+Q` (quit the app) is shown in the app menu but not wired in the prototype. `Super+Esc` for Task Manager comes from the earlier version of this document and is not in the prototype.
- In the browser, the prototype uses `Ctrl+Alt` in place of Super. ``Super+` `` exists because Windows reserves `Ctrl+Alt+Tab`.

Two choices avoid clashes with what the OS already does. **`Alt+F4` does not close windows**, because `Alt+F1` to `Alt+F4` switch virtual consoles in the kernel. The terminal's copy is `Ctrl+Shift+C`, because `Ctrl+C` must stay an interrupt there.

## 12. Rendering architecture and budget

### Model

A user-space **compositor** owns `/dev/fb0` on one virtual console, with a window surface per client. Each frame it:

1. collects damage from clients, moved windows, the pointer and animations;
2. walks the windows front to back, subtracting opaque areas, so hidden pixels are never drawn;
3. copies the visible parts to a back buffer, blending only shadows, rounded corners and fades;
4. presents each damaged rectangle.

The hardware cursor is unavailable on a plain framebuffer, so the pointer is a small software sprite with its own damage. It should bypass the full scene: restore the saved pixels, blit, present two small rectangles.

### Budget

These are **targets for real hardware, to be measured there**. QEMU without acceleration is much slower; its targets are set from measurements in the first milestones (plan D8 and D9).

| Item | Target at 1920x1080, 60 Hz |
|---|---|
| Frame budget | 16.6 ms |
| Compositor work, typical (pointer, typing, one small area) | under 2 ms |
| Window drag at full size | under 8 ms |
| Worst case, full-screen fade | under 14 ms, else skip to the end |
| Idle | no frames and about 0% CPU |

Two facts in today's kernel limit this. The timer ticks at 100 Hz, and sleeps, `poll` timeouts and the monotonic clock all move in 10 ms steps, so frames can be paced at 50 Hz but not at 60, and frame times cannot yet be measured. `FB_PRESENT` makes three passes over the pixels in the kernel (a copy from the program, a per-pixel conversion into the kernel's back buffer, a copy to the screen).

Rules: **no damage, no frame**. Copy opaque spans rather than blending. Pre-render shadows at load and the wallpaper at the screen size. The wallpaper is flat colour, so it needs no dithering. Live blur is GPU-tier only. On the CPU tier the bar, dock and menus are opaque.

### Memory

A 32-bit surface takes **3.0 MiB at 1024x768**, **7.9 MiB at 1920x1080** and **31.6 MiB at 3840x2160**. Each window keeps two buffers, so a maximized window costs about 5.3 MiB, 14.5 MiB and 60 MiB at those sizes. The current limit is 64 MiB per process. Window buffers are shared memory counted against the app that created them, not against the compositor, which keeps both smaller sizes within the limit. 4K waits until the limit is raised.

### GPU tier

Later, through virtio-gpu and then real drivers. It adds live blur, scaled transitions and dock magnification. It uses the same scene description, and if GPU setup fails the system falls back to the CPU tier without a user-visible difference in function.

## 13. Mapping to what exists

Checked against the code on 2026-11-25. The full list is in the plan's section 1.

| Existing piece | State today | Use in the GUI | Gap |
|---|---|---|---|
| `/dev/fb0` (`FB_ACQUIRE`, `FB_PRESENT`) | One owner per console, copies `0x00RRGGBB` rectangles | Compositor output | Fixed 1024x768. Three passes per present. No vsync or page flip, so tearing is possible. Acquiring blanks the screen |
| Key events on fb0 | Press, repeat, release with Shift, Control, Alt, Caps Lock; a queue of 64 | Compositor keys | **No Super modifier** (the PS/2 path drops the Super keys). `Ctrl+C` signals the owner's session |
| `/dev/mouse` (M14) | Relative `EV_REL` and `EV_KEY` records from USB mice, in its own 256-record ring | Pointer | No `EV_ABS`, so QEMU's absolute tablet is not usable. No PS/2 mouse or I2C touchpad |
| `Graphics::Surface` | Opaque fill, blit, color-keyed blit, text | Back buffer, client drawing | **No alpha channel**, blending, rounded shapes, 9-slice or clip stack |
| `Graphics::DirtyRegion` | 8 rectangles, merges touching ones, whole screen past half area | Simple programs | Too coarse for a compositor. Needs a region type with subtract |
| `Graphics::Display` | Open, present, key events | Full-screen programs | No pointer events. One program owns the screen |
| `Graphics::Font` and FFNT | Anti-aliased regular text in UTF-8 | All interface text | No bold, and no mono sizes for high scales. Glyphs blend over the pixel beneath, so text on alpha needs care |
| Timer | 100 Hz tick; 10 ms clock and timeouts | Frame pacing | No clock under 1 ms; no 60 Hz pacing |
| Memory | Heap growth only; 64 MiB per process | Window buffers | **No `mmap` or shared memory** |
| Sockets and pipes | IPv4 and IPv6 sockets, pipes | Client connections | **No Unix-domain sockets or descriptor passing** |
| Pseudo-terminals (M23) | 16 pairs | Terminal windows | The VT100 code lives in the kernel's `term.cpp`. The plan shares it with the Terminal app rather than moving it |
| `init` services, `/proc` | Service files; per-process CPU, memory and state; per-CPU busy time | Settings, Task Manager | No run-time service control. No disk or network rates |
| Users | None; file owners stored but not enforced | Login, lock | One real password; no permissions |
| Embedded Swift and C++ | `FeatherGraphics` module | Apps and toolkit bindings | The toolkit is C++; Swift reaches it through interop |

## 14. OS prerequisites

Required before a usable desktop. The plan schedules the first four as M35 and M36.

1. **A screen size taken from the bootloader** instead of the compile-time 1024x768.
2. **A compositor input path:**
   - a `Super` modifier on every keyboard path;
   - **a raw mode for the owner of `/dev/fb0`**, so that `Ctrl+C` meant for an app does not interrupt the compositor;
   - taking the screen without blanking it, for the handover from the boot picture;
   - absolute pointer events, for QEMU's tablet.
3. **Shared memory:** anonymous `mmap`, and a memory-object descriptor that two processes can map.
4. **IPC:** Unix-domain sockets, and `sendmsg`/`recvmsg` with descriptor passing. Clients need them to connect to the compositor and hand over surfaces.
5. **Display timing:** a clock finer than 1 ms now; a page flip or vblank signal where hardware allows it later. A timer-paced frame loop works until then.
6. **USB HID on real hardware:** run the laptop checklist in `docs/hardware.md`, which has no runs yet.
7. **Real-hardware display:** use the mode Limine and the firmware provide, and read EDID for size and DPI. Native mode-setting comes with GPU drivers.
8. **Power and hardware state:** ACPI for battery, sleep, the lid and the power button. Wi-Fi and sound are separate drivers.
9. **Users with permissions** before the login screen protects files; a password hash for the single-user login before that.

## 15. Decisions

**Decided:**

| Decision | Choice | Source |
|---|---|---|
| Default theme | Calm Dusk, with Light & Airy as its light mode | `HANDOFF.md` |
| Boot animation | The falling feather in section 4, final | `HANDOFF.md` |
| Window buttons | Left by default, right as a setting | Prototype |
| Compositor model | Client-drawn surfaces in shared memory, each app its own process from the start, kernel work first | Owner, 2026-11-25 |
| Toolkit language | C++ throughout; Swift through interop | Owner, 2026-11-25 |
| Screen target | Any resolution early; tests at 1024x768 and 1920x1080 | Owner, 2026-11-25 |
| Login | One user with one real password | Owner, 2026-11-25 |

**Recommended in the plan, to confirm:** window decorations drawn by the compositor, so every window is consistent and a hung app can still be moved and closed; the shell and the login screen as separate processes from the compositor; the rest of the plan's section 5.

**Open:**

1. **Quill's name and reach.** "Quill" is a working name. Decide later whether file search needs a persistent index and whether apps can register their own commands.
2. **Bold.** A second generated weight would improve hierarchy for about 1.5 MB more font data.
3. **Icons for the Settings panes.**
4. **Designs still missing:** an "app quit unexpectedly" dialog, a disk-full warning, a boot-failure screen, and a first-run setup flow.

## 16. Milestones

The earlier phases G0 to G9 are replaced by milestones in [../plans/gui-plan.md](../plans/gui-plan.md), which has the steps, tests and sizes.

| # | Milestone | Was phase |
|---|---|---|
| M35 | Screen and input foundations | G0 |
| M36 | Shared memory and local sockets | G0 |
| M37 | Pixels, design tokens and assets | G1 |
| M38 | Boot splash | G6 |
| M39 | Compositor and window protocol | G2 |
| M40 | Window management | G3 |
| M41 | Toolkit and UI Elements | G4 |
| M42 | Desktop shell | G5 |
| M43 | Session: login, lock, power | G6 |
| M44 | Core apps | G7 |
| M45 | Polish, scaling and real hardware | G8 |
| M46 | GPU tier | G9 |

**First proof point (end of M39):** a pointer and two overlapping windows from separate processes, dragged smoothly, with frame times measured.
