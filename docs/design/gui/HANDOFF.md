# FeatherOS desktop GUI prototype: handoff brief

Read this file first, then open `featheros-prototype.html`. The `cursors/` folder holds the pointer artwork as standalone files. Together they hold everything needed to continue the work: no earlier conversation is required.

## What this is

FeatherOS is a hobby operating system (x86-64 and AArch64, booted by Limine, ext4 root, framebuffer graphics, bitmap `.ffnt` fonts at 16/20/24/32 px). `featheros-prototype.html` is a single self-contained HTML/CSS/JS design prototype of its desktop. It is a design tool, not the OS: every value it shows is simulated.

Open it in any modern browser. The strip at the top ("FeatherOS GUI design prototype") is a studio toolbar outside the simulated OS. It has Theme, Mode, Compare themes, Rendering (CPU/GPU tier), Window buttons (left/right), Screen (fit or 1024×768) and Replay boot. Settings persist in `localStorage` under `fos-*` keys.

## How the file is organised

- One `<style>` block holds the design tokens and all component CSS. Themes are `[data-fos=<id>]` blocks of CSS custom properties on `#app`.
- One `<script>` IIFE holds all behaviour. Main parts, in file order:
  - icon paths `P`, `featherSvg`, `tile()`
  - `APPS` and `STATUS` registries, then state `S`
  - theme tables `V` (variants), `FAMS` (light/dark pairs), `ACC` (accents)
  - window manager: `openApp`, `focus`, `minimize`, `setSnap`, snap zones, workspaces
  - menus and popovers, menu bar, dock, toasts, dialogs
  - trash helpers, thumbnails, window switcher, workspace overview
  - apps: Files, Terminal, Notes, Calculator, game stubs, Settings, Task Manager, UI Elements gallery
  - Quill command center (`quillResults`, `openQuill`), Feather menu (`openFeather`)
  - boot (`boot`, `BOOTLOG`), lock/login (`lockScreen`), power actions
  - Compare themes page, theme functions (`applyTheme`, `setFamily`, `setMode`, `setAccent`), keyboard shortcuts, init
- `window.__fos` exposes `S`, `openApp`, `setSnap`, `setWs`, `boot` and `minimize` for testing.

## Shortcuts

Ctrl+Alt stands in for Super in the browser.

| Shortcut | Action |
|---|---|
| Super+Space | Quill command center (prefixes `>` commands, `=` calculate, `$` run, `/` files, `@` windows) |
| Super+Shift+Space | Feather menu |
| Super+Tab, or Super+\` | Window switcher; release the modifiers to switch |
| Super+O | Workspace overview |
| Super+L | Lock screen |
| Super+←/→/↑/↓ | Snap or maximize; ↓ restores or minimizes |
| Super+1…9, Super+Shift+1…9 | Go to a workspace; move the window to it |
| Super+M / Super+W / Super+D | Minimize / close / show desktop |

Windows reserves Ctrl+Alt+Tab, which is why Super+\` also works.

## Decisions already made (keep unless the owner says otherwise)

### Boot animation: final and approved, do not change
A feather falls in one swing and lands on its quill tip, with no bounce. The owner tried many versions and approved this exact one. It uses three nested elements, each on its own smooth curve, which is what removed an earlier stutter:

- `.bdrop`: horizontal, `bx`, 2.1 s. Keyframes: 0% −90px, 52% +46px, 100% 0. Ease-in-out on each segment.
- `.bfeather`: vertical, `by`, 2.1 s. Keyframes: 0% −150px, 27% −84px (bottom of the U), 50% −100px (small rise at the far end of the swing), 100% 0. The last segment uses `cubic-bezier(.5,0,.4,1)`.
- `.brot`: tilt around the centre (`transform-origin: 48% 52%`), `br`, 2.1 s. Keyframes: 0% 64deg, 52% 12deg, 100% 0.
- A shadow under the quill tip fades in from 55%. The wordmark fades in at 1.9 s, and the boot screen ends at 3.1 s.

Positive rotation is clockwise. `rotate(47deg)` makes the spine horizontal with the top pointing right. The resting pose (`transform: none`) is the logo, with the quill tip at bottom left.

Lesson learned: verify motion by rendering frames (for example with headless Chrome and Playwright, tracing the quill tip and the feather's centre) rather than reasoning about keyframes.

### Themes
Four families, each a dark/light pair, plus a Dark / Light / Auto mode. Auto is light from 07:00 to 19:00.

| Family | Dark | Light |
|---|---|---|
| Dusk and Airy | `dusk`: blue slate and amber; the recommended default | `airy`: off-white and sky blue |
| Paper and Ink | `ink`: warm charcoal, cream text, moss | `paper`: cream, ink and moss |
| Ember and Terracotta | `ember`: oxblood and coral | `terra`: cream and burnt orange |
| High contrast | `hcd`: black, white outlines, yellow accent | `hcl`: black on white |

In the high-contrast themes, windows, buttons and fields get 2 px borders, and the focused window gets an accent border.

There is an accent colour picker with eight options: theme default, blue, teal, green, amber, coral, pink and violet. Each colour has a light-theme and a dark-theme shade, so contrast holds. It is applied as inline custom properties on `#app`.

