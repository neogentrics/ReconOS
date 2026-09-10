/* The 8042 keyboard controller: a chip from 1984, and still how a laptop's
 * built-in keyboard reaches the processor.
 *
 * There is no PS/2 socket on a modern machine and this is not legacy support:
 * the keyboard that is part of the case is wired to this controller on a very
 * large number of x86 machines, and firmware emulates it for USB keyboards
 * during boot. It is the cheapest way to get a keypress into this kernel, and it
 * is the one that works before there is a USB HID driver.
 *
 * --- Whether it is there at all ---
 *
 * The FADT has a bit for this -- IAPC_BOOT_ARCH bit 1, "there is an 8042" --
 * and `acpi.c` has been reading it since the FADT landed. **It cannot be
 * believed when clear, and that is measured rather than suspected.** QEMU's
 * i440fx machine has a working 8042 and reports 0x0000; its q35 machine reports
 * 0x0002 for the same hardware. A kernel that trusted the bit would have no
 * keyboard on one and a keyboard on the other, with nothing anywhere saying why.
 *
 * So presence is established positively, by asking the controller to run its own
 * self-test and answer 0x55. A port nothing drives reads back 0xFF, so "the
 * status register looks plausible" proves nothing -- but 0x55 is a specific
 * answer only an 8042 gives. Every wait here is bounded, so probing a machine
 * that genuinely has none costs microseconds and cannot hang.
 *
 * The bit still earns its place: it says whether finding nothing is worth
 * reporting as a fault or is simply the truth about this machine.
 *
 * --- Scancode sets, and the one that is verified rather than hoped for ---
 *
 * A keyboard sends *set 2*. The controller can translate to *set 1* on the way
 * past, and the translation bit in its configuration byte says whether it does.
 * Almost every machine boots with translation on, which is why almost every
 * small kernel assumes set 1 and almost every one of them is silently wrong on
 * the machine where it is off -- every key comes out as a different key, with
 * nothing anywhere reporting a fault.
 *
 * So this **writes the configuration byte and reads it back**. If translation
 * did not stick, the driver says so and refuses to deliver keys rather than
 * delivering wrong ones. A keyboard that types the wrong letters is worse than
 * one that does not type: the first looks like a broken program.
 *
 * --- What the interrupt handler does ---
 *
 * Takes one byte. That is the whole of it.
 *
 * The controller holds exactly one, and the next keypress overwrites it -- so
 * the byte has to leave the port now, at interrupt level, however busy the
 * machine is. Everything after that is a state machine over a byte stream and
 * belongs in a thread, so it goes to `work_schedule`. This is the first driver
 * in the kernel to use deferred work for what it was built for, and the first
 * to ask for an interrupt line at all.
 */
#include <recon/kernel/input.h>
#include <recon/kernel/irq.h>
#include <recon/kernel/work.h>
#include <recon/kernel/acpi.h>
#include <recon/kernel/console.h>
#include <recon/kernel/lock.h>
#include <recon/kernel/time.h>
#include "x86_64.h"

#define PS2_DATA        0x60
#define PS2_STATUS      0x64
#define PS2_COMMAND     0x64

#define STATUS_OUTPUT_FULL   (1u << 0)	/* there is a byte to read */
#define STATUS_INPUT_FULL    (1u << 1)	/* do not write yet */
#define STATUS_FROM_MOUSE    (1u << 5)	/* the second port, not the keyboard */

#define CMD_READ_CONFIG   0x20
#define CMD_WRITE_CONFIG  0x60
#define CMD_DISABLE_PORT2 0xA7
#define CMD_ENABLE_PORT2  0xA8
#define CMD_TEST_PORT2    0xA9
#define CMD_SELF_TEST     0xAA
#define CMD_TEST_PORT1    0xAB
#define CMD_DISABLE_PORT1 0xAD
#define CMD_ENABLE_PORT1  0xAE

#define CONFIG_PORT1_IRQ    (1u << 0)
#define CONFIG_PORT2_IRQ    (1u << 1)
#define CONFIG_PORT1_CLOCK  (1u << 4)	/* set means the clock is *disabled* */
#define CONFIG_TRANSLATE    (1u << 6)

