# What an application is

A decision, taken 2026-09-05, about what ReconOS means by "application" once it
runs on its own kernel — and therefore what the module ABI is for.

It is written down because checkpoint 15 builds an installer, and an installer
is what fixes the meaning of the word. By then the answer has to be true rather
than intended.

---

## The question

ReconOS has two ways to put a window on screen and has always had both.

**In-process.** A module registers a `struct recon_appwin_impl` — fourteen
function pointers — and the compositor calls them: paint into this panel,
here is a key, here is a click on region 6. The Calculator works this way.

**Out-of-process.** A program connects over Wayland, owns its own buffer,
draws into it, and commits a frame. ReconOS has hosted these since v0.1.0 and
draws their title bars.

Everything a person could install today takes the first path. The question is
whether that survives having a kernel.

---

## The observation that decides it

From the kernel session, and it is correct:

> A function pointer means nothing across an address space.

`recon_appwin_impl` is a table of function pointers, and `draw` is handed a
`struct recon_panel *` — a pointer into the compositor's own memory that the
application writes pixels into. There is no version of that which crosses a
process boundary. It is not an ABI that needs versioning more carefully; it is
an ABI whose fundamental shape assumes shared memory and one address space.

The direction has to invert. Today the compositor calls the application. What
survives separation is the application owning its buffer, drawing when it
likes, and **submitting** a finished frame with a damage region — the
compositor never calling in, only compositing what it is offered.

That is Wayland. Which ReconOS already implements, and already hosts clients
for.

**So the answer is not a new module ABI. It is a migration onto a path that
already exists.**

---

## Three things that were checked rather than assumed

### 1. Is the round trip fast enough?

An in-process paint is a function call. A submitted frame is a round trip
through the compositor's event loop. Measured on a running ReconOS, with a
client submitting as fast as it is allowed and damaging the **whole** surface
every frame — the pessimistic case, since real applications damage less:

| | |
| --- | --- |
| mean | 18.96 ms (53 frames a second) |
| median | 18.90 ms |
| 95th percentile | 19.43 ms |
| worst | 19.69 ms |
| best | 17.93 ms |

**The spread between best and worst is 1.8 ms.** The compositor is pacing the
client at its frame rate and adding essentially no jitter of its own — the
19 ms is the display's cadence, not overhead. There is no latency argument
against out-of-process applications on this system.

The number that would have decided otherwise is the worst case, not the mean:
a mean of 4 ms with a worst case of 90 is a window that feels broken however
good the mean looks. That is why the distribution is recorded and not an
average.

### 2. Can the Control Panel be a client?

No, and the evidence is a one-line comparison. Distinct ReconOS subsystems
touched:

- **Calculator** — 11, of which 8 are drawing primitives. Nothing
  system-owned.
- **Control Panel** — 35, including the registry, accounts, the firewall,
  modules, TLS, volumes, displays, fonts, wallpapers, packages, processes and
  the shell itself.

The Control Panel is not an application that happens to read some settings; it
is the system's control surface. Putting a protocol in front of all of that is
a larger project than the kernel is.

**So in-process is a privileged tier, not a default** — and that is the part
worth naming rather than inheriting. The rule:

> `recon_appwin_impl` is for code that is part of ReconOS and ships with it.
> Anything a person **installs** is a client.

Stated that way it stops being a compromise. Every serious system has this
split; what makes it a mess elsewhere is that the boundary is accidental.

### 3. Where does `help` go?

Into an installed application's manifest. It is a string naming a help topic —
data, not behaviour — and data belongs in a manifest rather than in a protocol
message that would be sent once and never again.

---

## What the migration is not

It is **not** turning fourteen callbacks into fourteen protocol messages. Six
of them exist only because hit-testing currently lives in the compositor:

An in-process application registers hit regions into the compositor's panel
*while it draws*, and the compositor then routes clicks by region id — which
is why `click`, `motion`, `cursor` and `context` all take a `hit_id`. A client
receives pointer coordinates and does its own hit-testing, because it is the
only one that knows what it drew.

So the surface **shrinks**. `draw`, `click`, `key`, `motion`, `cursor`,
`scroll`, `context`, `context_action`, `focus_changed` and `visibility` become
"here is input, here is a frame". What is left over is `describe` — which is a
debugging affordance and wants a protocol of its own or nothing — and
`destroy`, which becomes the client exiting.

This also makes the `RECON_MODULE_ABI` rule — *any change to a struct a module
can hold is an ABI change, including adding a field at the end* — a rule
governing a **shrinking** surface. That rule has already cost one segfault in
the Calculator, and a rule that gets cheaper over time is the right shape for
one that expensive.

---

## What the kernel owes this

Named by the kernel session, and none of it is a detour — checkpoints 11 to 13
need the first two anyway:

1. An address space per process, extending the checkpoint 5 page-table work.
2. A page mappable into two address spaces at once, for the shared buffer.
3. A handle that can be passed across a system call.

Checkpoint 10 gave a privilege boundary and a per-process personality. It did
**not** give separate address spaces, and this document does not pretend
otherwise: today every thread shares one set of page tables.

---

## What is undecided

- **When.** Nothing changes until the kernel has address spaces. The in-tree
  applications keep working exactly as they do.
- **Whether the Calculator moves.** It is the one installed application today
  and the obvious first client, but moving it is a rewrite of its drawing, not
  a recompile.
- **What a client does about skins.** An in-process window asks
  `recon_theme_color` and gets the current palette. A client would need the
  palette sent to it, and to be told when it changes. That is a small protocol
  and it does not exist.
