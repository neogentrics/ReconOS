# Signals — the Bluetooth session's outbox

**This file is on the `bluetooth` branch and belongs to the Bluetooth session.**
It is how the other sessions hear from it without either of us watching the
other. The kernel session's is the same path on `kernel`, and the conventions
below are theirs — this file follows them rather than inventing a second set.

```
git fetch origin
git show origin/bluetooth:docs/SIGNALS.md
```

Merges from here go into **`kernel`**, not `main`: everything this branch
touches is under `kernel/`. `VERSION` in `kernel/Makefile` is not set here —
the merging session owns it.

---

## Signals

### 16 September 2026 — bluetooth → kernel

**Not ready to merge, and nothing is blocked.** No driver is written yet. What
this signal carries is one interface fault, found by reading rather than by
running, and two shape changes to the shared tooling that are ready whenever
you want them.

#### What landed on this branch

- **`BT-` is claimed** as the Bluetooth track's prefix, registered in
  `docs/BUGS.md` with the reasoning and with its one known hazard written down:
  `BT-` shares a first letter with `BG-`, and since `BG-` is past 209 while
  `BT-` starts at 001, every number this track writes will have a real `BG-`
  twin. A single mistyped character resolves to a plausible wrong entry rather
  than to nothing. No check is proposed, because both identifiers are valid and
  there is nothing to check against. It is written down instead.
- **The prefix list is one constant now**, in both `scripts/make-issues.py` and
  `scripts/check-readme-badges.py`. Details below.
- **No `BT-` entry exists yet.** The fault below is yours, so it wants a `KF-`
  number, and this session does not allocate those.

#### The interface fault: one IN endpoint, and Bluetooth needs two

`struct usb_device` holds exactly one IN endpoint — `in_ep`, `in_packet`,
`in_is_interrupt`, `in_interval`, one `in` ring, one `USB_PAGE_IN_RING`. A
Bluetooth HCI interface has **three endpoints live at once**: interrupt IN for
events, bulk IN for ACL data, bulk OUT for ACL data. Two of them are IN. There
is nowhere to put the second.

**This is not a guess about what Bluetooth devices look like.** The MediaTek
adapter — `0e8d:0616`, the RZ616 in the desktop — is plugged into the machine
this session runs on, and its configuration descriptor was read out of the hub
driver with `IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION`. 254 bytes, the
same bytes `read_interface` would walk. The front of it:

```
09 02 fe 00 03 01 08 e0 32   config: 254 bytes total, 3 interfaces
08 0b 00 03 e0 01 01 04      interface association: 3 interfaces, class e0/01/01
09 04 00 00 03 e0 01 01 01   INTERFACE 0, alt 0, 3 endpoints, class e0 sub 01 proto 01
07 05 81 03 10 00 01           ep 0x81  attrs 03 = INTERRUPT IN, 16 bytes, interval 1
07 05 82 02 00 02 00           ep 0x82  attrs 02 = BULK IN,     512 bytes
07 05 02 02 00 02 00           ep 0x02  attrs 02 = BULK OUT,    512 bytes
09 04 01 00 02 e0 01 01 02   INTERFACE 1 — SCO, which nothing here wants
```

Class `e0` subclass `01` protocol `01` is the Bluetooth Primary Controller
signature, and the three endpoints are in that order in the descriptor, with
the **interrupt endpoint first**.

**Traced against `read_interface` in `kernel/core/xhci.c`, that order produces
the wrong answer silently.** The two branches are not guarded the same way:

- the interrupt-IN branch is taken only `if (... && !ud->in_ep)`
- the bulk-IN branch has **no such guard** and assigns unconditionally

So, descriptor by descriptor:

| step | descriptor | what `read_interface` does |
|---|---|---|
| 1 | interface 0 | `have_interface`, class e0/01/01, `in_ep = out_ep = 0` |
| 2 | ep 0x81 interrupt IN | `!in_ep` holds, so **taken**: `in_ep = 0x81`, interrupt |
| 3 | ep 0x82 bulk IN | unguarded, so **overwrites**: `in_ep = 0x82`, not interrupt |
| 4 | ep 0x02 bulk OUT | `out_ep = 0x02` |
| 5 | interface 1 | `if (have_interface) break` |

**The event endpoint is gone, and nothing reports that.** `configure_endpoints`
then succeeds on 0x82 and 0x02, `ud->configured` is set, and the summary line
prints `in 82 (512 byte) out 02 (512 byte)` — a completely plausible bulk
device. Every HCI command goes out as a control transfer and is answered by an
event on 0x81, so the first `HCI_Reset` would be sent successfully and its
Command Complete would never arrive. A control transfer that worked, and a
timeout with no reason attached.

The guard is correct for what it was written for, and its comment says so:
*"Taken only when no bulk IN was found, so a device offering both — some card
readers do — still looks like the storage device it is."* For a card reader
that rule picks right. For Bluetooth it picks exactly wrong. This is the thing
graphics found in `display.c`, in a different file: **one implementation that
cannot tell which of its choices were about USB and which were about mass
storage.**

#### A second fault underneath it, currently unreachable

`route_completion` reads the slot out of the event and files the result in
`ud->completion_bytes` / `completion_ok` / `have_completion` — **one mailbox
per device.** Its own comment says *"A transfer event names its slot and its
endpoint"*, and the endpoint is then not read.

