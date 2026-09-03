# ReconOS

A Wayland compositor and desktop shell, built from scratch in C on [wlroots](https://gitlab.freedesktop.org/wlroots/wlroots).

ReconOS is the first phase of a longer project: an operating system built up
from its own parts rather than assembled from someone else's. Phase one is the
part you actually see and touch — the compositor, the window management, the
shell. It runs on the Linux kernel today; replacing that substrate comes later,
once the layer above it is worth running.

**Status: v0.1.0.** The version number tracks what works, not what is
planned, and this is the milestone that was defined as "a usable desktop": it
sets itself up on first run, asks who you are on every run after, and gives
that person a desktop of their own.

Still early. There is no kernel of its own, client windows draw their own
decorations, and the account roles are enforced by ReconOS inside ReconOS
rather than by anything underneath it. What is here works and is tested; what
is not here is listed at the end.

## What works right now

**It sets itself up.** A system with no accounts asks who is using it, offers a
look, offers the reading and colour-vision settings up front rather than buried
somewhere, and hands over a desktop. Every run after that asks which account
you are. Signing out returns to that screen; signing in as a *different* person
closes the previous one's windows, because the whole point of a login screen is
that one account does not see another's open documents.

**A Start menu** in two columns: applications on the left, places and the
Control Panel on the right, who is signed in across the top, and Sign Out /
Switch User / Restart / Shut Down along the bottom.

**A Control Panel** with four pages -- Accounts, Appearance, Reading, System.
Create and remove accounts, change passwords, promote and demote between
administrator and limited, choose a skin, adjust spacing and text size. Built
into ReconOS rather than shipped as a module, deliberately: this is where
somebody goes to repair a system, and it should not be a thing that can fail to
load.

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
the root is refused rather than quietly clamped.

**A recycle bin.** Deleting puts things there rather than destroying them, and
each item remembers where it came from so restoring puts it back — recreating
the folder it came from if that has gone since. It is the first icon on the
desktop and the last entry in the explorer's sidebar, and shows full and empty
differently. Shift+Delete skips it, after asking.

**Dialogs.** Anything destructive asks in a window, with Cancel last so it is
what both Enter and Escape choose. Clicking outside does not dismiss one.

**Skins.** Every colour is asked for by what it means rather than what it looks
like — `title.active`, not "dark blue" — so a skin is a set of answers and
swapping one restyles the whole system without a drawing call changing. Four
ship: **Recon** (the native look), **Classic** (the 95 era), **Aqua** (light),
**Midnight** (dark). They live in `/System/Themes` as text.

A skin file only says what it wants to change; everything else is inherited. A
file containing three lines restyles just the taskbar:

```
name = OnlyBar
bar = 802020
```

`theme` lists them, `theme <name>` puts one on and redraws everything, and
`theme roles` prints all 47 roles with what they currently resolve to — which
is what to start from when writing one. The choice is per-account and
remembered.

**Accessibility.** Five more skins, and they are verified rather than eyeballed:
**Deuteran**, **Protan** and **Tritan** for the three kinds of colour vision
deficiency — which need different answers, since protan and tritan want
opposite things — plus **Contrast** (black on white, nothing carried by hue)
and **Reading** (warm off-white, softened contrast). The semantic colours come
from the Okabe-Ito colour-universal set.

`tests/test_access.c` simulates each deficiency with the Vienot dichromat
projection in linear light and measures whether the pairs that must stay apart
actually do. It found three real defects in skins written by eye: two where an
active and an inactive title bar were a few units apart, so you could not tell
which window had the keyboard, and one where an accent and a warning were
nearly the same colour.

Separately, `access` adjusts how text is *read* rather than coloured: letter
spacing, line spacing, font and size, applied live and remembered.
`access reading` sets the spacing that suits a dyslexic reader. Extra letter
spacing is the best-supported single adjustment there; a typeface marketed for
dyslexia is deliberately not built in as a fix, because controlled studies have
not found it to beat an ordinary sans-serif — but the font is settable, so
anyone who finds one helps them can use it.

A palette is only half of accessibility. The other half is never encoding
meaning in colour alone, which is why "Not responding" says so in words and a
folder has an icon and a Type column as well as a colour.

**A registry** — what the system remembers between runs. Two hives, for the
reason Windows has two: `/System/Config/system.reg` belongs to the machine and
`/Users/<name>/user.reg` to one account, because a theme is a person's and a
module policy is not. Keys are paths (`apps/explorer/last-folder`,
`windows/Notepad/x`), values are text, and the typed accessors give the
fallback rather than zero when a value will not parse — a damaged file should
look like missing settings, not like a real setting of 0 that puts every window
in the corner.

Windows reopen where they were left, at the size they were, maximized if they
were — with the restore size kept separately, so unmaximizing gives back the
window rather than a screen-sized one. The file explorer opens where you left
it. Read and change any of it with `reg`.

**Modules** — code ReconOS loads at runtime, so a feature can arrive without
the core being rebuilt. `.rts` is a system module (a subsystem, driver or
service); `.rex` is an application, which appears in the Apps menu. The
extension names the ReconOS contract rather than the machine code inside it, so
a wrapped foreign binary can present the same interface later without the
format changing. The Calculator is the first thing to go through this path: it
is no longer in the core binary at all. See [docs/MODULES.md](docs/MODULES.md).

Applications are built the first time somebody opens one. An application nobody
touches costs an entry in a list rather than a window's worth of pixels, which
is most of the argument for modules over compiling everything in. The built-in
applications register through the same call a module uses — an extension path
that only outsiders take is an extension path nobody keeps working.

**An application table** — what is *running*, which is a different question
from what processes exist. Several built-in applications share ReconOS's
process, so no process list could show them separately, and a client program
can own several windows. The task manager works from this: End Task closes a
built-in, asks a client through its own window, and only offers to force one
that has been asked and has not answered.

Files can be **renamed, moved, copied and deleted**, from the file explorer, the
desktop, or the command line. Renaming happens in place — the row or the label
becomes a text box. Deleting asks first, and emptying a folder is a separate
question from deleting a file, so a tree is never lost to one stray click.
Cut, copy and paste share a single clipboard, so something copied in the
explorer pastes onto the desktop.

**A command interpreter** — ReconOS commands acting on ReconOS. Not a Unix
shell: nothing here reaches the host or runs host programs. Reachable from the
terminal window, and over a local socket so a running system can be examined
from outside.

**A way to drive the desktop from outside it**, over that same socket:

```
ui click 60 55          press where a person would
ui rclick 500 400       open a context menu
ui menu "New Folder"    choose an entry by name
ui hit 1006             press a registered region by id
ui answer Cancel        answer a dialog by button name
ui type Reports         type
state                   focus, windows, open menus and dialogs, with coordinates
ui app                  what the focused application believes about itself
```

This exists because a desktop cannot be tested by reasoning about it. Every
"the button does nothing" report needs the button pressed and the result
watched, and asking a person to click while somebody else reads the code is
not that. Input goes through the same entry points real input uses, so a pass
here means the thing a user touches works.

**Applications**, all native:

- **File Explorer** — back and forward through where you have been, a sidebar
  of places, folders and files with icons, renaming in place, cut/copy/paste,
  and deletion that asks first
- **Task Manager** — processes with CPU and memory, and a view of open windows
- **Terminal** — the command interpreter, emulating nothing
- **Notepad** — text editing, with a File menu and a file dialog for opening
  and saving. Closing with unsaved work asks where to put it rather than
  throwing it away
- **Calculator** — arithmetic by mouse or keyboard

**A file dialog** shared by the applications that need one. It is drawn inside
the window that asked for it, so it cannot be dragged away from its own
question or left behind its own parent.

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

Inside the file explorer:

| Input | Action |
| --- | --- |
| `F2` | Rename the selected item |
| `Delete` | Delete it (asks first) |
| `Ctrl` + `X` / `C` / `V` | Cut, copy, paste |
| `Left` / `Right` | Back and forward |
| `Backspace` | Up one folder |
| `F5` | Refresh |

Inside Notepad:

| Input | Action |
| --- | --- |
| `Ctrl` + `N` | New |
| `Ctrl` + `O` | Open |
| `Ctrl` + `S` | Save |
| `Ctrl` + `Shift` + `S` | Save As |

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

### Tests

The filesystem operations have their own tests, because renaming, copying and
deleting are what lose a user's work when they are wrong, and clicking through
a desktop is a poor way to find that out. They run against a throwaway root and
need no display:

```bash
./build/recon_fs_tests
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

### Installing it somewhere else

To take a build off the machine it was compiled on:

```bash
scripts/package.sh
```

That writes `dist/reconos-<version>-<arch>.tar.gz` containing the compositor,
its assets and an installer. On the target machine:

```bash
tar -xzf reconos-<version>-<arch>.tar.gz
cd reconos-<version>-<arch>
sudo ./install.sh
```

ReconOS lands in `/opt/reconos`, its filesystem at `/recon`, and a `reconos`
launcher on the path. Add `--boot-into` to start it on tty1 at boot instead of
a login prompt; `--uninstall` reverses everything except `/recon`, which is
left alone because it holds your files.

This installs onto a machine that already has a Linux kernel and wlroots. It is
not yet a bootable disk image — that comes with the kernel work, and calling a
tarball an "image" before then would be claiming something ReconOS cannot do.

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
modules/      applications and subsystems built as .rex / .rts
tests/        tests that need no display
scripts/      build, run, package and install
assets/       wallpaper and icons loaded at runtime
third_party/  vendored dependencies (stb_image)
docs/         roadmap, module interface, development notes
```

## Where this is going

See [docs/ROADMAP.md](docs/ROADMAP.md) for the full plan. The near-term target
is a bare-bones desktop — taskbar, start button, real window management — that
stays lightweight enough to run well on modest hardware.

## License

See [LICENSE.txt](LICENSE.txt).
