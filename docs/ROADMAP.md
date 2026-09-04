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

Fourteen sites remain, all the same shape: a name copied out of a path into a
name-sized buffer, where the bound holds in practice but the compiler cannot
see it. (A build reports them 26 times, because `recon_fs.c` and its
neighbours are compiled into seven targets — the site count is the honest
number.) Worth a sweep; not worth silencing individually.

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

## What is known to be missing

Collected from using it. Nothing here is started.

**Custom skins can be installed but not written from inside ReconOS.**
`theme install` takes a file and `theme remove` takes it away; there is no
editor for one, so writing a skin still means writing a text file.

**No paint program, and no properties dialogs.** Desktop icon positions are
not remembered. Client windows still draw their own decorations. The control
socket still has no authentication, only file permissions.

**Nothing can listen, and nothing is encrypted.** Streams connect outwards in
the clear; there is no TLS and no way to accept a connection.

**Window frame shapes and chrome geometry are fixed.** A skin can recolour a
title bar and now ramp it, but not change its height, its corner radius, or
where its buttons sit.

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
  clients needs the xdg-decoration protocol.
- The control socket has no authentication. It is now created readable and
  writable by its owner alone, so no other account on the machine can drive
  ReconOS through it, but that is a file permission rather than a login and it
  is not enough to carry the socket off the local machine.
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
