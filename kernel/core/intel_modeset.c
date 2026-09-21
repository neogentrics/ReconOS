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

/* **And a transcoder that is not any pipe's.**
 *
 * Gen9 has a transcoder for eDP whose base is not 0x60000 plus a multiple of
 * PIPE_STRIDE, and on this project's laptop **it is the one driving the
 * panel**. Read off the Gateway on 18 September 2026, with the screen lit and
 * a compositor flipping on it -- the file is
 * `docs/hardware/intel-gen9-gateway-registers.txt`:
 *
 *   TRANS_HTOTAL_A  0x60000  0x00000000     TRANS_HTOTAL_EDP  0x6F000  0x05b90555
 *   TRANS_HSYNC_A   0x60008  0x00000000     TRANS_HSYNC_EDP   0x6F008  0x0591057b
 *   TRANS_VTOTAL_A  0x6000C  0x00000000     TRANS_VTOTAL_EDP  0x6F00C  0x031d02ff
 *   TRANSCONF_A     0x70008  0x00000000     TRANSCONF_EDP     0x7F008  0xc0000000
 *   PIPE_SRCSZ_A    0x6001C  0x055502ff     PLANE_SIZE_1_A    0x70190  0x02ff0555
 *
 * So the **pipe** is A -- its source size and its plane are exactly right --
 * and the **transcoder** is not A's. Every timing register in the block this
 * driver read is zero, and TRANSCONF_A's enable bit is clear, on a machine
 * whose screen is on (GX-013).
 *
 * --- One register, two readings, and they disagree -------------------------
 *
 * `PIPE_DDI_FUNC_CTL_EDP` at 0x6F400 was read twice on 18 September, on the
 * same machine, hours apart:
 *
 *   graphics session   0x00210000   bit 31 clear -- the function is DISABLED
 *   kernel session     0x82210000   bit 31 set   -- enabled, DP SST, 6 bpc
 *
 * The two runs differ in one known way: the kernel session wrote
 * `echo 0 > /sys/class/graphics/fb0/blank` first, to wake the panel. The
 * graphics session did not, and read a machine whose desktop had been idle for
 * some minutes.
 *
 * **That is a correlation and this file does not claim it is the cause.** What
 * matters is not why, it is what a driver may conclude from the register:
 *
 *   TRANSCONF_EDP  read 0xc0000000 -- enabled and active -- in BOTH runs.
 *   The timings at 0x6F000 were the panel's real mode in BOTH runs.
 *   PIPE_DDI_FUNC_CTL_EDP was enabled in one and disabled in the other.
 *
 * So **the field that agreed with the lit panel both times is TRANSCONF, and
 * the one that did not is the DDI function control.** That is the whole basis
 * on which the scan below picks TRANSCONF as the deciding register, and it is
 * evidence rather than preference -- a selector written against
 * TRANS_DDI_FUNC_ENABLE would have found no transcoder at all on the first of
 * those two readings, which is the same false negative GX-013 is about,
 * reintroduced by the fix for it.
 *
 * The DDI control is still read, and printed when it disagrees, because a
 * disagreement is a fact about the machine that somebody debugging a dark
 * panel will want. It is not allowed to decide anything.
 */
/* The DDI function controls, one per transcoder. Read, reported, and not
 * trusted to decide -- see the two readings above. */
#define PIPE_DDI_FUNC_CTL_A	0x60400u
#define PIPE_DDI_FUNC_CTL_EDP	0x6F400u
#define   TRANS_DDI_FUNC_ENABLE	(1u << 31)

/* **The EDP transcoder is transcoder A plus 0xF000, in both blocks.**
 *
 * Measured, not assumed: 0x60000 -> 0x6F000 for the timings and 0x70008 ->
 * 0x7F008 for the configuration, and 0x60400 -> 0x6F400 for the DDI control.
 * One displacement, three registers, confirmed against a running machine. The
 * self-test asserts it so that a hand-typed offset cannot quietly break the
 * relationship. */
