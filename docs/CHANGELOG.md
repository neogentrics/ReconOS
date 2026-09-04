# ReconOS change log

What changed in each version, newest first. The version number tracks what
works, not what is planned.

This file is the source. `scripts/make-help.sh` turns it into the pages the
Help application shows, so there is one place to write a change down and no
way for the two to disagree.

---

## v0.2.17

**Services.** The parts of ReconOS that run are a list now, in Watchtower
beside Applications and Processes: the desktop shell, the control socket,
remote access, the firewall and networking. Each one says whether it is
running, stopped, or failed and with which error code, and how many times it
has been started this run -- one means a normal system, more than one means
somebody has been repairing something. Start, Stop and Restart. `services` in
the Terminal is the same list and the same registry.

Multitasking used to be a Control Panel page describing behaviour nobody could
change. It is a service now, which is what it always was.

**The desktop shell can be restarted.** It rebuilds the taskbar, the desktop,
the menus and the window management, and leaves your application windows open
-- so a taskbar can be repaired without costing you the document you were
writing. Whoever is signed in stays signed in: restarting the shell is a
repair, not a sign-out.

Two faults found by doing that, both of which had been waiting:

The UI font belonged to the shell and was freed with it, while every surviving
window still held the pointer. The first frame after a restart was a crash
inside the glyph rasteriser. The font belongs to the system now, loaded once
per size for the whole run.

Registering a built-in application twice was refused as a name collision, so a
restarted desktop had no Notepad and no File Explorer while the old windows
were still on screen. A built-in re-registering itself is an update in place;
a module trying to take a built-in's name is still refused.

**Bugs have numbers.** Every fault found in ReconOS -- sixty-two of them so
far -- is written down in `docs/BUGS.md` with an ID (`BG-001` upward), what it
actually was, how it surfaced, who found it, and what was done about it. The
ones found before today were numbered retroactively from the commit history.
They are GitHub issues too, so the record is public and dated.

---

## v0.2.16

**Errors have codes now.** When something goes wrong ReconOS names it —
`VT-A001`, `VT-G005` — so it can be written down and looked up. `errors` in
the Terminal says what a code means, `errors log` is what has happened on this
machine, and `docs/ERRORS.md` is the whole list for when you have a code and
not a working machine.

Three kinds: **STOP** (the system cannot continue and shows a screen with the
code on it), **fault** (something failed and the rest carried on), and **note**
(written down, nothing broke). If a run stops, the next start says so once,
with the code.

**A firewall.** Control Panel → Firewall, and `firewall` in the Terminal. It
decides what ReconOS itself opens and accepts: outgoing allowed, incoming
blocked, and a list of rules where the first match decides. Nine rules ship —
the incoming ones written down and switched off, so opening one is a switch
rather than remembering a port number. It is not the host's firewall and does
not touch it.

**The startup screen checks the system** instead of only counting it. Every
folder the system needs, the skins, the accounts, the programs, the firewall —
each line says what it found, and says the code when what it found is wrong.
Missing folders are rebuilt rather than only reported.

**Remote access, two ways.** Over SSH by forwarding the control socket, which
is encrypted and needs nothing from ReconOS; or over TCP 7420 with a key, which
is off by default and says plainly that the key crosses the network in the
clear. The firewall has to allow it either way.

## v0.2.15

**Help.** A new application, beside the Control Panel in the Start menu, with
a page for each part of the system and this change log underneath. The text is
written out of the system's own files every time it starts, so help describing
a version that is no longer running cannot survive an update.

**What changed, after an update.** The first time an account reaches the
desktop on a new version, a window says what that version brought, with an OK
button. Each account is told once. The whole log is in Help at any time,
back to the first version.

**The Control Panel and the Start menu have their own icons**, instead of
sharing the plain window that stood in for everything without one.

**Storage says where the room went.** The page used to be four notes about
things that do not work. It now measures what ReconOS owns — the system, the
programs, each account by name, the scratch space and the Recycle Bin — and
can empty the bin. How much room is left is still the host's answer, because
there is one filesystem and no volume layer under it.

**The Recycle Bin from the Terminal.** `bin` lists it, `bin <name>` puts
something in, `bin restore <name>` takes it back out, and `bin empty` clears
it. Before this the bin could only be reached from the File Explorer.

**Text that is not plain English draws properly.** Accents, dashes,
quotation marks and other alphabets used to be dropped silently — a sentence
would arrive with a hole where its punctuation should be. They are read as
UTF-8 now, in file names as well as in the help. A character the font has no
drawing for shows as an empty box rather than as nothing. A keyboard laid out
for a language with accents in it can type them, and Backspace removes a whole
character rather than a piece of one.

**The Update page says what version this is and what it brought**, with a
way through to the whole change log. It used to be three notes about things
that do not work; one of them stopped being true when the change log arrived.

**Notepad can replace.** Ctrl+H opens the find bar with a second field:
*Replace* changes the match in front of you and moves to the next, *All*
changes every one. Both undo.