### App icon family (applied)
- **Shape:** every app tile is one shape, a rounded square with 23% radius.
- **Signature fold:** the lower-right half is about 16% darker, split along the bottom-left to top-right diagonal, with a faint light hairline. This echoes the feather's spine, and it is drawn in pure CSS on `.tile.app`.
- **Glyphs:** the same white, rounded-stroke glyphs as the UI icon set.
- **Colour by category:**
  - System: Files `#3d78b0`, Terminal `#34465c`, Tasks `#2f6f8f`, Trash `#4a5563`
  - Accessories: Notes `#b0832a`, Calculator `#9a7b4f`, Elements `#a86a3c`
  - Games: Featherfall `#6f55a8`, Wickdeep `#8f4a6e`
  - Settings: `#555d68`
- **Size ladder:** 16 px is glyph only (menus). Small tiles (Quill rows, Feather menu, switcher labels) drop the fold and use thicker strokes. The dock and other large tiles get the full design.
- **High contrast:** tiles become black-and-white outlines.
- **Planned pipeline (not built):** author SVGs on a 64 px grid, then pre-render them to a bitmap atlas (16 to 64 px, 1× and 2×) at build time. **Superseded on 2026-11-26:** FeatherOS draws icons, tiles and pointers live from outlines at any size (`docs/design/M37.md`).

### Pointers (cursors) and scrollbars (applied)
Files for these are in `cursors/`:
- `svg/` holds the source for each pointer, drawn on a 24×24 grid.
- `png/` holds each pointer rendered at 24, 32 and 48 px with a transparent background.
- `cursors.json` lists each pointer's hotspot (the click point) and the browser pointer it falls back to.
- The accent-coloured parts in the files use Dusk amber `#f0a840`. The prototype recolours them to the live accent colour.

**Pointer style:**
- Dark fill `#141a23` with a white outline about 1.5 px wide and rounded joins, readable on every theme.
- The arrow's right half is 14% lighter, split along the feather diagonal (the same signature as the app icons).
- Accent-coloured details appear only where they add meaning: the dot on Link, the spinner on Working and Busy, the centre on Move and Moving, the centre of Precise.

**The 13 pointers and their hotspots (at 24 px):**

| Pointer | Hotspot |
|---|---|
| Arrow (`default`), Link (`pointer`), Working (`progress`) | 3,2 |
| Text, Busy (`wait`), Move (`grab`), Moving (`grabbing`), the four resize pointers (`ns`, `ew`, `nwse`, `nesw`), Not allowed (`no`), Precise (`cross`) | 12,12 |

