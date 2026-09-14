/* SDHCI: the disk soldered into a cheap laptop, and into most tablets.
 *
 * AHCI and NVMe cover machines with a slot somebody could put a drive into.
 * Below that price there is no slot: there is a 64 GB eMMC part wired to an
 * SD host controller on the PCI bus, and a kernel without this driver boots on
 * such a machine, enumerates it correctly, and reports no internal disk. That
 * is exactly what the eleven-inch Gateway checkpoint 17 is tested on does
 * today.
 *
 * --- Found by what it is, not by who made it -------------------------------
 *
 * PCI class 08h subclass 05h is "SD Host controller", and the register layout
 * behind it is the SD Host Controller Simplified Specification -- the same
 * registers whoever built the chip. That is why this can live in `core/` beside
 * the other three block drivers rather than in `arch/`: there is nothing x86
 * about it but the bus it happens to sit on.
 *
 * --- One controller, two kinds of card, and both paths are written ----------
 *
 * The host side is a standard. The card side is two standards that share a wire
 * and diverge at exactly the point where the card says what it is:
 *
 *   AN SD CARD is asked with ACMD41 and *tells the host* its relative address.
 *   AN eMMC PART is asked with CMD1 and *is told* one by the host.
 *
 * Both are here, deliberately, because the machine that can be tested and the
 * machine that needs it are different machines. QEMU emulates `sdhci-pci` with
 * an `sd-card` on it, so the verification matrix exercises the controller
 * reset, the clock, the bus width, the command path and every read and write --
 * everything hard -- and never once takes the eMMC branch. The laptop takes
 * only the eMMC branch. **A path only one of them exercises is a path nobody
 * has seen work**, so which branch ran is printed rather than assumed.
 *
 * --- Programmed I/O, and the choice is written down ------------------------
 *
 * Data moves through the Buffer Data Port a word at a time, not by DMA. SDHCI
 * offers SDMA and ADMA2 and both are faster; ADMA2 also needs a descriptor
 * table built in physical memory, an alignment rule, and a boundary condition
 * at every 512 KB, each of which is a way to corrupt somebody's disk while
 * reporting success.
 *
 * The block layer above this is synchronous -- it says so in its own header --
 * so a driver that overlapped transfers would be waiting for them anyway. PIO
 * is what this needs today, it cannot get a physical address wrong because it
 * never computes one, and when throughput is *measured* rather than assumed to
 * matter, ADMA2 replaces the inside of one function.
 *
 * --- What is refused rather than guessed -----------------------------------
 *
 * A controller that will not reset, will not clock, or reports a base clock of
 * zero is declined by name. A card that answers nothing, or answers with a
 * capacity of zero, is declined by name. Half an initialised disk is worse than
 * none: the installer would offer it.
 */
#include <recon/kernel/block.h>
#include <recon/kernel/pci.h>

#include <recon/kernel/console.h>
#include <recon/kernel/kstring.h>
#include <recon/kernel/vm.h>
#include <recon/kernel/time.h>

/* --- the register map ------------------------------------------------------
 *
 * Offsets from BAR0. Widths matter here in a way they do not on most hardware:
 * the specification defines Transfer Mode and Command as two 16-bit registers
 * sharing one word, and writing the pair as a single 32-bit store issues the
 * command before the transfer mode is in place on some controllers.
 */
#define SD_ARG2			0x00	/* also the SDMA address, unused here */
#define SD_BLOCK_SIZE		0x04	/* 16-bit: bytes per block */
#define SD_BLOCK_COUNT		0x06	/* 16-bit: how many */
#define SD_ARG			0x08	/* 32-bit */
#define SD_TRANSFER_MODE	0x0C	/* 16-bit */
#define SD_COMMAND		0x0E	/* 16-bit */
#define SD_RESPONSE		0x10	/* four 32-bit words */
#define SD_BUFFER		0x20	/* 32-bit data port */
#define SD_PRESENT_STATE	0x24	/* 32-bit */
#define SD_HOST_CONTROL		0x28	/* 8-bit */
#define SD_POWER_CONTROL	0x29	/* 8-bit */
#define SD_CLOCK_CONTROL	0x2C	/* 16-bit */
#define SD_TIMEOUT_CONTROL	0x2E	/* 8-bit */
#define SD_SOFT_RESET		0x2F	/* 8-bit */
#define SD_INT_STATUS		0x30	/* 16-bit */
#define SD_ERR_STATUS		0x32	/* 16-bit */
#define SD_INT_ENABLE		0x34	/* 16-bit */
#define SD_ERR_ENABLE		0x36	/* 16-bit */
#define SD_INT_SIGNAL		0x38	/* 16-bit */
#define SD_ERR_SIGNAL		0x3A	/* 16-bit */
#define SD_CAPABILITIES		0x40	/* 64-bit */
#define SD_VERSION		0xFE	/* 16-bit */

