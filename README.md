# smawm

A small floating window manager, merged from pieces of **dwm** and **sowm**:

- Client/tag model and the split bar come from dwm, trimmed to a single
  active tag instead of dwm's bitmask tags (no tiling means there's no need
  to view several tags at once), and to one screen instead of dwm's list of
  monitors.
- Mouse move/resize, the button-grab-on-root trick, and the overall shape
  of `smawm.c` come from sowm.
- The bar is drawn with Xft directly, keeping only the one thing `drw.c`
  really buys you — drawing into an off-screen pixmap and blitting it, so
  redraws don't flicker — and dropping the rest of the abstraction.
- The ICCCM/EWMH plumbing is dwm's: `WM_STATE`, synthetic `ConfigureNotify`,
  `_NET_SUPPORTING_WM_CHECK`/`_NET_SUPPORTED` and `UnmapNotify` handling.
  This is the part sowm leaves out entirely, and the part real toolkit
  applications (anything Qt or GTK) need in order to behave.
- `WM_NORMAL_HINTS` is read for minimum and maximum size only. dwm also
  reads base size, resize increments and aspect ratio, because there they
  decide whether a window may be tiled at all; smawm floats everything, so
  that question doesn't arise. See FEATURES.txt.

## Features

- 9 tags (`I`..`IX`), one visible at a time.
- Vacant tags are hidden from the bar; the currently selected tag always
  shows even when empty.
- Bar is split into two windows at the top of the screen: tags on the left,
  status text (from the root window's `WM_NAME`, i.e. `xsetroot -name "..."`
  or any status-bar script that does the same) on the right — the split
  follows dwm's `extrabar` patch as customized in `my-dwm-fork2`.
- Everything floats. No layouts, no master area, no gaps.
- `MODKEY`+drag with button 1 moves a window, button 3 resizes it (from the
  root, so it works no matter where on the window you grab).
- `MODKEY+Tab` returns to the tag you came from; pressing it again comes
  straight back, so it flips between the last two tags. dwm gets this from
  keeping two tag sets and flipping between them (`view` with argument 0);
  with a single selected tag it is one saved integer.
- `MODKEY+space` toggles maximize (fills the window area, bar and border
  stay visible); `MODKEY+f` toggles fullscreen (covers the whole screen, no
  border, raised above the bar). Fullscreen also responds to
  `_NET_WM_STATE_FULLSCREEN` client messages, so browsers/video players
  requesting fullscreen themselves work too.
- One screen, deliberately. smawm never asks Xinerama where the monitors
  are, so a dual-head setup behaves as a single wide screen — this is sowm's
  model, and it is why there is no `Monitor` type in the source. The cost is
  that fullscreen and maximize cover both outputs, and new windows centre on
  the seam. See FEATURES.txt.

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
(force a class to a tag), and keybindings. `config.h` was seeded
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