#define KBD_SET_SCANCODE  0xF0
#define KBD_ENABLE_SCAN   0xF4
#define KBD_RESET         0xFF
#define KBD_ACK           0xFA
#define KBD_SELF_TEST_OK  0xAA

#define PS2_IRQ 1

/* Raw bytes taken by the handler, waiting for the worker.
 *
 * Small on purpose. It holds what can arrive between one interrupt and the
 * worker being scheduled, which is a handful of bytes even on a machine that is
 * struggling -- and a large one here would only hide the fact that the worker
 * is not running. */
#define RAW_BYTES 64

static u8 raw[RAW_BYTES];
static unsigned raw_head, raw_count;
static struct spinlock raw_lock = SPINLOCK_INIT("ps2");

static struct work decode_work;
static bool present, translating, refused;
static u64 bytes_taken, bytes_lost, keys_made;

/* --- talking to the controller -------------------------------------------
 *
 * Every wait is bounded. A controller that never clears its input flag is a
 * controller that is not there or is wedged, and spinning on it at boot is a
 * machine that stops with nothing on screen -- which is the failure this whole
 * project keeps arguing against.
 */
static bool wait_writable(void)
{
	unsigned tries;

	for (tries = 0; tries < 100000; tries++)
		if (!(inb(PS2_STATUS) & STATUS_INPUT_FULL))
			return true;

	return false;
}

static bool wait_readable(void)
{
	unsigned tries;

	for (tries = 0; tries < 100000; tries++)
		if (inb(PS2_STATUS) & STATUS_OUTPUT_FULL)
			return true;

	return false;
}

static bool command(u8 c)
{
	if (!wait_writable())
		return false;

	outb(PS2_COMMAND, c);
	return true;
}

static bool write_data(u8 c)
{
	if (!wait_writable())
		return false;

	outb(PS2_DATA, c);
	return true;
}

static bool read_data(u8 *out)
{
	if (!wait_readable())
		return false;

	*out = inb(PS2_DATA);
	return true;
}

static void flush(void)
{
	unsigned tries;

	for (tries = 0; tries < 32; tries++) {
		if (!(inb(PS2_STATUS) & STATUS_OUTPUT_FULL))
			return;

		(void)inb(PS2_DATA);
	}
}

/* --- set 1 to keycodes ----------------------------------------------------
 *
 * The table is the translation, and it is a table rather than arithmetic
 * because the layout of set 1 is a historical accident with no pattern in it.
 * Zero means "this kernel has no name for that key yet", which is delivered as
 * nothing rather than as key zero -- key zero is a real keycode meaning "no
 * event" in the HID tables, and posting it would be a keypress nobody made.
 */
static const u8 set1[128] = {
	[0x01] = KEY_ESCAPE,
	[0x02] = 30, [0x03] = 31, [0x04] = 32, [0x05] = 33, [0x06] = 34,
	[0x07] = 35, [0x08] = 36, [0x09] = 37, [0x0A] = 38, [0x0B] = KEY_0,
	[0x0C] = KEY_MINUS, [0x0D] = KEY_EQUAL,
	[0x0E] = KEY_BACKSPACE, [0x0F] = KEY_TAB,
	[0x10] = 20, [0x11] = 26, [0x12] = 8,  [0x13] = 21, [0x14] = 23,
	[0x15] = 28, [0x16] = 24, [0x17] = 12, [0x18] = 18, [0x19] = 19,
	[0x1A] = 47, [0x1B] = 48,
	[0x1C] = KEY_ENTER,
	[0x1D] = KEY_LEFTCTRL,
	[0x1E] = KEY_A, [0x1F] = 22, [0x20] = 7,  [0x21] = 9,  [0x22] = 10,
	[0x23] = 11, [0x24] = 13, [0x25] = 14, [0x26] = 15,
	[0x27] = 51, [0x28] = 52, [0x29] = 53,
	[0x2A] = KEY_LEFTSHIFT,
	[0x2B] = 49,
	[0x2C] = KEY_Z, [0x2D] = 27, [0x2E] = 6,  [0x2F] = 25, [0x30] = 5,
	[0x31] = 17, [0x32] = 16,
	[0x33] = 54, [0x34] = 55, [0x35] = 56,
	[0x36] = KEY_RIGHTSHIFT,
	[0x37] = 85,			/* keypad * */
	[0x38] = KEY_LEFTALT,
	[0x39] = KEY_SPACE,
	[0x3A] = KEY_CAPSLOCK,
	[0x3B] = KEY_F1, [0x3C] = 59, [0x3D] = 60, [0x3E] = 61, [0x3F] = 62,
	[0x40] = 63, [0x41] = 64, [0x42] = 65, [0x43] = 66, [0x44] = 67,
	[0x57] = 68, [0x58] = KEY_F12,
};