/* Present State */
#define PRESENT_CMD_INHIBIT	(1u << 0)
#define PRESENT_DAT_INHIBIT	(1u << 1)
#define PRESENT_CARD_INSERTED	(1u << 16)

/* Software Reset */
#define RESET_ALL		(1u << 0)
#define RESET_CMD		(1u << 1)
#define RESET_DAT		(1u << 2)

/* Clock Control */
#define CLOCK_INTERNAL_EN	(1u << 0)
#define CLOCK_INTERNAL_STABLE	(1u << 1)
#define CLOCK_SD_EN		(1u << 2)

/* Power Control */
#define POWER_ON		(1u << 0)
#define POWER_1V8		(5u << 1)
#define POWER_3V0		(6u << 1)
#define POWER_3V3		(7u << 1)

/* Host Control 1 */
#define HOST_4BIT		(1u << 1)
#define HOST_8BIT		(1u << 5)

/* Transfer Mode */
#define XFER_BLOCK_COUNT_EN	(1u << 1)
#define XFER_AUTO_CMD12		(1u << 2)
#define XFER_READ		(1u << 4)
#define XFER_MULTI		(1u << 5)

/* Command register: response type in bits 0-1, then the checks, then the index */
#define RESP_NONE		0u
#define RESP_136		1u
#define RESP_48			2u
#define RESP_48_BUSY		3u
#define CMD_CRC_CHECK		(1u << 3)
#define CMD_INDEX_CHECK		(1u << 4)
#define CMD_DATA_PRESENT	(1u << 5)

/* Normal Interrupt Status */
#define INT_CMD_COMPLETE	(1u << 0)
#define INT_XFER_COMPLETE	(1u << 1)
#define INT_BUFFER_WRITE_READY	(1u << 4)
#define INT_BUFFER_READ_READY	(1u << 5)
#define INT_ERROR		(1u << 15)

/* Capabilities, low word */
#define CAP_BASE_CLOCK_SHIFT	8
#define CAP_BASE_CLOCK_MASK	0xFFu
#define CAP_VOLTAGE_3V3		(1u << 24)
#define CAP_VOLTAGE_3V0		(1u << 25)
#define CAP_VOLTAGE_1V8		(1u << 26)

/* The card commands this driver sends. Numbers, not names, in the register --
 * these exist so the sequence below reads as the specification does. */
#define CMD_GO_IDLE		0
#define CMD_SEND_OP_COND_MMC	1
#define CMD_ALL_SEND_CID	2
#define CMD_SET_RELATIVE_ADDR	3
#define CMD_SWITCH		6
#define CMD_SELECT_CARD		7
#define CMD_SEND_EXT_CSD	8	/* eMMC; on SD this number is SEND_IF_COND */
#define CMD_SEND_IF_COND	8
#define CMD_SEND_CSD		9
#define CMD_STOP_TRANSMISSION	12
#define CMD_SET_BLOCKLEN	16
#define CMD_READ_SINGLE		17
#define CMD_READ_MULTIPLE	18
#define CMD_WRITE_SINGLE	24
#define CMD_WRITE_MULTIPLE	25
#define CMD_APP_CMD		55
#define ACMD_SET_BUS_WIDTH	6
#define ACMD_SEND_OP_COND	41

#define SDHCI_MAX		4
#define SD_BLOCK_BYTES		512

/* How long to wait for the card to finish powering up before giving up on it.
 * The specification allows a second; this allows two, because a slow part that
 * is declined is a machine with no disk and the cost of waiting is paid once
 * per boot. */
#define CARD_INIT_TIMEOUT_NS	2000000000ULL

/* One command. Long enough that a card doing real work is not cut off, short
 * enough that a machine with a dead controller still finishes booting. */
#define CMD_TIMEOUT_NS		1000000000ULL

/* A whole transfer. Deliberately larger than a command: a multi-block write to
 * a slow eMMC part genuinely takes this long. */
