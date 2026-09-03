# ReconOS

A Wayland compositor and desktop shell, built from scratch in C on [wlroots](https://gitlab.freedesktop.org/wlroots/wlroots).

ReconOS is the first phase of a longer project: an operating system built up
from its own parts rather than assembled from someone else's. Phase one is the
part you actually see and touch — the compositor, the window management, the
shell. It runs on the Linux kernel today; replacing that substrate comes later,
once the layer above it is worth running.

**Status: v0.0.7 — pre-alpha.** It draws a desktop with a taskbar and an
apps menu, hosts real application windows, and manages them. The version
number tracks what works, not what is planned: v0.1.0 is the first milestone
that counts as a usable desktop, and it is not reached yet.

## What works right now

**A desktop** — wallpaper, icons, a taskbar listing every window, an apps menu,
right-click menus with hover feedback, and a Ctrl+Alt+Del box. Right-clicking a
window, a taskbar button, a desktop icon or the desktop itself all offer what
can be done there.

**Its own icons**, drawn by ReconOS at first run and written to
`/System/Icons` as `.ico` files. Nothing is bundled and nothing is borrowed,
and any icon can be replaced by dropping a different file over it — a replaced
one stays replaced.

**Windows the system owns** — minimize, maximize, close, dragging and resizing
belong to the window framework rather than to each application, so a program
supplies its contents and nothing else.

**A filesystem** with one root it cannot see outside of: `/System`, `/Apps`,
`/Users`, `/Temp`. `/System` is protected, and a path that would climb out of
the root is refused.

**A command interpreter** — ReconOS commands acting on ReconOS. Not a Unix
shell: nothing here reaches the host or runs host programs. Reachable from the
terminal window, and over a local socket so a running system can be examined
from outside.

**Applications**, all native:

- **File Explorer** — back and forward through where you have been, a sidebar
  of places, folders and files with icons, deletion that asks first
- **Task Manager** — processes with CPU and memory, and a view of open windows
- **Terminal** — the command interpreter, emulating nothing
- **Notepad** — text editing
- **Calculator** — arithmetic by mouse or keyboard

**Client windows** via `xdg-shell`, for programs written against Wayland.

## Controls

| Input | Action |
| --- | --- |
| `Alt` + `Enter` | Terminal |
| `Alt` + `N` | Notepad |
| `Alt` + `T` | Task Manager |
| `Alt` + `Tab` | Cycle windows |
| `Alt` + `C` | Close the focused window |
| `Ctrl` + `Alt` + `Del` | Task Manager or shut down |
| `Alt` + `Q` | Quit |

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
| `RECONOS_ROOT` | Where the ReconOS filesystem lives | `/recon`, or `~/.reconos` if that is not writable |
| `RECONOS_ASSETS` | Where the wallpaper is loaded from | the `assets/` dir at build time |
| `RECONOS_CONTROL_SOCKET` | The control socket's path | `/tmp/reconos.sock` |
| `RECONOS_CURSOR_THEME` | Cursor theme | the system default |
| `RECONOS_FONT` | Font file | the first system font found |

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
