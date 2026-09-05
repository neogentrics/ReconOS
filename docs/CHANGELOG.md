# ReconOS change log

What changed in each version, newest first. The version number tracks what
works, not what is planned.

This file is the source. `scripts/make-help.sh` turns it into the pages the
Help application shows, so there is one place to write a change down and no
way for the two to disagree.

---

## v0.3.0 — in progress

Nothing yet. The number moves because v0.2.17 shipped and because phase 2 has
started: the kernel boots on x86_64 and aarch64, in `kernel/`, built by its own
Makefile against no libc and no wlroots. Nothing in the desktop runs on it
yet, and this file will say so plainly when something does.

---

## v0.2.17

**An application can be turned off.** Programs → System Apps → Disable. It
stays registered and listed there, marked, and is offered nowhere else: not in
the menus, and not openable by any route. The list of turned-off names lives
in the system registry, so it survives a restart. This is distinct from
removed on purpose -- a built-in cannot be removed, it is compiled in, and "I
do not want this and cannot delete it" is a real thing to want.

**Repair says what it found.** For a program installed from a package, the
receipt names every file the install placed and each is checked. For one that
arrived as a bare `.rex` in /Apps, whether that module is still there and
loaded is the whole question. Neither puts a file back: that needs the package
it came from, and nothing keeps one, which is worth saying rather than having
a button that quietly does nothing.

**Install a Program opens the File Explorer.** It was a path to type. The same
right-click that adds a picture or a font now installs a program, on `.rex`,
`.rts` and `.rpk` -- three ways of saying "take this file into the system",
said one way.

**Storage and Disk Cleanup line up.** Figures are drawn from their right-hand
edge, because sizes are read against each other and that comparison is made on
the digits. A share bar's track is a groove rather than an empty text field,
and a category holding nothing draws no track at all. The volume selector is
the same tab bar Appearance and Network use -- these were buttons, and
choosing which of three spaces you are looking at is choosing a view, not
acting.

**One click highlights, everywhere.** Three pages forced a selection into
existence every frame, so they opened with the first row lit and the buttons
that act on it already armed. Switching lists, switching hives and removing a
row all reset to row zero, which is a real row -- so a Remove button came back
armed against whichever neighbour slid up into the gap.

**Tooltips, on anything with a clickable region.** A control says what it is
when somebody stops on it for half a second. The Control Panel's fourteen
icons carry one -- which is where the line of description under each name
went, because that line had to fit under an icon and so was cut off mid-word.
So do the window buttons, whose middle one says which of its two meanings it
currently has; the taskbar's window buttons, which lose their titles as more
windows open; the pager; and the Apps button.

**Network is four sections**: Status, Adapters, Data Used, Applications. It
was one page holding the machine's name, every interface, the gateway, every
resolver, the last test and two buttons, which fitted only because this
machine has two interfaces.

Adapters is the hardware question: every interface, and for the one picked,
its address, netmask, state and kind. Data Used is what each has carried since
it came up, with a total that leaves loopback out because loopback never left
the machine. Applications is which programs may open a connection, and Allow
and Block -- the sharing decision somebody actually gets to make, given
ReconOS has no stack of its own to share. Status keeps the rest and gains a
Firewall button, because the firewall belongs to the network and reaching it
meant going back to the front page to find a second icon.

**Fonts can be chosen and installed.** Display Settings lists what is
installed, where each came from, and which one is being drawn with. Installing
one is a right-click on any `.ttf`, `.otf` or `.ttc` anywhere -- the way a
picture becomes the background. It installs without switching to it: changing
every letter on the desktop without being asked is a different act from being
asked to keep a file.

**Wallpapers can be added and removed.** A picture anywhere becomes the
background from its right-click menu, and joins the list. The list says where
each one came from, so two folders that both hold a Sunset.png can be told
apart. One somebody added can be removed; one that ships cannot, and neither
can the one currently showing.

