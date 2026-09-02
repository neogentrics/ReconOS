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