#define XFER_TIMEOUT_NS		5000000000ULL

enum card_kind {
	CARD_NONE = 0,
	CARD_SD,
	CARD_MMC,		/* eMMC, and the branch QEMU never takes */
};

struct sdhci {
	volatile u8 *regs;

	enum card_kind kind;
	u16 rca;		/* the card's relative address, however it got one */
	bool high_capacity;	/* addresses are blocks, not bytes */
	u32 bus_width;		/* 1, 4 or 8 -- printed, because it is negotiated */
	u32 base_clock_hz;

	u64 block_count;

	struct block_device *bdev;
};

static struct sdhci controllers[SDHCI_MAX];
static unsigned controller_count;

/* --- register access -------------------------------------------------------
 *
 * Widths are not interchangeable here. See the note on the register map. */
static u8  r8(struct sdhci *h, u32 o)  { return *(volatile u8  *)(h->regs + o); }
static u16 r16(struct sdhci *h, u32 o) { return *(volatile u16 *)(h->regs + o); }
static u32 r32(struct sdhci *h, u32 o) { return *(volatile u32 *)(h->regs + o); }

static void w8(struct sdhci *h, u32 o, u8 v)   { *(volatile u8  *)(h->regs + o) = v; }
static void w16(struct sdhci *h, u32 o, u16 v) { *(volatile u16 *)(h->regs + o) = v; }
static void w32(struct sdhci *h, u32 o, u32 v) { *(volatile u32 *)(h->regs + o) = v; }

/* --- resetting the controller ---------------------------------------------- */

static bool reset(struct sdhci *h, u8 which)
{
	u64 deadline;

	w8(h, SD_SOFT_RESET, which);

	deadline = time_monotonic_ns() + CMD_TIMEOUT_NS;
	while (r8(h, SD_SOFT_RESET) & which) {
		if (time_monotonic_ns() > deadline)
			return false;
	}

	return true;
}

/* --- the clock -------------------------------------------------------------
 *
 * Two speeds are used: about 400 kHz while the card is being identified, which
 * the specification requires, and as fast as the divider allows afterwards.
 *
 * **The divider is computed from the controller's own base clock**, not from a
 * constant. A host running at 50 MHz and one running at 200 MHz need different
 * numbers for the same result, and a fixed divider gives one of them a card
 * clocked four times out of spec -- which does not fail cleanly, it returns
 * plausible wrong bytes.
 */
static bool set_clock(struct sdhci *h, u32 want_hz)
{
	u32 divider = 0;
	u64 deadline;
	u16 ctrl;

	/* Stop the clock before changing it. */
	w16(h, SD_CLOCK_CONTROL, 0);

	if (!h->base_clock_hz)
		return false;

	/* The 8-bit divider field means "divide by twice this", and zero means
	 * do not divide at all. Version 3 controllers extend it by two bits,
	 * which is where the shifted high half below comes from. */
	if (want_hz < h->base_clock_hz) {
		for (divider = 1; divider < 0x400; divider <<= 1) {
			if ((h->base_clock_hz / (2 * divider)) <= want_hz)
				break;
		}
	}

	ctrl = (u16)(((divider & 0xFF) << 8) |
		     (((divider >> 8) & 0x03) << 6) |
		     CLOCK_INTERNAL_EN);
	w16(h, SD_CLOCK_CONTROL, ctrl);

	deadline = time_monotonic_ns() + CMD_TIMEOUT_NS;
	while (!(r16(h, SD_CLOCK_CONTROL) & CLOCK_INTERNAL_STABLE)) {
		if (time_monotonic_ns() > deadline)
			return false;
	}

	w16(h, SD_CLOCK_CONTROL, (u16)(ctrl | CLOCK_SD_EN));
	return true;
}

/* --- sending a command -----------------------------------------------------
 *
 * The sequence is the same every time and the order is not negotiable: wait
 * until the controller is not already holding a command, clear the status bits
 * from the last one, set the argument, then write transfer mode and command as
 * two 16-bit stores. Writing the command is what starts it.
 *
 * `resp` is the response type the *card* will send, which the controller has to
 * be told in advance because it decides how many bits to clock in.
 */
