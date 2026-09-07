# Development Notes

## Test environments

ReconOS is developed against two targets. Both matter: the VM catches
portability problems, real hardware catches everything the VM papers over.

### Hyper-V VM (`ReconOS_Dev`)

Ubuntu Server 24.04, Generation 2 (UEFI). Server rather than desktop on
purpose — no competing display server fighting for the screen.

Graphics come from `hyperv_drm`, which is limited in a way worth knowing about:
it exposes a modesetting device (`/dev/dri/card1`) but **no render node**. There
is no GPU acceleration available, so the compositor must be told to accept
software rendering:

```bash
sudo WLR_RENDERER_ALLOW_SOFTWARE=1 XDG_RUNTIME_DIR=/tmp/recon_runtime ./build/ReconOS
```

Run it from the VM console (the Hyper-V window), not over SSH — the DRM backend
needs a real session that owns the display.

Two Hyper-V quirks to expect:

- The **Default Switch hands out a new IP after every host reboot**, so the VM's
  address drifts. Check with `ip addr show eth0`.
- **Clipboard → "Type clipboard text" corrupts input.** It sends keystrokes
  faster than the console accepts them, dropping and scrambling characters. Type
  by hand in that console, or work over SSH instead.

### Warnings

The build runs at `-Wall -Wextra`, and is clean. Keep it that way: the value of
a warning list is entirely in whether anybody reads it, and a list with twelve
known-harmless entries in it is a list nobody reads.

`-Wunused-parameter` is off on purpose. A callback's signature is fixed by
whoever calls it, so a handler that ignores an argument is the normal case and
not a smell.

Borrowed headers in `third_party/` are included as **system** directories, so
the compiler does not warn about them. That is not politeness -- `stb_truetype.h`
on its own produces about ninety "defined but not used" warnings, because a
header-only library is mostly functions any one program does not call, and the
six real dead functions in ReconOS's own code were invisible inside that.

Two warnings needed the code changed rather than explained, and both are the
same shape: `snprintf` with a source and a destination the compiler can see are
inside one object, even though they are different members or different rows.
`memcpy` and `memmove` say what is meant and are checkable; `snprintf` there was
only ever a habit.

### Building from a copy, and the trap in it

Working on one machine and building on another -- editing on Windows and
compiling in WSL, say -- means copying the tree across, and `rsync -a` is the
obvious way to do it. It has a trap that costs an afternoon exactly once.

`-a` implies `-t`, which preserves the *source's* modification time. So a file
that is edited, copied over, and then reverted arrives with an mtime OLDER than
the object file built from the version in between. `make` compares those two
times, decides the object is current, and skips it. The build succeeds, reports
nothing wrong, and produces a binary containing the change before last.

What it looks like from outside is a fix that did not take -- and then a second
attempt at the same fix, and a third, each one correct and each one measured
against a binary that does not contain it.

Copy without preserving times:

```bash
rsync -rlpgoD --checksum --no-times --delete --exclude build --exclude .git     /path/to/source/ ~/build-tree/
```

Files whose contents match are still skipped, which is the point of
`--checksum`; files that are copied get the current time, so make rebuilds
them.

### Headless testing

The compositor can run with no display at all, which makes automated testing
possible over SSH:

```bash
export XDG_RUNTIME_DIR=/tmp/recon_rt
mkdir -p "$XDG_RUNTIME_DIR" && chmod 700 "$XDG_RUNTIME_DIR"
WLR_BACKENDS=headless WLR_RENDERER_ALLOW_SOFTWARE=1 WLR_HEADLESS_OUTPUTS=1 \
    ./build/ReconOS
```

Then point a client at the socket it reports and confirm a window appears in the
log:

```bash
WAYLAND_DISPLAY=wayland-0 weston-terminal
```

A successful run logs `ReconOS: new toplevel window`.

## wlroots version notes

Built against **wlroots 0.17**. The API is explicitly unstable and moves between
releases; a few things that bit during development:

- `wlr_output_layout_create()` takes no arguments. Older code passed the display.
- `wlr_xcursor_manager_set_cursor_image()` is gone. Use
  `wlr_cursor_set_xcursor(cursor, manager, name)`.
- Scene buffers consume a `wlr_buffer`, not a `wlr_texture`. There is no
  `wlr_scene_buffer_set_texture()`. To render decoded image data, implement
  `wlr_buffer_impl` from `<wlr/interfaces/wlr_buffer.h>` and hand out the pixel
  pointer via `begin_data_ptr_access` — see `load_image()` in `src/main.c`.
- `wlr_buffer_init()` returns a buffer that is already referenced. After handing
  it to the scene graph, call `wlr_buffer_drop()` to release the initial
  reference.
- Creating `wlr_xdg_shell` is not sufficient for clients to work. Without
  `wlr_compositor_create()` there is no `wl_compositor` global, so clients cannot
  create surfaces and will **crash on connect**.

## Toolchain

CMake with the Ninja generator. `CMAKE_C_EXTENSIONS ON` is required — the system
headers wlroots pulls in do not compile under strict ISO C.

`xdg-shell-protocol.c/.h` are generated at build time by `wayland-scanner` from
the XML shipped in `wayland-protocols`. They are build artifacts and are not
committed.