**New Skin**, which does not need a skin selected first. That is the whole
point of it: Customize Skin copies the row you are pointing at, and somebody
who wants to make their own had to work out that pointing at somebody else's
was how. It asks which of three starting points -- light, dark or high
contrast -- because a skin has forty-eight roles and every one has to hold a
colour, so there is no blank to start from.

**One click chooses, two acts**, in the lists that were doing both at once.

**Reading is Display Settings**, which is what the page is. Screen resolution
is still the one thing on it that is not built.

**About is System Information**: what the machine is, what ReconOS is, and
what is underneath. Three groups, kept apart on purpose, because the third is
what explains how the first is readable at all. The processor and its core
count are read from the host rather than described in the abstract.

**Programs splits into Installed and System Apps.** They are different things
and the buttons that apply to them are different buttons.

**All Programs is a fly-out**, beside the menu rather than replacing what is
in it. Anything in it can be pinned to the Start menu from its right-click
menu, and anything pinned can be unpinned the same way.

**Disk Cleanup**, as its own Control Panel item: a space to clean, categories
with sizes and counts and a tick box each, View Files, and Clean Up System
Files. Each row says what it costs to tick it, because a size is not a
consequence. Cleaning removes rather than binning -- moving scratch into the
bin would free nothing -- and the question it asks first says so.

**Storage is three spaces**: System, Programs and User, each measured on its
own and each with its own recycle bin. Deleting routes itself -- a file goes to
the bin belonging to the space it came from -- and emptying one names the space
it is emptying, because deleting a document and deleting a system file are not
the same act.

Real partitions still need a kernel. This is the layer above them, and it is
the part that has to be right before anything can be moved onto a partition
later.

**The screen blanks when nothing is happening.** A timeout on the Power page
-- never, 1, 2, 5, 10, 15, 30 minutes or an hour -- and an option to ask for
your password when it wakes. It covers the screen rather than switching the
display off, which would need a kernel, and the page says so.

The key or click that wakes it is spent on waking: somebody coming back to
their desk presses a key to see what is there, and typing that key into a
document they cannot see yet is not what they asked for.

**The firewall can be changed from its page.** Add Rule offers nine presets --
web server, mail, a database, a run of the ports games take, and the two blunt
ones that refuse everything in or out -- because most people adding a rule want
one of those. "Something Else" takes a name, a port or a range, and three
buttons that cycle through direction, protocol and what to do. Remove Rule too.

**A search box in the Start menu.** Typing there has narrowed the list since
v0.2.15 and nothing said so, which meant nobody found out. There is a box in
the footer now with "Search programs" in it when it is empty.

**Icons** for Appearance, Programs, Modules, Network, Firewall and Recovery,
which had been sharing one generic red square. Update is the last one without
its own.

**The Control Panel is icons.** Fourteen of them, each with a line under it
saying what it is for. Clicking one opens it in a window of its own, named for
the item and stepped clear of whatever opened it -- so a wallpaper and a set of
colours can be worked on side by side. They are still the Control Panel: one
application, one entry in the menus, one window per item and no more.

**Appearance is three sections**: Themes, Colours, Wallpapers. Each gets the
whole window, which is why the wallpaper list shows all five now instead of the
two it had room for when the skins were above it.

Choosing a skin no longer puts it on. Clicking down the list to read the
descriptions restyled the desktop nine times on the way; there is a **Use This
Skin** button, and the row says which one is in use.

**Customize Skin**, not Copy This Skin. It asks first, then takes a name and a
line describing it, then opens the editor **as its own window** -- because
changing a colour is something you do while looking at the result, and an
editor covering the thing it is changing was the worst place to put it. The
desktop, the skin list and the colours are all on screen at once, and a colour
changes under all three.

Four faults underneath, all of which had to be fixed for any of it to work:
the shell held eight windows and quietly dropped the ninth; every window of an
application shared one remembered position, so they opened on top of each
other; the title bar drew the application's name while the taskbar drew the
window's; and a click that opened a window left the keyboard talking to the
window that was clicked. BG-065 to BG-068.

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