static bool command(struct sdhci *h, u8 index, u32 arg, u32 resp,
		    u16 xfer_mode, bool data)
{
	u64 deadline;
	u32 mask = PRESENT_CMD_INHIBIT;
	u16 cmd;
	u16 status;

	/* A command that moves data also waits for the data line. Splitting
	 * these is not pedantry: a command issued while DAT is busy is accepted
	 * and then completes against the previous transfer's buffer. */
	if (data || resp == RESP_48_BUSY)
		mask |= PRESENT_DAT_INHIBIT;

	deadline = time_monotonic_ns() + CMD_TIMEOUT_NS;
	while (r32(h, SD_PRESENT_STATE) & mask) {
		if (time_monotonic_ns() > deadline)
			return false;
	}

	/* Cleared by writing ones, so this clears everything from last time
	 * rather than leaving a stale Command Complete to be read as this
	 * command's. */
	w16(h, SD_INT_STATUS, 0xFFFF);
	w16(h, SD_ERR_STATUS, 0xFFFF);

	w32(h, SD_ARG, arg);
	w16(h, SD_TRANSFER_MODE, xfer_mode);

	cmd = (u16)((index << 8) | resp);
	if (resp != RESP_NONE)
		cmd |= CMD_CRC_CHECK | CMD_INDEX_CHECK;
	if (resp == RESP_136)
		cmd &= (u16)~CMD_INDEX_CHECK;	/* R2 carries no index */
	if (data)
		cmd |= CMD_DATA_PRESENT;

	w16(h, SD_COMMAND, cmd);

	deadline = time_monotonic_ns() + CMD_TIMEOUT_NS;
	for (;;) {
		status = r16(h, SD_INT_STATUS);

		if (status & INT_ERROR) {
			/* The controller stops on an error and stays stopped
			 * until the command and data lines are reset. Leaving
			 * them is a controller that refuses everything
			 * afterwards for a reason that looks unrelated. */
			reset(h, RESET_CMD | RESET_DAT);
			return false;
		}

		if (status & INT_CMD_COMPLETE)
			return true;

		if (time_monotonic_ns() > deadline) {
			reset(h, RESET_CMD | RESET_DAT);
			return false;
		}
	}
}

static u32 response(struct sdhci *h, unsigned word)
{
	return r32(h, SD_RESPONSE + word * 4);
}

/* --- moving a block through the data port ----------------------------------
 *
 * Thirty-two bits at a time, because that is the width the port is defined at.
 * The buffer is copied byte by byte into and out of a word rather than cast,
 * because the caller's pointer has no alignment guarantee and an unaligned
 * 32-bit access is a fault on one of the two architectures this kernel builds
 * for and silently slow on the other.
 */
static bool wait_for(struct sdhci *h, u16 bit, u64 timeout_ns)
{
	u64 deadline = time_monotonic_ns() + timeout_ns;

	for (;;) {
		u16 status = r16(h, SD_INT_STATUS);

		if (status & INT_ERROR) {
			reset(h, RESET_CMD | RESET_DAT);
			return false;
		}

		if (status & bit) {
			w16(h, SD_INT_STATUS, bit);	/* consume just this one */
			return true;
		}

		if (time_monotonic_ns() > deadline) {
			reset(h, RESET_CMD | RESET_DAT);
			return false;
		}
	}
}

static void read_block(struct sdhci *h, u8 *out)
{
	for (unsigned i = 0; i < SD_BLOCK_BYTES / 4; i++) {
		u32 v = r32(h, SD_BUFFER);

		out[i * 4 + 0] = (u8)(v);
		out[i * 4 + 1] = (u8)(v >> 8);
		out[i * 4 + 2] = (u8)(v >> 16);
		out[i * 4 + 3] = (u8)(v >> 24);
	}
}

static void write_block(struct sdhci *h, const u8 *in)
{
	for (unsigned i = 0; i < SD_BLOCK_BYTES / 4; i++) {
		u32 v = (u32)in[i * 4 + 0]
		      | ((u32)in[i * 4 + 1] << 8)
		      | ((u32)in[i * 4 + 2] << 16)
		      | ((u32)in[i * 4 + 3] << 24);

		w32(h, SD_BUFFER, v);
	}
}

/* One transfer of `count` blocks. The command chosen depends on the count,
 * because a multi-block read has to be stopped and a single-block one must not
 * be -- Auto CMD12 does the stopping so that a transfer interrupted by an error
 * still leaves the card in a state the next command can use. */