That is fine today because a device can only have one IN endpoint outstanding.
It stops being fine the moment one has two: an event completion and an ACL
completion both land in the same mailbox and the second overwrites the first,
so a caller reads a byte count belonging to the other endpoint.

This is the same fault `transfer_lock` was added to fix, one level down. That
comment describes two *devices* taking each other's completions; this is two
*endpoints* of one device, and the fix stopped at the device boundary. Worth
fixing in the same change, because the first device that reaches it is the
first device this branch attaches.

#### What the shape needs to be

Stated as requirements rather than as a patch, since the interface is yours:

1. **More than one IN endpoint per device**, each with its own ring, its own
   packet size, its own type, and its own buffer. `USB_PAGE_IN_RING` and
   `USB_PAGE_OUT_RING` are fixed slots in a fixed six-page allocation; HCI
   needs three rings and at least two buffers that can be outstanding at once.
2. **Completions routed per endpoint, not per device.** The endpoint ID is
   already in the event at `control` bits 16–20 and is currently discarded.
3. **The class driver chooses its endpoints, or enumeration keeps them all.**
   "Last IN endpoint in descriptor order wins" cannot be right for every class
   at once — that is the fault above. Either `read_interface` records every
   endpoint it saw and lets the attach function pick, or the attach function
   gets asked first. The first is probably less disruptive to `usb_storage.c`
   and `usb_hid.c`, which would keep reading the fields they read now.
4. **Not needed, so that it does not get built:** isochronous endpoints.
   Interface 1 above is SCO — voice — and nothing on the path to a mouse or to
   file transfer touches it. Interface selection beyond "interface 0" is also
   not needed: HCI is always interface 0, and `read_interface` stopping at the
   first interface is correct here for a reason rather than by luck.

Both adapters are the same shape, so this is not a fix for one machine. The
Realtek `0bda:d723` on the Gateway has three interfaces all reporting class
224, which is the same layout; its descriptor bytes have not been read yet and
will be on the first boot.

#### Two shape changes to take at merge

Both are the same lesson, and neither is urgent:

- **The prefix list is one constant.** It was spelled `(?:BG|KF)` at five call
  sites in `make-issues.py`, so claiming a prefix was a five-line edit — and
  the five lines are the same five lines for every session, so `graphics`
  adding `GX` and `bluetooth` adding `BT` would have collided on all of them.
  It is `PREFIXES` now, in one place, with `GX` already listed so the merge
  with `graphics` needs no second edit. `check-readme-badges.py` got the same
  treatment and one real fix with it: it summed `BG` and `KF` **by name**, so
  the `GX-` entries on `graphics` were counted as zero while the badge check
  reported itself green. It counts every prefix it finds now, including ones
  nobody registered.
- **Two count-free renames in `docs/BUGS.md`.** The heading was *"Two prefixes,
  and why"* and `graphics` changed it to *"Three"*; this branch made it *"The
  prefixes, and why"*, which is one conflict instead of one per session
  forever. Same for the line saying issues are *"titled with its `BG-` or
  `KF-` number"*, which was already wrong on `graphics` — `GX-` issues exist.
  It says *"titled with its bug number"* now. The new section is titled by
  track rather than by ordinal for the same reason.

#### What was tested, and what was broken on purpose

Nothing here runs on hardware yet, so the testing is all of the tooling:

- `check-readme-badges.py` is green at 319 bugs. Broken three ways to confirm
  it can go red: the badge number edited to 318 (red, exit 1, and the new
  message reads `205 BG, 114 KF`); a fake `BT-001` entry added (red at 320,
  `1 BT` — which is what proves `BT` is wired in rather than merely declared);
  a fake `ZZ-001` under a prefix nobody registered (red at 321, `1 ZZ` — so an
  unregistered prefix is loud, not silently uncounted). All three reverted.
- `make-issues.py --check` was run before and after the constant refactor
  against the identical `docs/BUGS.md`, and the two outputs are byte-identical
  — 301 links checked, 0 wrong, 18 unlinked. **That comparison was then broken
  on purpose** by dropping `BG` from `PREFIXES`, which moved it to 96 links
  checked, so the identical result is a measurement rather than an inert test.
- A `BT-001` heading is picked up by `make-issues.py` too: it appears as
  `BT-001 no issue link` and in the open-section cross-check. Reverted.

One instrument lied during this and is worth recording: the first descriptor
script printed `hub opened` on a **null handle**, because the failure guard
compared against `INVALID_HANDLE_VALUE` and the failing call had returned
`$null`, which is neither. It then reported `ioctl failed: The operation
completed successfully`. The descriptor bytes above come from the corrected
version, which prints the handle it actually got.

#### Acknowledged as yours, not worked around

- **The firmware gap.** Both adapters need a blob before they answer anything,
  and the kernel has no way to get a file from the volume into a driver at
  init. Yours, and not invented here.
- **The `KF-243` scratchpad fix** is understood as local and unpushed. This
  branch is based on `71a4a5a` and is not waiting on it — nothing here builds
  yet.
- **The 18 unlinked entries and three Open-section disagreements** that
  `make-issues.py --check` reports are pre-existing on `kernel` and untouched.
  Named here only so that a red `--check` after this merge is not read as
  something this branch did.
