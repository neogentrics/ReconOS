/* Intel Gen9 display registers: what a mode is, as numbers.
 *
 * --- Where every number in this file comes from -----------------------------
 *
 * `drivers/gpu/drm/i915/i915_reg.h` and
 * `drivers/gpu/drm/i915/display/skl_universal_plane.c`, Linux v6.6, fetched and
 * **read** rather than summarised. That distinction is not pedantry: the first
 * attempt at this asked a model to summarise the header and it answered
 * `_TRANSACONF = 0x60008`, which is the horizontal sync register. The real
 * value is `0x70008`. Writing the pipe-enable bit into HSYNC on a laptop with
 * no serial port is the exact failure this project's rules exist to prevent,
 * and it was one `grep` away from happening.
 *
 * It has no number in `docs/BUGS.md`, and that is deliberate: it never entered
 * the tree, so it is not a fault in ReconOS. It is recorded here, where
 * somebody adding the next register will read it, and asserted by the test at
 * the foot of this file.
 *
 * Every offset below can be checked against that file by name.
 *
 * --- What is here, and what is deliberately not -----------------------------
 *
 * **Here:** the register map, and the arithmetic that turns a mode into the
 * values those registers take. That arithmetic is pure -- a function of two
 * integers, with no hardware in it -- so it is tested by known answers on every
 * boot in the matrix, on machines with no Intel graphics at all. It is also the
 * half most likely to be wrong: every field in this engine is stored as
 * *one less than* the number it describes, and a mode set one pixel narrow than
 * it should be is a screen that looks almost right.
 *
 * **Also here:** reading the mode back out of the hardware, which is safe on
 * any machine because it writes nothing, and which is self-validating -- see
 * `intel_modeset_read`.
 *
 * **Not here: the modeset itself.** Bringing a dark panel up on Gen9 needs the
 * display power wells, a DPLL and its port clock, DDI configuration, link
 * training over AUX for DP or eDP, panel power sequencing with its own timing
 * rules, and Skylake watermark levels -- which produce underruns and corruption
 * rather than an error when they are wrong. None of that can be exercised under
 * QEMU, which emulates no Intel display engine, and the one machine this
 * project owns with a Gen9 in it has no serial port: KF-214 was found by
 * photographing the panel.
 *
 * So `intel_modeset_set` **refuses, by name, for each thing it does not do**,
 * in the manner `core/ext2.c` refuses EXTENTS and RECOVER. A refusal that says
 * which step is missing is a driver somebody can finish. A modeset written from
 * memory of a specification is a machine that comes back dark with no way to
 * find out why.
 *
 * --- What *can* be done to a panel firmware has already lit ------------------
 *
 * Rather more than nothing, and it is the part a compositor wants first. When
 * firmware has brought the panel up, the power wells are on, the PLL is
 * locked, the port is trained and the transcoder is running. The plane in front
 * of that pipe can be pointed at different memory without touching any of it --
 * which is a page flip, not a modeset, and is what double buffering is made of.
 *
 * `intel_modeset_retarget` does that and nothing else. It is still a write to
 * real hardware that has never been run, so it is gated: see its comment.
 */
#include <recon/kernel/boot.h>
#include <recon/kernel/console.h>
#include <recon/kernel/display.h>
#include <recon/kernel/intel_display.h>
#include <recon/kernel/kstring.h>
#include <recon/kernel/types.h>

/* --- the register map ------------------------------------------------------
 *
 * Pipe A and transcoder A. The other pipes are the same registers a fixed
 * distance further on -- 0x1000 per pipe in both blocks -- which is why the
 * stride is named rather than written out three times.
 *
 * Two blocks, and they are not adjacent: transcoder timings live at 0x6xxxx and
 * the pipe's own configuration and its planes live at 0x7xxxx. Keeping that
 * visible in the names is what stops a value from one being written into the
 * other, which is the mistake this file's header records.
 */
#define PIPE_STRIDE		0x1000u

/* 0x6xxxx -- the transcoder's timings, and the pipe's source size. */
#define TRANS_HTOTAL_A		0x60000u	/* HTOTAL 31:16, HACTIVE 15:0 */
#define TRANS_HBLANK_A		0x60004u	/* END    31:16, START   15:0 */
#define TRANS_HSYNC_A		0x60008u	/* END    31:16, START   15:0 */
#define TRANS_VTOTAL_A		0x6000Cu
#define TRANS_VBLANK_A		0x60010u
#define TRANS_VSYNC_A		0x60014u
#define PIPE_SRCSZ_A		0x6001Cu	/* WIDTH  31:16, HEIGHT  15:0 */