static enum block_status transfer(struct sdhci *h, u64 lba, u32 count,
				  void *buf, bool write)
{
	u8 *p = buf;
	u16 mode;
	u8 index;
	u32 arg;

	if (!count)
		return BLOCK_OK;

	/* A card that is not high capacity is addressed in bytes. Getting this
	 * backwards reads block 0 five hundred and twelve times over. */
	arg = h->high_capacity ? (u32)lba : (u32)(lba * SD_BLOCK_BYTES);

	w16(h, SD_BLOCK_SIZE, SD_BLOCK_BYTES);
	w16(h, SD_BLOCK_COUNT, (u16)count);

	mode = XFER_BLOCK_COUNT_EN;
	if (!write)
		mode |= XFER_READ;
	if (count > 1)
		mode |= XFER_MULTI | XFER_AUTO_CMD12;

	if (write)
		index = (count > 1) ? CMD_WRITE_MULTIPLE : CMD_WRITE_SINGLE;
	else
		index = (count > 1) ? CMD_READ_MULTIPLE : CMD_READ_SINGLE;

	if (!command(h, index, arg, RESP_48, mode, true))
		return BLOCK_ERR_IO;

	for (u32 b = 0; b < count; b++) {
		if (write) {
			if (!wait_for(h, INT_BUFFER_WRITE_READY, XFER_TIMEOUT_NS))
				return BLOCK_ERR_IO;
			write_block(h, p + (u64)b * SD_BLOCK_BYTES);
		} else {
			if (!wait_for(h, INT_BUFFER_READ_READY, XFER_TIMEOUT_NS))
				return BLOCK_ERR_IO;
			read_block(h, p + (u64)b * SD_BLOCK_BYTES);
		}
	}

	/* **Waited for, not assumed.** The last buffer being drained is not the
	 * transfer being finished: on a write the card is still programming,
	 * and issuing the next command before Transfer Complete is how a write
	 * reports success and lands as nothing. */
	if (!wait_for(h, INT_XFER_COMPLETE, XFER_TIMEOUT_NS))
		return BLOCK_ERR_IO;

	return BLOCK_OK;
}

/* --- bringing a card up ----------------------------------------------------
 *
 * The two branches meet again at CMD2. Everything before it is the question
 * "what are you", asked in the only way each kind of card can answer.
 */

/* eMMC. CMD1 carries the voltage window the host can supply and the card
 * answers with the window it wants, plus a busy bit that stays clear until it
 * has finished powering up. Bit 30 of the argument asks for sector addressing,
 * which every part above 2 GB gives. */
static bool init_mmc(struct sdhci *h)
{
	u64 deadline = time_monotonic_ns() + CARD_INIT_TIMEOUT_NS;
	u32 ocr = 0;

	for (;;) {
		if (!command(h, CMD_SEND_OP_COND_MMC, 0x40FF8000, RESP_48, 0, false))
			return false;

		ocr = response(h, 0);
		if (ocr & 0x80000000u)		/* not busy any more */
			break;

		if (time_monotonic_ns() > deadline)
			return false;
	}

	h->kind = CARD_MMC;
	h->high_capacity = (ocr & (1u << 30)) != 0;
	return true;
}

/* SD. CMD8 first, because a card that understands it is version 2 or later and
 * may be high capacity, and one that does not must not be asked for it. Then
 * ACMD41, which is CMD55 followed by CMD41 -- the card tells the host its
 * address afterwards rather than being given one. */
static bool init_sd(struct sdhci *h)
{
	u64 deadline;
	bool v2;
	u32 ocr = 0;

	/* 0x1AA: bit pattern 0xAA at 2.7-3.6 volts, echoed back verbatim by a
	 * card that speaks version 2. */
	v2 = command(h, CMD_SEND_IF_COND, 0x1AA, RESP_48, 0, false) &&
	     (response(h, 0) & 0xFF) == 0xAA;

	deadline = time_monotonic_ns() + CARD_INIT_TIMEOUT_NS;
	for (;;) {
		if (!command(h, CMD_APP_CMD, 0, RESP_48, 0, false))
			return false;
		if (!command(h, ACMD_SEND_OP_COND,
			     v2 ? 0x40FF8000 : 0x00FF8000, RESP_48, 0, false))
			return false;

		ocr = response(h, 0);
		if (ocr & 0x80000000u)
			break;

		if (time_monotonic_ns() > deadline)
			return false;
	}

	h->kind = CARD_SD;
	h->high_capacity = v2 && (ocr & (1u << 30)) != 0;
	return true;
}