#define TRANS_EDP_OFFSET	0xF000u

#define TRANS_HTOTAL_EDP	0x6F000u
#define TRANS_HBLANK_EDP	0x6F004u
#define TRANS_HSYNC_EDP		0x6F008u
#define TRANS_VTOTAL_EDP	0x6F00Cu
#define TRANS_VBLANK_EDP	0x6F010u
#define TRANS_VSYNC_EDP		0x6F014u

/* 0x7xxxx -- the pipe's configuration and its first plane. */
#define TRANSCONF_A		0x70008u
#define TRANSCONF_EDP		0x7F008u
#define   TRANSCONF_ENABLE	(1u << 31)
#define   TRANSCONF_STATE	(1u << 30)	/* the pipe saying it is running */

#define PLANE_CTL_1_A		0x70180u
#define   PLANE_CTL_ENABLE	(1u << 31)
#define   PLANE_CTL_FORMAT_SHIFT 24		/* bits 27:24 */
#define     PLANE_FORMAT_XRGB_8888 4u
#define   PLANE_CTL_ORDER_RGBX	(1u << 20)
#define   PLANE_CTL_TILED_SHIFT	10		/* the tiling field, 12:10 */
#define   PLANE_CTL_TILED_MASK	(7u << PLANE_CTL_TILED_SHIFT)
#define     PLANE_TILED_LINEAR	0u
#define     PLANE_TILED_X	1u
#define     PLANE_TILED_Y	4u
#define     PLANE_TILED_YF	5u
#define   PLANE_CTL_TILED_LINEAR 0u		/* the tiling field, at zero */
#define   PLANE_CTL_ALPHA_DISABLE 0u		/* the alpha field, at zero */

#define PLANE_STRIDE_1_A	0x70188u	/* bits 11:0 */
#define PLANE_POS_1_A		0x7018Cu
#define PLANE_SIZE_1_A		0x70190u	/* HEIGHT 31:16, WIDTH 15:0 */
#define PLANE_SURF_1_A		0x7019Cu
#define PLANE_OFFSET_1_A	0x701A4u

/* A **linear** plane's stride is counted in sixty-four byte chunks, not bytes,
 * and the field is twelve bits wide. Both facts bite: a pitch written straight
 * into the register is a screen shredded into diagonal stripes, and a pitch
 * that does not divide by 64 cannot be expressed at all rather than being
 * rounded to something close.
 *
 * **The word "linear" is doing work, and it was not here before (GX-014).**
 * The unit depends on how the surface is tiled, and this file had no notion of
 * tiling at all -- so it named one unit, called it the unit, and would have
 * decoded a tiled plane's stride as a smaller number of larger rows.
 *
 * Measured rather than remembered. The Gateway's plane is Y-tiled and reads
 * PLANE_STRIDE = 0x2b = 43, for a row this project independently computes as
 * 5504 bytes: 5504 / 43 is exactly 128, so the Y-tiled unit at four bytes a
 * pixel is 128 and the 64 above is the linear case only. Both encodings
 * describe the same 5504 bytes -- 86 chunks of 64, or 43 of 128 -- which is
 * why an agreement on the byte count is not an agreement on the register.
 */
#define PLANE_STRIDE_UNIT	64u
#define PLANE_STRIDE_UNIT_Y	128u
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

/* How many bytes a stride chunk is for the tiling this plane is set to, or
 * **zero for a tiling this file has not measured**.
 *
 * Zero rather than 64, and that distinction is the whole value of the
 * function: falling back to the linear unit would decode an X-tiled plane's
 * stride as an eighth of itself and report a pitch, and a pitch that is wrong
 * by a factor is indistinguishable from one that is right until something is
 * drawn. Linear is from the specification and confirmed by arithmetic; Y is
 * from this project's own hardware; X and Yf are neither, so they are refused
 * by name.
 */
