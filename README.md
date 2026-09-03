# ReconOS

A Wayland compositor and desktop shell, built from scratch in C on [wlroots](https://gitlab.freedesktop.org/wlroots/wlroots).

ReconOS is the first phase of a longer project: an operating system built up
from its own parts rather than assembled from someone else's. Phase one is the
part you actually see and touch — the compositor, the window management, the
shell. It runs on the Linux kernel today; replacing that substrate comes later,
once the layer above it is worth running.

**Status: v0.2.6.** The version number tracks what works, not what is
planned.

v0.1.0 was the milestone defined as "a usable desktop": it sets itself up on
first run, asks who you are on every run after, and gives that person a desktop
of their own. v0.1.1 made that last part true -- see *One account at a time*
below -- and put the shape of the rest of the system into the Control Panel,
most of it marked plainly as not built yet. v0.1.2 and v0.1.3 are the rounds
that came out of somebody actually using it: setup that looks like it belongs
to something, accounts with faces, a login that asks who you are before it
asks anything else, and a long list of things that were wrong once a person
clicked them.

v0.2.0 added the first networking: ReconOS can see the network and reach
across it. It does not implement one -- see *The network* below, which is
blunt about the difference. v0.2.1 added connections that carry data, the
rule about which programs may open one, screen capture, installing and
removing programs, and wallpapers the system draws for itself. v0.2.2 gave
the Start menu an All Programs list and restricted the control socket to the
account that owns it. v0.2.3 added gradients to the skin system, a boot
splash, and fixed the unreadable labels that finding the gradients turned up.
v0.2.4 made skins installable, which the file format had been waiting for
since the skin system was built. v0.2.5 made the registry changeable from the
Control Panel rather than only readable there. v0.2.6 added window snapping,
and fixed Alt+Tab, which had been quietly dead since ReconOS started drawing
its own windows.

[docs/ROADMAP.md](docs/ROADMAP.md) has the full plan, what each version did,
and the list of what is known to be missing.

Still early. There is no kernel of its own, client windows draw their own
decorations, and the account roles are enforced by ReconOS inside ReconOS
rather than by anything underneath it. What is here works and is tested; what
is not here is listed at the end.

## What works right now

**It announces itself.** A splash with the Recon Towers mark, two rings
turning around it at different speeds, and a line naming each part of the
system as it is counted — icons, skins, wallpapers, applications, accounts.
Those are real numbers rather than a bar pretending to drive work that has
already finished, so a zero means something is genuinely missing.
`RECONOS_NO_SPLASH` skips it.

**It sets itself up.** A system with no accounts walks through five short
screens: welcome, who you are, what the machine is called, whether anything
about reading or colour needs adjusting, and how it should look. The
accessibility list is only shown to somebody who says they want it -- asking
everybody to rule out six conditions before they can use their computer is the
wrong shape for a question most people answer "none of these" to. The skin
list shows a small picture of each skin, drawn from that skin's own palette.

Every run after that asks **which account** first -- a grid of faces with a
name and role under each, and nothing about how any of them signs in. Choosing
one goes to that account's own screen: their picture, their name, their role,
and a password box. It asks even when there is one account, because a single
account with its password box already open tells whoever is standing at the
machine how to get in before they have chosen anything. Signing out returns
to it.
Signing in as a *different* person ends the previous one's windows, because
the whole point of a login screen is that one account does not see another's
open documents. **Lock** covers the screen without ending the session, so the
account stays signed in with its windows open and coming back is the same
desktop.

**Every account has a face.** A set of pictures ReconOS draws for itself at
first run, written into `/System/Icons` like every other icon so a photograph
dropped over one replaces it. An account that has not chosen gets a coloured
disc with its initial, the colour worked out from the name so it is stable
without being stored anywhere.

**One account at a time, properly.** Each account gets its own settings
hive, its own folders, and its own windows. A limited account cannot read
another account's files, only administrators can; and signing in as a different
person ends the previous person's windows rather than handing them over. This
did not work in v0.1.0 -- the hive was written per account and read once at
boot, and application windows were built once and reused -- so the second
person to sign in got the first one's File Explorer, still in their folder.

**A Start menu** in two columns: applications on the left, places and the
Control Panel on the right, who is signed in across the top, and Lock / Sign
Out / Switch User / Restart / Shut Down along the bottom. Lock covers the
screen without ending the session: the account stays signed in, the windows
stay open, and coming back is the same desktop.

The left column is the six applications this account opens most, counted per
account since which programs somebody reaches for is a fact about them rather
than about the machine. **All Programs** at the foot of it lists everything
installed, alphabetically. The two are not the same list truncated: the column
answers what you use, All Programs answers what is here, and only one of those
is in an order you can search by name.

**A Control Panel** with thirteen pages. Seven do something: Accounts,
Appearance, Reading, Programs, Modules, Registry, About. Modules loads and
unloads code in the running system and says why anything refused to load.

The Registry page asks for the administrator's password before showing what
the system remembers, locks itself again when you leave it, and can change
what it shows — the key of an existing setting is not editable, because a key
you can type over is a rename, and a rename here is a delete and an add that
look like one act. Saving redraws everything, since a setting here is one
something is already using: set `theme` by hand and the desktop restyles
while you watch.

The other six -- Power, Storage, Multitasking, Update, Troubleshoot and
Recovery -- are there and say plainly that
they are not built, and what has to exist first. Usually a kernel: a hosted
process cannot suspend a machine, partition a disk, or reinstall itself. A
gap nobody can see is a gap nobody remembers.

The Control Panel is built into ReconOS rather than shipped as a module,
deliberately: this is where somebody goes to repair a system, and it should not
be a thing that can fail to load.

**A Task Manager** with three tabs. Applications is what is running and can
close it, politely or by force. Processes is the machine's. Users is who has an
account and what they are using, expandable to what each has open. Columns can
be dragged wider. Two buttons there do nothing yet and say so.

**A File Explorer** with an address bar you can type into and a drop-down of
everywhere worth going, a toolbar of icons in groups, and a sidebar that
separates your own folders from the machine's. It hides the system's own
bookkeeping from your home folder.

**The network — seen, not implemented.** ReconOS has no kernel, so it has no
ARP, no IP and no TCP. What it has is `recon_net`, which presents the host's
network in ReconOS's own terms: which interfaces exist, their addresses, the
gateway, the resolvers, and whether there is a way out. It can resolve a name
and ask whether a host answers, without freezing the desktop while a dead
host times out.

That is the same bargain `recon_fs` makes with the filesystem, and it buys the
same thing: one file knows Linux is underneath, and when ReconOS has a stack
of its own that file is what changes. The Control Panel's Network page and the
`net` command both say so on screen, because a page listing an IP address and
a gateway looks exactly like an operating system doing networking, and this
one is not.

**Connections that carry data**, with a rule about who may open one. Every
stream is opened in the name of an application, and one nobody has allowed
cannot open it; the default is no, because a system that never asks has
answered "all of them" without saying so. `net apps`, `net allow` and `net
deny` show and change it. This is not isolation and says so: ReconOS is one
process, so it binds what goes through ReconOS, the same way accounts are
bound.

Deliberately not there yet: listening, and TLS. Accepting connections means
deciding what may reach this machine, which is a bigger question than what
this machine may reach. Without TLS this is `http://` only.

**Screen capture.** Print Screen, or `capture` from the terminal, writes a PNG
into the account's Pictures folder. It asks for the *next* frame rather than
grabbing the last one, because a compositor keeps no copy of what it drew.
ReconOS writes the PNG itself -- 8-bit RGB, no interlacing, no palette -- since
the alternative was a second image library for one direction of one format.

**Installing and removing programs.** A `.rex` is copied into `/Apps` and
loaded; removing unloads it and deletes the file. Both from the Control Panel
or from `install` and `uninstall`. Administrator-only: a module runs inside
ReconOS with everything ReconOS can do, so installing one is closer to
installing a driver than to saving a file.

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

**Snapping and Alt+Tab.** Drag a window until the pointer touches the left or
right edge and it fills that half; the top fills the screen. The pointer
decides which edge, not the window — a window dragged by the middle of a wide
title bar has its own edge far from the screen's while your hand is against
it. A snapped window restores to where it was when the drag started, and the
restore button already undoes it, because snapping counts as maximized.

Alt+Tab steps through everything the taskbar lists, skipping what is
minimized.

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

A skin file only says what it wants to change; everything else is inherited
from **Recon**, the native skin — so a light skin should say what its desktop
labels look like, or it gets Recon's white-on-dark ones, which are meant for a
night sky. A file containing three lines restyles just the taskbar:

```
name = OnlyBar
bar = 802020
```

`theme` lists them, `theme <name>` puts one on and redraws everything,
`theme install <file>` adds one and `theme remove <name>` takes it away, and
`theme roles` prints all 48 roles with what they currently resolve to — which
is what to start from when writing one. The choice is per-account and
remembered.

**Gradients** are opt-in per role. A `.to` beside a colour makes that surface
ramp from one to the other, top to bottom:

```
title.active    = 2A5BC8
title.active.to = 1B429E
```

A skin that says nothing ramps nothing, which is why this is not a role of its
own — most surfaces do not want one. Beacon, Recon and Aqua ship with a few;
Classic does not, because the era it comes from filled flat, and Contrast
cannot, because a ramp behind text is a range of contrast ratios where that
skin promises a number.

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
| `RECONOS_CONTROL_SOCKET` | The control socket's path. Created readable and writable by its owner alone; refused if longer than 107 bytes, which is all a Unix socket address holds | `/tmp/reconos.sock` |
| `RECONOS_CURSOR_THEME` | Cursor theme | the system default |
| `RECONOS_NO_SPLASH` | Set to anything to start without the boot splash | unset |
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