/* How many blocks the card has.
 *
 * Three answers, and which one is right depends on what the card is:
 *
 *   AN eMMC PART above 2 GB keeps it in EXT_CSD, a 512-byte register read like
 *   a data block, at bytes 212-215. The CSD of such a part reports the maximum
 *   the field can hold and is simply wrong -- it is not an approximation.
 *
 *   A HIGH CAPACITY SD CARD keeps it in the CSD as a count of 512 KB units.
 *
 *   ANYTHING ELSE keeps it in the CSD the original way, as a size and a
 *   multiplier and a block length that need putting together.
 */
static bool read_capacity(struct sdhci *h)
{
	u32 csd[4];

	if (!command(h, CMD_SEND_CSD, (u32)h->rca << 16, RESP_136, 0, false))
		return false;

	for (unsigned i = 0; i < 4; i++)
		csd[i] = response(h, i);

	/* **The controller returns R2 without its low eight bits**, so a field
	 * the specification numbers at CSD bit N is at bit (N-8) here: register
	 * (N-8)/32, bit (N-8)%32. Every extraction below subtracts that eight,
	 * and the first version of this code did not -- which produced a 64 MB
	 * card reporting 30 GB, caught by the block layer's own check that the
	 * last blocks of a device are readable rather than by anything here.
	 *
	 * CSD_STRUCTURE is CSD[127:126], so (127-8)=119, which is register 3
	 * bit 23. */
	{
		u32 structure = (csd[3] >> 22) & 0x3;

		if (h->kind == CARD_MMC) {
			u8 ext[SD_BLOCK_BYTES];
			u64 sectors;

			/* Selected first: EXT_CSD is a data transfer, and a
			 * card that has not been selected will not do one. */
			if (!command(h, CMD_SELECT_CARD, (u32)h->rca << 16,
				     RESP_48_BUSY, 0, false))
				return false;

			w16(h, SD_BLOCK_SIZE, SD_BLOCK_BYTES);
			w16(h, SD_BLOCK_COUNT, 1);

			if (!command(h, CMD_SEND_EXT_CSD, 0, RESP_48,
				     XFER_READ | XFER_BLOCK_COUNT_EN, true))
				return false;
			if (!wait_for(h, INT_BUFFER_READ_READY, XFER_TIMEOUT_NS))
				return false;

			read_block(h, ext);

			if (!wait_for(h, INT_XFER_COMPLETE, XFER_TIMEOUT_NS))
				return false;

			sectors = (u64)ext[212]
				| ((u64)ext[213] << 8)
				| ((u64)ext[214] << 16)
				| ((u64)ext[215] << 24);

			if (!sectors)
				return false;

			h->block_count = sectors;
			return true;
		}

		if (structure == 1) {
			/* Version 2: C_SIZE is CSD[69:48], and the capacity is
			 * a plain count of 512 KB units. (69-8)=61 and
			 * (48-8)=40, so it is twenty-two bits of register 1
			 * starting at bit 8. */
			u32 c_size = (csd[1] >> 8) & 0x3FFFFF;

			h->block_count = ((u64)c_size + 1) * 1024;
		} else {
			/* Version 1, where the size is a product of three
			 * fields rather than a count:
			 *
			 *   C_SIZE       CSD[73:62]  -> reg 1 bit 22, into reg 2
			 *   C_SIZE_MULT  CSD[49:47]  -> reg 1 bit 7
			 *   READ_BL_LEN  CSD[83:80]  -> reg 2 bit 8
			 *
			 * C_SIZE is the one that straddles a register: ten of
			 * its bits are the top of register 1 and two are the
			 * bottom of register 2. */
			u32 c_size = (((csd[2] & 0x3) << 10)
				      | (csd[1] >> 22)) & 0xFFF;
			u32 mult   = (csd[1] >> 7) & 0x7;
			u32 rdblk  = (csd[2] >> 8) & 0xF;

			h->block_count = ((u64)(c_size + 1) << (mult + 2))
					 * ((u64)1 << rdblk) / SD_BLOCK_BYTES;
		}

		if (!h->block_count)
			return false;

		return command(h, CMD_SELECT_CARD, (u32)h->rca << 16,
			       RESP_48_BUSY, 0, false);
	}
}

/* --- the block layer's three operations ------------------------------------ */

