# Roles, and the parallel machines behind them

**The grand plan.** Written down 15 September 2026 so that both sessions are
building toward the same thing rather than each toward a guess at it. Nothing
here is built. Nothing here is scheduled. This is the shape the work is meant
to arrive at, and the point of writing it early is that several decisions made
now are cheaper than the same decisions made later.

Joshua's framing: *right now the desktop track is building the workstation —
the general desktop anyone would use. It is not aware of the other modes and
does not need to be.* That is correct and deliberate. The other modes are
branches, not distractions.

---

## The one decision that is already load-bearing

**The installer installs the whole operating system, every time. It never asks
which edition to put down.**

The role is chosen on the *first boot after installation*, not during it. So
there is one install medium, one kernel, one image, and the role is
**configuration rather than a build**.

This is worth stating first because it is the decision that everything else
gets to lean on:

- One medium to build, write and verify. Five editions would be five media,
  five matrix runs and five ways to ship the wrong one.
- No conditional compilation by role, so no configuration that only some
  builds ever exercise — which is the shape of fault that survives testing.
- A machine can *change* role without being reinstalled, because nothing about
  its role is baked into what was written to the disk.
- It sets a floor on the install size. That floor is a real requirement and
  should be measured rather than estimated, once there is something to measure.

**There is already a mechanism waiting for it.** The `cmdline` file on the EFI
partition carries `recovery` and `noinit` today and is read by the loader
before the kernel starts (KF-131). A chosen role is the same kind of fact: a
short word, written once, read every boot, changeable from another machine if
the first will not start.

---

## The five roles

| role | what it is | where the work lives |
|---|---|---|
| **Workstation** | a computer somebody sits at | being built now |
| **Server** | DNS, web, DHCP, DDNS | own branch |
| **Firewall** | what pfSense/OPNsense are for | own branch, and see below |
| **Thin client** | finds a server, binds to it | own branch |
| **NAS** | storage for the network | own branch |

Each gets its own desktop and layout, so the widget layer has to be
role-parameterised rather than assuming the workstation's shape.

**Isolated by default.** A server doing DHCP and DNS has no reason to reach
into a workstation, and a workstation needs nothing from it but an address.
That isolation is why these can be separate branches that rarely conflict:
they touch different directories and meet only at protocols.

---

## Parallel instances — the actual goal

On first boot a machine looks at the network for another ReconOS in the same
role. Finding one, it offers to become a **parallel** of it: authenticate,
clone the configuration, and adapt it to the hardware actually present — a
box with four disks cloning a box with three lays out what it has rather than
refusing or pretending.

Two parallel NAS machines should present to the network as **one storage
unit**. Joshua expects 5–10 GbE to be the point at which this is worth doing.

### This is two problems and they should not share a branch

**Cloning configuration onto unlike hardware is tractable.** It is close to
what `install_plan_run` already does in miniature: read what is there, decide
a layout, refuse rather than guess when it does not fit.

**Two machines presenting as one storage unit is distributed storage.**
Consistency, split-brain, what happens when a node returns holding stale data.
That is a different order of problem from the other four modes combined, and
it is where designs of this shape usually stall — not for want of effort, but
because the hard part is invisible until two nodes disagree.

Recorded here so that it is scoped as its own project from the start rather
than discovered halfway through one.

---

## What the kernel owes each role, checked rather than assumed

- **Workstation** — reachable today. Framebuffer, input, processes,
  filesystems, the block layer.

- **Server and thin client** — mostly userland, with two kernel-side gaps.
  Discovery has to run *before* the first screen, and the network stack has
  **no DHCP lease renewal**: a machine up longer than its lease keeps an
  address it no longer owns. Harmless on a test boot; not acceptable on a
  machine meant to stay up for months. (Static addressing sidesteps it and is
  a legitimate answer for a server, but not for the clients behind it.)

- **NAS** — the block layer and ReconFS exist. A network file protocol does
  not.

- **Firewall is the expensive one, and it reopens a ruling rather than
  building on one.** `docs/` records that IP fragment reassembly is *refused,
  not unimplemented* — "a reassembly queue is where a decade of security holes
  lived". That is the right call for an endpoint. It is not available to a
  firewall: a filter that cannot reassemble fragments is evadable by splitting
  a payload so no single fragment matches a rule, which is among the oldest
  bypasses there is. Firewall mode has to **overturn that decision and own the
  queue**, with the risk it was avoiding.

  It also needs packet forwarding between interfaces, connection tracking for
  NAT, and **more than one network card — only virtio-net is supported.** Real
  hardware NIC drivers are a prerequisite here, not a finishing touch.

---

## Patches and updates are different things

Joshua's distinction, and it sharpens the versioning rule rather than
replacing it:

- **A patch** makes the kernel work on hardware it was already built toward.
  Everything the Gateway has produced this month is patches: KF-214, KF-219,
  KF-223, KF-233, KF-234. The support was intended; it was wrong.
- **An update** adds hardware the kernel did not previously support. That is
  new capability and it is what a minor number is for.

So the kernel's patch number moving quickly is not churn — it is a machine
finding faults that were always there. The number stops moving when the
hardware stops disagreeing, not when the work stops.

---

## On branches

The separation Joshua wants — *the kernel is stable, the OS churns, do not let
the churn reach the kernel* — is real and worth having. It is worth being
precise about where it actually comes from.

**It comes from the directory split and the matrix, not from which branch is
called `main`.** The kernel is `kernel/` and `boot/`; the desktop is
`userland/`, `src/` and `modules/`. Two sessions have worked the same
repository for weeks and today's merge of ninety-nine files had exactly one
conflict — one line, the version number — because the tracks do not share
files. Renaming `main` would not have changed that number.

What *does* protect the kernel is `scripts/verify-kernel.sh`: 1530 self-tests
across twenty-eight boot paths, run green before anything is pushed. That is
the gate, and it is already in place.

**A role per branch is straightforwardly right** and needs no restructuring to
start. Firewall, NAS and server work touch different directories from each
other and from the desktop; they can each sit on their own branch and merge
when they work.

---

*Related: `docs/ROADMAP.md` (the desktop's plan), `docs/KERNEL.md` (the
kernel's checkpoints), `docs/KERNEL-WANTS.md` (what the desktop needs from the
kernel next).*
