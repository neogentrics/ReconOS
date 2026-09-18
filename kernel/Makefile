# ReconOS kernel build.
#
# Deliberately a Makefile and not CMake, and deliberately separate from the
# CMake build at the top of this repository. Those two builds target different
# machines: the compositor is a Linux program that links against wlroots, and
# the kernel is a freestanding image that links against nothing at all. Making
# one build system serve both means every kernel flag needs a guard saying it
# does not apply to userland, and vice versa.
#
#   make ARCH=x86_64          build (the default architecture)
#   make ARCH=aarch64
#   make all-arches           build every architecture, which is how a change
#                             is checked for having stayed portable
#   make run ARCH=aarch64     boot it under QEMU, serial on this terminal
#   make check-portable       fail if core/ has learned about a machine
#   make clean

# What works, not what is planned.
#
# **A patch per bug fixed.** The six that 0.1.0 sat through -- KF-163, 182,
# 183, 184, 185, 186 -- are why it is not 0.1.1; KF-188 made it 0.1.7, and
# KF-189, KF-190 and KF-191 make it 0.1.10, KF-193 makes it 0.1.11, and
# KF-194, KF-195 and KF-196 -- all three from the network stack -- make it
# 0.1.14. KF-197 and KF-198, both out of making a device able to go away,
# make it 0.1.16, and KF-199 makes it 0.1.17.
#
# **0.2.0 on 12 September 2026**, because the rule was met rather than because
# the change felt large: every row in sections 1.1 to 1.9 of the blueprint audit
# is Built. That is a fact about those tables anybody can check, which is the
# whole reason it was written as one. KF-199's fix rides with it, so the version
# line opens at 0.2.1. The number had held still through
# all six and twenty-five commits, not because anything forbade a bump but
# because nothing asked for one. That is the same failure as a counter nobody
# prints: it is only correct while somebody remembers.
#
# **KF-187 has not earned one and is not counted.** Half of it is fixed -- the
# two volume boots are asserted now, which is what found the three above. The
# other half cannot even be exercised yet, because the only second-boot-on-one-
# disk in the matrix turns out to have no volume on it (KF-192). A partial fix
# does not move the number.
#
# **KF-189 is why this rule needed writing down at all.** Raising the version
# used to rebuild nothing, so the kernel went on printing the old number while
# the Makefile claimed the new one -- and a full verification run passed against
# the mislabelled binary. Every object depends on this file now.
#
# **0.2.0 is not reached until sections 1.1 to 1.9 of the blueprint audit are
# all built.** Joshua's rule, and a better one than "a minor when it gains a
# capability" -- that version was a judgement call about what counts as big,
# made by whoever happened to be bumping the number. This one is a fact about
# the audit that anybody can check.
#
# **Five rows stand in the way**, plus the whole of 1.8:
#
#   1.3  IPC              signals, sockets, message queues, FIFOs
#   1.4  HPET             the timer wheel is built; no HPET, and nothing needs one
#   1.6  ext2/4 + procfs  ramfs and devfs are built; these two are not
#   1.7  USB host         xHCI is built; no EHCI, no hubs, no hot-plug
#   1.7  I2C / SPI        nothing needs them yet
#   1.8  Network          all of it
#
# Corrected 12 September. This comment previously said eleven rows and listed
# self-relocation, video, embedded filesystems, initrd and UEFI among them.
# **Those are section 2 -- the bootloader -- and the gate is 1.1 to 1.9.** The
# script that produced the list matched only headings beginning `1.`, so five
# bootloader rows were filed under 1.9 and counted against a gate they are not
# part of. 1.1, 1.2, 1.5, 1.8 and 1.9 have no open rows at all.
#
# 0.1.0 was raised from 0.0.11 on the older argument -- the kernel had stopped
# fixing things and started gaining them -- and a patch number that only ever
# goes up is a version that has stopped saying anything. That reasoning still
# explains 0.1.0; it is not what decides 0.2.0.
#
# **0.2.15 on 13 September 2026, for KF-211.** Entering user mode on aarch64
# programmed ELR_EL1 and SPSR_EL1 with interrupts on -- and those are the
# registers exception entry writes. A tick landing in that window overwrote
# ELR_EL1 with a kernel PC, the handler restored it faithfully, and the eret
# then dropped to EL0 at an address inside arch_enter_user. Any user program on
# this architecture could fail to start, at random, and has been able to since
# checkpoint 10. x86_64 never could: its return frame is on the stack, where an
# interrupt cannot reach it.
#
# **0.2.14 on 13 September 2026: the suspend framework.** No bug, and chosen
# rather than compelled -- the same call as 0.2.5, and for the same reason. Every
# boot now says by name what would not survive a suspend, which is a different
# machine from one that did not know. Suspend itself still refuses, deliberately:
# there is no wake source, and a machine told to sleep with nothing to wake it
# does not come back.
#
# **0.2.13 on 13 September 2026, for KF-210.** The crash-consistency test proves
# its checker against a known-good image before believing any round, and made
# that image by booting the guest and killing it after six seconds measured from
# launch. KF-160 fixed exactly this for the rounds below it -- they wait for the
# guest to say it has started replacing -- and left the control holding a
# stopwatch. Measured: the guest commits after 4.17 s idle, a thirty per cent
# margin, in a rig that runs many guests at once. It waits for the same line
# now. A bigger number would have to be big enough for the busiest the rig ever
# gets, which is not a thing anybody can know.
#
# **0.2.12 on 13 September 2026, for KF-209 -- and checkpoint 21 is finished.**
# A program can open /dev/fb0, be told what the screen is, map the whole of it
# and draw. Tearing that program down then handed the framebuffer's pages to
# the page allocator, because the teardown knew about two kinds of page it must
# not free and a mapped device was a third. On x86_64 that is silent: the
# aperture is above the bitmap's base, both bounds checks pass, and the screen
# is given to whoever allocates next. aarch64 panicked, because there the
# aperture is below RAM. The allocator answers whether a page is its own now,
# rather than the teardown carrying a list of exceptions.
#
# **0.2.11 on 13 September 2026, for KF-208.** The BIOS tamper test wrote an
# `A` at a fixed offset to make a kernel differing from the one that was signed,
# and byte 40000 of the kernel became an `A`. So the tampered file *was* the
# signed file, the loader said `signature: good` because it was good, and the run
# reported the worst fault this project could have -- a boot chain accepting a
# kernel changed after signing. It flips a bit now, and then checks that the file
# actually changed, which is the half that was missing.
#
# **0.2.10 on 13 September 2026, for KF-207.** The power-off check greps the
# boot log for `power:` and calls a hit a failure, because that is how
# `power_off_or_say_why` reports one. Checkpoint 24 then added a *passing*
# self-test line beginning `power:`, so the check has been red since 0.2.5 on a
# machine that powers off exactly as asked -- and, worse, had stopped being able
# to tell the two cases apart. It asks the positive question now: on success the
# guest dies inside `power_off()`, so `Powering off.` is the last line.
#
# **0.2.9 on 13 September 2026, for KF-206, which 0.2.7 caused.** `timer_start`
# filed deadlines at `wheel_now + ticks` -- the wheel's hand plus the delay --
# and that was right while the hand *was* the clock. KF-204 made them two
# numbers, the clock now runs up to a tick ahead of the hand between interrupts,
# and every timer in the kernel could fire ten milliseconds early. Measured from
# `time_ticks()` now, which is what every assertion in the file already uses.
#
# Worth saying plainly: **the fix caused this, not the test.** Removing a second
# author turned an identity into an inequality, and the one line that depended
# on the identity had no way to announce itself.
#
# **0.2.8 on 13 September 2026, for KF-205.** The procfs snapshot test compared
# two *different* snapshots and required them to be the same length -- so it
# went red whenever the millisecond field gained a digit in between, about half
# a percent of boots, and could only catch the smear it exists for by the same
# accident. It reads the same snapshot twice now, rewound, which needed a `seek`
# on procfs that should have been there anyway.
#
# **0.2.7 on 13 September 2026, for KF-204** -- and this one the rule
# compels. The tick count had two authors, an interrupt that incremented it and
# a resync that set it from the clock, so it double-counted and only ever
# forward. The wheel is turned against that count, so the wheel outran the clock
# and *every filed timer in the system fired early*, by a margin that grew with
# every idle cycle. Found on the 8K path because that path idles longest; every
# other path was carrying it under the threshold.
#
# **0.2.6 on 13 September 2026, for KF-203.** The console scrolls by redrawing
# its whole grid, which was a million writes when firmware chose the mode and is
# thirty-three million now that the kernel does. It is bounded to a window.
# Filed and numbered separately from KF-204 because I fixed this one first
# believing it was the other, and it was still worth fixing -- but a symptom
# getting smaller is not a cause being found.
#
# **0.2.5 on 13 September 2026: checkpoint 24's tick.** No bug, and the rule is
# a patch per bug *fixed* -- so this one is not compelled, it is chosen. Every
# idle processor on every machine now behaves differently, and a version that
# cannot tell the two apart is a version that has stopped identifying the
# build. The rule mandates a bump for a bug; it has never forbidden one.
#
# **0.2.4 on 13 September 2026, for KF-202** -- found while building
# checkpoint 21, because a new self-test showed up on every boot path except
# the UEFI ones. Booting the install image writes OVMF's variables into it,
# which makes it newer than the kernel it was built from, which makes `make
# esp` a no-op for ever. Checkpoint 21's mode setting rides along in this
# version: it is a feature, and the rule is a patch per bug fixed.
#
# **0.2.3 on 13 September 2026, for KF-201** -- found by the matrix run that
# 0.2.2 required, on a tree whose only change was a version string. The bounded-
# queue self-test had been asserting an overflow it could not force, and passing
# wherever the machine was slow enough to let the queue fill. It had never
# exercised the drop path it exists to check.
#
# **0.2.2 on 13 September 2026, for KF-200**, and it is worth saying why the
# obvious objection was refused. KF-200 is in `scripts/`: nothing it changed
# alters a kernel instruction, and raising this line rebuilds the whole tree --
# including the tree matrix 30 had just been run against. So the case for
# skipping it was real, and it was still the wrong case, because *the change
# does not feel large enough* is precisely the reasoning this comment exists to
# refuse. A rule that only applies to changes somebody already agrees are big
# is not a rule. The matrix runs again; that is the price and it is affordable.
#
# Written here rather than in a document because this is the line that has to
# change, and a rule kept anywhere else is a rule read after the fact.
# 0.2.36: the desktop session reached 0.2.34 and this one reached 0.2.31, from
# the same 0.2.29, without either being able to see the other. Two of this
# session's three bugs were fixes, so they are laid after theirs rather than
# interleaved -- see KF-230 and KF-231, which say so on their own entries.
# 0.2.38 and 0.2.39 were allocated in parallel and did not collide, which is
# the first time that has been true. This session took 0.2.38 for KF-233 on the
# kernel branch; the other saw it there, stepped over it, and took 0.2.39 for
# SYS_MAP's anonymous mapping. Both numbers stand and mean what their entries
# say. The merged tree is the later of the two.
VERSION := 0.2.48