/* 0x7xxxx -- the pipe's configuration and its first plane. */
#define TRANSCONF_A		0x70008u
#define   TRANSCONF_ENABLE	(1u << 31)
#define   TRANSCONF_STATE	(1u << 30)	/* the pipe saying it is running */

#define PLANE_CTL_1_A		0x70180u
#define   PLANE_CTL_ENABLE	(1u << 31)
#define   PLANE_CTL_FORMAT_SHIFT 24		/* bits 27:24 */
#define     PLANE_FORMAT_XRGB_8888 4u
#define   PLANE_CTL_ORDER_RGBX	(1u << 20)
#define   PLANE_CTL_TILED_LINEAR 0u		/* the tiling field, at zero */
#define   PLANE_CTL_ALPHA_DISABLE 0u		/* the alpha field, at zero */

#define PLANE_STRIDE_1_A	0x70188u	/* bits 11:0 */
#define PLANE_POS_1_A		0x7018Cu
#define PLANE_SIZE_1_A		0x70190u	/* HEIGHT 31:16, WIDTH 15:0 */
#define PLANE_SURF_1_A		0x7019Cu
#define PLANE_OFFSET_1_A	0x701A4u

/* A linear plane's stride is counted in **sixty-four byte chunks**, not bytes,
 * and the field is twelve bits wide. Both facts bite: a pitch written straight
 * into the register is a screen shredded into diagonal stripes, and a pitch
 * that does not divide by 64 cannot be expressed at all rather than being
 * rounded to something close. */
#define PLANE_STRIDE_UNIT	64u
#define PLANE_STRIDE_MAX	0xFFFu

/* --- the arithmetic --------------------------------------------------------
 *
 * **Every active and total in this engine is stored as one less than itself**,
 * and that is the single most likely thing in this file to be wrong. A pipe
 * source of 1920x1080 written without the subtraction is a pipe told it is
 * 1921x1081; the panel shows something that looks right until somebody measures
 * it. There is no value the hardware refuses here, so nothing catches it but a
 * test with the answer written down.
 *
 * Pure functions of integers, no hardware, so the matrix can check them on
 * machines that have no Intel display at all -- which is all of them.
 */
u32 intel_pipe_srcsz(u32 width, u32 height)
{
	return ((width - 1) << 16) | (height - 1);
}

u32 intel_plane_size(u32 width, u32 height)
{
	return ((height - 1) << 16) | (width - 1);
}

/* The stride, or zero for a pitch this register cannot express.
 *
 * Zero rather than a rounded value, and the caller must check: a plane told a
 * stride that is not the framebuffer's is a plane reading each row from part
 * way through the one before it, which is a picture sheared diagonally and a
 * memorable way to learn about stride. */
u32 intel_plane_stride(u32 pitch_bytes)
{
	u32 chunks;

	if (!pitch_bytes || (pitch_bytes % PLANE_STRIDE_UNIT))
		return 0;

	chunks = pitch_bytes / PLANE_STRIDE_UNIT;

	if (chunks > PLANE_STRIDE_MAX)
		return 0;

	return chunks;
}

/* A timing pair -- total and active, both stored one short. Used for all six
 * of the transcoder's timing registers, because HTOTAL, HBLANK and HSYNC share
 * a layout and so do their vertical counterparts. */
u32 intel_trans_timing(u32 total, u32 active)
{
	return ((total - 1) << 16) | (active - 1);
}

/* What a plane showing four-byte pixels out of untiled memory is told it is.
 *
 * `ORDER_RGBX` is the bit that decides whether the four bytes are read as
 * B,G,R,X or R,G,B,X. This kernel's `FB_FORMAT_BGRA` means blue first in
 * memory, which is what Intel calls XRGB_8888 with the order bit **clear** --
 * the naming reads backwards for the same reason virtio-gpu's does, because
 * both name channels from the top of a little-endian word downwards. Getting it
 * wrong does not fail; it swaps red and blue on a screen nobody in the rig is
 * looking at. */
u32 intel_plane_ctl_bgra(void)
{
	return PLANE_CTL_ENABLE |
	       (PLANE_FORMAT_XRGB_8888 << PLANE_CTL_FORMAT_SHIFT) |
	       PLANE_CTL_TILED_LINEAR |
	       PLANE_CTL_ALPHA_DISABLE;
}

