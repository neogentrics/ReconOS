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
| 2 | Window management — focus, move, resize, close, alt-tab | **Done** |
| 3 | Shell chrome — taskbar, drawn by the compositor itself | **Done** |
| 4 | Apps menu and application launcher | **Done** |
| 5 | Native first-party applications | **Done** — six of them |
| 6 | Idle CPU/RAM baseline established and enforced | **Done** — 0.00% CPU, 17MB |
| 7 | Skin system plumbing — chrome driven by data, not hardcoded | **Done** — 47 roles, 9 skins |
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

Two things found while doing it. A socket path over 107 bytes was silently
truncated by the kernel, so bind used one name while the unlink, the chmod and
the shutdown cleanup used another -- the socket would have come up unprotected
under a name nothing could remove. And the startup line reporting the network
compared the gateway against a raw NUL *byte* written into the source instead
of the escape, so a machine with no gateway did not say "(none)".

## What is known to be missing

Collected from using it. Nothing here is started.

**No custom skins.** The format exists and there is no way to install one.

**No paint program, and no properties dialogs.** Desktop icon positions are
not remembered. Client windows still draw their own decorations. The control
socket still has no authentication, only file permissions.

**Nothing can listen, and nothing is encrypted.** Streams connect outwards in
the clear; there is no TLS and no way to accept a connection.

**Every skin fills flat.** Gradients are the single thing keeping Beacon from
looking like the era it is reaching for.

**No boot splash.** The system appears without announcing itself.

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
- Properties is in the context menu and disabled: there is no properties view
  yet, and hiding the entry would suggest there never will be.
- The registry can be read from the Control Panel but not changed there. The
  terminal's `reg` command changes it for anyone who means to.

Fixed since this list was written: `include/ReconOS.h` is now the version
header and is in the build; folders and files are named by typing rather than
being given a name; and there is a login screen.
