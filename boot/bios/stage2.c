/* Stage 2 -- the part with room to work in.
 *
 * Reached from stage1.S, in 16-bit real mode, with the boot drive as its one
 * argument. Compiled with `clang -m16`, so this is ordinary C emitting
 * real-mode instructions: the BIOS is still reached through interrupts, and
 * every pointer is a 16-bit offset from a zero segment.
 *
 * What belongs here, in order:
 *
 *   - the memory map, from INT 15h AX=E820           [done]
 *   - a FAT32 reader, because the BIOS hands out sectors and the kernel is a
 *     file on the EFI partition                       [next]
 *   - the signature check, because **a BIOS path that skips it is the
 *     off-switch the UEFI path deliberately does not have**
 *   - A20, a GDT, page tables and the switch to long mode
 *   - the ReconBoot handoff, identical to the one the UEFI loader builds, so
 *     that the kernel cannot tell which loader started it
 *
 * The kernel lives on the EFI partition as a file, and is read from there
 * rather than from raw blocks the installer reserved. Raw blocks would remove
 * the FAT32 reader below and is the cheaper thing to build -- and it would put
 * **a second copy of the kernel on the disk**, which a system update would have
 * to keep in step with the first. A BIOS machine silently booting the kernel
 * from before the last update is exactly the class of divergence this project
 * keeps finding in itself, and the way not to have it is not to have the second
 * copy.
 */

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef unsigned long long u64;

/* --- talking to the machine ------------------------------------------------
 *
 * Every BIOS call clobbers whatever its specification does not promise to
 * preserve, so each one lists the registers it touches. INT 10h AH=0Eh promises
 * AX and nothing else, which cost an evening once (BG-137).
 */

static void putc_screen(char c)
{
	__asm__ __volatile__("int $0x10"
			     :
			     : "a"((u16)(0x0E00 | (u8)c)), "b"((u16)0x0007)
			     : "cc", "memory");
}

static u8 inb(u16 port)
{
	u8 v;
	__asm__ __volatile__("inb %1, %0" : "=a"(v) : "d"(port));
	return v;
}

static void outb(u16 port, u8 v)
{
	__asm__ __volatile__("outb %0, %1" : : "a"(v), "d"(port));
}

/* The screen and the serial port are two different readers: a person standing
 * in front of a machine that will not start is looking at one, and the test
 * harness and every headless machine are looking at the other. */
static void putc(char c)
{
	putc_screen(c);

	while (!(inb(0x3FD) & 0x20))
		;
	outb(0x3F8, (u8)c);
}

static void print(const char *s)
{
	while (*s) {
		if (*s == '\n')
			putc('\r');
		putc(*s++);
	}
}

static void print_dec(u32 v)
{
	char buf[11];
	int i = 0;

	if (!v) {
		putc('0');
		return;
	}
	while (v) {
		buf[i++] = (char)('0' + (v % 10));
		v /= 10;
	}
	while (i)
		putc(buf[--i]);
}

static void print_hex8(u8 v)
{
	const char *d = "0123456789abcdef";

	putc(d[v >> 4]);
	putc(d[v & 0x0F]);
}

/* --- the memory map --------------------------------------------------------
 *
 * UEFI hands this over as a finished table. Here it is a call made repeatedly,
 * each time passing back the continuation value the last one returned, until
 * the BIOS says there is no more.
 */

struct e820_entry {
	u64 base;
	u64 length;
	u32 type;
	u32 attributes;
};

#define E820_MAX 64

static struct e820_entry e820[E820_MAX];
static unsigned e820_count;

static unsigned e820_read(void)
{
	u32 next = 0;
	unsigned n = 0;

	do {
		/* Each register is one operand, read-write, initialised on the
		 * way in and read back on the way out. Naming a register in
		 * both the input and the output list instead -- "+a" for the
		 * returned signature and "a" for the 0xE820 going in -- puts
		 * two values in one register, and the loop then read the map
		 * out of a BIOS call it had never correctly made: nought
		 * regions, on every machine, in a routine whose assembly
		 * version had worked. */
		u32 eax = 0x0000E820;
		u32 ecx = sizeof(e820[0]);
		u32 edx = 0x534D4150;	/* "SMAP" */
		int failed;

		__asm__ __volatile__(
			"int $0x15"
			: "=@ccc"(failed), "+a"(eax), "+b"(next),
			  "+c"(ecx), "+d"(edx)
			: "D"(&e820[n])
			: "memory");

		/* A BIOS without this call can return with the carry flag
		 * clear, so the echoed signature is the check that matters. */
		if (failed || eax != 0x534D4150)
			break;

		n++;
	} while (next && n < E820_MAX);

	return n;
}

static u32 e820_usable_mb(void)
{
	u64 total = 0;
	unsigned i;

	for (i = 0; i < e820_count; i++)
		if (e820[i].type == 1)
			total += e820[i].length;

	return (u32)(total >> 20);
}

/* --- what there is to say so far ------------------------------------------ */

void stage2_main(u32 boot_drive)
{
	print("stage2 ok, drive 0x");
	print_hex8((u8)boot_drive);
	print("\n");

	e820_count = e820_read();

	print("e820: ");
	print_dec(e820_count);
	print(" regions, ");
	print_dec(e820_usable_mb());
	print(" MB usable\n");

	for (;;)
		__asm__ __volatile__("hlt");
}