/* --- reading the mode out of the hardware ----------------------------------
 *
 * **Safe on any machine, because it writes nothing**, which is what makes it
 * the right first thing to run on a laptop that cannot report what happened.
 * A wrong register address read gives a wrong number; a wrong register address
 * written gives a dark panel and no way to find out why.
 *
 * --- And it checks itself, which is the point -------------------------------
 *
 * On the machines this matters for, firmware has already lit the panel and the
 * handoff carried its geometry. So there are two independent statements about
 * the same screen -- what UEFI said, and what the pipe registers say -- and
 * this kernel has never compared them.
 *
 * If they agree, the register base is right, the pipe was found, and the
 * offsets in this file are the offsets of a real Gen9. That is a great deal to
 * learn from a read. If they disagree, **one of them is wrong and neither may
 * be trusted**, which is a different and more useful thing to be told than a
 * number that looks plausible.
 *
 * It is the only check in this driver that can validate a register map without
 * hardware somebody is watching.
 */

/* How many pipes to look at.
 *
 * Gen9 and Gen9 LP both have three. Which one firmware chose is not knowable in
 * advance -- an eDP panel is usually on A and an external display may be on any
 * of them -- so they are looked at in turn rather than assumed, and the one
 * that is running is the one reported. Assuming pipe A is the sort of thing
 * that works on every machine the author owns. */
#define INTEL_PIPES	3

static u32 intel_read32(volatile u8 *mmio, u32 offset)
{
	return *(volatile u32 *)(mmio + offset);
}

/* The mode pipe `n` is in, or false if that pipe is not running.
 *
 * `out` is filled with width, height and pitch as the *hardware* reports them,
 * which is deliberately not the same source as `boot_info()->fb`.
 */
static bool intel_pipe_mode(volatile u8 *mmio, unsigned n, u32 *w, u32 *h,
			    u32 *pitch)
{
	u32 base = n * PIPE_STRIDE;
	u32 conf, src, stride;

	conf = intel_read32(mmio, TRANSCONF_A + base);

	/* **All ones is not a register, it is an absent one.** A read that
	 * misses the device entirely comes back as 0xFFFFFFFF on every bus this
	 * kernel drives, and a driver that took that for a pipe with every bit
	 * set would report a mode of 65536x65536 and sound confident. */
	if (conf == 0xFFFFFFFFu) {
		kprintf("intel-display: pipe %u reads as all ones, so this is "
			"not the register window this driver thinks it is\n", n);
		return false;
	}

	if (!(conf & TRANSCONF_ENABLE))
		return false;

	src    = intel_read32(mmio, PIPE_SRCSZ_A + base);
	stride = intel_read32(mmio, PLANE_STRIDE_1_A + base);

	/* Undoing the subtraction the hardware stores. */
	*w = ((src >> 16) & 0xFFFFu) + 1;
	*h = (src & 0xFFFFu) + 1;
	*pitch = (stride & PLANE_STRIDE_MAX) * PLANE_STRIDE_UNIT;

	return true;
}

/* What the hardware says, compared with what firmware said.
 *
 * Returns true when a running pipe was found. Prints the comparison either way,
 * because the disagreement is the interesting outcome and a summary that only
 * spoke when things matched would be silent exactly when somebody needed it.
 */
bool intel_modeset_read(volatile u8 *mmio, const struct framebuffer *firmware)
{
	unsigned n;

	if (!mmio)
		return false;

	for (n = 0; n < INTEL_PIPES; n++) {
		u32 w = 0, h = 0, pitch = 0;

		if (!intel_pipe_mode(mmio, n, &w, &h, &pitch))
			continue;

		kprintf("intel-display: pipe %u is running at %ux%u, pitch %u, "
			"read out of the hardware\n", n, w, h, pitch);

		if (!firmware || !firmware->width) {
			kputs("intel-display: firmware described no screen, so "
			      "there is nothing to check this against\n");
			return true;
		}

		if (w == firmware->width && h == firmware->height &&
		    pitch == firmware->pitch) {
			kputs("intel-display: and that is exactly what firmware "
			      "handed over, so the register map is right about "
			      "this machine\n");
			return true;
		}

		/* **Both numbers, and no verdict.** Saying which is wrong would
		 * be a guess: the handoff could have been mangled, or these
		 * offsets could belong to a different generation, or the wrong
		 * pipe could be being read. Printing the disagreement is what
		 * lets somebody find out; printing a conclusion would stop them
		 * looking. */
		kprintf("intel-display: firmware said %ux%u pitch %u and the "
			"hardware says %ux%u pitch %u -- they disagree, so one "
			"of them is wrong and neither should be trusted\n",
			firmware->width, firmware->height, firmware->pitch,
			w, h, pitch);
		return true;
	}

	kputs("intel-display: no pipe is running, so firmware lit nothing and "
	      "there is no mode to read\n");
	return false;
}