**Where each pointer is used:**
- **Resize:** window edges and corners.
- **Text:** text fields and the Terminal.
- **Move:** items in Customize Panels.
- **Moving:** dragging a file to the Trash, or a window in the overview.
- **Working:** the boot screen, and the first 0.5 s after an app opens. The spinner animates by swapping between 8 rotated frames every 90 ms.

**Pointer size:** Settings → Mouse → Pointer size: Small 24, Medium 32 or Large 48.

**How it's built:** pointers are built at runtime as SVG data URIs in `curSvg()`, `curValue()` and `applyCursors()`, and stored as `--cur-*` custom properties on `#app`. They apply only inside `#screen`, so the studio toolbar keeps normal browser pointers.

**Scrollbars:**
- At rest, a 12 px gutter with no track and a 6 px rounded thumb (`--text` at 30%).
- On hover, the thumb grows to 8 px and `--text` at 48%. While dragged, it turns the accent colour.
- High-contrast themes use a solid thumb on a `--sunken` track.
- Implemented with `::-webkit-scrollbar` (Chrome and Safari), with `scrollbar-width` and `scrollbar-color` as the fallback for Firefox.
- The UI Elements app shows all the pointers (hover a tile to try one) and a scrolling sample.

### Other features in place
- **Desktop:** top menu bar with customizable status items, and a dock (left, bottom or right; resizable; can hide automatically). Panels have an edit mode for reordering and removing items.
- **Windows:** snapping to halves and quarters, four workspaces (up to nine), and a GPU tier with blur and scale effects versus a plain CPU tier.
- **Trash can in the dock:** it shows a full icon when it holds items. You can drag files from Files onto it or onto the desktop Trash icon, use right-click "Move to Trash", or press Delete. The Trash folder offers Put Back and Empty Trash (with confirmation).
- **Window switcher:** live thumbnails made by cloning window elements, ordered most recent first.
- **Workspace overview:** a workspace strip with mini layouts and an Add button, and a grid of window thumbnails. Click a window to go to it, drag it onto a workspace to move it, or use × to close it.
- **Login and lock screen:** wallpaper, clock, avatar and password field (any non-empty password works; an empty one shakes the card and shows an error), a feather/guest user picker, and sleep, restart and shut-down buttons. Boot leads to login unless Settings → Users → Automatic login is on. Lock uses the same screen.

## Known issues
None open. Fixed in this copy of the prototype on 2026-11-25:
1. **Overview thumbnails overflowed vertically.** A thumbnail whose width was rounded up by half a pixel pushed the last window onto an extra row, and the top row slid under the workspace strip. `drawOverview()` now measures `.ovmain` after clearing the old grid, sizes thumbnails in whole pixels and caps each label to its thumbnail's width. On a screen too small for the minimum thumbnail size, the grid scrolls from the top.
2. **Unused keyframes.** `@keyframes bfall` and `@keyframes draw` are removed. The boot animation's rules are unchanged.
3. **Dead menu branch.** The workspaces menu (`wsmenu` in `statusPop`) now opens with a right-click on the workspace dots. A left click still opens the overview.

## Backlog (suggested, not started)
- Export the design tokens (colours, radii, spacing, timings) as a C header or JSON, so the real compositor shares one source of truth with the prototype.
- Render the prototype with the real bitmap font sizes (16/20/24/32). The mock currently uses Inter at 13 px.
- Test at 800×600 and at 200% scale.
- Write down the drawing operations the CPU tier needs (rounded rects, 1 px lines, one drop shadow) to keep the software renderer small.
- Design error states: an "app quit unexpectedly" dialog, a disk-full warning, and a boot-failure screen (the feather lands, then a message and the log).
- A first-run setup flow: choose a theme, then which side the window buttons go on, then create a user.
- Draw real icons for the whole app family. Settings pane icons still use generic tiles.

## Suggested first message in a new chat
> I'm attaching `HANDOFF.md`, `featheros-prototype.html` and the `cursors/` folder (pointer artwork), a design prototype for my hobby OS's desktop. Read the brief, then open the HTML. Keep everything listed under "Decisions already made", especially the boot animation. We'll pick from the backlog.
