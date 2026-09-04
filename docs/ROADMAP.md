# ReconOS Roadmap

## Design principles

**Adaptive, not bloated.** The system should use what the hardware gives it and
waste nothing. No background daemons burning cycles for features nobody asked
for; no 4GB machine that needs 16GB to feel responsive. Every architectural
decision gets checked against this.

**Event-driven throughout.** Work happens because something happened — input,
a client request, a display change. Nothing polls.

**Built from its own parts.** Where a component can reasonably be written
rather than imported, it gets written. Not for novelty — because understanding
the whole stack is the point of the project.

## How work is tracked

Four places, and each answers a different question:

| Where | Question |
|---|---|
| This file | What is planned, and why each decision went the way it did |
| [docs/CHANGELOG.md](CHANGELOG.md) | What shipped in each version |
| [docs/BUGS.md](BUGS.md) | Every fault ever found, and what it actually was |
| [Issues](https://github.com/neogentrics/ReconOS/issues) | What is open now, and the discussion |

Bugs are numbered `BG-001` upward, in the order they were found. Errors the
running system raises are numbered `VT-A001` and so on, by area -- see
[docs/ERRORS.md](ERRORS.md). Those are two different things: a bug is an event
that happened once and was fixed, an error code is a category the system can
raise forever.

Every bug is a GitHub issue labelled `bug`; features, patches and releases are
issues too, with the labels [docs/BUGS.md](BUGS.md#labels) lists. A commit
fixing a bug names its number.

This started on 4 September 2026, with the first sixty-two bugs numbered
retroactively out of the commit history. Before that the record was commits
only, which says what changed and not that anything was ever wrong.

## Phases

### Phase 1 — Desktop shell (current)

A Wayland compositor on the Linux kernel. This is where the system's identity
first becomes visible: the shell, the window management, the interaction model.
Linux supplies drivers, filesystems, and memory management so the work stays
focused on the layer people actually touch.

### Phase 2 — Kernel

Replace the Linux substrate with a purpose-built kernel, once phase 1 is a real
daily driver and its requirements are known from use rather than guesswork.
Bootloader, memory management, scheduling, drivers. BIOS and UEFI both.

### Parallel tracks

These develop alongside the main line and never block it:

- **Application compatibility** — improving Wine upstream rather than
  reimplementing it, once its internals are well enough understood to contribute
  meaningfully.
- **A systems language** — an optional language for writing ReconOS
  applications. It earns its way in; the OS never depends on it.
- **Cryptography research** — a self-contained module for studying cipher
  design from the ground up. Explicitly a learning exercise: vetted primitives
  guard anything that actually protects user data.

## What ReconOS does and does not do yet

The classic operating system components, and who currently implements each.
This is the honest picture, and it is also the plan: everything Linux handles
today is phase 2 work.

| Component | Today | Phase 2 |
| --- | --- | --- |
| Process management | Linux | ReconOS |
| Main memory management | Linux | ReconOS |
| File management | Linux | ReconOS |
| System calls | Linux | ReconOS |
| Signals | Linux | ReconOS |
| I/O device management | Linux (via wlroots) | ReconOS |
| Secondary storage management | Linux | ReconOS |
| Network management | Linux (read and used via `recon_net`) | ReconOS |
| Security management | Linux | ReconOS |
| Command interpreter | `bash` | ReconOS |

ReconOS today is the layer above all of that: the compositor, the window
management, the shell. That layer does not appear on the list because the list
describes a kernel, and a kernel has no opinion about what a desktop looks
like. When ReconOS launches a program it calls `fork()` and asks Linux to
create the process; it does not create one itself.

Naming this plainly matters, because a compositor can feel like an operating
system long before it is one. The parts that make it an operating system are
still ahead.

## v0.1.0 — first usable milestone

**Done means:** a bare-bones desktop shell with a taskbar and start button,
hosting real application windows, staying light at idle, with the skin system
architected and a first-run setup flow in place.

| # | Checkpoint | Status |
| --- | --- | --- |
| 0 | Verified build and run baseline on VM and hardware | **Done** (VM) |
| 1 | Application windows via `xdg-shell` | **Done** |
| 2 | Window management — focus, move, resize, close, alt-tab | **Done** — alt-tab regressed and was fixed in v0.2.6 |
| 3 | Shell chrome — taskbar, drawn by the compositor itself | **Done** |
| 4 | Apps menu and application launcher | **Done** |
| 5 | Native first-party applications | **Done** — six of them |
| 6 | Idle CPU/RAM baseline established and enforced | **Done** — 0.00% CPU, 17MB |
| 7 | Skin system plumbing — chrome driven by data, not hardcoded | **Done** — 48 roles, 10 skins |
| 8 | Minimal first-run setup flow | **Done** |

## Since v0.1.0

### v0.1.1 — one account at a time, and the shape of the rest

**Fixed.** Opening the File Explorer as one account showed the folder a
different account had left it in. Two causes: the user registry hive was
written per account but only read once at boot, and application windows are
built once and handed out again on every open. Windows now end at a change of
person, and the hive is re-read. A limited account could also list and read
another account's files, which it no longer can.

**Added.** A Task Manager tab for Users, with draggable columns. A Control
Panel of thirteen pages: Programs, Modules, Registry and About work; Power,
Storage, Multitasking, Update, Troubleshoot and Recovery are present and say
what has to exist before they can. Lock, which covers the screen without
ending the session. An address bar you can type into, with a drop-down of
places. A toolbar of drawn icons.

**Also fixed.** Back and Forward had each other's arrows. The Start menu
stayed open over a window that was clicked. `/System` protection compared
paths by prefix, so `/Systems` was protected. Home went to the root of the
filesystem instead of the user's folder. Home folders showed `.Trash` and the
registry hive.

### v0.1.2 — setup that looks like it belongs to something

**Added.** A band across every setup and login screen carrying the Recon
Towers mark, and a larger font for headings. A step asking what the machine is
called. One question deciding whether the accessibility list is shown at all,
instead of showing it to everybody. Pictures of each skin in the skin list,
drawn from the palette. Account pictures, eight of them drawn at first run
plus a coloured disc with the account's initial for anyone who has not chosen.
A login screen built around the person signing in.

**Fixed.** Lock offered every account, so anybody could sign in over a locked
session; it is locked to whoever locked it, enforced and not merely drawn.
Double-clicking the Recycle Bin did nothing, because the desktop's open-an-item
switch had no case for it while the context menu's Open — a second copy of the
same logic — did. The Task Manager's Type column said where an application came
from rather than what it is, its Memory column said "in ReconOS", its menus
highlighted nothing under the pointer and stayed open when the window lost
focus. The address bar's drop-down listed your own folders twice when you stood
in your own folder. `scripts/run-windowed.sh`, which runs the whole system in a
window on an existing desktop, including on Windows through WSLg.

### v0.1.3 — choose an account, then sign in

The login screen did both at once: a password box for whichever account
happened to be selected, with the others as a strip of faces underneath.
A stray click while typing changed who you were signing in as. It is two
screens now, with Back to accounts and Escape returning to the first. A
locked machine skips the grid, because there is exactly one account it will
accept.

Restart and shut down are two small round buttons in the bottom right. There
was no way to restart from the login screen at all, so a machine that needed
one had to be shut down and started by hand.

The File Explorer's toolbar and address bar became one row -- navigation left,
address in the middle, actions right -- with a File/Edit/View menu bar above
it. The address bar's drop-down is sized to its contents rather than to the
window, and highlights what the pointer is over.

**Updates announce themselves.** The system records the version it last ran
as; when that does not match the version running now, the first start after
an update says so, with a bar that advances as it fills in the icons, themes
and applications the new version has and the old one did not. On the way *up*
rather than the way down, because that is when the work happens -- nothing is
installed during a restart, and a progress bar before shutting down would be
an animation of nothing.

## v0.2.0 — the network, seen

The first networking, and the first version number to move its middle digit,
because this is a capability the system did not have rather than a round of
polish.

`recon_net` is to the network what `recon_fs` is to the filesystem: the one
file that knows Linux is underneath, presenting what it finds in ReconOS's own
terms. Interfaces, addresses, the gateway, the resolvers, whether there is a
way out, resolving a name, and asking whether a host answers.

**It does not block.** A reachability test is started, handed to the event
loop, and answered later -- a network that is not there takes the whole
timeout to say so, and freezing a desktop for three seconds is a poor way to
report it. Verified: while a probe to an unroutable address was timing out,
the system answered on its control socket immediately.

Because an answer arrives after whatever asked has finished running, the
outcome is recorded. `net reach` starts a test and `net` reports what came
back; the Control Panel's Network page reads the same record.

`recon_net` takes an event loop rather than the server, because an event loop
is all it needs. That is what lets it be tested without a compositor, and it
is also the more honest boundary: this file knows about Linux, not about
ReconOS's window system.

**What is deliberately absent:** listening, sending and receiving. An
application that wants a connection needs a stream API and a policy about
which applications may open one. Neither exists, and inventing a socket API
before anything wants one is how an interface gets fixed in place before it
is understood.

## v0.2.1 — connections, capture, and installing things

**Streams.** Opening a connection, writing to it, reading from it, and having
it close -- non-blocking throughout, on the event loop. Send queues into a
fixed buffer and refuses when full, which is real backpressure: a stream that
lets a caller queue without limit turns a slow network into memory that grows
until something dies.

**The permission rule** came with it. Every stream is opened in the name of an
application; one nobody has allowed cannot open one; the default is no. It is
not isolation and says so -- ReconOS is one process, so anything that wanted
to bypass it could call the host's socket() directly. It binds what goes
through ReconOS, which is all of ours.

Two bugs found by running it rather than reading it. The permission prefix
ended in a slash, and the registry compares prefixes by whole segment, so
every permission was written correctly and none could be listed. And registry
keys may not contain a space, so an application whose name has one -- most of
them -- recorded nothing at all, silently.

**Screen capture.** Print Screen, or `capture`. It asks for the next frame
rather than grabbing the last one, because a compositor keeps no copy of what
it drew. The frame loop is untouched unless a capture is pending:
`wlr_scene_output_commit` renders and commits in one step and leaves no moment
at which the finished frame can be read, so building the state separately is
the long way round and is only taken when somebody asked.

This one changed how the rest of the work gets done. Verifying the desktop
used to mean dumping individual panels and reasoning about how they would
compose. Now there is a picture of the whole screen -- wallpaper, windows,
taskbar, and the cursor, which no panel dump ever showed.

**Installing and removing programs.** The Programs page said this was not
built because nothing recorded which files an application owns. Something
does: a module knows the path it was loaded from. Installing copies a `.rex`
into `/Apps` and loads it; if it copies and will not load, the copy is removed
again. Removing unloads first and then deletes, because unload already refuses
when something the module registered is still in use.

**Run New Task** runs one, now that there is a registry of installed
applications to run it from.

**Wallpaper is part of the theme, and the system draws its own.** Four are
generated on first run -- Night Sky, Deep Field, Daybreak, Ember -- as
ordinary PNGs in `/System/Wallpapers` that anybody can replace. Two dark, one
light, one warm, so every skin has something that does not fight it. Stars are
placed by hashing the position, so redrawing one gives back the same picture
without a copy being kept. A skin names one it suits; an account's own choice
beats the skin; a setting pointing at a deleted file falls back to any
wallpaper rather than to a blank desktop.

**Beacon**, a tenth skin: bright blue chrome, light frames, a green accent,
paired with Daybreak. The palette and the feeling of the early 2000s, not the
shapes -- and one skin among ten rather than the default.

Capture earned its place immediately. It found that desktop labels were white
with a dark shadow, which reads on a night sky and vanishes on a daytime one;
four skins now use dark labels with a light halo. It also found the horizon
meeting the sky at a different colour than the ground, so it read as a band
rather than as light behind an edge. Neither is expressible as an assertion.

## v0.2.2 — the menu learns, and the socket is the owner's

**All Programs.** The left column of the Start menu is now the six
applications this account opens most, counted per account. All Programs at the
foot of it lists everything installed, alphabetically. The two are not the
same list truncated -- the column answers what you use, All Programs answers
what is here -- so it is always offered, rather than appearing once a seventh
application is installed and changing the menu's shape under somebody.

Opening the menu now redraws and re-sizes it. It never had to before, because
nothing in it changed between one opening and the next; the panel kept
whatever was last painted, and the Apps button showed the order as it stood
when the menu was first built.

**The control socket is restricted to its owner.** It accepts every command
the terminal accepts, and it had no authentication *and* no permissions of its
own -- the mode came from the umask, which leaves a socket in `/tmp` that
every other account on the machine can drive. Set after bind and before
listen, so there is no moment where it is both connectable and open to
everybody. A file permission, not a login.

One real bug found while doing it: a socket path over 107 bytes was silently
truncated by the kernel, so bind used one name while the unlink, the chmod and
the shutdown cleanup used another -- the socket would have come up unprotected
under a name nothing could remove.

**A correction to what this originally said.** It also claimed the startup
line reporting the network was broken, because it compared the gateway
against a raw NUL *byte* written into the source rather than the two-character
escape. The source really was malformed and is fixed, but the claim about the
behaviour was wrong: a character constant holding a single NUL byte has the
value 0, exactly as `'\0'` does, so the comparison had always been correct and
a machine with no gateway had always said "(none)". Checked afterwards by
compiling both forms rather than by reading the warning and assuming.

## v0.2.3 — gradients, and the labels they showed were wrong

**Gradients**, opt-in per role. A skin names a second colour and that surface
ramps from one to the other, top to bottom; a skin that says nothing fills
flat, which is why this is not a role of its own. Built-in skins declare a
sparse list; a skin file writes a `.to` beside the colour. Drawing sites call
`recon_fill_role`, so adding a ramp stays a change to the skin rather than to
the code that draws.

Beacon, Recon and Aqua get one. Classic does not: it is the 95 era, and 95
filled flat. Contrast cannot: it exists so that nothing depends on a shade,
and a ramp behind text is a range of contrast ratios where the skin promises
a number. The dichromat skins are measured as pairs of flat colours a fixed
distance apart, and a ramp would put one end of a pair a different distance
from its partner than the other.

**Then capture showed every taskbar label in Beacon was white on near-white.**
`bar.text` was chosen to sit on the blue bar, and no label is ever drawn on
the bar -- they are all on buttons, which Beacon fills light. The
accessibility test had been measuring `bar.text` against the bar, a surface
nothing is drawn on, and passing.

Pairing each label with the surface it is actually drawn on found the same
fault in the high-contrast skin, where it should have been impossible: its
Apps button was white text on a white button. That skin fills an ordinary
button white and a pressed one black, so no single colour serves both, which
is what the new `button.text` role is for -- a button's own label, as against
the name of the window a taskbar button stands for. The corrected pairing also
found four skins dimming a background window's name below readable. That name
is what somebody reads to find their window: being quieter than the current
one is what marks it, not being hard to see.

**Two things hardened.** The skin tables are sized by their contents and
asserted against the role count, so a table one value short is a compile
error naming the skin rather than a silent zero-fill that draws a role as
transparent black. And a minimized taskbar button keeps the button fill
instead of the bar's -- sinking into the bar works while a skin's bar and its
buttons are near-identical greys, which was true of every skin when the rule
was written.

### The splash

Added in v0.2.3. The mark, two rings with arcs travelling round them at
different speeds and in opposite directions, the name and version, a bar, and
a line naming each part of the system as it is counted.

The lines report a count of what was brought up rather than progress through
work being done, because the startup work finishes before the screen can
exist. A bar pretending to drive it would be a decoration in front of
nothing, so it counts instead: real numbers read as each line appears, and a
zero means something is genuinely missing. `RECONOS_NO_SPLASH` skips it,
which is what the harnesses use.

## v0.2.4 — a skin you can install

Added in v0.2.4. `theme install <file>` reads and parses the file *before*
copying it into `/System/Themes`, so something that is not a skin is refused
while it is still somebody else's file rather than becoming a thing every
start has to skip. A name already taken is refused, because a file cannot
shadow a built-in and installing one under an existing name would put a file
in place that the system then ignores. `theme remove <name>` takes an
installed skin away, putting the default on first if it was the one in use,
and refuses a built-in. Administrator only.

`ui account <name>` came with it: the login screen's account grid was the one
part of the gate nothing outside could drive, so lock, switch-user and the
limited-account refusals were all being checked by hand.

## v0.2.5 — the registry can be changed

It could be read from the Control Panel, behind an administrator's password,
and changing it was still the terminal's `reg` command. Change, Add and
Remove now sit under the list.

The key of an existing setting is not editable. A key you can type over is a
rename, and a rename here is a delete and an add that look like one act --
which is how somebody ends up with the old key still in the file and no idea
it is there. Removing asks first and says what removing does: whatever reads
the setting goes back to its default, which may not be what is on screen.

Saving redraws everything, because a setting here is one something is already
using -- the skin, the spacing, where a window opens. Setting `theme` by hand
restyles the desktop while you watch, which is the difference between a page
that shows the registry and a page that is the registry.

Three things found while building it: the status line read the key from the
edit field *after* clearing the field, so it said `Saved ''`; a key longer
than the registry allows was being shortened rather than refused, which would
store a setting under a name nobody typed; and the key being changed had to
be copied out when the field opened, because adding or removing anything
renumbers the hive.

`question_target` was an account name's length and had been silently
truncating module names; `set_status(cp, false, "")` was six calls with an
empty printf format, which is enough noise to hide a real warning. (This
section originally claimed the build was warning-free after that. It was not
— that was true of the one file being rebuilt at the time. A clean build had
36 warnings; see v0.2.6.)

## v0.2.6 — snapping, and a shortcut that had stopped working

**Injected keys went straight to the shell**, bypassing the compositor's
shortcut handler, so no system shortcut could be driven from outside at all:
not Alt+Tab, not Alt+Q, not Ctrl+Alt+Delete, not Print Screen. None of them
had ever been tested.

Sending one the same route a real key takes immediately found that **Alt+Tab
had stopped working**. It cycled the compositor's list of *client* windows,
which was right when the only windows were clients and became a shortcut that
did nothing once ReconOS drew its own — a desktop with a Notepad and a
Terminal on it has no clients in that list. A shortcut nothing can press is a
shortcut nobody notices the loss of. It now cycles what the taskbar lists,
skipping minimized windows rather than restoring them.

**Snapping.** Drag a window until the pointer touches the left or right edge
and it fills that half; the top fills the screen. The pointer decides which
edge, not the window, because the gesture people make is "put the mouse where
I want the window to go". The top is checked before the sides, since a
pointer in the top-left corner is touching both. A snapped window restores to
where it was when the drag *started* rather than where the drag left it,
which is in the corner. Snapping counts as maximized, so the restore button
already undoes it. Only a move snaps: a resize that ends at an edge is
somebody sizing a window against it.

Two of the four things the Multitasking page listed as not built are built,
and the page no longer says otherwise. Print Screen and Ctrl+Alt+Delete are
now verified from a test rather than by hand.

### The warnings

A clean build had 36 warnings, which is enough that a new one would not be
noticed. Fixed in v0.2.6:

- Five `implicit declaration of strcasecmp` — two files call it without
  `<strings.h>`, so the compiler was assuming a signature. It happens to work
  on this target and is undefined behaviour everywhere.
- Four paths built from a folder and a name where the two together can exceed
  the buffer. A truncated path is not a shortened name for the same file, it
  is the name of a different one, and these go on to move, copy or delete what
  they name. `recon_fs_join` refuses instead.
- The empty-format `set_status` calls in the explorer, as in the Control Panel.

Fourteen sites remained after that, all the same shape: a name copied out of a
path into a name-sized buffer, where the bound holds in practice but the
compiler cannot see it. (A build reported them 26 times, because `recon_fs.c`
and its neighbours are compiled into seven targets — the site count is the
honest number.) They were swept in v0.2.15, and three of them turned out to be
real. The build has no warnings in it now.

**Separately**, the filesystem had been leaking host paths into messages
people read — deleting something that was not there said `cannot read
'/tmp/lookroot/Users/Joshua/Documents/x'`. That is a path which does not
exist as far as anyone using ReconOS is concerned, and a break in the one
rule `recon_fs.c` exists to keep. Fixed in v0.2.6.

## v0.2.7 — four desktops

The last of the four things the Multitasking page listed as not built. It now
lists nothing.

Alt+1 to Alt+4 switches; the same numbers sit on the taskbar as a pager, so
the shortcut and the button agree and the feature is findable by looking
rather than by reading. A dot under a number says that desktop has windows on
it. Adding Shift takes the current window along and follows it — moving a
window somewhere and staying behind means watching it disappear and then
switching to check it arrived.

A window on another desktop is off the screen *and* off the taskbar. A bar
listing every window on every desktop would make these a way of hiding
windows rather than of arranging them.

Being on another desktop is kept apart from being minimized, even though both
end with the window not drawn, because they mean different things to the
taskbar: a minimized window is listed on its own desktop's bar, and one
sitting elsewhere is not listed here at all. Folding them together would have
put every window from every desktop on every bar, all marked as put away.

Opening something brings it here rather than taking you to it. An application
has one window, so "open Notepad" while it sits elsewhere has to mean one or
the other, and being moved somewhere you did not ask to go is the more
startling of the two.

Client windows are in it too. A client's surface is not the shell's to draw,
but its place in the scene is the shell's to switch off, which is all a
desktop needs. One that maps while you are on desktop three arrives on
desktop three — the field starts at zero, and without saying so a program
launched from anywhere else would appear not to have started at all.

### And a role nothing read

`title.text-inactive` was defined, answered by all ten skins, and read by
nothing — an unfocused window's name was drawn in the colour meant for a
focused one. On skins whose two title bars are near-identical greys that
looked fine, which is why it survived.

The accessibility test had measured title text against the *active* bar and
never against the inactive one, the same omission as the taskbar labels.
Adding the check failed eight of the ten skins: they had been tuned to look
quiet against a bar the text was never actually drawn on, so quiet was as far
as anyone took it.

That is twice now that getting the *pairs* right — rather than adding new
kinds of check — has found something invisible on screen. The lesson worth
keeping: a palette test is only as good as its list of what is drawn on what.

## v0.2.8 — Properties

It had been in the context menu since the menus existed, greyed out, because
there was nothing to show. It shows the name, what it is, how big, where it
lives, and when it last changed.

**One answer, in one place.** The desktop and the explorer offer the same
entry, and two copies of "how big is this and when did it change" is two
places to answer it differently — which was not hypothetical. The explorer
already had its own rounding, so a file said "2.4 KB" in the listing and
would have said "2441 bytes" in its properties; and its own type naming, so
the Type column said "File" for a thing whose properties call it a Text file.
Both now come from `recon_props`.

Deliberately not in `recon_fs`: counting a folder's contents is a filesystem
question, but "2.4 KB" and "3 September 2026" are decisions about how to say
a number to a person, and the filesystem should not be making those.

A folder is measured in items rather than bytes — the size of a directory is
a number about the filesystem, not about anything the person put there.

Two things it needed: files now carry a modification time, which nothing had
recorded; and a dialog message can break its own lines. The wrapping was on
width alone, which is right for a sentence and wrong for four short facts,
and the line limit was three, which quietly dropped "Changed ..." and left a
box that looked complete without it.

## v0.2.9 — a desktop you can arrange, and files that open

Two things a desktop is expected to do that this one did not.

**Icons could not be dragged at all.** They were laid out in a grid on every
start and stayed there. They move now, and where they were put is remembered
— the whole arrangement as one registry value rather than a key per icon,
because a key per icon would be named after the file and registry keys cannot
contain spaces, so "My Notes" and "My-Notes" would collide on the one thing a
layout must not get wrong. Positions are a grid column and row rather than
pixels, so an arrangement saved on one screen and restored on a smaller one
comes in rather than falling off the edge.

Only icons that were moved are written down: saving every position would fill
the layout with places nobody chose, and a new icon would then have nowhere
to go that was not already claimed. The grid fills in around what has been
placed.

**Nothing opened a file by being clicked.** The explorer put its size in the
status bar; the desktop opened the *folder the file was in*, so
double-clicking a document opened the Desktop. Notepad could open one, but
only through its own File menu — so a document on the desktop was something
you could see and not something you could read.

`recon_props_opener` says which application opens a name, beside the type
name, because they answer the same question. A picture or a module returns
nothing rather than being handed to Notepad, which would fill a window with
binary and look like damage — the explorer says "Nothing here opens a
Picture" instead.

Notepad refuses while it holds unsaved work. Opening from its File menu asks
what to do about that, because the person asking is looking at it; this is
somebody double-clicking a file elsewhere on the screen, quite possibly
having forgotten the window is open.

## v0.2.10 — a skin can say what shape a window is

Colours were the whole of a skin. It could recolour a title bar and not
change its height, its corner radius or how big its buttons were — so every
skin, from the 95 one to the early-2000s one, drew a frame of exactly the
same proportions in different colours. That is most of why Beacon read as
"blue" rather than as the era it reaches for.

Four measurements — title height, border, corner radius, button size — as
numbers rather than roles, and optional. A skin that says nothing gets the
shape ReconOS has always had. That is deliberately unlike the colours, where
every skin answers every role: a colour nobody chose should be obvious on
screen, and a measurement nobody chose should simply be the usual one.

**Clamped**, because a skin is a text file somebody edits and the failure
mode of an unchecked number here is not an ugly window — it is a title bar
taller than the screen, or a border of zero with no edge to grab, and either
way a window that cannot be closed by pointing at it. The test writes a skin
asking for a 4000-pixel title bar and a negative button and checks what comes
back.

Beacon gets a taller bar, rounded corners and bigger buttons; Aqua rounds
further on a thinner border. Reading and Contrast get a taller bar and bigger
buttons and stay square — somebody who turned on the reading settings has
said things are hard to see, and a bigger target follows from that while a
rounded corner does not. Classic stays square because 95 was square, and the
three colour-vision skins keep the default because they differ from each
other only in palette.

The corner is anti-aliased by sampling coverage. The first version cleared
whole pixels and gave a staircase, which is worse than a square corner: a
rounded corner is the one shape people read as smooth, so steps look like a
fault rather than a decision. What it clears goes fully transparent rather
than being filled, because a window sits over the wallpaper and over other
windows — there is no one colour a corner could be painted that would be
right anywhere but where it was chosen.

## v0.2.11 — Notepad can undo

A text editor that cannot undo is a text editor that loses work, and this one
now opens files by being clicked, so more text passes through it than when
the only way in was to type it.

An edit records what changed and where, not a copy of the document —
snapshots would mean holding a hundred copies of a file to step back a
hundred keystrokes.

**Grouped by word.** The first version grouped only at line breaks, which
made typing a sentence a single undo: one press and the whole line was gone,
with no way back to the word. A word ends when the run of spaces after it
does, so the trailing space goes with the word it follows.

**Opening a file forgets the history.** Undoing past that moment would put
the previous document's characters back into this one a few at a time, which
is not "the change before this one" by any reading and is how an editor
quietly corrupts a file somebody trusted it with.

And an **Edit menu**, because Ctrl+Z is only discoverable to somebody who
already knows it. Every entry shows its shortcut, right-aligned and dim.

Three things went wrong making the menu bar hold two menus, all the same
shape as bugs already fixed elsewhere here: the second menu's hit id was the
text area's, so clicking Edit put the caret in the document; `menu_open` came
from `calloc` as zero and zero had just become the File menu, so a new window
opened with File already down — the file explorer had that exact bug for that
exact reason; and the branch handling entries ran before the branch handling
names, so with a menu down, clicking a name read as an entry index past the
end of it.

## v0.2.12 — a text clipboard, and something to select with

There was **no way to move text from one place to another**. The clipboard in
`recon_fs` holds a path — what the explorer and the desktop copy between them
— and text was not something the system could carry, so a line in the
Terminal could be read and typed back into Notepad by hand and that was all.

The text clipboard is separate from the file one rather than a field on it.
They are the same idea and not the same thing: copying a file and then
copying a sentence should not make the file un-pasteable.

**Notepad can select.** An anchor and the cursor, either able to come first,
because dragging backwards from a word is a selection too. Typing over a
selection replaces it; backspace removes it; pasting over it replaces it. The
highlight is drawn per line and clipped to that line, since a selection
across three lines is three rectangles — and a line whose break is inside the
selection shows a little past its last character, so the newline reads as
part of what was taken.

A selection deleted or pasted is **one undo**, not one per character.

Cut, copy, paste and select all are in the Edit menu with their shortcuts
beside them, and in every text field in the system — renaming a file, typing
a path, naming one to save. That lives in `recon_edit` rather than in each
field's owner, because a clipboard that works in some fields and not others
is worse than none: which is which cannot be seen.

The terminal pastes too. A newline **ends** the paste rather than submitting
the line: pasting several commands and having them all run is how somebody
loses a directory to a paste they meant to read first.

## v0.2.13 — asked for while watching it run

**Removing an account can take its files.** Closing an account and destroying
somebody's documents are separate decisions, and the dialog offered only
"Remove" — which had already made the second one on their behalf. Keep Files,
Delete Files, Cancel. The terminal takes `users remove <name> files`.

The account goes first and the folder second. If the folder cannot be removed
the account is still gone: the login is what was asked about, and leaving it
in place because a file was locked would be answering a different question.

That changed the shape of the answer handling, which read "anything but the
first button means cancel" — untrue the moment a question has two ways of
saying yes. It is the last button now, which is the one Enter and Escape
choose anyway.

**The Start menu's footer is five icons in the bottom right.** Spelled out,
they were labels of different lengths in buttons of identical width, which
reads as a row of text rather than a row of controls — and "Sign Out" beside
"Switch User" is two phrases that must be read before either can be told
apart. The name of whichever the pointer is over appears on the left, in
space nothing else uses.

The footer is filled with the menu's colour now rather than the taskbar's. It
left its ink with no role guaranteed to read on it: `menu.text` is measured
against the menu and `bar.text` against a taskbar button, and the footer was
neither. On a skin with a deep blue bar the glyphs came out dark on dark.

**Buttons round off**, on a fifth metric. All four corners, unlike a window's,
and the corner is filled back to what is behind rather than cleared — a
window clears because the wallpaper is behind it, a button would show the
wallpaper through the middle of a title bar.

**The splash takes about a second and three quarters**, up from just over
one. It went past faster than it could be read, which is the wrong failure
for a screen whose job is to say the machine is starting.

## v0.2.14 — packages, and Find

**Installing was `install <file.rex>`**: copy one shared object into /Apps and
load it. That works for a program which is only code, and every program is
more than that — an icon, a default setting, a file it ships alongside
itself. There was nowhere to put any of it, which is why the Calculator's icon
is drawn by ReconOS rather than supplied by the Calculator. There was also no
record of what an install had placed, so uninstalling deleted the one file it
knew about.

A package is a folder with a manifest, named `.rpk`. A folder rather than an
archive because ReconOS cannot read one, and writing a container format buys
nothing yet: nothing here is sent over a network, and a folder can be copied,
inspected and repaired with the tools the system already has. A single-file
form can wrap this later without the manifest changing.

Installing writes a **receipt** naming every path it placed; removing deletes
exactly those. A receipt rather than rederiving it at removal time — what an
install placed is a fact about that install, and working it out again from a
manifest that may have been edited is guessing about somebody else's disk.

The receipt is written **before** the module is loaded, because loading is
the step that can fail unpredictably and the files are already on disk when
it does. That failure path is the one this was tested on first: a module that
would not load rolled the whole install back and left nothing behind.

An icon already present is left alone and not recorded, so uninstalling one
package cannot take away an icon belonging to another. A file the receipt
names but which is gone is not an error either.

**Find, in a bar rather than a dialog.** A find dialog covers the thing being
searched. Case-insensitive always, wrapping once — without wrapping, a search
started halfway down a file reports nothing for a word that only appears
above the cursor, which reads as "not here" and is wrong. Searching starts one
past the selection rather than at the cursor, because a match is left
selected and searching from the cursor would find the same one for ever.

## v0.2.17 — the parts that run, and a Control Panel you can open twice

### The Control Panel is icons

Joshua's read of it, while watching it run:

> The control panel is supposed to have, like, icons and text. When you click
> on them or double click on them, it should automatically open a separate
> window. They're not separate apps. They're all still part of the control
> panel. That way, if I wanna mess with a screensaver and a wallpaper at the
> same time I can.

So: fourteen tiles with a line under each saying what it is for, and clicking
one opens it in a window of its own. The window is named for the item, gets
the help topic for that item, and is stepped down and across from whatever
opened it. One window per item and no more -- two windows both editing the
registry would be two views of one file, each unaware the other had written to
it, so a second click brings the existing window forward and the tile is
shaded to say so.

A page window has no sidebar. It is a window about one thing, and a list of
the other thirteen down its left edge would be thirteen ways to turn it into a
window you did not ask for.

This subsumes the earlier request that each Power item open its own window
with Save and Cancel: Power is one of the fourteen.

### Four faults that had been waiting for a second window

None of this worked until they were fixed, and none of them could have been
found without something that opens more than one window per application.

`struct recon_appwin *apps[8]` -- eight. Seven built-ins and a Calculator is
eight, so the ninth window was refused and the refusal was invisible from the
application's side: the window was built, was drawn, had no taskbar button,
took no clicks and could not be reached by Alt+Tab. It looked like a window
and behaved like a picture. That is BG-065.

A window's remembered position was keyed on `impl->title` -- the
*application's* name, which is the same as the window's for an application
with one window, which until now was all of them. All fourteen Control Panel
windows shared one saved position, opened exactly on top of each other, and
moving any one of them wrote that position for all the rest. BG-066.

The title bar drew `impl->title` while the taskbar drew the window's own name.
A comment three functions away claimed both read through `recon_appwin_title`
"so they stay in step", which was true of one of them. BG-067.

And after offering a click to an application, the shell raised and focused the
window that had been clicked -- unconditionally, including when handling the
click had deliberately focused something else. A tile opened its window in
front and had focus taken straight back. The shell notes which window held
focus before the click now, by identity rather than by index, and only focuses
the clicked one if the application did not move focus itself. BG-068.

### Appearance, in three sections

Themes, Colours, Wallpapers. Each gets the whole window, which fixes BG-060 --
two wallpapers showing out of five -- by construction rather than by
arithmetic. The skin list took `(height - y) / ROW_HEIGHT` rows, meaning
everything left, and the wallpapers underneath got what was over. An
arithmetic fix would have held until the next thing was added between them.

Choosing a skin no longer applies it. It used to, which made choosing one to
copy indistinguishable from changing the whole desktop: somebody clicking down
the list to read the descriptions restyled the system nine times on the way.
There is a **Use This Skin** button, and the row says `(in use)` rather than
relying on a highlight that also meant "this is the one you clicked" -- two
different facts were wearing one appearance.

**Customize Skin**, not "Copy This Skin". Copying is what happens underneath
and it is not what anybody came here to do; they came to change how the system
looks and are told, correctly, that a built-in cannot be changed. Naming the
button after the thing they want rather than after the mechanism they have to
use is the difference between a system that helps and one that explains
itself.

It asks before it does anything, because making a skin leaves a file behind
with a name in it. Then a name and a line describing it -- the built-ins each
have a description and it is the only thing distinguishing them in a list of
names, so a skin of your own with a blank one is the odd entry out.

Then the editor, **in its own window**. Joshua asked for that directly, and he
is right about why: changing a colour is something you do while looking at the
result, and an editor covering the very thing it is changing was the worst
possible place to put it. The desktop, the skin list and the colours are on
screen together, and a colour changes under all three.

The Colours section shows the colours of whichever skin is on. Changing one on
a built-in cannot write to it, so rather than refusing, the button says
Customize Skin and offers to make the change on a copy. Being told "you cannot
edit this" and left to work out that copying it first is the way round is the
system making its own limitation into the reader's problem.

### Storage, decided

Joshua asked directly, and was owed an answer:

> Have we decided on the storage system? You haven't told me anything, or at
> least I haven't seen you give me an artifact or anything to look at. System
> storage should be separated from program storage, and system storage should
> be separated from the user storage. There should be, like, recycling bin for
> system storage and recycling bin for users.

**System**, **Programs** and **User** are separate spaces. Real partitions need
a kernel and are Phase 2; what is here is the layer above them -- the one that
decides what a space *means* -- and it is worth having first, because it is the
part that has to be right before anything can be moved onto a partition later.

The separation is real in the ways that matter without a kernel:

- Each space is measured on its own. "How much room have my files taken" and
  "how much room has the system taken" are different questions with different
  things to do about them, and a single number answers neither.
- Each has its own bin, living **inside** it. A bin outside its own space would
  mean deleting a system file put bytes into somebody's account, and the number
  on the Storage page would move for the wrong space.
- Deleting routes itself. `recon_fs_trash` puts a file in the bin belonging to
  the space it came from, so nothing that deletes has to know there is more
  than one bin. Every existing caller got the new behaviour without changing.

The user's bin stays per account rather than per space. One bin for `/Users`
would be one account's deleted documents sitting where another account can read
them, which is exactly what the account boundary exists to prevent (BG-026).

The Storage page is a selector across the top -- each space with its own size
on the button -- and then that space's parts and that space's bin. Emptying
names the space it is emptying: one button for all three would be a button
nobody could press safely.

The total shown is the space's own, not the sum of the rows. They are different
numbers, and printing the smaller one beside a button showing the larger would
be dishonest; where they differ the page says by how much.

Two claims the page was making are no longer true and are gone: that ReconOS
has no volume layer to list, and that choosing where new files go needs more
than one volume to mean anything. What is left on its not-built list is Disk
Cleanup, moving a space onto its own disk, and formatting -- the last two
genuinely needing the kernel.

### The screen blanks

The one thing on the Power page that never needed a kernel, which Joshua said
while looking at it:

> Screen blanking should work with a timeout and a login-on-wake option. Power
> mode needs the kernel.

Both right. A timer, reset by any input, that raises a black panel over
everything; and a setting saying whether coming back asks who you are.

Stepped through a short list rather than typed. The values anybody wants are
never, one, two, five, ten, fifteen, thirty and an hour, and a field that
accepts 7 is a field somebody has to be told 7 is allowed in.

It is described as covering the screen, not as switching the display off.
ReconOS is a process on somebody else's kernel and has no way to touch a
panel's power state; saying otherwise would be a claim about hardware, on the
page whose whole character is saying plainly what it cannot do. What this
saves is the picture rather than the watt, which is still worth having: nobody
walking past reads a document that is not on screen.

The input that wakes it is spent on waking -- for a key or a click, not for
pointer motion. Somebody coming back to their desk presses a key to see what
is there, and typing that key into a document they cannot see yet is not what
they asked for; a mouse move, on the other hand, does nothing on its own, and
a pointer that refused to move until the second nudge would feel broken.

Injected input takes the same route, which is not a detail: without it,
blanking could not be driven from outside, and a thing that cannot be driven
from outside is a thing that stops working without anybody noticing. That is
BG-038, and the lesson held.

`blank`, `blank now` and `blank wake` in the Terminal, because a test that has
to wait a minute to find out whether blanking works is a test nobody runs.

### The firewall, from its own page

Joshua, on seeing it: *"Oh, wow. Firewall looks nice. I like that... I don't
see a button to add new rules or to create a custom firewall template. They
should be presets. And then they should do the option to create a custom
one."*

So Add Rule is two steps. Nine presets first -- web server, web server over
TLS, mail sending, mail collecting, file transfer, a database, a run of the
ports games take, and the two blunt ones that refuse everything in or
everything out. Then "Something Else" for the rest: a name, a port or a range
written the way somebody would say it (`80`, or `27015-27030`, or nothing for
every port), and three buttons that cycle through direction, protocol and
action. Each button says which value it is on, so the control and its readout
are one object rather than two that can fall out of step.

A preset arrives switched on. The nine shipped rules are written down and off
because they exist to be found rather than to be in force; one somebody has
just picked out of a list is the opposite -- they asked for it, and adding it
switched off would be adding nothing.

A port that is not a port is refused with the reason. A name too long for the
rule is refused rather than shortened, for the reason the skin copy refuses
one: the name is what the log says when the rule fires, and half a name in a
log is a rule nobody can find again.

The page used to end with a note saying that adding and removing rules was the
Terminal's job because a form for five fields was a bigger piece of work than
the page it would sit on. That was true and is not now.

### A search box in the Start menu

Typing in the Start menu has narrowed it since v0.2.15. Nothing on screen said
so, so nobody found out -- and the only acknowledgement was a "Finding: x" row
that appeared *after* the first keystroke to explain what had just happened.

A box in the footer, bottom left, where Joshua circled it. "Search programs"
when it is empty, what has been typed when it is not, and a caret. The row
above the list is gone: a box that is always there says beforehand what the
row could only say afterwards, and beforehand is the part that matters.

### Icons

Six generated, on Joshua's Ideogram credits and at his direction, in the navy
and oxblood the existing three use: Appearance, which had none, Programs,
which had one he did not like, and Modules, Network, Firewall and Recovery,
which had all been drawing the same generic red square. Update is the last one
without its own.

Not from his photo library, which he has ruled out.

### Services

Watchtower has a Services tab. Five entries: the desktop shell, the control
socket, remote access, the firewall, and networking. Each says whether it is
running, stopped, or failed and with which error code, and how many times it
has been started this run. `services` in the Terminal is the same registry
read a different way -- not a second copy of it, because two lists of what is
running eventually disagree and the one somebody happened to be looking at
would be the one they believed.

Multitasking used to be a Control Panel page listing behaviour nobody could
change: "Title bar buttons -- real, but not settable". Joshua's read of it was
right and worth writing down:

> Multitasking doesn't need to be a separate tab in the control panel. It
> needs to show up in the task manager. It's one of the services that can be
> seen running that can be turned on or off or restarted.

So the page is gone and the shell is a service.

`starts` rather than an uptime, because ReconOS has no clock of its own to
measure one against, and the count says the thing worth knowing: one means a
normal system, more than one means somebody has been repairing something.

An essential service refuses Stop and allows Restart. The desktop shell is the
only one so far. Stopped is a decision and failed is a problem, and a list
that showed them the same way would hide every fault behind something that
looks intentional -- so they are two states, and a failed one carries its
code.

### Restarting the shell

The interesting half. Stopping the shell destroys the taskbar, the desktop,
the menus and the session screen. It leaves the application windows alone,
because they belong to the application registry rather than to the shell, and
the new shell adopts whatever is still open.

That is the whole point: a taskbar can be repaired without costing somebody
the document they were writing. Whoever is signed in stays signed in -- being
asked for a password because a taskbar needed rebuilding would be the repair
costing more than the fault did.

Two faults had been sitting in the code waiting for something to run this
path.

The UI font was loaded by the shell and freed with it. Every surviving window
holds that pointer and several cache a copy of their own, so the first frame
after a restart was a segmentation fault five frames deep in the glyph
rasteriser -- reproducible three times out of three. The font belongs to the
system now: loaded once per size for the whole run, freed after nothing is
left that could draw. `recon_font_reload` already worked this way for changing
a typeface; ownership is what was wrong. That is BG-061.

And registering a built-in application twice was refused as a name collision.
A second shell registers the same seven built-ins, so a restarted desktop had
no Notepad and no File Explorer while the old windows were still on screen -- a
desktop you cannot open anything from. A built-in re-registering itself is an
update in place; a module trying to take a built-in's name is still refused,
because that is the collision the check exists for and the shell's own
built-ins are not two pieces of code.

Finding it took making the test harness sign a user in first. Without an
account signed in the restart takes the other branch and does not crash, which
is why three runs under gdb looked clean while every run outside it dumped
core.

### Bugs have numbers

[docs/BUGS.md](BUGS.md), and sixty-two GitHub issues. See
[How work is tracked](#how-work-is-tracked) at the top of this file.

Joshua asked for it in these terms:

> People like to see a track record. People like to see that there's actually
> been progression, not just random commits and updates every time something's
> done.

---

## v0.2.16 — saying what went wrong, and a boundary around what opens

Four things, and three of them were asked for together because they belong
together: a system that can be reached from elsewhere needs something deciding
what may be reached, and a system doing either needs a way to say what went
wrong.

### Error codes

    VT-A001
    ^^ ^ ^^^
    |  | +--- which fault, 001 upward within that area
    |  +----- which area of the system
    +-------- Void Tower

**The letter is the area, not the severity.** The same area produces faults of
every kind — the filesystem can fail to read a file (recoverable) and fail to
open at all (not) — so severity in the letter would scatter one subsystem
across the alphabet. Worse, a code would have to change if a fault were ever
reclassified, and *a code that changes is a code nobody can look up*. Severity
is a field on the entry.

**`I` and `O` are not area letters.** This is a code somebody reads off a
screen and types into a search box, and in that setting `I` is `1` and `O` is
`0`. Losing two letters costs nothing; a support answer about the wrong fault
costs everything.

**A hundred per letter**, continuing at a second letter when an area fills
rather than renumbering, because the old codes are already written down
somewhere. A number is never reused. Twenty-four letters at a hundred each is
two thousand four hundred, a long way past anywhere this is going.

Forty-two codes to start, across twelve areas — the faults the system can
actually reach today, not a guess at the ones it might.

**One list.** `include/recon_errors.def` is included by the header to build an
enumeration, by the source to build the table, read by `errors`, quoted by the
stop screen, and turned into `docs/ERRORS.md` by `scripts/make-errors.sh`. A
`_Static_assert` checks the enumeration and the table are the same length,
because a mistake there would make every code read as its neighbour.

**Three severities, and they decide what happens** rather than only how it
reads: STOP draws the screen and ends the session, fault is reported where it
happened, note is written down.

The stop screen is drawn in **fixed colours rather than the skin's**, because
the one fault it has to survive is the one where the colours are the problem.
The code is the largest thing on it: somebody reading it is not going to debug
it, they are going to write it down, restart, and look it up.

The record is written to disk **before** the screen is drawn — the screen is
the part most likely to fail when what failed is the drawing — so the next
start can say what happened to the last one. It does, on a card after the
startup screen, once. A card and not a full screen: the machine in front of
them is working, and saying otherwise would be a lie told in a large font.

A crash cannot be drawn at all, so `SIGSEGV` and its neighbours write the
record with `write(2)` — one of the few things a signal handler may do — and
hand back to the default so a core file still happens.

`recon_error.c` does not depend on the thing that draws. The session
*registers* the screen. That is not tidiness: recording a fault has to work
where the compositor does not exist — a test binary, and the moment before the
display comes up, which is exactly when the worst faults happen.

### The firewall

It cannot filter packets, and saying it could would be the same lie as a
network page that pretended to implement a stack. What it does is decide
**what ReconOS itself opens and accepts**, which is a real boundary with real
teeth: every outgoing connection asks first, and the remote listener cannot
open a port the firewall has not been told to allow.

The shape most systems have, because that is the shape people already know: a
switch, a default per direction, and a numbered list of rules. **First match
wins**, not most-specific — reading the list top to bottom is the only way a
person can work out what it does, and the page shows the numbers because the
order is part of the rule.

Nine rules ship. The incoming ones are written down and **off**: the useful
state for a rule about remote access is there, correct, and not in force until
somebody wants remote access, so turning it on is a switch rather than an
exercise in remembering a port number. The outgoing ones are on even though
the default would allow them anyway — so a machine whose outgoing default is
changed to block still works, and what has to keep working is visible in one
place.

It comes up on its built-in defaults whatever happens to its file. **A
firewall that fails open because its file is missing is worse than no
firewall, because it looks like one.**

Its own Control Panel page rather than a section of Network, because it is the
page somebody comes to the Control Panel for — and because it is meant to be
replaced. A firewall with per-program rules, profiles and its own history
belongs in an application; when that application arrives it takes this page's
place, which is a cleaner thing to replace than half of another page.

Rules that are off are drawn dim whether or not they are selected: being
written down and not in force is the single most important thing about such a
rule, and a row that looks the same either way is a row that gets misread.

Not done: adding and removing a rule from the page. Five fields and a name is
a form, and a form is a bigger piece of work than the page it would sit on —
so the page says plainly that it is the Terminal's job for now.

### The startup screen checks

It reported a count of what had been brought up and nothing else, because the
startup work was finished before the screen could exist and a bar pretending
to drive it would have been a decoration in front of nothing.

Now each line looks at the part of the system it names: every folder the
system cannot do without, both settings hives, the icon folder, the skins and
whether the one in use is still among them, the wallpapers, the programs and
how many refused to load, each account and whether it still has somewhere to
keep files, and what the firewall is doing.

A failure raises its code, shows it on the line in the warning colour, and
carries on. Almost everything here is survivable, and **a machine that refuses
to boot because one icon folder is unreadable is worse than one that says so
and keeps going.** What it cannot survive raises a STOP and never reaches this
screen.

The programs are **not** loaded and unloaded to test them. They are already
loaded by this point, so "did it load" is a question the module layer has
answered and this reports; loading them again to test them would run their
startup code twice, which is a worse thing to do to a program than not testing
it.

Missing things are repaired where repairing is obvious — a system folder is
recreated, and so is an account's folder, because the account is what the
login screen offers and offering one that cannot be signed into is worse than
an empty Documents.

The screen takes a moment longer than it did, and the moment is the checking
rather than a delay added to look like checking.

### Remote access, both ways

Two transports, and they are not equivalent — so both are described everywhere
either is, because a command that mentioned only the network port would be
recommending the worse option by omission.

**The Unix socket** carries no authentication of its own and does not need
any: it is created readable and writable by its owner alone, so the filesystem
*is* the authentication. SSH can carry it across a network, which is the
secure route and needs nothing from ReconOS at all — SSH does the encryption
and the identity, both of which it is much better at than anything written
here would be.

**The network port** is off by default, listens on TCP 7420, and asks each
connection for a key. It exists because it is what people expect a system to
be able to do, and because the SSH route needs an account on the host
underneath — which a machine ReconOS owns will not have.

It is honest about what it is: **the key crosses the network in the clear.**
There is no TLS yet. `remote` says so every time and `remote on` says it
again.

Three things must be true before the port opens, and each refusal says which
one and what to do: there is a key, the firewall allows incoming TCP on that
port, and the port can be bound. The firewall is asked **again for every
connection**, not only when the port was opened — a rule turned off while the
listener is up should take effect, not take effect after a restart. And the
"remote access was on" setting is re-checked against the firewall at startup,
because a setting that outranked the firewall would make the firewall a
suggestion.

Only the key's hash is kept, salted and stretched with PBKDF2 — a short secret
somebody typed is exactly the thing a plain hash of is worth guessing at. The
key is shown once. Losing it costs one command; a key that can be read back is
a key sitting in a file.

Turning remote access off shuts the port and leaves connections already up
alone. Closing the door is not throwing out whoever is inside, and somebody
turning it off while using it remotely would otherwise cut themselves off
mid-command with no way back.

**Two faults found by connecting rather than by reading.** The authentication
step closed the connection from inside `handle_line`, which freed the client
the read loop was standing on — the loop went on to touch it, corrupting the
heap quietly, and the process aborted on the *next* connection. That is the
shape of fault that takes a day to find from the symptom. `handle_line` returns
a verdict now and the caller does the closing, which is the only place that
may.

And the accepted key was then run as a command, which failed and said so —
quoting the key back into the output, where it would sit in whatever scrollback
the other end keeps. A secret that has been checked has done its job; it should
not then be repeated.

## v0.2.15 — help, and telling people what changed

Every version so far shipped with no way for the system to explain itself.
The control panel says what is not built, which is honest and is not the same
as help: somebody who does not know how to reach a second desktop has nowhere
to look, and the answer only existed in a README they would have to leave the
machine to read.

**The source is two Markdown files**, `docs/HELP.md` and `docs/CHANGELOG.md`,
split into topics by `scripts/make-help.sh`. Prose about a feature lives
beside the prose about every other feature rather than in the C file that
implements it — that is the arrangement under which it stays written. The
split runs at build time and its output is checked in, so a system with no
awk still ships with help.

**Written out on every start**, unlike icons and skins, which are written
only when missing. There is nothing in a help page somebody would have
customised and want kept, and help describing a version the system is no
longer running is worse than no help at all.

**One window, two documents.** The help and the change log are the same
question asked at different times, and somebody who has found one has found
the other. A rule divides them, because the halves are read differently: one
is looked up, the other is read through, and thirty entries in a single
unbroken list hides that.

**Paragraphs are reflowed, not lines.** The source is hard-wrapped to about
eighty columns because it is a file people edit. Wrapping each of those lines
on its own produced a ragged column — a full line, then two words, then a
full line — which reads as though the text were broken rather than as though
the window were narrow. Doing it properly turned up a second thing:
`strtok_r` runs a row of newlines together as one separator, so the blank
line between two entries never arrived and the two were welded into a single
block of prose. Blank lines are the only paragraph marks the source has, so
they are now read by hand rather than by a function that throws them away.

**A notice after an update**, once per account per version. Per account
rather than per machine: an update is news to each person the first time they
sign in after it, and somebody who has been away for three versions should
still be met with the current one on the first desktop they see. Pressing OK
records the version in that account's hive; so does opening the full log,
because leaving it unread would punish the person who took the trouble to
read further.

The notice is registered as an application so the shell builds it, focuses it
and lists it on the taskbar the way it does every other window — but it is
kept out of the menus, because nobody goes looking for it. It comes to them.

**Icons for the Control Panel, the Start menu and Help**, which had been
sharing the plain window that stood in for anything without one.

### One button shape

The corner rounding a skin asks for was written three times — in the taskbar,
in the window frame, and in the notice after an update — and every button that
had not been rewritten stayed square. On Beacon that meant one skin producing
two shapes of button inside one window, which is not a theme so much as a list
of the files somebody remembered to change.

`recon_draw_button_edge` is that decision in one place: the bevel, then the
radius rounded off it. Bevel first and rounding after, because the bevel is
what says pressed from raised and a skin asking for round corners is not
asking to give that up. Sunken text fields and panel outlines still call the
bevel directly — they are not buttons and do not round.

Beacon's button radius went from three to five: at three the corner was there
but had to be looked for, which is the cost of rounding without the difference
it was meant to make.

### Storage, which mostly was not blocked

The page was four notes about things that do not work. Three were the same
missing thing said three ways — one filesystem, hosted in a folder, no volume
layer under it. The fourth, *find what is taking room and offer to remove it*,
was not blocked on anything. It had just not been written.

It now walks what ReconOS owns and says where the room went: the system, the
programs, each account **by name**, the scratch space, and the bin. Accounts
one by one because "the accounts take most of the room" is not an answer
anybody can act on and "this account takes most of the room" is.

The bars are a **share of the total, not of a disk**. Without a volume layer
there is no capacity to be a fraction of, and drawing one anyway would be
inventing the number the page opens by saying it does not have. The bin's
bytes are shown but not added to the total: they are already inside the
account folder above it.

Measuring walks the tree, so it happens when the page is opened and when
somebody asks again, rather than on every frame.

The three things still blocked are listed underneath. The page used to be
nothing but that list, and dropping it now that most of the page works is how
a gap stops being visible.

### Writing a skin from inside ReconOS

Backlog item 2, and the last thing on that list nothing outside was blocking.
A skin could be installed and removed since v0.2.4 and still could not be
*made* here: authoring one meant editing a text file somewhere else and
bringing it in.

**Copy first.** A copy rather than a blank file, because the questions a skin
has to answer — forty-eight roles, the ramps, the frame shape — are the part
nobody knows in advance, and a complete file to change is a far better
starting point than an empty one to fill.

**The whole list of roles**, with a swatch showing what each currently is. A
list of hex numbers is not a list of colours to anybody. The whole list rather
than a chosen few: which roles matter depends entirely on what somebody is
trying to change, and a shortened list is a guess about that made by whoever
wrote the page.

**Editing puts the skin on first.** Changing colours you cannot see is
guessing at hex numbers. Every change is live, because the palette everything
draws from *is* the loaded skin — the taskbar changes colour while the button
that changed it is still under the pointer — and it is written to the file at
the same moment, so there is no save to forget.

**Built-ins are refused rather than written over.** A file cannot shadow a
built-in, so the edit would be saved, ignored, and lost on the next start:
worse than a refusal, because it looks like it worked. The button says Copy
for a built-in and Edit for a file rather than offering both and refusing one.

The file writer turned up a defect. `recon_theme_write_defaults` emitted
colours and metrics and **not gradients**, so every shipped skin file on disk
was missing its ramps. Nothing broke — those files are never read back,
because a file cannot shadow a built-in — but anybody copying `Beacon.theme`
to start their own from got a Beacon with the ramps quietly gone. There is one
writer now, so a copied skin contains exactly what a shipped one does. It was
found by reading the file, which needed `theme copy` in the Terminal to exist.

Not done: the ramps and the frame metrics can be removed from the editor but
not set there, and a skin cannot be renamed or deleted from it. The file is
still editable by hand, which is the way to do any of those today.

### Client windows, framed by ReconOS

The claim in the known-missing list was that this was not started because
there was no Wayland client in the development environment to test against.
The first half of that was untrue: `weston-terminal` was installed the whole
time and nobody had looked. The second half — that shipping window management
nothing has driven is how Alt+Tab came to be dead for months — is why this
work is mostly instruments.

Three of them, and each found something:

- **`spawn`**, which wires up `recon_spawn`. That function had been in
  `main.c` since the compositor could host a client and had *never been
  called*: a capability nothing exercises is a capability nobody knows the
  state of. It reaches the machine underneath, which the help says plainly
  that nothing here does, so it exists only when `RECONOS_ALLOW_SPAWN` is
  set. It also passed the whole command line as the program name, so it could
  never have launched anything with an argument.
- **`tests/decor_client.c`**, the smallest Wayland client that uses
  xdg-decoration. weston's own demos do not use the protocol at all and draw
  their frames regardless, so nothing on hand could have told the difference
  between this working and this not existing.
- **Running the drag through the injected-input path** as well as the real
  one, so dragging a client window is something that can be watched.

What they found:

**Forcing SERVER_SIDE on every client gave weston-terminal a ReconOS title
bar above its own.** Two bars, one window — worse than the mismatch the
feature exists to fix. Only clients that actually use the protocol are
decorated now; one that draws its own frame cannot be told to stop and is
left alone.

**A maximized window filled the screen from y=0**, putting its own title bar
at minus its height, which is to say nowhere. A maximized client could not be
moved, minimized or closed except from the taskbar.

The bar reads the same skin metrics a built-in window's frame does — title
height, button size, corner radius — so a client window and a ReconOS window
are the same window.

**Resizing by an edge** followed, in the same version. The grab region is a
six-pixel margin *outside* the window rather than a border inside it, because
inside belongs to the client: a terminal's last column of text is not a resize
handle. The cursor changes over the margin, since an invisible grab region
nobody can see is one nobody finds.

Two more paths that existed and had never been taken:

- The margin is outside the window, which puts it over the desktop -- and the
  desktop answers a click nothing else wanted, so going through the shell
  first meant the backdrop swallowed every resize before anything could see
  it. It looked exactly like the edge detection not working. The edge is
  checked first now, guarded so the shell still wins where the shell is.
- **Injected pointer motion never ran the move or resize modes at all.** They
  were reachable from a test and did nothing, so `ui move` while dragging an
  edge moved the pointer and left the window where it was. Injected motion
  takes the same order the real path does now, which is the property the
  injection exists to have.

Not done: an icon that means anything. A Wayland client hands its compositor
an `app_id`, not a picture.

### Update, and a font that draws no dashes

The Update page was three notes about things that do not work. One of them --
*the history exists in the repository; nothing brings it into the system* --
stopped being true when the change log arrived, and a page that keeps saying
so after the fact is worse than one that never said it.

It now says what version is running, whether the installed version matches it,
the first few lines of what this version brought, and offers a way through to
the whole log in Help. The first few lines rather than all of them: this is a
page about the state of the system, not a place to read a document in, and a
summary that grows until it fills the page is the document shown in the wrong
reader.

Building it found something that had been on screen for a while without being
noticed: **every em dash in the help came out as nothing at all** — a sentence
with a hole where its punctuation should be, which reads as a bug in the
sentence.

The first fix was to fold the punctuation to ASCII in `make-help.sh`. That was
a patch on the symptom, and the symptom was not the interesting part: text was
walked *a byte at a time* and glyphs were cached only for 32..126, so any
UTF-8 sequence was three or four bytes each of which drew nothing. Not a box,
not a question mark — nothing. **The typeface had the glyphs the whole time.**

So the walk decodes UTF-8, and the glyph cache gained a second half: a
direct-mapped table of 128 entries for everything past ASCII, because the
alternative to a fixed size is a cache that grows with whatever a person
types. A collision costs one rasterization.

Accents, dashes, quotation marks and other alphabets now draw — in file names
as well as in the help — and a character the font has no drawing for shows as
an empty box, which is the font saying so rather than the character being
lost. The ASCII folding came back out; the source keeps its real punctuation.

**Typing followed**, because the two halves are one thing: a field that shows
a character it cannot navigate is worse than one that shows neither. The caret
is still a byte offset -- that is what the text is -- but the arrow keys and
Backspace step over whole characters by walking off the continuation bytes,
and `recon_edit_key` accepts everything above the control range rather than
only ASCII. A keyboard laid out for a language with accents in it sends those
characters, and refusing them meant a person could name a file only in English
on a system that would happily store the name.

Verified end to end: typed `Café` into the skin-copy field, pressed Enter, and
found `Café.theme` on disk under that name and in the skin list. Backspace
over `α` and `ñ` removed two characters and left `Café` intact rather than
half of a two-byte sequence.

### F1

`recon_help_show_topic` had existed since the help did and nothing called it:
the help could be opened at a page and there was no way to ask it to be, so
somebody stuck in the Control Panel had to find Help and then find the right
page in it.

An application declares its page in its `recon_appwin_impl`, and one with views
of its own moves it with `recon_appwin_set_help_topic` as it moves between
them. Declared rather than worked out by the shell from a window's title,
because the two are not the same thing and should not be made to look like it:
"Watchtower" is a window, "Programs" is a page.

Wiring it up found that `recon_help_show_topic` chose the page and did not ask
the window to redraw, so it went on showing the one before — which looks
exactly like the topic not being found. It was found by asking the window what
it thought it was showing (`ui app`), not by looking at the screen: the screen
had nothing to say about the difference. The refresh is inside
`recon_help_show_topic` now, because every caller of it wants the window to
show what it has just been told to.

### A build with no warnings in it

Twenty-six warnings across fourteen sites, all `-Wformat-truncation`, all of
the form *this snprintf might cut its argument short*. A build with fourteen
warnings in it is a build where the fifteenth is invisible — which is how
Alt+Tab stayed dead for months in a tree that was compiling clean apart from
the noise nobody read any more.

Eleven were telling the truth. A window title has room for a name and not a
path; a status line quoting something somebody typed should stop at the edge
of the line; a note about why a module would not load is a sentence, not a
record. Cutting is what those want. They say so now, through
`recon_text_copy` and `recon_text_printf`, which name the intent rather than
leaving the compiler to guess at it. Both are `static inline` in `ReconOS.h`
because half a dozen test binaries link a handful of source files each, and a
new object for five lines would mean adding it to every one of those lists
and to every one added later.

Three were not, and are fixed rather than declared:

- The File Explorer's drop-down built the path to each subfolder by pasting
  strings. A path cut short is a path to somewhere else, and this one is
  compared against the known places and then navigated to.
- The wallpaper and avatar lists stored a shortened name when one did not
  fit. That name is what the file is asked for by later, so a shortened one
  is not a picture with a shorter name — it is an entry in the list that
  cannot be loaded, offered to somebody who will click it. They are skipped
  now.
- Moving a file to the bin pasted its destination the same way.

The remaining risk is the obvious one: a wrapper that hides truncation
warnings for every argument at once is only correct while it is used for text
a person reads. Its comment says so twice.

### Typing in the Start menu

The menu never took a key — not even Escape — so the only way to reach an
application was to find its name with the mouse, in a list whose only order is
alphabetical. Backlog item 3 asked for categories and a search box; what it
actually needed was for the menu to hear the keyboard at all.

Typing narrows to names containing what has been typed, and forces the
alphabetical order whatever mode the column was in: somebody typing letters is
looking for a name, and most-used is not an order you can look a name up in.
The line saying what is being looked for appears only once something has been
typed — a search box standing empty in a menu of seven applications is
furniture.

The arrows move the highlight and Enter opens it, falling back to the first
match when nothing is highlighted, so three letters and Enter is enough. The
highlight is the same `menu_hover` the pointer sets, so a menu driven from the
keyboard looks like one driven from the mouse and the mouse takes over without
a fight.

Escape steps back one thing at a time — what was typed, then the long list,
then the menu — because closing outright throws away a search somebody is
halfway through correcting.

Categories are still not done. With seven applications there is nothing to
categorise, and a taxonomy invented before there is anything to sort is a
taxonomy that will be wrong.

### The bin, from outside a window

Building the above needed a `bin` command, because the Recycle Bin could only
be reached from the File Explorer — nothing outside a window could look at it,
put anything back, or empty it, so nothing could test that emptying it works.
`bin` lists, `bin <name>` fills, `bin restore` and `bin purge` act on one item,
`bin empty` clears it.

`del` is deliberately not this: it has meant "gone" since DOS, and a word that
has meant one thing for forty years is a poor place to put a surprise. But it
does mean the two ways of removing a file behave differently, which is worth
being able to see rather than worth hiding.

## What is known to be missing

Collected from using it. Nothing here is started.

**The skin editor sets colours and nothing else.** A ramp can be removed but
not set, a frame measurement cannot be touched at all, and a skin cannot be
renamed or deleted from the page it is edited on. Those are still done by
editing the file by hand.

**Buttons in applications round off; nothing else does.** `metric.corner` is
read by the window frame and `metric.button-corner` by every button, and a
list, a text field and a menu are all still square whatever the skin says.

**No paint program.** Nothing in ReconOS draws a picture; the icons and
avatars it ships are drawn by code rather than by anybody.

**No TLS.** Remote access over the network sends its key in the clear, which
is why it is off by default and why every place that offers it also offers the
SSH-forwarded socket. The forwarded socket is not a workaround — it is the
right answer for reaching a machine across a network somebody else can see —
but a system that owns its own machine will not have an SSH to lean on, and
then this has to exist.

**The firewall cannot filter packets**, and will not until ReconOS has a
network stack of its own. What it governs is what ReconOS itself opens and
accepts, which is a real boundary and is not the same boundary.

**A client window's title bar carries a generic icon.** A Wayland client
hands its compositor an `app_id`, not a picture; guessing an icon from a
reverse-DNS string would be wrong more often than right, and looking one up
against `/System/Icons` needs a mapping nothing writes yet.

**Help has no search, and no way in from where you are.** The topic list is
short enough to read, but the change log makes it thirty entries and neither
half can be searched. Nor can an application ask for the page about itself:
`recon_help_show_topic` exists and nothing calls it, so somebody stuck in the
Control Panel has to find Help and then find the right page.

**Calculator is still deliberately shallow.** Notepad can select, cut, copy,
paste, undo, find and replace; the Calculator does arithmetic and nothing
else, which is the right amount for a thing that exists to prove an
application can be a module.

**A package cannot ship a setting or a data file.** The manifest understands
a module and an icon; anything else a program wants to place has nowhere to
be declared. And there is no signing, no dependency between packages, and no
upgrade -- installing over an existing package is refused rather than
replacing it.

**Only text files open.** `recon_props_opener` maps a name to an application
and knows about text; a picture has nowhere to go because nothing views one,
and the mapping is a list in one function rather than anything a program can
register itself with.

**Nothing can listen, and nothing is encrypted.** Streams connect outwards in
the clear; there is no TLS and no way to accept a connection.

**Where a title bar's buttons sit is fixed.** A skin can set the frame's
height, border, corner radius and button size; it cannot move the buttons to
the left, or ask for fewer of them.

**No recovery, no advanced startup, no reinstall.** All three need a kernel
and a bootloader, and all three have a Control Panel page saying so.

**Relay is a name, not a language.** Modules are native code loaded by
`dlopen` behind an ABI check, which is a real gate; the language and its
interpreter do not exist.

### On the skin system

The shell is meant to support four interchangeable layouts sharing one set of
underlying behavior: a native ReconOS look, plus Windows-like, Mac-like, and
Linux-like arrangements. v0.1.0 ships only the native skin, but the chrome must
be data-driven from the start — four hardcoded rendering paths would be a
rewrite later.

## What ReconOS has

**The system**

- A filesystem with one root it cannot see outside of, laid out as `/System`,
  `/Apps`, `/Users`, `/Temp`. `/System` is protected from deletion, and a path
  that would climb out of the root is refused rather than clamped.
- User accounts, each with Desktop, Documents, Downloads, Music, Pictures and
  Videos. The administrator is created on first run.
- A command interpreter running ReconOS commands against ReconOS — not a Unix
  shell, and with no way to reach the host or run host programs.
- A control socket carrying those same commands, so a running system can be
  examined from outside. No authentication: private only as far as its file
  permissions, which is adequate locally and is not adequate over a network.
- Icons loaded by name from `/System/Icons`, in `.ico` or `.png`.

**The desktop**

- Taskbar listing every window, client or built-in, with minimize and restore.
- Apps menu, and a Ctrl+Alt+Del box that dims the desktop behind it.
- Desktop icons, as a view of the user's Desktop folder rather than a separate
  store, so anything writing a file there puts it on the desktop.
- Right-click menus on the taskbar and the desktop.
- Window frames owned by the system: minimize, maximize, close, dragging and
  resizing belong to the window framework, so an application cannot ship them
  broken because it does not implement them.

**Applications**, all native, all built on that framework

- Task Manager — processes with CPU and memory, Applications and Processes
  views, End Task
- File Explorer — browsing, folders, deletion with confirmation
- Terminal — a view onto the command interpreter, emulating nothing
- Notepad — text editing
- Calculator — arithmetic by mouse or keyboard

## Known issues

- Client windows draw their own title bars, so a client looks like whatever
  toolkit built it. Built-in windows are framed by ReconOS; extending that to
  clients needs the xdg-decoration protocol. *(Done in v0.2.15.)*
- The control socket has no authentication of its own, and does not need
  any: it is readable and writable by its owner alone, so the filesystem is
  the authentication for something that cannot leave the machine. Carrying it
  further is SSH's job (forwarding) or the network listener's (a key), both
  added in v0.2.16.
- Properties tells you what something is, not what it is for. There is no way
  to change anything from it — no read-only flag, no "open with", because
  neither exists to be changed.
- Nothing warns before a registry change breaks something. The page asks for
  the administrator's password and asks again before a removal, and that is
  all: it does not know which settings the system depends on, so setting
  `theme` to a skin that does not exist is refused by the skin system and
  setting it to nonsense in some future key would not be.

Fixed since this list was written: `include/ReconOS.h` is now the version
header and is in the build; folders and files are named by typing rather than
being given a name; and there is a login screen.