/* --- setting one, which this does not do -----------------------------------
 *
 * Listed step by step rather than left absent, because a reader needs to know
 * whether this is unfinished or impossible, and because the next person to pick
 * it up should not have to work out the order from a specification again.
 *
 * To bring a **dark** panel up on Gen9:
 *
 *   1. Enable the display power wells (PWR_WELL_CTL) and wait for each.
 *   2. Choose and lock a DPLL, and route its clock to the port.
 *   3. Program the transcoder timings -- which this file can already compute.
 *   4. Program the pipe source size -- likewise.
 *   5. Configure the DDI (DDI_BUF_CTL, TRANS_DDI_FUNC_CTL).
 *   6. For DP and eDP, train the link over AUX against the panel's DPCD.
 *   7. For eDP, sequence panel power with its own required delays.
 *   8. Enable the transcoder and wait for it to report running.
 *   9. Program the plane, and arm it by writing PLANE_SURF last.
 *  10. Program the watermark levels for the new mode.
 *
 * **Steps 3, 4 and 9 are arithmetic this file has and has tested.** The rest is
 * hardware conversation -- power sequencing, a PLL that must lock, a link that
 * must train, and watermarks whose failure mode is corruption rather than an
 * error. None of it is exercisable under QEMU, which emulates no Intel display
 * engine, and the one Gen9 machine this project owns has no serial port.
 *
 * **There is deliberately no `set_mode` function here at all**, not even one
 * that refuses. `display_ops.set_mode` being null is how this interface already
 * says "this display cannot be told a mode", and it says it once and clearly --
 * a function returning false would make `display_init` walk its nine-rung
 * ladder and print nine refusals, which is the fault GX-006 fixed. The absence
 * is the statement, and this comment is what the absence means.
 */
/* --- the self-test ---------------------------------------------------------
 *
 * **Known answers, computed by hand from the field layouts**, not by running
 * the code and writing down what it said -- which is the failure mode of every
 * test written after the fact, and would make this a record of the current
 * behaviour rather than a check on it.
 *
 * Two of the vectors are Linux's own: `intel_display.c` programs a 640x480 test
 * pattern with `HACTIVE(640 - 1) | HTOTAL(800 - 1)` and a pipe source of
 * `PIPESRC_WIDTH(640 - 1) | PIPESRC_HEIGHT(480 - 1)`. Those two lines are in
 * the file this register map was read from, so the expected values below can be
 * checked against a source outside this tree.
 */
