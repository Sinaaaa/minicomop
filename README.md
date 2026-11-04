# minicomop

A minimal X11 compositing manager derived from `xfwm4`'s `compositor.c`.

## Features

- **XRender Compositing:** Composites top-level windows using XRender with an overlay window as output.
- **Translucency:** Supports window translucency via `_NET_WM_WINDOW_OPACITY` and ARGB visuals.
- **Anti-Tearing Strategies:** Uses the same strategies and priority order as `xfwm4`:
  - `auto`: Uses **GLX** (swap interval + XSync fence + GL fence), unless the GL renderer is on `xfwm4`'s "prefer XPresent" list (e.g., Intel) or blacklist (software renderers); falls back to **XPresent** (2 buffers), and finally `off` if neither is available.
  - `glx`: Forces **GLX** only.
  - `xpresent`: Forces **XPresent** only.
  - `off`: Plain `XRenderComposite` direct to the screen.

## Non-Goals (What it does *not* do)

`minicomop` is strictly a minimal compositing manager meant to run alongside any window manager that lacks built-in compositing. It intentionally omits:

- Drop shadows
- Desktop zoom
- Window frame opacity
- Software cursor painting
- Unredirection of fullscreen windows
- Window manager integration

## Dependencies

### Arch Linux
```bash
sudo pacman -S libx11 libxext libxcomposite libxdamage libxfixes libxrender libxpresent libepoxy
```

## Build Instructions

Compile using `gcc` and `pkg-config`:

```bash
gcc -O2 -Wall -o minicomop minicomop.c \
    $(pkg-config --cflags --libs x11 xext xcomposite xdamage xfixes xrender xpresent epoxy)
```

### Build Flags
- Add `-DNO_GLX` to build without GLX / `libepoxy` support.
- Add `-DNO_PRESENT` to build without XPresent support.

## Usage

```bash
minicomop [-d display] [--vblank=auto|glx|xpresent|off]
```

## License & Acknowledgments

- **xcompmgr** — Copyright © 2003 Keith Packard
- **xfwm4** — Copyright © 2005–2011 Olivier Fourdan

Distributed under the terms of the **GNU General Public License (GPL)**, Version 2 or (at your option) any later version.
