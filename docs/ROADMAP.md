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
| 2 | Window management — focus, move, resize, close, alt-tab | Next |
| 3 | Shell chrome — taskbar and start button via `wlr-layer-shell` | |
| 4 | Start menu and application launcher | |
| 5 | A native first-party application | |
| 6 | Idle CPU/RAM baseline established and enforced | |
| 7 | Skin system plumbing — chrome driven by data, not hardcoded | |
| 8 | Minimal first-run setup flow | |

### On the skin system

The shell is meant to support four interchangeable layouts sharing one set of
underlying behavior: a native ReconOS look, plus Windows-like, Mac-like, and
Linux-like arrangements. v0.1.0 ships only the native skin, but the chrome must
be data-driven from the start — four hardcoded rendering paths would be a
rewrite later.

## Known issues

- `include/ReconOS.h` is unfinished scaffolding from an earlier iteration and is
  not currently part of the build.
- There is no `assets/power.png`; the power button falls back to a flat colored
  square. The original asset was lost to a failed download.
- The cursor is a plain red square rather than a real cursor theme.
- Windows render but cannot yet be focused, moved, resized, or closed — that is
  checkpoint 2.
