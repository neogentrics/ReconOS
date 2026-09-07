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

### Checking the help against the system

The help is what somebody reads, so a sentence in it that is not true is worse
than a missing feature. Most of its claims are checkable from outside: a
shortcut either does the thing or it does not, a command either exists or it
does not, a menu either has five entries or it has four.

`scripts/look.sh` is how. Drive the desktop, capture, and read the answer —
`state` lists the Start menu's entries with their coordinates, `help` lists
every command, and a photograph settles anything about what is drawn.

It is worth doing after anything that changes behaviour somebody was told
about, and it has paid twice: once finding that F1 opened the wrong page for
five applications, and once finding a caret bug in the picture taken to prove a
different sentence true.

### The sanitizers

```bash
./scripts/check.sh
```

Builds every test suite with the address and undefined-behaviour sanitizers and
runs them. Silence is the result; anything printed is a real finding.

They catch what a passing test does not: reading one byte past an array,
freeing something twice, using memory after it was freed, shifting by more than
a word, signed overflow, a misaligned load. Every one of those is a bug that
passes on the machine it was written on and fails somewhere else, which is the
worst kind to own -- and every one of them is invisible to a test that only
checks the answer.

Kept out of the ordinary build because the sanitizers are two to three times
slower and change the memory layout, which is exactly why they find things and
exactly why they are not what you want while iterating.

Worth running before cutting a release, and after anything that touches
parsing: a decoder handed a malformed file is where these live.

All eighteen suites were clean the first time it was run, which is worth
knowing precisely because it means the next thing it says will be worth
believing.

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
