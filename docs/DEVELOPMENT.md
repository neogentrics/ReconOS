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

### The leak checker, on the compositor itself

`scripts/check.sh` covers the test suites. The compositor is not in them --
most of what it does needs a screen -- so it has to be checked separately:

```bash
cmake -S . -B build-leak -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_FLAGS="-fsanitize=address -fno-omit-frame-pointer -g" \
    -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address"
cmake --build build-leak --target ReconOS
```

Then run it headless with `ASAN_OPTIONS=detect_leaks=1:log_path=/tmp/leak`,
drive a session through `scripts/look.sh`, and **shut down with the `shutdown`
command** rather than killing it -- a process that is killed never runs its
teardown, and the report is then a list of everything, which is a list nobody
reads.

Most of what it reports belongs to wlroots and libwayland and is held to exit.
Grep the stacks for `src/` and `modules/` frames; those are ours.

**Do not measure this with RSS.** Thirty shell restarts leaking three panels
and a timer each showed *no* change in resident memory, because the allocator
does not return the pages. The obvious measurement says there is nothing wrong.


Fixed limits are fine and often necessary -- a reader that grows to fit
whatever it is handed is one a hostile file can exhaust. What is never fine is
reaching one quietly.

A page cut off looks exactly like a page that ended. A folder listing 512 of
600 files looks exactly like a folder of 512. Nobody scrolls to the bottom of a
document to check whether it finished, so the failure is invisible from the
outside and stays that way for as long as it takes somebody to count.

So: when a limit is reached, say so where the result is shown, and say it as a
problem rather than a note. Three subsystems had this at once -- the help, the
web viewer and File Explorer -- and in one of them the number that did not fit
was already known and was being thrown away on the line that knew it.

Where the thing is being *built* rather than displayed, refuse instead. A
package manifest with more entries than can be held is refused with the limit
named; installing most of a package and reporting success is how a missing file
turns up as something not working weeks later.

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

All eighteen suites there were at the time were clean the first time it was
run, which is worth knowing precisely because it means the next thing it says
will be worth believing. It has stayed clean since, through nineteen.

### What the tests actually run

```
./scripts/coverage.sh            what each file under test comes to
./scripts/coverage.sh --zero     the functions no suite runs
```

The fourth question for the tools, and it answers a different kind of doubt
from the other three. The warnings, the sanitizers and the analyzer look at
code; this looks at the *tests*, and says which parts of the code they have
never touched.

It earns its place because twice in one night a test passed while testing
nothing -- the MP4 fixture in `tests/test_malformed.c` was a header with no
sample table, so the walk it was written for never ran, and that was found by
deleting a bounds check on purpose rather than by reading anything. A number
saying "this function is at 0%" would have said it directly.

**What it found the first time it was run:** two files at zero.
`recon_firewall.c`, 249 lines of a security component with no suite at all --
compiled into the network target as a dependency and never called, which is the
worst shape a gap can have, because the file appears in a test target's source
list and looks tested from every angle except the one that counts. And
`recon_titlebar.c`, which holds the rule this project states most loudly and
had been checked by photographing one skin.

**The number is the most any single suite runs, not the union.** A file linked
into fourteen targets is compiled fourteen times, and gcov given all fourteen
`.gcda` files reports the sums -- `recon_fs.c` came out as "21% of 10920 lines"
when it has 780. Both columns were wrong and the first version of this script
printed them. Each suite is measured on its own now and the best figure kept,
which answers "is anything not run at all" exactly and deliberately does not
answer "how much of this is covered in total".

It is a measurement, not a target. Chasing a percentage produces tests that
execute code without asserting anything about it, which is worse than no test
because it looks like coverage.

### Malformed input

```
./build/recon_malformed_tests
```

The twentieth suite, and the only one that asks what a parser does with a file
that is *wrong*. Every other suite asks whether a decoder gets the right answer
from a good file. This one matters for anything a person can be sent: a picture
in an email, a page from a web server, a video off a stick.

Three ways of being wrong, all deterministic -- a finding that cannot be
reproduced has not been found:

- **truncated** at every length from nothing to the whole file
- **one byte changed** at every position, to `0x00`, to `0xFF`, and with its
  top bit flipped
- **random bytes behind a real header**, from a generator written into the file
  rather than the library's, so the same seed gives the same bytes everywhere

A truncated file has no right answer, so "wrong answer" is not the failure. The
failures are a read outside the buffer (which is why this earns most of its
keep under `check.sh`), a loop that does not end, and **an offset handed back
that points outside the input** -- which is the same bug one step earlier: the
decoder has not crashed, it has told its caller exactly where to.

**Break a check before believing this.** The first MP4 fixture here was a
header and an empty `moov`, and it tested nothing: deleting the bounds check in
`recon_mp4_sample` on purpose produced five thousand cases and no failures,
because no fixture ever produced a sample and the check was never reached.
Reading the test would not have shown that. With a real sample table the same
deletion is caught and named -- "MP4 with byte 276 set to 0xFF -- a sample
points outside the file".

### The static analyzer

```
./scripts/analyze.sh
```

A third question for the tools, and it finds a different class from the other
two. The warnings look at one statement. The sanitizers look at the paths a
test actually takes. This walks paths nothing has ever run: the branch where a
malloc fails, the third way into a function that clears a struct owning a
pointer.

What it found the first time it was run, none of which any test could have:

- **A memset over a struct that owns a pointer**, in Notepad's undo stack. It
  was correct, and correct for a reason two functions away that nothing stated
  at that line. Now it frees first, which costs nothing and stops the argument
  being needed.
- **An account slot the code was trusted to find**, on the strength of a count
  matching the number of used slots -- an invariant kept by hand in five
  places. It would have been a memset through NULL while creating an account.
- **Three test allocations that never checked their malloc.** A test binary
  that segfaults reports nothing at all: not a failure, not which check it died
  on, just a signal. The sanitized build allocates several times what the
  ordinary one does, and the sanitized build is the one run before a release.

It is noisier than the other two, because an analyzer that follows every path
also imagines some. The ones it imagines here are ownership handed to somebody
else -- a file descriptor given to the Wayland event loop looks like a leak,
since the loop closes it somewhere the analysis cannot see. Those two are
listed by file and line in the script rather than silenced with a flag, so the
list stays short enough to read and anything new in it is new.

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
