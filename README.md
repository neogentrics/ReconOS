# ReconOS

**An operating system built from its own parts, rather than assembled from
somebody else's.**

[![version](https://img.shields.io/badge/version-0.3.0_in_progress-1f6feb?style=flat-square)](https://github.com/neogentrics/ReconOS/releases)
[![release](https://img.shields.io/badge/latest_release-v0.2.17-238636?style=flat-square)](https://github.com/neogentrics/ReconOS/releases/tag/v0.2.17)
[![language](https://img.shields.io/badge/C11-555?style=flat-square)](#building)
[![tests](https://img.shields.io/badge/tests-9_suites-238636?style=flat-square)](#tests)
[![bugs](https://img.shields.io/badge/bugs_recorded-93-da3633?style=flat-square)](docs/BUGS.md)
[![licence](https://img.shields.io/badge/licence-CC0--1.0-555?style=flat-square)](LICENSE.txt)

---

## The two phases

ReconOS is being built in two halves, and **both are being worked on at the
same time.**

| | **Phase 1 — the desktop** | **Phase 2 — the kernel** |
| --- | --- | --- |
| What it is | The compositor, window management, the shell, the applications | Boot, memory, drivers, processes |
| Where | `src/`, `include/`, `modules/` | `kernel/` |
| Built against | wlroots and the Linux kernel | no libc, no wlroots, nothing |
| State | A usable desktop. v0.2.17 released | Runs its own memory, threads and clocks. v0.0.10 |

**The kernel today** boots on x86_64 and aarch64 — under legacy BIOS, under
UEFI, and via device tree, **from a bootloader we wrote**; GRUB left the boot
path at checkpoint 4. It reports which firmware is underneath it, reads what
the processor can actually do, manages physical memory on page tables it built
itself, allocates by the byte, reports a fault instead of resetting the
machine, keeps a monotonic clock and a wall clock read off the hardware, and
runs threads it can take execution away from.

It runs nothing of the desktop's yet, and will not until checkpoint 10. That
sentence stays in every time this paragraph is rewritten, because the one
before it is impressive enough to be misread.

**Phase 1 currently runs on Linux, and that is temporary.** wlroots and the
Linux kernel underneath are scaffolding, not architecture. Every place the
desktop calls them is a place that will be removed — not a second backend to
be kept working alongside the real one.

That is why the desktop keeps putting its own interface in front of borrowed
answers. `recon_display_*` asks what size the screen can be; `recon_volume_*`
asks what storage there is. Today those questions are answered by wlroots and
by three directories. When phase 2 can answer them, the file in between is
deleted rather than adapted, and nothing above it changes.

[docs/KERNEL-WANTS.md](docs/KERNEL-WANTS.md) is where the two halves meet:
every place the desktop currently works around not having its own kernel,
written down as it was hit rather than guessed at in advance.

---

## At a glance

| | |
| --- | --- |
| **Written in** | C11, no framework |
| **Draws** | its own windows, menus, icons and text — nothing is a toolkit widget |
| **Depends on** | wlroots (temporarily), stb for image and font decoding, mbedTLS for encryption in both directions |
| **Applications** | File Explorer, Notepad, Terminal, Watchtower, Mail, Photos, Calendar, Calculator, Control Panel, Help |
| **Skins** | ten, including three for colour vision and one for reading |
| **Accounts** | real ones, with roles — enforced by ReconOS inside ReconOS, and honestly labelled as such |
| **Tests** | 11 suites, no display needed |

Everything here works and is tested. What is *not* here is listed plainly —
in the Control Panel itself, page by page, and in
[docs/ROADMAP.md](docs/ROADMAP.md).

---

## Version history

Newest first. The number tracks what works, not what is planned.

| Version | What it brought |
| --- | --- |
| **0.3.0** *in progress* | TLS both ways — the port, and outgoing with the far end verified. Mail over IMAP and POP3. A clock, Photos, a Calendar. The Calculator gains five modes. Applets update on their own. A fixed-width terminal with colour schemes. Screen resolution. The kernel begins, alongside |
| **0.2.17** | The Control Panel becomes icons, one window per item. Appearance, Network and Programs split into sections. Storage becomes three spaces with a bin each, plus Disk Cleanup. Tooltips. Fonts and wallpapers installable from a right-click. Presets that cannot be deleted |
| **0.2.16** | Error codes with a screen and a log. A firewall. Remote access, two ways. A startup screen that checks rather than counts |
| **0.2.15** | Help and the change log, in the system. Skins writable from inside it. Storage. Typing in the Start menu |
| **0.2.14** | Packages — a manifest and a receipt, so removing takes back exactly what installing placed. Find |
| **0.2.13** | Removing an account can take its files. The menu footer becomes icons |
| **0.2.12** | A text clipboard, which the system did not have at all |
| **0.2.11** | Notepad can undo, and a menu to find it in |
| **0.2.10** | A skin can set the *shape* of a window frame, not only its colours |
| **0.2.9** | Desktop icons move and are remembered. Files open when you click them |
| **0.2.8** | Properties — and the explorer and the box agreeing about what a file is |
| **0.2.7** | Four desktops, the last thing the Multitasking page said was missing |
| **0.2.6** | Window snapping, and Alt+Tab — which had been quietly dead for months |
| **0.2.5** | The registry can be changed, not only read |
| **0.2.4** | Skins can be installed, which the file format had been waiting for |
| **0.2.3** | Gradients, a boot splash, and the unreadable labels that finding them revealed |
| **0.2.2** | All Programs. The control socket restricted to the account that owns it |
| **0.2.1** | Connections that carry data. Screen capture. Installing programs. Wallpapers |
| **0.2.0** | The network — seen and reached across, not implemented |
| **0.1.3** | Choose an account, then sign in |
| **0.1.2** | Setup that looks like it belongs to something. Accounts with faces |
| **0.1.1** | One account at a time, properly. The shape of the rest, in the Control Panel |
| **0.1.0** | The milestone defined as *a usable desktop* |

[docs/ROADMAP.md](docs/ROADMAP.md) has what each version did in full, and what
is known to be missing. [docs/BUGS.md](docs/BUGS.md) has every fault ever
found — what it actually was, how it surfaced, who found it, and what was done
about it. Every entry is also a
[GitHub issue](https://github.com/neogentrics/ReconOS/issues).

---

## Contents

- [What it looks like](#what-it-looks-like) — screenshots of a running system
- [What works right now](#what-works-right-now)
  - [Starting up, and signing in](#starting-up-and-signing-in)
  - [The desktop](#the-desktop)
  - [Files](#files)
  - [Settings, and how it looks](#settings-and-how-it-looks)
  - [Applications](#applications)
  - [The network](#the-network)
  - [Driving it, and finding out what went wrong](#driving-it-and-finding-out-what-went-wrong)
- [Controls](#controls) — every keyboard shortcut
- [Building](#building) · [Running](#running) · [Configuration](#configuration)
- [Bugs](#bugs) — how faults are recorded
- [Where this is going](#where-this-is-going)

---

## What it looks like

Every one of these is a real screen capture of a running ReconOS, taken by
ReconOS's own `capture` command with the system driven over its control
socket. Nothing here is a mock-up, and nothing is arranged by hand -- the
script that takes them is the same harness the tests use, so a picture that
stops being true stops being taken.

![The desktop, with the Start menu open](docs/images/desktop.png)

The desktop and the Start menu: pinned programs on the left, the places that
belong to the account on the right, All Programs with a fly-out beside it, and
a search box in the footer. The bar along the bottom carries one button per
window and four desktops at the right end.

![The Control Panel](docs/images/control-panel.png)

The Control Panel is fourteen icons. Clicking one opens it in a window of its
own, named for the item and stepped clear of whatever opened it, so two
settings can be worked on side by side. Hovering one says what it is for.

![Appearance](docs/images/appearance.png)

Ten skins ship, including three for colour vision and one for reading. A skin
sets forty-eight semantic roles, the shape of a window frame, and a wallpaper.
Any of them can be copied and changed; none of them can be deleted, because a
preset that can be deleted is gone with nowhere to get it back from.

![Two windows at once](docs/images/explorer.png)

ReconOS draws its own windows: the frames, the title bars, the buttons, the
menus. The File Explorer is looking at the root of the ReconOS filesystem,
which is a real directory tree on disk.

![The firewall](docs/images/firewall.png)

A rule list rather than a page about one -- consulted in order, first match
decides, and the ports most people would want are already written down and
switched off. The rules are a text file, and the same list is reachable from
the terminal.

![Network](docs/images/network.png)

Network in four sections. This is Adapters: every interface the host has, and
for the one picked, its address, netmask, state and kind.

![System Information](docs/images/system-information.png)

What the machine is, what ReconOS is, and what is underneath -- three groups,
kept apart on purpose, because the third is what explains how the first is
readable at all.

## What works right now

### Starting up, and signing in

**It announces itself.** A splash with the Recon Towers mark, two rings
turning around it at different speeds, and a line naming each part of the
system as it is counted — icons, skins, wallpapers, applications, accounts.
Those are real numbers rather than a bar pretending to drive work that has
already finished, so a zero means something is genuinely missing.
`RECONOS_NO_SPLASH` skips it.

**A startup screen that checks.** It used to count what had been brought up.
Now each line looks at the part of the system it names -- every folder the
system needs, the skins, the accounts, the programs, the firewall -- says what
it found, and raises a code when what it found is wrong. A missing folder is
rebuilt rather than only reported. A failure does not stop the start: a
machine that refuses to boot because one icon folder is unreadable is worse
than one that says so and carries on.

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

### The desktop

**A desktop** — wallpaper, icons, a taskbar listing every window, an apps menu,
right-click menus with hover feedback, and a Ctrl+Alt+Del box. Right-clicking a
window, a taskbar button, a desktop icon or the desktop itself all offer what
can be done there.

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

**Typing narrows it.** The menu took no keys at all before v0.2.15 -- not even
Escape -- so the only way to reach an application was to find its name with the
mouse. Now typing filters the list, the arrows move the highlight, Enter opens
it, and Escape steps back one thing at a time: what was typed, then the long
list, then the menu.

**Its own icons**, drawn by ReconOS at first run and written to
`/System/Icons` as `.ico` files. Nothing is bundled and nothing is borrowed,
and any icon can be replaced by dropping a different file over it — a replaced
one stays replaced.

**A desktop you can arrange.** Icons are a view of your Desktop folder, so
anything that writes a file there puts it on the desktop. Drag one and it
stays where you put it — the arrangement is remembered as grid positions, so
it survives a change of screen size rather than falling off the edge. Only
icons you moved are recorded; the grid fills in around them.

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

**Four desktops.** Alt+1 to Alt+4 switches between them, and the same numbers
sit on the taskbar as a pager so the shortcut and the button agree. A dot
under a number means that desktop has windows on it. Adding Shift takes the
current window along and follows it. A window on another desktop is off the
screen *and* off the taskbar — a bar listing every window everywhere would
make these a way of hiding windows rather than of arranging them.

### Files

**A filesystem** with one root it cannot see outside of: `/System`, `/Apps`,
`/Users`, `/Temp`. `/System` is protected, and a path that would climb out of
the root is refused rather than quietly clamped.

**A File Explorer** with an address bar you can type into and a drop-down of
everywhere worth going, a toolbar of icons in groups, and a sidebar that
separates your own folders from the machine's. It hides the system's own
bookkeeping from your home folder.

**Files open when you click them.** A text file opens in Notepad, from the
desktop or the explorer. Something with nothing to open it says so — "Nothing
here opens a Picture" — rather than being handed to Notepad, which would show
a window of binary and look like damage. Notepad refuses while it is holding
unsaved work, because a double-click somewhere else on the screen should not
be able to discard what you were writing.

**A recycle bin.** Deleting puts things there rather than destroying them, and
each item remembers where it came from so restoring puts it back — recreating
the folder it came from if that has gone since. It is the first icon on the
desktop and the last entry in the explorer's sidebar, and shows full and empty
differently. Shift+Delete skips it, after asking.

From the terminal it is the `bin` command: `bin` lists it, `bin <name>` fills
it, `bin restore` and `bin purge` act on one item, `bin empty` clears it. It
had been reachable only from the File Explorer, which meant nothing outside a
window could look at it, put anything back, or empty it -- and so nothing
could test that emptying it works. `del` is deliberately not this: it has
meant "gone" since DOS.

**A file dialog** shared by the applications that need one. It is drawn inside
the window that asked for it, so it cannot be dragged away from its own
question or left behind its own parent.

### Settings, and how it looks

**A Control Panel of fifteen icons.** Clicking one opens it in a window of
its own, named for the item and stepped clear of whatever opened it — so a
wallpaper and a set of colours can be worked on side by side. One window per
item and no more: two windows both editing the registry would be two views of
one file, each unaware the other had written to it.

| | | |
| --- | --- | --- |
| **Accounts** | who may sign in | ✅ |
| **Appearance** | skins, colours and wallpaper | ✅ |
| **Display Settings** | text size and spacing, fonts, screen resolution | ✅ |
| **Programs** | what is installed, and what ships | ✅ |
| **Modules** | code the system loads, and why anything refused | ✅ |
| **Network** | adapters, data used, which programs may connect | ✅ |
| **Firewall** | what may be opened, and to what | ✅ |
| **Storage** | three spaces, and where the room went | ✅ |
| **Disk Cleanup** | what can be freed, and what it costs | ✅ |
| **Registry** | what the system remembers between runs | ✅ |
| **System Information** | what the machine is, and what is underneath | ✅ |
| **Power** | sleep, hibernate, power modes | needs a kernel |
| **Update** | fetching and applying one | needs somewhere to fetch from |
| **Troubleshoot** | finding and fixing a fault | needs the parts it would fix |
| **Recovery** | going back to a working system | needs a kernel |

The four that are not built say so on the page, and say what has to exist
first. A gap nobody can see is a gap nobody remembers.

The Registry page asks for the administrator's password before showing what
the system remembers, locks itself again when you leave it, and can change
what it shows — the key of an existing setting is not editable, because a key
you can type over is a rename, and a rename here is a delete and an add that
look like one act. Saving redraws everything, since a setting here is one
something is already using: set `theme` by hand and the desktop restyles while
you watch.

The Control Panel is built into ReconOS rather than shipped as a module,
deliberately: this is where somebody goes to repair a system, and it should
not be a thing that can fail to load.

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

**Skins you can write here.** *Copy This Skin* on the Appearance page writes
the chosen skin out under a name of your own and opens it for editing; the
editor lists every role with a swatch of what it currently is, and a colour
changed there is on screen and in the file at the same moment. A copy rather
than a blank file, because the questions a skin has to answer are the part
nobody knows in advance. The ten that ship are compiled in and are refused
rather than written over -- a file cannot shadow a built-in, so the edit
would be saved, ignored, and lost on the next start.

**Gradients** are opt-in per role. A `.to` beside a colour makes that surface
ramp from one to the other, top to bottom:

```
title.active    = 2A5BC8
title.active.to = 1B429E
```

A skin that says nothing ramps nothing, which is why this is not a role of its
own — most surfaces do not want one.

**Frame shapes** are numbers rather than colours, and also optional:

```
metric.title-height = 30
metric.corner       = 7
metric.button-size  = 18
metric.border       = 3
```

They are clamped, because a skin is a text file somebody edits and a title bar
of 4000 pixels is not a look — it is a window you cannot close by pointing at
it. Beacon and Aqua round their top corners; Reading and Contrast get a taller
bar and bigger buttons and stay square, because somebody who turned on the
reading settings wants a bigger target rather than a softer edge. Beacon, Recon and Aqua ship with a few;
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

**Text is UTF-8.** Accents, dashes, quotation marks and other alphabets draw
the way they are written, in file names as well as in the help. Text used to
be walked a byte at a time with glyphs cached only for 32..126, so any
multi-byte character was three or four bytes each of which drew nothing --
not a box, not a question mark, nothing -- and the typeface had the glyphs
the whole time. A character the font has no drawing for now shows as an empty
box. A keyboard laid out for a language with accents in it types them: the
caret is still a byte offset, because that is what the text is, but the arrow
keys and Backspace step over whole characters by walking off the continuation
bytes.

**Dialogs.** Anything destructive asks in a window, with Cancel last so it is
what both Enter and Escape choose. Clicking outside does not dismiss one.

### Applications

**Applications**, all native:

- **File Explorer** — back and forward through where you have been, a sidebar
  of places, folders and files with icons, renaming in place, cut/copy/paste,
  and deletion that asks first
- **Task Manager** — processes with CPU and memory, and a view of open windows
- **Terminal** — the command interpreter, emulating nothing. Fixed-width, so
  the tables the interpreter writes line up, with failures in red and four
  colour schemes: Recon, which follows the skin, PowerShell, Green Screen and
  Paper
- **Notepad** — text editing, with a File menu and a file dialog for opening
  and saving. Undo by word, find and replace, word wrap, its own font and
  size, and a clipboard shared with the rest of the system. Closing with
  unsaved work asks where to put it rather than throwing it away
- **Mail** — IMAP and POP3, both over TLS with the server's certificate
  checked. Nothing is ever deleted on the server, and the password is not
  stored — it is asked for and forgotten when the window closes, and the
  window says so
- **Photos** — one picture at a time, fitted to the window and never enlarged
  past its own size, on a dark mat
- **Calendar** — a month at a time, with what is on each day kept as a text
  file anything can read
- **Calculator** — five modes: Standard, Scientific, Programmer, Date and
  Convert. Programmer works in whole numbers rather than doubles, because a
  bit pattern has to be exact in all sixty-four bits and a double is exact in
  fifty-three. Convert covers twelve families with exact factors — and not
  currency, which is a fact about this afternoon rather than a ratio
- **Help** — a page for each part of the system, and under a rule, the change
  log back to the first version

**A Task Manager** with three tabs. Applications is what is running and can
close it, politely or by force. Processes is the machine's. Users is who has an
account and what they are using, expandable to what each has open. Columns can
be dragged wider. Two buttons there do nothing yet and say so.

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

**Installing and removing programs.** A `.rex` is copied into `/Apps` and
loaded; removing unloads it and deletes the file. Both from the Control Panel
or from `install` and `uninstall`. Administrator-only: a module runs inside
ReconOS with everything ReconOS can do, so installing one is closer to
installing a driver than to saving a file.

**Modules** — code ReconOS loads at runtime, so a feature can arrive without
the core being rebuilt. `.rts` is a system module (a subsystem, driver or
service); `.rex` is an application, which appears in the Apps menu. The
extension names the ReconOS contract rather than the machine code inside it, so
a wrapped foreign binary can present the same interface later without the
format changing. The Calculator is the first thing to go through this path: it
is no longer in the core binary at all. See [docs/MODULES.md](docs/MODULES.md).

**Packages** are how a program arrives when it is more than code. A `.rpk` is
a folder with a manifest in it:

```
Notes.rpk/
    package.txt          name, version, publisher, module, icon
    Notes.rex            the code
    notes.png            an icon
```

A folder rather than an archive, because ReconOS cannot read one and writing
a container format buys nothing yet — a folder can be copied, inspected and
repaired with the tools the system already has.

Installing writes a **receipt** into `/System/Installed` naming every path it
placed, and removing deletes exactly those. The receipt is written before the
module is loaded, so a module that refuses to load leaves an install that can
be rolled back rather than files nothing knows about. `install` takes a
package folder or a bare module; `packages` lists what is installed.

Applications are built the first time somebody opens one. An application nobody
touches costs an entry in a list rather than a window's worth of pixels, which
is most of the argument for modules over compiling everything in. The built-in
applications register through the same call a module uses — an extension path
that only outsiders take is an extension path nobody keeps working.

**Client windows** via `xdg-shell`, for programs written against Wayland --
**framed by ReconOS**, not by the toolkit that built them. A client used to
arrive with a frame of its own in somebody else's colours; it now gets the
same title bar every other window has, reading the same skin metrics, and
drags, resizes, minimizes, maximizes and closes the same way. A client that
insists on drawing its own frame still may, and is not given a second one on
top of it.

Resizing is by a six-pixel margin *outside* the window rather than a border
inside it: inside belongs to the client, and a terminal's last column of text
is not a resize handle. The cursor changes over the margin, because an
invisible grab region nobody can see is one nobody finds.

`tests/decor_client.c` is the smallest Wayland client that uses
xdg-decoration, built alongside ReconOS. It exists because weston's demos do
not use the protocol at all -- so without it, nothing on hand could tell the
difference between this working and this not existing.

### The network

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

**Encrypted, when asked for.** A stream is opened plain or encrypted, and an
encrypted one has the far end's certificate checked against a bundle of
trusted roots and against the name that was asked for. There is no verify-off
switch: that is a switch that ends up on, and what it produces is an encrypted
connection to whoever answered, which is not the same thing as an encrypted
connection to who you meant.

The handshake runs a step at a time between turns of the event loop, because it
is several round trips to somebody else's network and doing it inline would
freeze the screen for as long as they took to answer. A stream does not report
itself open until the handshake finishes, so nothing can write a password into
a socket that has proved nothing.

A refused certificate is its own outcome and not "unreachable" — those mean
opposite things to whoever reads them, and it says which check failed, because
a wrong clock, a missing root and an interception are three different problems
and only one of them is frightening.

Deliberately not there yet: listening. Accepting connections means deciding
what may reach this machine, which is a bigger question than what this machine
may reach.

**A firewall.** It cannot filter packets -- ReconOS has no network stack of
its own -- and saying it could would be the same lie as a network page that
pretended to implement one. What it does is decide what ReconOS itself opens
and accepts, which is a real boundary: every outgoing connection asks first,
and the remote listener cannot open a port the firewall has not been told to
allow.

The shape most systems have, because that is the shape people already know: a
switch, a default per direction, and a numbered list where the first match
decides -- not the most specific, because reading top to bottom is the only
way a person can work out what it does. Nine rules ship, the incoming ones
written down and off. The rules are a text file in `/System/Config`, because
a firewall whose rules cannot be read outside the program that wrote them is
a firewall nobody can audit.

**Remote access, two ways.** SSH can forward the control socket, which needs
nothing from ReconOS at all. Or TCP 7420, off by default and gated by the
firewall, which speaks TLS with a certificate the machine makes for itself and
asks for a key over it. Only the key's hash is kept, stretched with PBKDF2;
the key is shown once when it is made.

There is no certificate authority, and no pretence of one. A machine that owns
itself has no upstream to ask for an identity, so it asserts its own and the
fingerprint is shown wherever remote access is turned on — the client pins it
on first connect, the way SSH does. A chain would answer "did somebody vouch
for this name", and nobody has vouched for this machine. Pinning answers "is
this the same machine as last time", which is the question being asked.

Going *out* is the opposite problem with the opposite answer, and it is built
that way. Connecting to somebody's mail server, "is this really
imap.example.com" is exactly the question a certificate authority exists for
and somebody has vouched for that name — so outgoing verifies a chain, where
incoming pins a fingerprint. Same file, two honest answers to two different
questions.

### Driving it, and finding out what went wrong

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

**Screen capture.** Print Screen, or `capture` from the terminal, writes a PNG
into the account's Pictures folder. It asks for the *next* frame rather than
grabbing the last one, because a compositor keeps no copy of what it drew.
ReconOS writes the PNG itself -- 8-bit RGB, no interlacing, no palette -- since
the alternative was a second image library for one direction of one format.

**F1 opens the help at the page about whatever is in front.** An application
declares which page is about it, and one with views of its own -- the Control
Panel -- moves that as it moves between them, so F1 from the Appearance page
opens the skins page rather than the front of the help.

**Help that stays true.** The pages and the log are one document each in
`docs/`, split into topics by a script and written into `/System/Help` every
time ReconOS starts. Prose about a feature lives beside the prose about every
other feature rather than in the file that implements it, which is the
arrangement under which it stays written — and rewriting it on every start
means help describing a version that is no longer running cannot survive an
update.

The first time an account reaches the desktop on a new version, a window says
what that version brought, with an OK button. The version is recorded against
that account, so each person is told once and nobody is told twice.

**Error codes.** Every fault ReconOS can report has a name of the form
`VT-A001` -- `VT` for Void Tower, a letter for the area of the system, a
number for the fault. The letter is the *area* rather than the severity,
because the same area produces faults of every kind and a code that changed
when a fault was reclassified would be a code nobody could look up. `I` and
`O` are not area letters: this is read off a screen and typed into a search
box, and there `I` is `1` and `O` is `0`.

Three severities, and they decide what happens rather than only how it
reads. A **STOP** draws a screen with the code on it, in fixed colours rather
than the skin's -- the one fault that screen has to survive is the one where
the colours are the problem. The record is written to disk *before* the screen
is drawn, because the screen is the part most likely to fail when what failed
is the drawing, so the next start can say what happened to the last one. A
crash cannot be drawn at all, so the signal handler writes the record with
`write(2)` and hands back to the default.

The list lives in `include/recon_errors.def`, once: the header builds an
enumeration from it, `errors` reads it, the screen quotes it, and
`scripts/make-errors.sh` writes [docs/ERRORS.md](docs/ERRORS.md) from it.

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
| `F1` | Help, at the page about whatever is in front |
| `Print Screen` | A picture of the screen |
| Any key on the stop screen | Shut down |

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
    libwlroots-dev libwayland-dev libxkbcommon-dev wayland-protocols \
    libmbedtls-dev
```

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

A clean build produces no warnings. That is worth keeping: a build with
fourteen warnings in it is a build where the fifteenth is invisible, which is
how Alt+Tab stayed dead for months here. Where a truncation is intended, say
so with `recon_text_copy` or `recon_text_printf` rather than leaving the
compiler to guess.

### Tests

The filesystem operations have their own tests, because renaming, copying and
deleting are what lose a user's work when they are wrong, and clicking through
a desktop is a poor way to find that out. They run against a throwaway root and
need no display:

```bash
ctest --test-dir build
```

Or one suite on its own:

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
boot/         reconboot — the UEFI bootloader, its own build (clang, PE)
kernel/       the kernel — phase 2, its own build
third_party/  vendored dependencies (stb)
docs/         roadmap, bug register, module interface, development notes
```

## Bugs

Eighty-one faults have been found in ReconOS so far. Every one of them has a
number -- `BG-001` upward, assigned in the order it was found and never
reused -- and an entry in [docs/BUGS.md](docs/BUGS.md) saying what was
actually wrong rather than what it looked like.

A commit says what changed. It does not say something was broken, that
somebody hit it, or that it is fixed now. The register does, and the
[issues](https://github.com/neogentrics/ReconOS/issues) carry the dates.
`python scripts/make-issues.py` keeps the two in step.

The interesting ones were found by using the system, not by reading it. Four
faults in the Help window in one sitting; a Calculator that took the whole
desktop down because a struct grew a field and the ABI number did not; every
context menu entry in the system silently doing nothing for weeks because
nothing could press a button without a person there to do it. That last one
is why ReconOS can drive its own input now.

## Where this is going

Both phases are being built at once now. The desktop keeps growing; the kernel
has begun underneath it. They meet when the kernel can hold a framebuffer and
a filesystem, and the first thing to cross over will be small — a recovery
screen, most likely, because it needs almost nothing but somewhere to draw.

[docs/ROADMAP.md](docs/ROADMAP.md) has the full plan.
[docs/KERNEL-WANTS.md](docs/KERNEL-WANTS.md) is the other half of it: every
place the desktop currently works around not having its own kernel, written
down as it was hit rather than guessed at in advance. That file is the list
the kernel is being built against.

## License

See [LICENSE.txt](LICENSE.txt).
