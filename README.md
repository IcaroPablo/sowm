# smawm

A small floating window manager, merged from pieces of **dwm** and **sowm**:

- Client/monitor/tag model and the split bar come from dwm, trimmed to a
  single active tag per monitor instead of dwm's bitmask tags (no tiling
  means there's no need to view several tags in one screen at once).
- Mouse move/resize, the button-grab-on-root trick, and the overall shape
  of `smawm.c` come from sowm.
- The bar is drawn with Xft directly, keeping only the one thing `drw.c`
  really buys you — drawing into an off-screen pixmap and blitting it, so
  redraws don't flicker — and dropping the rest of the abstraction.
- The ICCCM/EWMH plumbing is dwm's, unchanged: `WM_STATE`, synthetic
  `ConfigureNotify`, `_NET_SUPPORTING_WM_CHECK`/`_NET_SUPPORTED`,
  `UnmapNotify` handling and `WM_NORMAL_HINTS`. This is the part sowm
  leaves out, and the part real toolkit applications (anything Qt or GTK)
  need in order to behave.

## Features

- 9 tags per monitor (`I`..`IX`), one visible at a time.
- Vacant tags are hidden from the bar; the currently selected tag always
  shows even when empty.
- Bar is split into two windows at the top of each monitor: tags on the
  left, status text (from the root window's `WM_NAME`, i.e. `xsetroot -name
  "..."` or any status-bar script that does the same) on the right — only
  the focused monitor shows status text, matching dwm's `extrabar` patch as
  customized in `my-dwm-fork2`.
- Everything floats. No layouts, no master area, no gaps.
- `MODKEY`+drag with button 1 moves a window, button 3 resizes it (from the
  root, so it works no matter where on the window you grab).
- `MODKEY+space` toggles maximize (fills the monitor's window area, bar and
  border stay visible); `MODKEY+f` toggles fullscreen (covers the whole
  monitor, no border, raised above the bar). Fullscreen also responds to
  `_NET_WM_STATE_FULLSCREEN` client messages, so browsers/video players
  requesting fullscreen themselves work too.
- Xinerama multi-monitor support: independent tag set and bar per monitor,
  `MODKEY+Left/Right` to move focus between monitors, `MODKEY+Shift+Left/Right`
  to send the focused window to the next monitor.

## Build

```
make
sudo make install   # installs to /usr/local/bin
```

`config.h` is copied from `config.def.h` on first build (edit `config.h`
afterwards, `config.def.h` is just the template — same convention as dwm).

## Config

Everything lives in `config.h`: `MODKEY`, colors, the font (an Xft/fontconfig
pattern, e.g. `CozetteVector:pixelsize=13`; `monospace:size=10` is used
automatically if the configured one fails to load), tags, per-app rules
(force a class to a tag or monitor), and keybindings. `config.h` was seeded
from the trimmed `my-dwm-fork2/config.h` keybindings/app choices — a few
entries (`~/.scripts/sp`, `brave`, `ranger`, `slock`) assume tools from that
setup; adjust or delete what doesn't apply to your machine.

## Testing changes

Nested, without touching a live session:

```
Xephyr :7 -screen 1000x700 -ac &
DISPLAY=:7 ./smawm &
DISPLAY=:7 st &
```

Things that are easy to get wrong and worth exercising there: closing a
window that the application only *hides* rather than destroys (Qt's
`hide()`, e.g. an image preview) — the tag must stop showing as occupied and
the window must open again when the app asks; dragging a window that keeps
asking to be somewhere else; and a window that maps itself already
fullscreen.