/* The ones behind an 0xE0 prefix. A separate table because they are a separate
 * namespace: 0x1D is left control on its own and right control after 0xE0, and
 * folding them into one array is how a kernel reports the wrong modifier. */
static u8 extended(u8 code)
{
	switch (code) {
	case 0x1D: return KEY_RIGHTCTRL;
	case 0x38: return KEY_RIGHTALT;
	case 0x48: return KEY_UP;
	case 0x4B: return KEY_LEFT;
	case 0x4D: return KEY_RIGHT;
	case 0x50: return KEY_DOWN;
	case 0x5B: return KEY_LEFTMETA;
	case 0x5C: return KEY_RIGHTMETA;
	case 0x1C: return 88;		/* keypad enter */
	default:   return 0;
	}
}

/* --- the worker ---------------------------------------------------------- */

static void decode(void *arg)
{
	/* The extended-prefix state, carried between calls because a prefix
	 * and its code can arrive in separate interrupts.
	 *
	 * A static is only safe here because there is exactly one worker -- see
	 * work.h, which chose that on purpose so work items cannot race each
	 * other. A pool would need this on the work item. */
	static bool saw_e0;

	(void)arg;

	for (;;) {
		u64 flags = spin_lock_irq(&raw_lock);
		u8 byte;

		if (!raw_count) {
			spin_unlock_irq(&raw_lock, flags);
			return;
		}

		byte = raw[(raw_head + RAW_BYTES - raw_count) % RAW_BYTES];
		raw_count--;
		spin_unlock_irq(&raw_lock, flags);

		if (byte == 0xE0) {
			saw_e0 = true;
			continue;
		}

		/* 0xE1 begins the pause key, which is six bytes and has no
		 * release at all. Not decoded: a key that cannot be released
		 * would be reported as held for ever, which is worse than not
		 * reporting it. */
		if (byte == 0xE1) {
			saw_e0 = false;
			continue;
		}

		{
			bool release = (byte & 0x80) != 0;
			u8 make = byte & 0x7F;
			u8 code = saw_e0 ? extended(make) : set1[make];

			saw_e0 = false;

			/* Anomaly: a scancode this table has no name for.
			 * Dropped rather than posted as zero -- zero is a real
			 * keycode meaning "no event", and posting it is a
			 * keypress nobody made. */
			if (!code)
				continue;

			input_post(code, release ? INPUT_RELEASE : INPUT_PRESS);
			keys_made++;
		}
	}
}

/* --- the interrupt -------------------------------------------------------- */

