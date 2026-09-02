# ReconOS

A Wayland compositor and desktop shell, built from scratch in C on [wlroots](https://gitlab.freedesktop.org/wlroots/wlroots).

ReconOS is the first phase of a longer project: an operating system built up
from its own parts rather than assembled from someone else's. Phase one is the
part you actually see and touch — the compositor, the window management, the
shell. It runs on the Linux kernel today; replacing that substrate comes later,
once the layer above it is worth running.

**Status: v0.7 — pre-alpha.** It draws a desktop, hosts real application
windows, and quits when you tell it to. It is not yet a usable desktop.

## What works right now

- Wayland compositor on wlroots, driven by a scene graph
- **Real application windows** via `xdg-shell` — external clients connect and render
- Wallpaper rendering from disk (JPEG/PNG via `stb_image`)
- Mouse tracking with a click-target power button
- Keyboard input with compositor-level shortcuts
- Launches client applications from inside the session

## Controls

| Input | Action |
| --- | --- |
| `Alt` + `Enter` | Launch a terminal |
| `Alt` + `Q` | Quit the compositor |
| Click the red square | Power button — quits |

## Building

Requires a Linux system with wlroots 0.17 and its development headers.

```bash
sudo apt install -y build-essential cmake ninja-build pkg-config \
    libwlroots-dev libwayland-dev libxkbcommon-dev wayland-protocols
```

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

## Running

ReconOS needs direct access to the display, so run it from a bare TTY
(`Ctrl`+`Alt`+`F3`), not from inside an existing desktop session.

```bash
sudo WLR_RENDERER_ALLOW_SOFTWARE=1 XDG_RUNTIME_DIR=/tmp/recon_runtime ./build/ReconOS
```

`WLR_RENDERER_ALLOW_SOFTWARE=1` is required on hardware without a DRM render
node — virtual machines especially. On a machine with working GPU drivers you
can drop it.

To run without a display at all, useful for testing:

```bash
WLR_BACKENDS=headless WLR_RENDERER_ALLOW_SOFTWARE=1 ./build/ReconOS
```

### Configuration

| Variable | Purpose | Default |
| --- | --- | --- |
| `RECONOS_ASSETS` | Directory to load wallpaper and icons from | the `assets/` dir at build time |
| `RECONOS_TERMINAL` | Terminal launched by `Alt`+`Enter` | `weston-terminal` |

## Layout

```
src/          compositor source
include/      project headers
assets/       wallpaper and icons loaded at runtime
third_party/  vendored dependencies (stb_image)
docs/         roadmap and development notes
```

## Where this is going

See [docs/ROADMAP.md](docs/ROADMAP.md) for the full plan. The near-term target
is a bare-bones desktop — taskbar, start button, real window management — that
stays lightweight enough to run well on modest hardware.

## License

See [LICENSE.txt](LICENSE.txt).