ARCH ?= x86_64
BUILD := build/$(ARCH)

# --- Per-architecture toolchain and flags ------------------------------------

ifeq ($(ARCH),x86_64)
  CROSS   ?=
  ARCHFLAGS := -m64 -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mno-80387 \
               -mcmodel=kernel
  QEMU     := qemu-system-x86_64
  # The ELF carries its own boot headers -- Multiboot2 and a PVH note -- so it
  # is what gets booted directly.
  BOOT_IMAGE = $(KERNEL)
  QEMUFLAGS = -kernel $(KERNEL) -m 512M -no-reboot
else ifeq ($(ARCH),aarch64)
  CROSS   ?= aarch64-linux-gnu-
  # -mno-outline-atomics: by default gcc does not emit atomic instructions
  # directly. It emits a *call* to a libgcc helper which checks at run time
  # whether this CPU has the large-system-extension atomics and picks the fast
  # or the slow path. That is a good default for a program and useless for a
  # kernel: there is no libgcc, so the link fails on __aarch64_swp1_acq with no
  # hint as to where it came from. Inline atomics work on every ARMv8, and
  # checkpoint 3 already detects LSE for ourselves.
  ARCHFLAGS := -mgeneral-regs-only -mno-outline-atomics
  QEMU     := qemu-system-aarch64
  # aarch64 loaders read a header at the front of a *flat* image, not an ELF.
  # The ELF is kept for debugging; the .img is what boots.
  BOOT_IMAGE = $(BUILD)/reconos-kernel.img
  QEMUFLAGS = -M virt -cpu cortex-a72 -m 512M -kernel $(BOOT_IMAGE)