static void ps2_interrupt(void *arg)
{
	u8 status = inb(PS2_STATUS);

	(void)arg;

	if (!(status & STATUS_OUTPUT_FULL))
		return;

	/* The mouse shares this controller and this port. Its bytes are a
	 * different protocol entirely, and feeding them to the keyboard state
	 * machine produces keypresses out of mouse movement. Read and dropped,
	 * because leaving the byte in the buffer stops the keyboard too. */
	if (status & STATUS_FROM_MOUSE) {
		(void)inb(PS2_DATA);
		return;
	}

	{
		u8 byte = inb(PS2_DATA);
		u64 flags;

		/* Under the lock, like every other toucher of this ring.
		 *
		 * The first version wrote it bare, reasoning that an interrupt
		 * handler cannot be preempted. It cannot be preempted *on its own
		 * processor* -- and the worker draining this ring runs on whichever
		 * processor the scheduler put it on, while this line is delivered
		 * to one. Two processors, one ring, no lock. Correct on a machine
		 * with a single processor and corrupt on every other. */
		flags = spin_lock_irq(&raw_lock);

		bytes_taken++;

		if (raw_count == RAW_BYTES) {
			/* The worker is not keeping up, which on a keyboard
			 * means it is not running at all. Counted, and the
			 * newest byte dropped -- unlike the event queue above,
			 * because half a scancode sequence is worse than none:
			 * dropping the oldest here would leave an 0xE0 prefix
			 * attached to somebody else's key. */
			bytes_lost++;
			spin_unlock_irq(&raw_lock, flags);
			return;
		}

		raw[raw_head] = byte;
		raw_head = (raw_head + 1) % RAW_BYTES;
		raw_count++;

		spin_unlock_irq(&raw_lock, flags);
	}

	work_schedule(&decode_work);
}

/* --- bringing it up ------------------------------------------------------- */

static bool configure(void)
{
	u8 config, back, reply;

	/* Both ports off while the controller is being set up. A keypress
	 * arriving mid-sequence is a byte the configuration read mistakes for
	 * its own answer. */
	command(CMD_DISABLE_PORT1);
	command(CMD_DISABLE_PORT2);
	flush();

	/* The controller's own self-test, and the only positive proof that
	 * there is a controller. A port nothing drives reads back 0xFF, which
	 * is why "the status register looks plausible" proves nothing -- but
	 * 0x55 is a specific answer that only an 8042 gives.
	 *
	 * It runs before the configuration byte is written, because passing it
	 * resets the controller and would undo anything set first. */
	if (!command(CMD_SELF_TEST) || !read_data(&reply) ||
	    reply != 0x55) {
		return false;
	}

	if (!command(CMD_READ_CONFIG) || !read_data(&config)) {
		kputs("  ps2: the controller did not answer\n");
		return false;
	}

	/* Interrupts on for the keyboard, its clock enabled, translation on so
	 * that what arrives is set 1. */
	/* Translation on, and the keyboard's interrupt deliberately **off**.
	 *
	 * Everything below polls for its replies, and an interrupt handler would
	 * take those bytes out of the port first -- so every read here would time
	 * out on a controller that was answering perfectly. The interrupt is
	 * turned on afterwards, once there is something to receive it. */
	config &= (u8)~CONFIG_PORT1_IRQ;
	config |= CONFIG_TRANSLATE;
	config &= (u8)~CONFIG_PORT1_CLOCK;
	config &= (u8)~CONFIG_PORT2_IRQ;

	if (!command(CMD_WRITE_CONFIG) || !write_data(config))
		return false;

	/* Read back, because a bit that did not stick is the difference between
	 * the right keys and different keys. */
	if (!command(CMD_READ_CONFIG) || !read_data(&back)) {
		kputs("  ps2: the configuration could not be read back\n");
		return false;
	}

	translating = (back & CONFIG_TRANSLATE) != 0;

	if (!translating) {
		kputs("  ps2: this controller will not translate scancodes, "
		      "so every key would arrive as a different key -- no keys "
		      "are delivered rather than wrong ones\n");
		return false;
	}

	if (!command(CMD_ENABLE_PORT1))
		return false;

	/* Reset the keyboard and wait for it to say it passed. A device that
	 * does not answer here is one the firmware left in a state this driver
	 * does not know, and going on would be guessing. */
	if (!write_data(KBD_RESET) || !read_data(&reply) || reply != KBD_ACK) {
		kputs("  ps2: the keyboard did not acknowledge a reset\n");
		return false;
	}

	if (!read_data(&reply) || reply != KBD_SELF_TEST_OK) {
		kprintf("  ps2: the keyboard's self-test answered 0x%x\n",
			reply);
		return false;
	}

	if (!write_data(KBD_ENABLE_SCAN) || !read_data(&reply) ||
	    reply != KBD_ACK) {
		kputs("  ps2: the keyboard would not start scanning\n");
		return false;
	}

	flush();
	return true;
}