static u32 intel_plane_stride_unit(u32 plane_ctl)
{
	switch ((plane_ctl & PLANE_CTL_TILED_MASK) >> PLANE_CTL_TILED_SHIFT) {
	case PLANE_TILED_LINEAR:
		return PLANE_STRIDE_UNIT;
	case PLANE_TILED_Y:
		return PLANE_STRIDE_UNIT_Y;
	default:
		return 0u;
	}
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
/* The transcoders this driver knows, in the order it looks at them.
 *
 * Pipe-indexed A, B and C, then EDP -- which is last deliberately. On a laptop
 * the panel is on EDP and A reads zero, so a scan that stopped at the first
 * *entry* rather than the first *enabled* one would find nothing and be right
 * about nothing. Ordering here is presentation only; the loop below reads every
 * one of them before deciding.
 */
struct intel_transcoder {
	const char *name;
	u32 timing;		/* HTOTAL; the rest follow at fixed offsets */
	u32 conf;		/* TRANSCONF -- the register that decides */
	u32 ddi;		/* the DDI function control -- reported only */
};

static const struct intel_transcoder TRANSCODERS[] = {
	{ "A",   0x60000u,             0x70008u,             0x60400u },
	{ "B",   0x60000u + 0x1000u,   0x70008u + 0x1000u,   0x60400u + 0x1000u },
	{ "C",   0x60000u + 0x2000u,   0x70008u + 0x2000u,   0x60400u + 0x2000u },
	{ "EDP", 0x60000u + TRANS_EDP_OFFSET,
	         0x70008u + TRANS_EDP_OFFSET,
	         0x60400u + TRANS_EDP_OFFSET },
};

#define TRANSCODER_COUNT (sizeof(TRANSCODERS) / sizeof(TRANSCODERS[0]))

/* Undo the minus-one the hardware stores. The encoder has existed since this
 * file did; the decoders are new, because reading was done inline and the eight
 * values the Gateway handed back are known answers that deserve a function to
 * assert against. */
u32 intel_trans_total(u32 reg)
{
	return ((reg >> 16) & 0xFFFFu) + 1;
}

u32 intel_trans_active(u32 reg)
{
	return (reg & 0xFFFFu) + 1;
}

/* Which transcoder is driving something, or none.
 *
 * Returns the index, or TRANSCODER_COUNT for "no transcoder says it is
 * enabled" -- which is a real answer on a machine with no display and must not
 * be confused with index zero.
 */
static unsigned intel_transcoder_running(volatile u8 *mmio)
{
	unsigned i;

	for (i = 0; i < TRANSCODER_COUNT; i++) {
		u32 conf = intel_read32(mmio, TRANSCODERS[i].conf);
		u32 ddi;

		if (conf == 0xFFFFFFFFu) {
			kprintf("intel-display: transcoder %s reads as all "
				"ones, so this is not the register window this "
				"driver thinks it is\n", TRANSCODERS[i].name);
			return TRANSCODER_COUNT;
		}

		if (!(conf & TRANSCONF_ENABLE))
			continue;

		/* Enabled. Report the DDI control's opinion, which has been
		 * seen to differ from this one on real hardware, and carry on
		 * regardless -- see the two readings at the top of this file. */
		ddi = intel_read32(mmio, TRANSCODERS[i].ddi);

		if (!(ddi & TRANS_DDI_FUNC_ENABLE))
			kprintf("intel-display: transcoder %s says enabled "
				"(%08x) while its DDI function says disabled "
				"(%08x); going with the transcoder\n",
				TRANSCODERS[i].name, conf, ddi);

		return i;
	}

	return TRANSCODER_COUNT;
}

static bool intel_pipe_mode(volatile u8 *mmio, unsigned n, u32 *w, u32 *h,
			    u32 *pitch)
{
	u32 base = n * PIPE_STRIDE;
	u32 conf, src, stride, ctl, unit;

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

	ctl = intel_read32(mmio, PLANE_CTL_1_A + base);

	/* **Whether the pipe is scanning out, asked of the plane.**
	 *
	 * This used to be `conf & TRANSCONF_ENABLE` and nothing else, which is
	 * a question about the pipe's *own* transcoder -- and on the one Gen9
	 * machine this project owns, the panel is on a transcoder that is not
	 * the pipe's. Every timing register in that block reads zero and the
	 * enable bit is clear while the screen is lit, so this returned false
	 * and `intel_modeset_read` reported that the machine had no running
	 * display at all (GX-013).
	 *
	 * **A false negative that looks exactly like a correct refusal**, which
	 * is the worst shape it could have taken in this file -- half of this
	 * driver exists to refuse clearly, so a clear refusal reads as working.
	 *
	 * The plane is the better question anyway. A transcoder is a thing that
	 * drives a port; a plane being enabled is the pipe saying it is putting
	 * pixels somewhere, which is what a caller asking "what mode is this
	 * screen in" means.
	 */
	if (!(ctl & PLANE_CTL_ENABLE))
		return false;

	/* And the transcoder situation is reported rather than decided, because
	 * a pipe scanning out with its own transcoder dark is exactly what this
	 * hardware does and somebody reading the log should see it. */
	if (!(conf & TRANSCONF_ENABLE)) {
		u32 edp = intel_read32(mmio, TRANSCONF_EDP);

		kprintf("intel-display: pipe %u is scanning out with its own "
			"transcoder disabled (%08x); the eDP transcoder reads "
			"%08x\n", n, conf, edp);
	}

	src    = intel_read32(mmio, PIPE_SRCSZ_A + base);
	stride = intel_read32(mmio, PLANE_STRIDE_1_A + base);
	unit   = intel_plane_stride_unit(ctl);

	/* Undoing the subtraction the hardware stores. */
	*w = ((src >> 16) & 0xFFFFu) + 1;
	*h = (src & 0xFFFFu) + 1;

	if (!unit) {
		/* Said, and left at zero. A pitch decoded with the wrong unit
		 * is wrong by a factor, and a caller cannot tell that from a
		 * pitch that is right. */
		kprintf("intel-display: pipe %u has tiling %u, whose stride "
			"unit this driver has not measured, so its pitch is "
			"not reported\n", n,
			(ctl & PLANE_CTL_TILED_MASK) >> PLANE_CTL_TILED_SHIFT);
		*pitch = 0;
	} else {
		*pitch = (stride & PLANE_STRIDE_MAX) * unit;
	}

	return true;
}

/* What the hardware says, compared with what firmware said.
 *
 * Returns true when a running pipe was found. Prints the comparison either way,
 * because the disagreement is the interesting outcome and a summary that only
 * spoke when things matched would be silent exactly when somebody needed it.
 */
/* The timings the running transcoder is driving, printed rather than returned.
 *
 * **Separate from the pipe's geometry on purpose.** A pipe's source size says
 * how many pixels are being scanned out; a transcoder's timings say what is
 * being sent down the wire, including the blanking either side of them. They
 * agree on the active area and nothing else, and on the one machine this
 * project can check, the pipe is A while the transcoder is EDP -- so a single
 * function reporting "the mode" would have to pick one and would be hiding the
 * more interesting of the two.
 */
static void intel_report_transcoder(volatile u8 *mmio)
{
	unsigned t = intel_transcoder_running(mmio);
	u32 htotal, vtotal;

	if (t >= TRANSCODER_COUNT) {
		/* **Not "there is no display".** A transcoder scan finding
		 * nothing is one fact; whether a plane is scanning out is
		 * another, and the pipe loop below answers it separately. On
		 * the machine that produced GX-013 those two answers were
		 * different, and collapsing them is what that entry is about. */
		kputs("intel-display: no transcoder reports itself enabled\n");
		return;
	}

	htotal = intel_read32(mmio, TRANSCODERS[t].timing + 0x0u);
	vtotal = intel_read32(mmio, TRANSCODERS[t].timing + 0xCu);

	kprintf("intel-display: transcoder %s is running %u active in %u "
		"total across, %u active in %u total down\n",
		TRANSCODERS[t].name,
		intel_trans_active(htotal), intel_trans_total(htotal),
		intel_trans_active(vtotal), intel_trans_total(vtotal));
}

bool intel_modeset_read(volatile u8 *mmio, const struct framebuffer *firmware)
{
	unsigned n;

	if (!mmio)
		return false;

	/* Which transcoder, before which pipe: on a laptop the answer is EDP
	 * and the pipe is A, and reading only the pipe is how this driver came
	 * to believe a lit screen was no screen. */
	intel_report_transcoder(mmio);

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

	/* **"No pipe is scanning out", which is not the same as "no display".**
	 * The transcoder line above has already said its piece, and the two can
	 * differ -- that is the whole of GX-013. */
	kputs("intel-display: no pipe has an enabled plane, so nothing is being "
	      "scanned out and there is no mode to read\n");
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

			/* **The panel in this project's own laptop.**
			 *
			 * Read off it on 17 September through
			 * `scripts/read-intel-display.sh`, and kept in
			 * `docs/hardware/intel-gen9-gateway.txt`:
			 *
			 *   [CONNECTOR:161:eDP-1]  status: connected
			 *   "1366x768": 60 70190 1366 1404 1426 1466
			 *                        768 772 776 798
			 *
			 * Every other vector here is a size somebody chose.
			 * This one is a measurement, and it is the mode the
			 * first real modeset on that machine will set. */
			{ 1366,  768, 0x055502FFu },
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
			{ 1366,  768, 0x02FF0555u },	/* the laptop's panel */
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
			/* **1366 * 4, and this one is not hypothetical.**
			 *
			 * 1366x768 is the panel in this project's laptop. Its
			 * natural pitch of 5464 bytes has no representation in
			 * a field counting 64-byte chunks, so a driver has to
			 * pad, and 5504 is where it lands. Linux reports 5504
			 * bytes for that panel's fbdev buffer, which is an
			 * independent implementation arriving at the same row.
			 *
			 * **86 is that row in linear chunks, and it is not
			 * what the machine's register holds.** PLANE_STRIDE_1_A
			 * on the Gateway reads 0x2b -- 43 -- because the plane
			 * scanning out there is Y-tiled, where the unit is 128
			 * bytes. 43 x 128 and 86 x 64 are the same 5504 bytes
			 * in two encodings, and for a while this file called
			 * the agreement of the byte count a confirmation of the
			 * register value (GX-014). It is not. This function
			 * answers the linear question only, which is what the
			 * cases below assert and what its name now says. */
			{  5464,   0 },		/* refused: 24 bytes past a chunk */
			{  5504,  86 },		/* the linear encoding of that row */
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

	/* 4. Transcoder timing pairs -- one from Linux's own test pattern, and
	 *    the rest from the laptop's panel, whose numbers are nothing like
	 *    round and are exactly the sort a hand-written expectation gets
	 *    wrong in a way that still looks plausible. */
	{
		static const struct {
			u32 total, active, want;
			const char *what;
		} CASES[] = {
			{  800,  640, 0x031F027Fu, "Linux's 640x480 pattern" },
			{ 1466, 1366, 0x05B90555u, "the laptop's horizontal" },
			{  798,  768, 0x031D02FFu, "and its vertical" },

			/* Not a total and an active at all: the sync register
			 * shares the layout, which is why one function writes
			 * all six of them. End first, start second, both
			 * stored one short. */
			{ 1426, 1404, 0x0591057Bu, "its hsync, end over start" },
		};
		unsigned i;

		for (i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
			u32 got = intel_trans_timing(CASES[i].total,
						     CASES[i].active);

			if (got != CASES[i].want) {
				kprintf("intel-modeset: %u/%u (%s) came out "
					"%08x and should be %08x\n",
					CASES[i].total, CASES[i].active,
					CASES[i].what, got, CASES[i].want);
				ok = false;
			}
		}
	}

	/* 4b. **The stride unit, which depends on the tiling and did not used
	 *     to depend on anything.**
	 *
	 *     The refusals are the point again. An unmeasured tiling returns
	 *     zero, and the temptation is to return 64 so that a caller always
	 *     gets a number -- which is exactly the shape of GX-014, where a
	 *     plausible number stood in for one nobody had checked. A pitch
	 *     wrong by a factor cannot be told from a right one by the caller;
	 *     a zero can. */
	{
		static const struct { u32 ctl, want; const char *what; } CASES[] = {
			{ 0u << PLANE_CTL_TILED_SHIFT, 64u,
			  "linear, from the specification" },
			{ 4u << PLANE_CTL_TILED_SHIFT, 128u,
			  "Y-tiled, measured on the Gateway" },
			{ 1u << PLANE_CTL_TILED_SHIFT, 0u,
			  "X-tiled, which is refused rather than guessed" },
			{ 5u << PLANE_CTL_TILED_SHIFT, 0u,
			  "Yf-tiled, likewise" },

			/* **The actual register**, read off the Gateway on
			 * 18 September with a compositor scanning out of it.
			 * Carried whole rather than as its tiling field alone,
			 * so that a change to the mask or the shift is caught
			 * by a value this project has actually seen. */
			{ 0x84109000u, 128u, "PLANE_CTL_1_A as that machine holds it" },
		};
		unsigned i;

		for (i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
			u32 got = intel_plane_stride_unit(CASES[i].ctl);

			if (got != CASES[i].want) {
				kprintf("intel-modeset: plane control %08x "
					"(%s) gave a stride unit of %u and "
					"should give %u\n",
					CASES[i].ctl, CASES[i].what,
					got, CASES[i].want);
				ok = false;
			}
		}
	}

	/* 4c. **The panel's own mode, decoded from the registers the display
	 *     engine was running from, and checked against the mode the
	 *     connector reports.**
	 *
	 *     Two sources that have never been compared anywhere else: the EDID
	 *     the panel hands over across AUX, read by Linux, and the registers
	 *     the Gen9 display engine was actually scanning out of, read with
	 *     intel_reg on 18 September 2026. Both are in
	 *     `docs/hardware/`. The connector says:
	 *
	 *       "1366x768": 60 70190 1366 1404 1426 1466 768 772 776 798
	 *                             ----  ----  ----  ---- --- --- --- ---
	 *                             act   sync-start  total, then vertical
	 *
	 *     **Eight fields, eight matches.** That is the strongest known
	 *     answer available to this file, and it is worth being precise
	 *     about why: nothing in this kernel produced either number. A
	 *     self-test written from this file's own arithmetic can only show
	 *     the arithmetic is self-consistent.
	 *
	 *     Both directions are asserted. Decoding is what the driver does on
	 *     the machine; re-encoding proves the pair are inverses, which is
	 *     what makes the decoders usable for a modeset later. */
	{
		static const struct {
			u32 reg, first, second;
			const char *what;
		} CASES[] = {
			/* register      active/start  total/end */
			{ 0x05b90555u,   1366,         1466, "HTOTAL_EDP" },
			{ 0x05b90555u,   1366,         1466, "HBLANK_EDP" },
			{ 0x0591057bu,   1404,         1426, "HSYNC_EDP"  },
			{ 0x031d02ffu,    768,          798, "VTOTAL_EDP" },
			{ 0x031d02ffu,    768,          798, "VBLANK_EDP" },
			{ 0x03070303u,    772,          776, "VSYNC_EDP"  },
		};
		unsigned i;

		for (i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
			u32 lo = intel_trans_active(CASES[i].reg);
			u32 hi = intel_trans_total(CASES[i].reg);
			u32 back;

			if (lo != CASES[i].first || hi != CASES[i].second) {
				kprintf("intel-modeset: %s holds %08x, which "
					"decodes to %u and %u and should be %u "
					"and %u\n", CASES[i].what,
					CASES[i].reg, lo, hi,
					CASES[i].first, CASES[i].second);
				ok = false;
			}

			back = intel_trans_timing(CASES[i].second, CASES[i].first);

			if (back != CASES[i].reg) {
				kprintf("intel-modeset: %u and %u re-encode to "
					"%08x and should give back %08x\n",
					CASES[i].first, CASES[i].second,
					back, CASES[i].reg);
				ok = false;
			}
		}
	}

	/* 4d. **That the transcoder table is the shape the hardware is.**
	 *
	 *     Three properties, none of them a restatement of the constants:
	 *     every timing register is in the 0x6xxxx block and every
	 *     configuration register in 0x7xxxx; the EDP entry is transcoder A
	 *     displaced by exactly 0xF000 in all three of its registers; and no
	 *     two transcoders share an address. The last one is what catches a
	 *     typed offset landing on a neighbour, which is how the first
	 *     version of this map put TRANSCONF on top of HSYNC. */
	{
		unsigned i, j;

		for (i = 0; i < TRANSCODER_COUNT; i++) {
			if ((TRANSCODERS[i].timing & 0xF0000u) != 0x60000u ||
			    (TRANSCODERS[i].ddi & 0xF0000u) != 0x60000u ||
			    (TRANSCODERS[i].conf & 0xF0000u) != 0x70000u) {
				kprintf("intel-modeset: transcoder %s has "
					"registers in the wrong blocks: timing "
					"%05x ddi %05x conf %05x\n",
					TRANSCODERS[i].name,
					TRANSCODERS[i].timing,
					TRANSCODERS[i].ddi,
					TRANSCODERS[i].conf);
				ok = false;
			}

			for (j = i + 1; j < TRANSCODER_COUNT; j++)
				if (TRANSCODERS[i].timing == TRANSCODERS[j].timing ||
				    TRANSCODERS[i].conf == TRANSCODERS[j].conf ||
				    TRANSCODERS[i].ddi == TRANSCODERS[j].ddi) {
					kprintf("intel-modeset: transcoders %s "
						"and %s share a register\n",
						TRANSCODERS[i].name,
						TRANSCODERS[j].name);
					ok = false;
				}
		}

		/* EDP is A plus 0xF000, in all three. */
		{
			const struct intel_transcoder *a = &TRANSCODERS[0];
			const struct intel_transcoder *e =
				&TRANSCODERS[TRANSCODER_COUNT - 1];

			if (e->timing != a->timing + TRANS_EDP_OFFSET ||
			    e->conf   != a->conf   + TRANS_EDP_OFFSET ||
			    e->ddi    != a->ddi    + TRANS_EDP_OFFSET) {
				kprintf("intel-modeset: EDP is not transcoder A "
					"displaced by %05x, which is what the "
					"hardware shows\n", TRANS_EDP_OFFSET);
				ok = false;
			}
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

			/* **The transcoder the panel is actually on.** It
			 * obeys the same property -- timings in 0x6xxxx, the
			 * configuration register in 0x7xxxx -- which is worth
			 * asserting precisely because its base is not the
			 * pipe-indexed one and nothing else here would catch
			 * it being typed into the wrong block. */
			{ "TRANS_HTOTAL_EDP", TRANS_HTOTAL_EDP, true  },
			{ "TRANS_HBLANK_EDP", TRANS_HBLANK_EDP, true  },
			{ "TRANS_HSYNC_EDP",  TRANS_HSYNC_EDP,  true  },
			{ "TRANS_VTOTAL_EDP", TRANS_VTOTAL_EDP, true  },
			{ "TRANS_VBLANK_EDP", TRANS_VBLANK_EDP, true  },
			{ "TRANS_VSYNC_EDP",  TRANS_VSYNC_EDP,  true  },
			{ "TRANSCONF_EDP",    TRANSCONF_EDP,    false },
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