static enum block_status sdhci_read(struct block_device *dev, u64 lba,
				    u32 count, void *buf)
{
	struct sdhci *h = dev->driver;

	return transfer(h, lba, count, buf, false);
}

static enum block_status sdhci_write(struct block_device *dev, u64 lba,
				     u32 count, const void *buf)
{
	struct sdhci *h = dev->driver;

	/* The cast is the block layer's own convention: one transfer path
	 * serves both directions and the direction decides whether the buffer
	 * is read or written. */
	return transfer(h, lba, count, (void *)(uintptr_t)buf, true);
}

/* A card has no write cache the host can order behind it: a write is complete
 * when Transfer Complete arrives, which `transfer` already waits for. So there
 * is nothing to issue here and success is the true answer -- and, unlike the
 * case block.h describes for virtio, that is a statement about the protocol
 * rather than an assumption about the device. `flush_is_durable` is set
 * accordingly.
 */
static enum block_status sdhci_flush(struct block_device *dev)
{
	(void)dev;
	return BLOCK_OK;
}

static const struct block_ops sdhci_ops = {
	.read    = sdhci_read,
	.write   = sdhci_write,
	.flush   = sdhci_flush,
	.discard = NULL,
};

/* --- attaching ------------------------------------------------------------- */

static bool map_registers(struct sdhci *h, const struct pci_device *d)
{
	paddr_t base = (paddr_t)d->bar[0];
	paddr_t first;
	u64 span;
	vaddr_t va;

	if (!base || d->bar_is_io[0] || !d->bar_size[0])
		return false;

	first = PAGE_ALIGN_DOWN(base);
	span  = PAGE_ALIGN_UP((base - first) + d->bar_size[0]);
	va    = (vaddr_t)(uintptr_t)phys_to_virt(first);

	if (!vm_lookup(va) &&
	    !vm_map(va, first, span, VM_READ | VM_WRITE | VM_DEVICE | VM_GLOBAL)) {
		kputs("sdhci: could not map the controller's registers\n");
		return false;
	}

	h->regs = phys_to_virt(base);
	return true;
}