/* Turns the keyboard's interrupt on, once a handler exists to take it.
 *
 * Split from configure() because the order is the whole of it. The first
 * version armed the device before registering, and the keyboard's own reset
 * and acknowledgement bytes arrived at a line nobody had claimed. Nothing
 * read the data port, so the controller kept its output buffer full -- and an
 * 8042 with a byte nobody has taken **raises no further interrupts**. Four
 * bytes in and the keyboard was silent for the rest of the boot, with every
 * register correctly programmed. */
static bool arm(void)
{
	u8 config;

	/* Anything the setup left behind goes now. A stale byte would be
	 * delivered as the first keypress, and it is not one. */
	flush();

	if (!command(CMD_READ_CONFIG) || !read_data(&config))
		return false;

	config |= CONFIG_PORT1_IRQ;

	return command(CMD_WRITE_CONFIG) && write_data(config);
}

void arch_input_probe(void)
{
	struct acpi_fadt_facts fadt;

	/* What the firmware claims. Not what is acted on -- see below. */
	bool firmware_says_yes = !acpi_fadt(&fadt) || fadt.has_8042;

	work_init(&decode_work, decode, 0);

	/* **A clear bit is not proof of absence, and this was measured.**
	 *
	 * The specification says IAPC_BOOT_ARCH bit 1 means the machine has
	 * an 8042. It does not say firmware sets it when there is one --
	 * and QEMU's i440fx machine, which has a working 8042, reports
	 * 0x0000. Its q35 machine reports 0x0002 for the same hardware.
	 * A kernel that believed the bit has no keyboard on the first and
	 * one on the second, with nothing anywhere saying why.
	 *
	 * So the bit decides whether the probe is *expected* to find
	 * something, not whether to run it. What proves presence is the
	 * controller answering its own self-test with 0x55, which is a
	 * specific value a floating bus does not produce -- positive proof
	 * rather than an inference from an absent one. Every wait inside is
	 * bounded, so probing a machine that genuinely has none costs a few
	 * microseconds and never hangs. */
	if (!configure()) {
		if (firmware_says_yes)
			kputs("  ps2: the firmware says there is an 8042 and it "
			      "did not come up\n");

		refused = firmware_says_yes;
		return;
	}

	if (!firmware_says_yes)
		kputs("  ps2: the firmware said there was no 8042 and there is "
		      "one, so the bit was not believed\n");


	/* The handler first, then the line, then the device. Each step makes
	 * an interrupt possible and the one before it has to be ready. */
	if (!irq_register(PS2_IRQ, ps2_interrupt, 0, "ps2")) {
		refused = true;
		return;
	}

	if (!x86_irq_enable_line(PS2_IRQ)) {
		kputs("  ps2: the interrupt line could not be opened\n");
		irq_release(PS2_IRQ);
		refused = true;
		return;
	}

	if (!arm()) {
		kputs("  ps2: the controller would not raise interrupts\n");
		irq_release(PS2_IRQ);
		refused = true;
		return;
	}

	present = true;
}

void arch_input_print(void)
{
	if (!present) {
		kprintf("  keyboard     : %s\n",
			refused ? "an 8042 that would not come up"
				: "none found");
		return;
	}

	kprintf("  keyboard     : 8042, set 1 by translation, on line %u\n",
		(unsigned)PS2_IRQ);
	/* The counts, but only once there are any.
	 *
	 * This summary is printed during boot, seconds before anybody could
	 * have touched the machine, so "0 taken" is the honest number and
	 * reads as a broken keyboard to everybody who sees it. Saying what
	 * the zero means costs one line and stops the question. */
	if (!bytes_taken) {
		kputs("  scancodes    : none yet -- this is printed before "
		      "anybody could have typed\n");
		return;
	}

	kprintf("  scancodes    : %llu taken, %llu turned into keys\n",
		(unsigned long long)bytes_taken,
		(unsigned long long)keys_made);

	if (bytes_lost)
		kprintf("  lost         : %llu, because the decoder was not "
			"running\n", (unsigned long long)bytes_lost);
}