**Programs written for Wayland get a ReconOS title bar.** A program that
was not written for ReconOS used to arrive with a frame of its own, in
somebody else's colours, with its own buttons. Now it is framed the way every
other window is — same colours, same buttons, same corners — and can be
dragged, resized, minimized, maximized and closed like one. A program that
insists on drawing its own frame still may; it is not given a second one.

**F1 opens the help about whatever you are looking at.** From Notepad it opens
Writing; from a Control Panel page it opens the page about that page.

**Three quiet bugs, found by clearing the compiler's warnings.** The File
Explorer's folder drop-down could point at the wrong place for a deeply nested
folder; a wallpaper or account picture with a very long file name was listed
but could not be loaded; and a file with a very long name could be moved to
the wrong place on its way to the bin. The build now compiles with nothing to
say, so the next warning will be one somebody notices.

**Skins can be written here.** *Copy This Skin* on the Appearance page writes
the chosen one out under a name of your own; *Edit This Skin* lists every
colour with a swatch and lets you change it. The change is immediate and is
saved as you make it. The ten that ship are built in and cannot be edited —
copy one first. `theme copy <name>` does the same from the Terminal.

**Type in the Start menu to find something.** The list narrows to names
containing what you typed; the arrows move the highlight and Enter opens it.
Escape steps back one thing at a time. The menu took no keys at all before
this, not even Escape.

**Every button is the same shape.** The rounded corners a skin asks for were
written in three places and every button that had not been rewritten stayed
square. On Beacon that meant two shapes of button in one window.

## v0.2.14

**Programs arrive as packages.** A `.rpk` is a folder with a manifest in it,
so a program can bring an icon and other files rather than being one file of
code. Installing writes a receipt naming everything it placed, and removing
takes back exactly that. The receipt is written before the program is loaded,
so one that refuses to load can be rolled back instead of leaving files
nothing knows about.

**Find in Notepad.** Ctrl+F opens a bar above the status line. It ignores
case and wraps around the end of the document.

## v0.2.13

**Removing an account can keep or delete its files.** They are two decisions,
and only one of them can be undone.

**The Start menu's power buttons are icons**, in the bottom right. The name of
whichever one you point at appears on the left.

**Buttons have rounded corners** where the skin asks for them, and the startup
screen is slower, because it went past faster than it could be read.

## v0.2.12

**A text clipboard.** Cut, copy and paste work in Notepad, in every text field
in the system, and into the Terminal. Before this, text could not be moved
from one place to another at all.

**Notepad can select text** — with the mouse, or Shift and the arrow keys.

## v0.2.11

**Notepad can undo**, by word rather than by keystroke, and has an Edit menu
so the shortcuts are findable.

## v0.2.10

**Skins can change the shape of a window**, not only its colours: the height
of a title bar, the thickness of its border, how far its corners are rounded,
and how big its buttons are.

## v0.2.9

**Desktop icons can be dragged**, and stay where they are put.

**Files open when you click them.** A text file opens in Notepad. Before this,
nothing in the system opened a file by being clicked.

## v0.2.8

**Properties** tells you what something is: its name, kind, size, where it
lives and when it last changed. It had been in the menu and greyed out since
the menus existed.

## v0.2.7

**Four desktops.** Alt+1 to Alt+4 switches between them; the numbers on the
right of the taskbar do the same. Alt+Shift and a number takes the current
window with you.

## v0.2.6

**Windows snap to the edges.** Drag one to the left or right edge for that
half of the screen, or to the top to fill it.

**Alt+Tab works again.** It had quietly stopped working when ReconOS began
drawing its own windows.

## v0.2.5

**The registry can be changed from the Control Panel**, not only read. Saving
redraws the system, so a setting takes effect while you watch.

## v0.2.4

**Skins can be installed.** `theme install` takes a skin file and adds it to
the list; `theme remove` takes it away.

## v0.2.3

**Gradients.** A skin can ask for a surface to fade from one colour to
another, which is what the Beacon skin needed to look like the era it is
reaching for.

**A startup screen**, with the Recon Towers mark and a count of what the
system found as it came up.

## v0.2.2

**All Programs** at the foot of the Start menu lists everything installed. The
column above it is what this account opens most.

## v0.2.1

**Connections that carry data**, with a rule about which programs may open
one. **Screen capture** on Print Screen. **Installing and removing programs.**
**Wallpapers the system draws for itself.**

## v0.2.0

**The network, seen but not implemented.** ReconOS has no kernel, so it has no
network stack of its own; it reports the host's, and says so on screen.

## v0.1.3

**Choose an account, then sign in** — two screens rather than one, so nothing
about how an account signs in is shown before one is chosen.

**Updates announce themselves** on the first start after one.

## v0.1.2

**Setup that looks like it belongs to something**, with the Recon Towers mark
and a picture of each skin. **Account pictures.**

## v0.1.1

**One account at a time, properly.** Each account gets its own settings, its
own folders and its own windows.

**A Control Panel** with the shape of the whole system in it, including the
parts that are not built, each saying what has to exist first.

## v0.1.0

The first version that was a usable desktop: it sets itself up on first run,
asks who you are on every run after, and gives that person a desktop of their
own. A taskbar, a Start menu, windows, a file explorer, a terminal, a task
manager, Notepad and a calculator.