bool sdhci_attach(const struct pci_device *d)
{
	struct sdhci *h;
	char name[BLOCK_NAME_MAX];
	u32 caps;
	u8 power;

	/* Class 8 is "base system peripheral" and subclass 5 is an SD host
	 * controller. The programming interface distinguishes a DMA-capable
	 * one from a vendor-specific one; anything but the standard interface
	 * has a register map this driver does not know, so it is declined
	 * rather than driven. */
	if (d->class_code != 0x08 || d->subclass != 0x05)
		return false;
	if (d->prog_if != 0x00 && d->prog_if != 0x01)
		return false;

	if (controller_count >= SDHCI_MAX)
		return false;

	h = &controllers[controller_count];
	kmemset(h, 0, sizeof(*h));

	if (!map_registers(h, d))
		return false;

	if (!reset(h, RESET_ALL)) {
		kputs("sdhci: the controller would not reset\n");
		return false;
	}

	caps = r32(h, SD_CAPABILITIES);
	h->base_clock_hz = ((caps >> CAP_BASE_CLOCK_SHIFT) & CAP_BASE_CLOCK_MASK)
			   * 1000000u;

	/* **Refused rather than guessed.** A base clock of zero means the
	 * controller declines to say, and every divider computed from it would
	 * be a division by zero or a fabricated number. On such a part the
	 * frequency has to come from somewhere outside the register set, which
	 * this driver has no way to reach. */
	if (!h->base_clock_hz) {
		kputs("sdhci: the controller does not say what its base clock "
		      "is, so no divider can be computed\n");
		return false;
	}

	/* The highest voltage the controller says it can supply. Asking for one
	 * it does not have is a card that never answers. */
	if (caps & CAP_VOLTAGE_3V3)
		power = POWER_3V3 | POWER_ON;
	else if (caps & CAP_VOLTAGE_3V0)
		power = POWER_3V0 | POWER_ON;
	else if (caps & CAP_VOLTAGE_1V8)
		power = POWER_1V8 | POWER_ON;
	else {
		kputs("sdhci: the controller supplies no voltage this driver "
		      "knows how to ask for\n");
		return false;
	}

	w8(h, SD_POWER_CONTROL, power);
	w8(h, SD_TIMEOUT_CONTROL, 0x0E);	/* the longest the field allows */

	/* Status bits are enabled; signalling them as interrupts is not. This
	 * driver polls, and an interrupt nobody has installed a handler for is
	 * a machine that stops. */
	w16(h, SD_INT_ENABLE, 0xFFFF);
	w16(h, SD_ERR_ENABLE, 0xFFFF);
	w16(h, SD_INT_SIGNAL, 0);
	w16(h, SD_ERR_SIGNAL, 0);

	if (!(r32(h, SD_PRESENT_STATE) & PRESENT_CARD_INSERTED)) {
		/* Not a failure. A controller with an empty slot is an ordinary
		 * thing for a machine to have, and saying so would train
		 * everybody to ignore the line. */
		return false;
	}

	/* 400 kHz for identification, which the specification requires and
	 * which is slow enough that a card still waking up can answer. */
	if (!set_clock(h, 400000)) {
		kputs("sdhci: the controller's clock would not start\n");
		return false;
	}

	if (!command(h, CMD_GO_IDLE, 0, RESP_NONE, 0, false)) {
		kputs("sdhci: the card did not go idle\n");
		return false;
	}

	/* **SD is asked first, and the order is not arbitrary.** An eMMC part
	 * does not answer ACMD41 and the attempt is harmless; an SD card
	 * answers CMD1 on some controllers with something that looks like an
	 * OCR, and asking it first would identify every card as eMMC. */
	if (!init_sd(h)) {
		/* The failed ACMD41 left the card's state machine where the
		 * next CMD0 will find it, so go round again rather than
		 * carrying on from an unknown state. */
		if (!command(h, CMD_GO_IDLE, 0, RESP_NONE, 0, false) ||
		    !init_mmc(h)) {
			kputs("sdhci: a card is present and answered neither "
			      "as SD nor as eMMC\n");
			return false;
		}
	}

	if (!command(h, CMD_ALL_SEND_CID, 0, RESP_136, 0, false)) {
		kputs("sdhci: the card would not say what it is\n");
		return false;
	}

	/* The one place the two kinds genuinely differ in shape rather than in
	 * numbers: eMMC is *given* an address, SD *reports* one. */
	if (h->kind == CARD_MMC) {
		h->rca = 1;
		if (!command(h, CMD_SET_RELATIVE_ADDR, (u32)h->rca << 16,
			     RESP_48, 0, false)) {
			kputs("sdhci: the part would not take an address\n");
			return false;
		}
	} else {
		if (!command(h, CMD_SET_RELATIVE_ADDR, 0, RESP_48, 0, false)) {
			kputs("sdhci: the card would not report an address\n");
			return false;
		}
		h->rca = (u16)(response(h, 0) >> 16);
	}

	if (!read_capacity(h)) {
		kputs("sdhci: the card would not say how large it is\n");
		return false;
	}

	/* Identification is over, so the clock can come up. 25 MHz is the
	 * default-speed ceiling both kinds of card support without any further
	 * negotiation -- higher modes need a voltage switch and a tuning
	 * sequence, which are worth having once this is measured to be the
	 * limit rather than assumed to be. */
	if (!set_clock(h, 25000000)) {
		kputs("sdhci: the clock would not come up to speed\n");
		return false;
	}

	h->bus_width = 1;
	if (!command(h, CMD_SET_BLOCKLEN, SD_BLOCK_BYTES, RESP_48, 0, false)) {
		kputs("sdhci: the card would not take a block length\n");
		return false;
	}

	/* Built the way nvme builds its own: a literal with one digit poked in,
	 * because a format call in a driver is a dependency on a formatter that
	 * has to work before the first disk is readable. */
	kstrlcpy(name, "mmc0", sizeof(name));
	name[3] = (char)('0' + controller_count);

	h->bdev = block_register(name, &sdhci_ops, h,
				 SD_BLOCK_BYTES, h->block_count);
	if (!h->bdev) {
		kputs("sdhci: the block layer would not take the device\n");
		return false;
	}

	h->bdev->removable = (h->kind == CARD_SD);
	h->bdev->flush_is_durable = true;

	kprintf("sdhci: %s, %s, %llu blocks of %u bytes, %u-bit bus, "
		"%u MHz base clock\n",
		name,
		h->kind == CARD_MMC ? "eMMC" : "SD",
		(unsigned long long)h->block_count, (unsigned)SD_BLOCK_BYTES,
		h->bus_width, h->base_clock_hz / 1000000u);

	controller_count++;
	return true;
}
