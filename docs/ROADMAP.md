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
| Network management | Linux | ReconOS |
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
| 7 | Skin system plumbing — chrome driven by data, not hardcoded | |
| 8 | Minimal first-run setup flow | |

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

- `include/ReconOS.h` is unfinished scaffolding from an earlier iteration and is
  not currently part of the build.
- Client windows draw their own title bars, so a client looks like whatever
  toolkit built it. Built-in windows are framed by ReconOS; extending that to
  clients needs the xdg-decoration protocol.
- The control socket has no authentication.
- There is no way to type a name, so New Folder and New Shortcut pick one.
- Properties is in the context menu and disabled: there is no properties view
  yet, and hiding the entry would suggest there never will be.
- No login: the system starts as the administrator.