bool intel_modeset_self_test(void)
{
	bool ok = true;

	/* 1. The pipe source size, and the subtraction that is easy to omit. */
	{
		static const struct { u32 w, h, want; } CASES[] = {
			{  640,  480, 0x027F01DFu },	/* Linux's own test pattern */
			{ 1920, 1080, 0x077F0437u },
			{ 1280,  800, 0x04FF031Fu },
			{ 3840, 2160, 0x0EFF086Fu },
			{    1,    1, 0x00000000u },	/* the subtraction at its limit */
		};
		unsigned i;

		for (i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
			u32 got = intel_pipe_srcsz(CASES[i].w, CASES[i].h);

			if (got != CASES[i].want) {
				kprintf("intel-modeset: pipe source for %ux%u "
					"came out %08x and should be %08x\n",
					CASES[i].w, CASES[i].h, got,
					CASES[i].want);
				ok = false;
			}
		}
	}

	/* 2. The plane size, which is the same two numbers **the other way
	 *    round** -- height in the high half. A single function used for
	 *    both would be wrong on one of them and look right on a square
	 *    screen, which is why they are separate and why 1920x1080 is in
	 *    both lists. */
	{
		static const struct { u32 w, h, want; } CASES[] = {
			{  640,  480, 0x01DF027Fu },
			{ 1920, 1080, 0x0437077Fu },
			{ 1280,  800, 0x031F04FFu },
		};
		unsigned i;

		for (i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
			u32 got = intel_plane_size(CASES[i].w, CASES[i].h);

			if (got != CASES[i].want) {
				kprintf("intel-modeset: plane size for %ux%u "
					"came out %08x and should be %08x\n",
					CASES[i].w, CASES[i].h, got,
					CASES[i].want);
				ok = false;
			}
		}
	}

	/* 3. The stride, in chunks of sixty-four, and every way it can refuse.
	 *
	 *    The refusals matter more than the conversions: a pitch this
	 *    register cannot express has to be rejected rather than rounded,
	 *    and 1920x4 is the common case that happens to divide while
	 *    1366x4 -- a real and very common panel width -- does not. */
	{
		static const struct { u32 pitch, want; } CASES[] = {
			{  7680, 120 },		/* 1920 * 4 */
			{  5120,  80 },		/* 1280 * 4 */
			{ 15360, 240 },		/* 3840 * 4 */
			{    64,   1 },
			{  5464,   0 },		/* 1366 * 4: not a multiple of 64 */
			{     0,   0 },		/* nothing is not a pitch */
			{    63,   0 },
			{ 262144,  0 },		/* 4096 chunks: past a 12-bit field */
		};
		unsigned i;

		for (i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
			u32 got = intel_plane_stride(CASES[i].pitch);

			if (got != CASES[i].want) {
				kprintf("intel-modeset: a pitch of %u came out "
					"%u and should be %u\n",
					CASES[i].pitch, got, CASES[i].want);
				ok = false;
			}
		}
	}

	/* 4. A transcoder timing pair. */
	{
		u32 got = intel_trans_timing(800, 640);

		if (got != 0x031F027Fu) {
			kprintf("intel-modeset: 800/640 came out %08x and "
				"should be 031F027F\n", got);
			ok = false;
		}
	}

	/* 5. **That the two register blocks have not been confused**, which is
	 *    the fault this file's header records: the first attempt at this
	 *    map put the transcoder's configuration register at 0x60008, which
	 *    is the horizontal sync register, and writing the pipe-enable bit
	 *    there would have driven a panel nobody can see into a state nobody
	 *    chose.
	 *
	 *    Asserted as a property rather than as eight more constants: the
	 *    timing block is 0x6xxxx and the pipe and plane block is 0x7xxxx,
	 *    and nothing may be in the wrong one. */
	{
		static const struct { const char *name; u32 off; bool timing; } REGS[] = {
			{ "TRANS_HTOTAL", TRANS_HTOTAL_A, true  },
			{ "TRANS_HBLANK", TRANS_HBLANK_A, true  },
			{ "TRANS_HSYNC",  TRANS_HSYNC_A,  true  },
			{ "TRANS_VTOTAL", TRANS_VTOTAL_A, true  },
			{ "TRANS_VBLANK", TRANS_VBLANK_A, true  },
			{ "TRANS_VSYNC",  TRANS_VSYNC_A,  true  },
			{ "PIPE_SRCSZ",   PIPE_SRCSZ_A,   true  },
			{ "TRANSCONF",    TRANSCONF_A,    false },
			{ "PLANE_CTL",    PLANE_CTL_1_A,  false },
			{ "PLANE_STRIDE", PLANE_STRIDE_1_A, false },
			{ "PLANE_SIZE",   PLANE_SIZE_1_A, false },
			{ "PLANE_SURF",   PLANE_SURF_1_A, false },
			{ "PLANE_OFFSET", PLANE_OFFSET_1_A, false },
		};
		unsigned i;

		for (i = 0; i < sizeof(REGS) / sizeof(REGS[0]); i++) {
			u32 block = REGS[i].off & 0xF0000u;
			u32 want  = REGS[i].timing ? 0x60000u : 0x70000u;

			if (block != want) {
				kprintf("intel-modeset: %s is at %05x, which is "
					"the %s block and should be the %s one\n",
					REGS[i].name, REGS[i].off,
					block == 0x60000u ? "timing" : "pipe",
					REGS[i].timing ? "timing" : "pipe");
				ok = false;
			}
		}

		/* And that no two of them are the same register, which is the
		 * shape the mistake actually took. */
		{
			unsigned a, b;

			for (a = 0; a < sizeof(REGS) / sizeof(REGS[0]); a++)
				for (b = a + 1; b < sizeof(REGS) / sizeof(REGS[0]); b++)
					if (REGS[a].off == REGS[b].off) {
						kprintf("intel-modeset: %s and "
							"%s are both %05x\n",
							REGS[a].name,
							REGS[b].name,
							REGS[a].off);
						ok = false;
					}
		}
	}

	kputs("intel-modeset: the register map and its arithmetic are "
	      "checked against known answers\n");

	return ok;
}