else
  $(error Unknown ARCH '$(ARCH)'. Supported: x86_64 aarch64)
endif

CC := $(CROSS)gcc
LD := $(CROSS)ld
OBJCOPY := $(CROSS)objcopy

# -ffreestanding: there is no libc and no runtime under us.
# -fno-stack-protector: the guard would call a libc symbol that does not exist.
# -fno-pic/-no-pie: the image is loaded at a fixed physical address.
CFLAGS := -std=c11 -O2 -g \
          -ffreestanding -fno-stack-protector -fno-pic -fno-builtin \
          -fno-omit-frame-pointer \
          -Wall -Wextra -Werror -Wno-unused-parameter \
          -Iinclude -I../boot/include \
          -DRECONOS_KERNEL_VERSION=\"$(VERSION)\" \
          $(ARCHFLAGS)

ASFLAGS := $(ARCHFLAGS) -fno-pic -Iinclude -I$(BUILD)/user
LDFLAGS := -nostdlib -no-pie -z noexecstack -z max-page-size=4096

# --- Sources -----------------------------------------------------------------

CORE_SRCS := $(wildcard core/*.c)
ARCH_SRCS := $(wildcard arch/$(ARCH)/*.c)
ARCH_ASM  := $(wildcard arch/$(ARCH)/*.S)

# core/ has one .S, and it holds no instructions -- it carries a file into
# the image with .incbin. check-portable reads these too, so "portable" keeps
# meaning the same thing here as it does for the C.
CORE_ASM  := $(wildcard core/*.S)

OBJS := $(patsubst %.c,$(BUILD)/%.o,$(CORE_SRCS) $(ARCH_SRCS)) \
        $(patsubst %.S,$(BUILD)/%.o,$(ARCH_ASM) $(CORE_ASM))

LINKER_SCRIPT := arch/$(ARCH)/linker.ld
KERNEL := $(BUILD)/reconos-kernel.elf

# --- The program the kernel loads --------------------------------------------
#
# A real ELF, linked by the ordinary cross linker, embedded in the kernel image
# and parsed at run time by the kernel's own loader.
#
# Built rather than hand-written for the same reason the partition fixtures are
# built by sgdisk and the FAT32 ones by mkfs.vfat: a loader tested against a
# structure this project also wrote is tested against its own misunderstandings.
# The linker decides the program headers here, including the ones nobody
# thought to arrange.
#
# It is embedded rather than read from a disk *for now* -- there is no path from
# a file on a volume to a running program yet, and this is the half of it that
# can be finished first. The bytes are identical either way, which is the point:
# when the filesystem half arrives, the loader does not change.
USER_ELF := $(BUILD)/user/hello.elf

# And the second one, which is written in C.
#
# hello.S proves the *loader*: two segments, a .bss, and a register convention.
# This proves the *language* -- that a program compiled from the same C the
# desktop is written in runs here at all. It is a separate program rather than
# a rewrite of the first because the first tests things a C program cannot
# check from inside itself, and losing those to prove something else would be
# a trade in the wrong direction.
PAINT_ELF := $(BUILD)/user/paint.elf
SOCKP_ELF := $(BUILD)/user/socket_probe.elf

# And the third, which is not a test.
#
# `hello.S` proves the loader and `paint.c` proves the language. This one is
# what somebody sees: it asks the machine what it is, asks the screen how it is
# arranged, asks the volume what is on it, and draws a screen a person standing
# in front of a computer that has just started can read.
#
# It is the first program built against `userland/libc/` rather than against
# nothing, which is the point of having built that library.
INIT_ELF := $(BUILD)/user/recon_init.elf

# Its own flags, and the difference from USERCFLAGS is one line: **vectors are
# allowed**.
#
# USERCFLAGS turns SSE off, which is right for the kernel's own code and was
# inherited by the user programs without anybody needing it to be otherwise --
# `paint.c` has no floating point in it. `snprintf` does: `%f` exists in
# `userland/libc/printf.c`, and on x86_64 a function that takes a double cannot
# even be *compiled* without SSE, whether or not anything calls it.
#
# Allowing it is safe here and not an assumption: `struct thread` carries 576
# bytes of vector state and `sched_switch` saves and restores it on every
# switch. See KF-146, which is the entry about that buffer being sixteen bytes
# too small -- a fault that only exists because the state is real.
#
# Deferred with = rather than := on purpose: this appears above where
# USERCFLAGS is defined, and an immediate expansion here would capture an
# empty variable and silently build the program with no include path at all.
INITCFLAGS = $(filter-out -mno-sse -mno-sse2 -mno-mmx -mno-80387,$(USERCFLAGS))

# --- Which role's init goes into this image ----------------------------------
#
# `docs/ROLES.md` settles the shape: the installer puts down the whole
# operating system every time and **the role is chosen on the first boot
# afterwards**, so a role is configuration rather than a build. This variable
# stands in for that choice while the mechanism that will make it -- a word in
# the `cmdline` file on the EFI partition, read by the loader (KF-131) -- does
# not exist yet.
#
# Written as an override with a default rather than as two equal halves, and
# that is deliberate: `make ARCH=x86_64` with nothing else said builds exactly
# the bytes it built before this existed. A role that has to be asked for
# cannot be the cause of a surprise in somebody else's build.
#
#   make ARCH=x86_64                 the workstation, unchanged
#   make ARCH=x86_64 ROLE=server     the server, from ../server/
#
# **This is not the switching mechanic.** That belongs with the installer and
# is not built. This is how the server role is tested before it exists.
ROLE ?= workstation

# The C library, which every role links and none of them differ over.
INIT_LIBC := ../userland/libc/string.c ../userland/libc/printf.c \
	     ../userland/libc/ctype.c ../userland/libc/stdlib.c \
	     ../userland/libc/stdio.c ../userland/libc/syscalls.c \
	     ../userland/libc/malloc.c ../userland/libc/mem_recon.c \
	     ../userland/libc/errno.c ../userland/libc/scanf.c \
	     ../userland/libc/posix.c ../userland/libc/inet.c \
	     ../userland/libc/socket.c

ifeq ($(ROLE),server)

# The server role draws with the workstation's `screen.c` and `layout.c` on
# purpose -- a person who has seen one ReconOS machine should recognise the
# next one. What differs is the program in front of them, not the look of it.
INIT_SRCS := ../server/init/server_init.c \
	     ../server/http/request.c ../server/http/serve.c \
	     ../server/http/files.c ../server/http/form.c ../server/http/cache.c ../server/http/range.c ../server/http/escape.c ../server/http/json.c ../server/http/multipart.c ../server/auth.c ../server/dns.c ../server/ntp.c ../server/identity.c ../server/service.c ../server/log.c ../server/logfile.c ../server/dial.c \
	     ../userland/init/screen.c ../userland/init/layout.c \
	     $(INIT_LIBC)

else

INIT_SRCS := ../userland/init/recon_init.c ../userland/init/screen.c \
	     ../userland/init/layout.c \
	     $(INIT_LIBC)

endif

# --- A role change must rebuild the init -------------------------------------
#
# `make` compares timestamps and knows nothing about variables, so switching
# ROLE leaves `recon_init.elf` newer than every source and it is not rebuilt.
# The build then reports *Nothing to be done* and the machine boots the other
# role's program -- which is the exact shape of fault this project keeps
# naming, because the build says it succeeded and the difference only shows on
# screen.
#
# Found by hitting it: a workstation boot to compare against a server boot ran
# the server's init, and the comparison it was supposed to settle was silently
# between a thing and itself.
#
# The stamp is named after the role, and making it removes the other roles'
# stamps. So the file the init depends on genuinely changes when the role does.
ROLE_STAMP := $(BUILD)/user/.role-$(ROLE)



# --- Rules -------------------------------------------------------------------

.PHONY: all all-arches run clean check-portable info

all: $(BOOT_IMAGE)

# The stamp's rule lives *below* `all`, and that is not cosmetic.
#
# make's default goal is the first target in the file. Written above `all`,
# this rule became the default goal: `make ARCH=x86_64` touched a stamp,
# printed "is up to date", exited 0, and built no kernel at all. Found by two
# roles producing byte-identical images, which they cannot.

$(ROLE_STAMP):
	@mkdir -p $(dir $@)
	@rm -f $(BUILD)/user/.role-*
	@touch $@

$(BUILD)/reconos-kernel.img: $(KERNEL)
	$(OBJCOPY) -O binary $< $@
	@echo "  built $@"

$(KERNEL): $(OBJS) $(LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) -T $(LINKER_SCRIPT) -o $@ $(OBJS)
	@echo
	@echo "  built $@"
	@size $@ 2>/dev/null || $(CROSS)size $@

# Every object depends on this file, and that is not tidiness.
#
# VERSION is handed to the compiler with -D, so it is baked into whichever
# objects happen to be rebuilt after it changes. Without this prerequisite,
# raising the version rebuilds *nothing*: main.c still holds the old number,
# the kernel prints the old number, and the Makefile says the new one. The
# tree and the binary disagree and neither is obviously wrong.
#
# Found on 11 September when a boot reported 0.1.0 from a tree that said 0.1.7
# -- and it was found only because a self-test assertion was added to a boot
# nothing had been reading (KF-187, KF-189). A whole verification run had
# already passed against the mislabelled kernel.
#
# The cost is that editing this file rebuilds everything. That is the correct
# price: almost anything changed here changes how the code is compiled.
$(BUILD)/%.o: %.c Makefile
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/%.o: %.S Makefile
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -MMD -MP -c $< -o $@

# -nostdlib and its own script: no C runtime, no libc, and nothing between
# `_start` and the first instruction.
$(USER_ELF): user/hello.S user/user.ld
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -c user/hello.S -o $(BUILD)/user/hello.o
	$(LD) -nostdlib -no-pie -z noexecstack -T user/user.ld -o $@ $(BUILD)/user/hello.o
	@echo "  built $@"

# --- The C program ---
#
# Built with the kernel's own compiler and *not* with the kernel's flags: the
# kernel is compiled -mcmodel=kernel, which places code in the top two
# gigabytes, and a user program lives at 0x400000. Passing the kernel's flags
# to a user program is the kind of mistake that links and then faults on the
# first instruction.
#
# -ffreestanding says there is no library underneath. -fno-stack-protector
# because the guard needs a runtime that does not exist. -fno-builtin so the
# compiler does not replace a loop with a call to something unlinkable --
# memset and memcpy are provided by the program and are the only two it may
# assume.
USERCFLAGS := -std=c11 -O2 -g -Wall -Wextra -Werror -ffreestanding \
	      -fno-stack-protector -fno-builtin -fno-pic -fno-pie \
	      -fno-asynchronous-unwind-tables -I../userland/include

ifeq ($(ARCH),x86_64)
USERCFLAGS += -m64 -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mno-80387
endif

# The program is the kernel's -- it is a self-test -- but the environment it
# is built against is the *desktop's*, in ../userland. That is the right way
# round: a C runtime is not a kernel component, and there being one copy of
# `recon.h` rather than one per program is what stops a system call number
# drifting between two of them.
# --- the socket probe --------------------------------------------------------
#
# A third C program, and the only one that exists to test a *system call*
# rather than the loader or the screen. The five socket calls cannot be
# exercised from a kernel thread -- no process, so no descriptor table -- so
# the claim that a socket is an ordinary descriptor had nothing checking it
# until this ran.
$(SOCKP_ELF): user/socket_probe.c ../userland/crt0.S \
	      ../userland/include/recon.h user/user.ld
	@mkdir -p $(dir $@)
	$(CC) $(USERCFLAGS) -c user/socket_probe.c -o $(BUILD)/user/socket_probe.o
	$(CC) $(ASFLAGS) -I../userland/include -c ../userland/crt0.S \
		-o $(BUILD)/user/crt0.o
	$(LD) -nostdlib -no-pie -z noexecstack -T user/user.ld -o $@ \
		$(BUILD)/user/crt0.o $(BUILD)/user/socket_probe.o
	@echo "  built $@"

$(PAINT_ELF): user/paint.c ../userland/crt0.S ../userland/include/recon.h \
	      user/user.ld
	@mkdir -p $(dir $@)
	$(CC) $(USERCFLAGS) -c user/paint.c -o $(BUILD)/user/paint.o
	$(CC) $(ASFLAGS) -I../userland/include -c ../userland/crt0.S \
		-o $(BUILD)/user/crt0.o
	$(LD) -nostdlib -no-pie -z noexecstack -T user/user.ld -o $@ \
		$(BUILD)/user/crt0.o $(BUILD)/user/paint.o
	@echo "  built $@"

# The first program that is a system rather than a self-test.
#
# Built from ../userland entirely: the program, the drawing, and the C library
# it is the first real customer of. Nothing under kernel/ is compiled into it.
$(INIT_ELF): $(INIT_SRCS) $(ROLE_STAMP) ../userland/crt0.S \
	     ../userland/include/recon.h \
	     user/user.ld
	@mkdir -p $(dir $@)
	@rm -f $(BUILD)/user/init-*.o
	@set -e; for s in $(INIT_SRCS); do \
		o=$(BUILD)/user/init-`basename $$s .c`.o; \
		$(CC) $(INITCFLAGS) -I../boot/include -I../userland/init \
			-c $$s -o $$o; \
	done
	$(CC) $(ASFLAGS) -I../userland/include -c ../userland/crt0.S \
		-o $(BUILD)/user/crt0-for-init.o
	$(LD) -nostdlib -no-pie -z noexecstack -T user/user.ld -o $@ \
		$(BUILD)/user/crt0-for-init.o $(BUILD)/user/init-*.o
	@echo "  built $@"

# The blob that carries them into the kernel image. `.incbin` rather than
# objcopy's `-I binary`, because objcopy names its symbols after the path it
# was given and that name changes when the build directory does.
$(BUILD)/core/user_elf.o: core/user_elf.S $(USER_ELF) $(PAINT_ELF) \
                          $(INIT_ELF) $(SOCKP_ELF)

-include $(OBJS:.o=.d)

all-arches:
	$(MAKE) ARCH=x86_64
	$(MAKE) ARCH=aarch64

run: $(BOOT_IMAGE)
	$(QEMU) $(QEMUFLAGS) -display none -serial mon:stdio

# The rule that keeps arch.h meaningful: nothing under core/ may name a
# machine, embed assembly, or reach a hardware address. When this fails, the
# fix is a new function in include/recon/kernel/arch.h, not an #ifdef.
#
# Comments are stripped before the check runs. The rule is about what the code
# does, not what the prose is allowed to mention -- a portable file explaining
# *why* it is portable will name the machines it is avoiding, and failing the
# build for that teaches people to write worse comments.
#
# --- The numeric clause was replaced, and this is why ---
#
# It used to flag any hex constant of six digits or more, on the theory that
# such a number is a hardware address. Over checkpoints 11 and 12 that clause
# fired six times and was right none of them: it caught "virt" read out of a
# 32-bit register, every-block-there-could-be, the all-ones written to size a
# base address register, a CRC polynomial, and two published checksum values.
# Each time, correct code was rewritten to appease it. A check that makes people
# rephrase working code is teaching the wrong lesson -- it becomes the author's
# adversary rather than the reader's friend -- and one with no true positives is
# not paying for that.
#
# What replaced it is what a hardware address actually *looks like when it is
# used*: an integer literal cast to a pointer. That is the only way core/ can
# reach a fixed location, because everything else it touches arrives as a
# pointer from arch/ or from a bus. Checked against a file of nine real
# violations and a file of legitimate constants before it was adopted, rather
# than assumed to work.
#
# Port instructions are named too. They exist on exactly one architecture, and a
# portable file containing one is not portable no matter what it looks like.
#
# This is a smoke alarm, not a proof. The real guarantee is that core/ compiles
# unchanged for both architectures and is linked against two different arch/
# directories, and a genuinely machine-specific assumption fails there.
check-portable:
	@bad=""; \
	for f in core/*.c core/*.h core/*.S; do \
		[ -e "$$f" ] || continue; \
		hit=$$(sed -e 's|/\*.*\*/||g' -e '\|/\*|,\|\*/|d' -e 's|//.*||' "$$f" \
		      | grep -nE '__asm__|asm volatile|\bx86|\baarch64|\briscv|\*[[:space:]]*\)[[:space:]]*0x[0-9a-fA-F]{6,}|\bout[bwl]\b|\bin[bwl]\b' \
		      | sed "s|^|$$f:|"); \
		[ -n "$$hit" ] && bad="$$bad$$hit\n"; \
	done; \
	if [ -n "$$bad" ]; then \
		echo "core/ is not portable any more:"; printf "$$bad"; exit 1; \
	fi; \
	echo "core/ is clean: no machine-specific code outside arch/"

# The interface the desktop compiles against, built for the *host* rather than
# for the kernel image: it is userland code that will one day call into a
# kernel, and today it links against stubs that say so. Kept honest by being
# compiled with the same warnings-as-errors as everything else, so a header
# that does not compile is found here rather than in the compositor's build.
API_LIB := build/api/librecon_kernel_stubs.a
HOSTCC  ?= cc

.PHONY: api
api: $(API_LIB)

$(API_LIB): api/stubs.c include/recon_kernel.h
	@mkdir -p build/api
	$(HOSTCC) -std=c11 -O2 -Wall -Wextra -Werror -Wno-unused-parameter \
	          -Iinclude -c api/stubs.c -o build/api/stubs.o
	ar rcs $@ build/api/stubs.o
	@echo "  built $@"

info:
	@echo "ARCH    = $(ARCH)"
	@echo "CC      = $(CC)"
	@echo "KERNEL  = $(KERNEL)"
	@echo "SOURCES = $(CORE_SRCS) $(ARCH_SRCS) $(ARCH_ASM)"

clean:
	rm -rf build
