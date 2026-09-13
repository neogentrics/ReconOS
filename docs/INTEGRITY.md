# Keeping the kernel from being modified

Asked for as *"a security layer for the kernel so that outside sources can't
edit, modify, or change it"* — something like BitLocker or Gatekeeper, refusing
to load what it should not without permission from the Control Panel.

Two different mechanisms get merged under that heading and they solve different
problems. Separating them first, because building the wrong one is worse than
building neither: it produces a system that *feels* protected.

| | Protects against | Does not protect against |
|---|---|---|
| **Encryption** (BitLocker) | somebody taking the drive out and reading it | a modified kernel. A decrypted, running system runs a tampered one happily |
| **Code integrity** (Secure Boot, Gatekeeper) | running something that is not what it claims to be | somebody reading the disk |

The request is the second one.

## The part that decides whether any of it is real

**A verification is worth exactly what its root of trust is worth.**

If our bootloader checks the kernel but anybody can replace our bootloader, an
attacker replaces both and the check is theatre. Every signature scheme has to
answer *what checks the checker*, and the answer determines whether the whole
thing is security or decoration.

The chain, from the bottom:

```
firmware  ──verifies──▶  reconboot  ──verifies──▶  kernel  ──verifies──▶  drivers, programs
   ▲
   └── UEFI Secure Boot, keys held in firmware.
       This link does not exist yet.
```

Everything above the missing link is worth building and is honest about what it
buys. The link itself is a **decision**, not code:

1. **The machine's owner enrols a ReconOS key into their firmware.** Works on
   any machine, costs nothing, and is a procedure a person has to be walked
   through — into their firmware settings, with the machine's own interface.
2. **Our loader is signed by Microsoft's UEFI CA, via shim.** Works out of the
   box on everything, and means accepting somebody else's signing policy and
   review process.

Until one of those is chosen, signature-checking the kernel is still worth
having — it catches a corrupted download, a half-written update, a disk going
bad, and casual tampering. It is **not** protection against an attacker who can
write to the EFI System Partition, and this document says so rather than letting
the feature imply otherwise.

## What is being built now

The loader verifies the kernel before jumping to it.

- A detached signature beside the kernel on the ESP, so the kernel file itself
  stays an ordinary ELF that every other tool can still read.
- The public key is **compiled into the loader**, not read from the disk beside
  the signature. A key read from the same volume as the thing it verifies is
  not a check: an attacker who can replace the kernel can replace the key.
- Verification failure is a **refusal to boot**, with the reason on screen. Not
  a warning, not a countdown: a kernel that fails its signature is either
  damaged or hostile, and both are reasons to stop rather than to continue after
  five seconds because nobody was watching.

### And the one thing that must not happen

There is no flag that turns it off. This project's standing rule about safety
checks applies here more than anywhere: a verification with an override is a
verification an attacker turns off, and an override that exists "for
development" is the one shipped by mistake.

Development builds are handled by *signing them*, which is a build step, not a
runtime switch.

#### The exception, which was found by reading a test's own output

That paragraph was not the whole truth, and the loader has been saying so on
every boot:

    signature    : not checked (this loader was built without a key)

`verify_kernel()` is compiled out entirely when `RECONOS_KEY_PRESENT` is not
defined, so **a loader built without a key runs anything.** There is no runtime
flag — the claim above is true as far as it goes — but a build-time absence
reaches the same place, and a sentence that says "no way to turn it off" while a
build says otherwise is the shape of mistake this register is full of.

Why it is still defensible, stated rather than assumed: a keyless loader is a
build artifact of the test rig, and every harness that is *about* signing
generates a key first — `scripts/signed-kernel-test.sh` proves the loader runs
one signed kernel and refuses four that are not. Putting a keyless loader on
somebody's ESP means replacing their loader, which is the root-of-trust problem
already described above and is not made worse by this.

**What has to change so this cannot ship:** the release build must fail when no
key is present, rather than quietly producing a loader that verifies nothing.
Written down now, while it is a two-line makefile guard, rather than after a
release goes out with the announcement scrolling past on line four.

## The run-time half, which is the desktop's

Refusing to load code the system has not been told to trust — the
Gatekeeper-shaped part, adjustable by somebody with authority over the machine —
mostly belongs to the desktop rather than the kernel.

The kernel's share is the enforcement point for anything loaded into its own
address space, and the per-process syscall personality from checkpoint 10 is
already the seam a compatibility layer hangs off. A program running under a
Windows or Linux personality is exactly the case where "should this be allowed
to run" needs an answer, and the mechanism to ask is already in place.
