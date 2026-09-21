/* Stage 2 -- the part with room to work in.
 *
 * Reached from stage1.S, in 16-bit real mode, with the boot drive as its one
 * argument. Compiled with `clang -m16`, so this is ordinary C emitting
 * real-mode instructions: the BIOS is still reached through interrupts, and
 * every pointer is a 16-bit offset from a zero segment.
 *
 * What belongs here, in order:
 *
 *   - the memory map, from INT 15h AX=E820                          [done]
 *   - a FAT32 reader, because the BIOS hands out sectors and the
 *     kernel is a file on the EFI partition                         [done]
 *   - moving it above the first megabyte, which real mode cannot
 *     address, through INT 15h AH=87h                               [done]
 *   - the signature check, because **a BIOS path that skips it is the
 *     off-switch the UEFI path deliberately does not have**         [done]
 *   - A20, a GDT, page tables and the switch to long mode           [done]
 *   - the ReconBoot handoff, identical to the one the UEFI loader
 *     builds, so that the kernel cannot tell which loader started
 *     it                                                            [done]
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

/* The same file the UEFI loader opens, by the same name.
 *
 * One kernel in one place, read by both loaders. The alternative -- a copy for
 * this path -- is a second thing a system update has to keep in step, and a
 * BIOS machine quietly booting the kernel from before the last update is
 * exactly the divergence this project keeps finding in itself. */
#define KERNEL_NAME "kernel-x86_64.elf"

/* Where the kernel goes is not a constant here, and the definition that used to
 * say 0x100000 has been removed rather than left to be believed. **The ELF
 * says** where each of its segments belongs, and place_kernel() puts them
 * there; a loader that writes them to an address of its own choosing works
 * exactly until the linker script changes.
 *
 * The file is read to KERNEL_STAGE first because it has to be hashed before
 * anything acts on it, and what the CPU runs is not the file. */

/* The UEFI loader's SHA-256 and RSA, compiled for real mode and used unchanged.
 *
 * Not a copy: the same two source files. Two implementations of a signature
 * check is two chances to be wrong and one set of tests, and the untested one
 * is the one that will be wrong. If the padding check in rsa.c is ever
 * corrected, both loaders are corrected.
 *
 * efi.h is included for its integer types only. Nothing in it that touches
 * firmware is reachable from here, which is why the ms_abi calling convention
 * on its protocol declarations is warned about and ignored in the makefile. */
#include "efi.h"
#include "crypto.h"
#include "reconboot.h"

#if __has_include("signing_key.h")
#include "signing_key.h"
#endif

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef unsigned long long u64;

/* --- talking to the machine ------------------------------------------------
 *
 * Every BIOS call clobbers whatever its specification does not promise to
 * preserve, so each one lists the registers it touches. INT 10h AH=0Eh promises
 * AX and nothing else, which cost an evening once (KF-137).
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

/* --- reading the disk ------------------------------------------------------
 *
 * INT 13h AH=42h, the extended read, which takes a block number rather than a
 * cylinder, head and sector. Stage 1 has already established that this BIOS
 * has it -- a machine without extensions cannot address a GPT disk at all, and
 * says so there rather than failing obscurely here.
 *
 * The packet is a static rather than a local because the BIOS is handed its
 * address, and an address on a stack that C is free to reuse is the sort of
 * thing that works until the compiler inlines something.
 */

struct dap {
	u8  size;
	u8  zero;
	u16 count;
	u16 offset;
	u16 segment;
	u64 lba;
} __attribute__((packed));

static struct dap dap;
static u8 sector[512];

/* --- what size are this drive's sectors ------------------------------------
 *
 * 512 on every hard disk and 2048 on every CD. Everything above `disk_read`
 * counts in 512-byte blocks -- the GPT is at block 1, the partition entry names
 * the ESP's first block, FAT32 counts clusters in them -- and none of it should
 * have to know what it is running on. So the difference is absorbed here.
 *
 * Asked once. AH=48h is a real interrupt and `read_file` calls `disk_read`
 * thousands of times; the answer cannot change while the machine runs.
 */
static u16 sector_bytes;	/* 0 until asked */

/* One optical sector, and it lives at a fixed low address rather than in BSS.
 *
 * Everything in stage 2 must fit below 0x10000 -- real mode with a zero
 * segment addresses 64KB and every pointer here is an offset into it -- and the
 * linker enforces that. Two more kilobytes of BSS pushed it over, which it said
 * plainly rather than producing a loader that wrapped.
 *
 * 0x1000 is clear of the interrupt table and the BIOS data area below it, and
 * clear of the stack, which starts at 0x7000 and grows down with twenty
 * kilobytes to spare. It is inside the range the handoff already hands the
 * kernel as the bootloader's own, so nothing has to be told about it. */
static u8 *const big = (u8 *)0x1000;
#define BIG_MAX 2048u

struct drive_params {
	u16 size;
	u16 flags;
	u32 cylinders, heads, sectors_per_track;
	u64 total_sectors;
	u16 bytes_per_sector;
} __attribute__((packed));

static struct drive_params params;

static int raw_read(u8 drive, u64 lba, u16 count, void *buf)
{
	u16 ax = 0x4200;
	int failed;

	dap.size    = sizeof(dap);
	dap.zero    = 0;
	dap.count   = count;
	dap.offset  = (u16)(u32)buf;
	dap.segment = 0;
	dap.lba     = lba;

	__asm__ __volatile__("int $0x13"
			     : "=@ccc"(failed), "+a"(ax)
			     : "d"((u16)drive), "S"(&dap)
			     : "memory");

	return !failed;
}

static void learn_sector_size(u8 drive)
{
	u16 ax = 0x4800;
	int failed;

	params.size = sizeof(params);

	__asm__ __volatile__("int $0x13"
			     : "=@ccc"(failed), "+a"(ax)
			     : "d"((u16)drive), "S"(&params)
			     : "memory");

	/* A drive that will not say is taken as 512, which is what this loader
	 * assumed unconditionally before and is right for every disk. An answer
	 * that is not a sane power of two is treated the same way: a wrong
	 * number here turns every later read into a read of somewhere else. */
	if (failed || params.bytes_per_sector < 512 ||
	    params.bytes_per_sector > BIG_MAX)
		sector_bytes = 512;
	else
		sector_bytes = params.bytes_per_sector;
}

static int disk_read(u8 drive, u64 lba, u16 count, void *buf)
{
	u16 per, shift;
	u8 *out = buf;

	if (!sector_bytes)
		learn_sector_size(drive);

	/* The ordinary case, and the one every hard disk takes: hand the
	 * request straight to the firmware with nothing in between. */
	if (sector_bytes == 512)
		return raw_read(drive, lba, count, buf);

	/* A shift, not a division.
	 *
	 * `lba` is 64 bits and this is a freestanding 16-bit build with no
	 * libgcc: `lba / per` asks the linker for __udivdi3, which does not
	 * exist here. Every sector size is a power of two, so the shift is
	 * exact rather than an approximation -- and the linker refusing was a
	 * better way to find that out than a slow divide would have been.
	 *
	 * 512 -> 0, 2048 -> 2, 4096 -> 3. */
	shift = 0;
	for (per = (u16)(sector_bytes / 512); per > 1; per >>= 1)
		shift++;

	per = (u16)(1u << shift);

	while (count) {
		u64 which = lba >> shift;	/* the real sector holding it */
		u16 off   = (u16)(lba & (per - 1));
		u16 take  = (u16)(per - off);	/* how much of it we want */
		u16 i;

		if (take > count)
			take = count;

		if (!raw_read(drive, which, 1, big))
			return 0;

		for (i = 0; i < (u16)(take * 512); i++)
			out[i] = big[off * 512 + i];

		out   += take * 512;
		lba   += take;
		count -= take;
	}

	return 1;
}

/* --- finding the EFI partition ---------------------------------------------
 *
 * The kernel is a file on the EFI System Partition, so the first question is
 * where that partition starts. It is read out of the GPT here rather than
 * assumed, because the installer places partitions according to what was
 * already on the disk and there is no fixed answer.
 */

/* C12A7328-F81F-11D2-BA4B-00A0C93EC93B, as the sixteen bytes appear on disk:
 * the first three fields are little-endian and the last two are not. Written
 * out byte by byte rather than assembled from a string, because getting that
 * mixed endianness wrong produces a GUID that never matches anything and looks
 * entirely plausible in a hex dump. */
static const u8 esp_type[16] = {
	0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
	0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B
};

static int same16(const u8 *a, const u8 *b)
{
	int i;

	for (i = 0; i < 16; i++)
		if (a[i] != b[i])
			return 0;
	return 1;
}

/* Returns the first block of the EFI System Partition, or zero if there is
 * none. Zero is safe as "none": block zero is the protective MBR and can never
 * be the start of a partition. */
static u64 find_esp(u8 drive)
{
	u32 entry_lba, entry_count, entry_size, i;

	if (!disk_read(drive, 1, 1, sector))
		return 0;

	/* "EFI PART" */
	if (sector[0] != 'E' || sector[1] != 'F' || sector[2] != 'I' ||
	    sector[3] != ' ' || sector[4] != 'P' || sector[5] != 'A' ||
	    sector[6] != 'R' || sector[7] != 'T')
		return 0;

	entry_lba   = *(u32 *)&sector[72];
	entry_count = *(u32 *)&sector[80];
	entry_size  = *(u32 *)&sector[84];

	/* A header whose numbers are impossible is a header not to act on. The
	 * kernel's own GPT reader refuses the same way and for the same reason:
	 * a disk whose layout cannot be read is the one disk never to touch. */
	if (!entry_size || entry_size > 512 || entry_count > 512)
		return 0;

	for (i = 0; i < entry_count; i++) {
		u32 per_sector = 512 / entry_size;
		u32 off = (i % per_sector) * entry_size;

		if (off == 0 && !disk_read(drive, entry_lba + i / per_sector,
					   1, sector))
			return 0;

		if (same16(&sector[off], esp_type))
			return *(u64 *)&sector[off + 32];
	}

	return 0;
}

/* --- finding the boot volume on a disc --------------------------------------
 *
 * A disc has no partition table. xorriso decides where things land and there
 * is no GPT to walk -- but a bootable disc has to tell the *firmware* where its
 * boot images are, and that table is readable by anyone.
 *
 * So on optical media the EFI System Partition is a file in the ISO, and its
 * first block is read out of the El Torito boot catalogue: the same entry the
 * UEFI firmware follows to find BOOTX64.EFI. **One volume, one copy, both
 * firmwares reading the same bytes** -- which is what an appended GPT partition
 * was supposed to achieve and did not, because the firmware here would not
 * follow a boot entry that pointed into one. (scripts/make-disc.sh records the
 * measurement: three discs differing in one thing each, and only the one whose
 * EFI image was a file in the ISO tree booted.)
 *
 * **Which medium this is, is asked of the medium, not guessed from the sector
 * size.** An ISO 9660 volume says so: "CD001" at block 16, where the standard
 * puts the primary volume descriptor and nothing else ever does. A disk is
 * silent there and goes down the GPT path. Neither is a fallback from the
 * other failing.
 */

/* Volume descriptors are 2048 bytes each and start at block 16. In this
 * loader's units -- disk_read speaks 512-byte blocks whatever the medium's
 * real sectors are -- that is block 64, four apiece. */
#define ISO_VD_FIRST	64u
#define ISO_VD_STRIDE	4u
#define ISO_VD_LIMIT	16u	/* descriptors to look at before giving up */

static int iso_id(const u8 *b)
{
	return b[1] == 'C' && b[2] == 'D' && b[3] == '0' &&
	       b[4] == '0' && b[5] == '1';
}

/* Does this medium say it is an ISO 9660 volume? */
static int is_iso9660(u8 drive)
{
	if (!disk_read(drive, ISO_VD_FIRST, 1, sector))
		return 0;

	return sector[0] == 0x01 && iso_id(sector);	/* primary descriptor */
}

/* Returns the first block of the EFI boot image, in this loader's 512-byte
 * units, or zero if this disc carries no UEFI El Torito entry.
 *
 * Zero is safe as "none" here for the same reason it is in find_esp: block
 * zero cannot be the start of anything this loader wants. */
static u64 find_eltorito(u8 drive)
{
	u32 catalogue = 0;
	u32 i;
	u8 platform;

	/* The boot record descriptor: type 0, and its system identifier says
	 * which specification. Walked rather than assumed to be block 17,
	 * because the order of the descriptors is the image builder's choice
	 * and a terminator (type 255) can arrive first. */
	for (i = 1; i < ISO_VD_LIMIT; i++) {
		if (!disk_read(drive, ISO_VD_FIRST + i * ISO_VD_STRIDE, 1,
			       sector))
			return 0;

		if (!iso_id(sector))
			return 0;
		if (sector[0] == 0xFF)			/* terminator */
			return 0;
		if (sector[0] != 0x00)
			continue;

		if (sector[7] == 'E' && sector[8] == 'L' && sector[9] == ' ' &&
		    sector[10] == 'T' && sector[11] == 'O') {
			catalogue = *(u32 *)&sector[0x47];
			break;
		}
	}

	if (!catalogue)
		return 0;

	/* The catalogue is one 2048-byte block of 32-byte entries.
	 *
	 * The first is a validation entry, and **its platform byte belongs to
	 * the default entry that follows it** -- so it is read here rather than
	 * skipped. Without it the default entry, which is the BIOS image and
	 * also starts with 0x88, would be indistinguishable from the UEFI one
	 * and this would hand back stage 2's own boot image. */
	platform = 0xFF;

	for (i = 0; i < 4; i++) {
		u32 e;

		if (!disk_read(drive, (u64)catalogue * 4 + i, 1, sector))
			return 0;

		for (e = 0; e < 512; e += 32) {
			u8 *ent = &sector[e];

			if (ent[0] == 0x01) {		/* validation */
				platform = ent[1];
				continue;
			}

			if (ent[0] == 0x90 || ent[0] == 0x91) {	/* section */
				platform = ent[1];
				continue;
			}

			if (ent[0] != 0x88)		/* not bootable */
				continue;

			if (platform == 0xEF)		/* UEFI */
				return (u64)(*(u32 *)&ent[8]) * 4;
		}
	}

	return 0;
}

/* --- FAT32, read-only ------------------------------------------------------
 *
 * The EFI System Partition is FAT32 by specification, and the kernel is a file
 * on it. Under UEFI the firmware reads that file for us; here there is nothing
 * below us but a call that fetches a block, so the format has to be understood.
 *
 * Read-only, and only enough to open one path. The kernel's own FAT32 code in
 * kernel/core/ writes as well as reads, handles both allocation tables, and
 * derives the type from the cluster count -- none of which is needed to find
 * one file, and all of which would be code running before anything has checked
 * a signature.
 *
 * Directories are walked a **sector** at a time rather than a cluster at a
 * time, so no buffer has to be as large as a cluster. Long-name fragments can
 * straddle a sector boundary, so the name being assembled lives outside the
 * loop that reads sectors.
 */

static u32 fat_begin;		/* first block of the first allocation table */
static u32 data_begin;		/* first block of cluster 2 */
static u8  sectors_per_cluster;
static u32 root_cluster;

static u16 rd16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static u32 rd32(const u8 *p)
{
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) |
	       ((u32)p[3] << 24);
}

static int fat_mount(u8 drive, u32 part_lba)
{
	u16 bytes_per_sector, reserved;
	u8 fats;
	u32 fat_size;

	if (!disk_read(drive, part_lba, 1, sector))
		return 0;

	bytes_per_sector    = rd16(&sector[11]);
	sectors_per_cluster = sector[13];
	reserved            = rd16(&sector[14]);
	fats                = sector[16];
	fat_size            = rd32(&sector[36]);
	root_cluster        = rd32(&sector[44]);

	/* 512 only, said rather than assumed. A volume with 4096-byte sectors
	 * is legal and this reader would produce confident nonsense on one;
	 * refusing is the difference between "not supported" and "started the
	 * wrong kernel". */
	if (bytes_per_sector != 512 || !sectors_per_cluster || !fats ||
	    !fat_size || root_cluster < 2)
		return 0;

	fat_begin  = part_lba + reserved;
	data_begin = fat_begin + (u32)fats * fat_size;
	return 1;
}

static u32 cluster_lba(u32 cluster)
{
	return data_begin + (cluster - 2) * sectors_per_cluster;
}

/* The next cluster in a chain, or 0 at the end.
 *
 * Only the first allocation table is consulted. The kernel compares both,
 * because it is deciding whether a volume is sound; this is deciding which
 * block to read next, and a disagreement between the tables would be a reason
 * to stop rather than something this can repair. */
static u32 fat_next(u8 drive, u32 cluster)
{
	u32 entry_sector = fat_begin + (cluster / 128);
	u32 next;

	if (!disk_read(drive, entry_sector, 1, sector))
		return 0;

	next = rd32(&sector[(cluster % 128) * 4]) & 0x0FFFFFFF;

	/* Anything past the end-of-chain mark, and anything that would not move
	 * forward, ends the walk. A chain that loops is not a theoretical disk;
	 * it is what a half-overwritten one looks like. */
	if (next < 2 || next >= 0x0FFFFFF8)
		return 0;

	return next;
}

/* --- names -----------------------------------------------------------------
 *
 * Matched on the long name where there is one, and on the 8.3 name otherwise.
 *
 * Not on the 8.3 alias alone: an alias is generated by whatever wrote the file
 * and is allowed to change when the directory is rewritten, which is KF-130 --
 * every rewrite renamed the file, and the listing still looked right because
 * the long name had not moved. A loader that finds its kernel by alias finds it
 * until the day something rewrites the directory.
 */

#define NAME_MAX 64

static char found_name[NAME_MAX];

static char lower(char c)
{
	return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static void name_copy(char *dst, const char *src)
{
	int i = 0;

	while (src[i] && i < NAME_MAX - 1) {
		dst[i] = src[i];
		i++;
	}
	dst[i] = 0;
}

static int name_eq(const char *a, const char *b)
{
	while (*a && *b) {
		if (lower(*a) != lower(*b))
			return 0;
		a++;
		b++;
	}
	return !*a && !*b;
}

/* The thirteen UTF-16 units a long-name entry carries, at three disjoint
 * offsets the specification chose to fit around the 8.3 fields it overlays.
 * Only the low byte of each is taken: every name this loader looks for is
 * ASCII, and a kernel filename that is not is a problem to have deliberately
 * rather than to half-support. */
static void lfn_chars(const u8 *e, char *out)
{
	static const u8 off[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24,
				    28, 30 };
	int i;

	for (i = 0; i < 13; i++)
		out[i] = (char)e[off[i]];
}

/* Renders SHORTNM.EXT from the padded 8.3 field. */
static void short_name(const u8 *e, char *out)
{
	int i, n = 0;

	for (i = 0; i < 8 && e[i] != ' '; i++)
		out[n++] = lower((char)e[i]);
	if (e[8] != ' ') {
		out[n++] = '.';
		for (i = 8; i < 11 && e[i] != ' '; i++)
			out[n++] = lower((char)e[i]);
	}
	out[n] = 0;
}

/* Finds one component in one directory. Returns its first cluster, with the
 * size written through `size` when it is a file, or 0 when there is no such
 * entry. */
static u32 dir_find(u8 drive, u32 dir_cluster, const char *want, u32 *size)
{
	char lfn[NAME_MAX];
	int lfn_len = 0;
	u32 cluster = dir_cluster;

	lfn[0] = 0;

	while (cluster) {
		u8 s;

		for (s = 0; s < sectors_per_cluster; s++) {
			int i;

			if (!disk_read(drive, cluster_lba(cluster) + s, 1,
				       sector))
				return 0;

			for (i = 0; i < 512; i += 32) {
				u8 *e = &sector[i];
				u8 attr = e[11];
				char part[13];
				int seq, k;

				if (e[0] == 0x00)
					return 0;	/* end of directory */
				if (e[0] == 0xE5) {
					lfn_len = 0;
					lfn[0] = 0;
					continue;
				}

				if ((attr & 0x0F) == 0x0F) {
					/* Fragments arrive last-first, so each
					 * one is prepended rather than
					 * appended. */
					seq = (e[0] & 0x3F);
					if (seq < 1 || seq > 4)
						continue;
					lfn_chars(e, part);
					for (k = 0; k < 13; k++) {
						int at = (seq - 1) * 13 + k;

						if (at >= NAME_MAX - 1)
							break;
						lfn[at] = part[k] == (char)0xFF
							  ? 0 : part[k];
						if (at + 1 > lfn_len)
							lfn_len = at + 1;
					}
					lfn[lfn_len < NAME_MAX ? lfn_len
							       : NAME_MAX - 1] = 0;
					continue;
				}

				if (attr & 0x08) {	/* volume label */
					lfn_len = 0;
					lfn[0] = 0;
					continue;
				}

				if (lfn_len)
					name_copy(found_name, lfn);
				else
					short_name(e, found_name);

				lfn_len = 0;
				lfn[0] = 0;

				if (name_eq(found_name, want)) {
					if (size)
						*size = rd32(&e[28]);
					return ((u32)rd16(&e[20]) << 16) |
					       rd16(&e[26]);
				}
			}
		}

		cluster = fat_next(drive, cluster);
	}

	return 0;
}

/* --- getting bytes above the first megabyte --------------------------------
 *
 * Real mode addresses one megabyte, and every byte this loader can reach lives
 * below 0x10000 -- the segment registers are zero, so a C pointer here is a
 * 16-bit offset. The kernel is seven hundred kilobytes and belongs at 0x100000.
 *
 * INT 15h AH=87h is the way across: the BIOS enters protected mode, copies from
 * one descriptor to another, and comes back. Up to 64KB a call, and the count
 * is in **words**, which is the sort of detail that produces a transfer of
 * exactly half the intended length and an image that looks almost right.
 *
 * The alternative is unreal mode -- enter protected mode, load a data segment
 * with a four-gigabyte limit, drop back to real mode and keep the limit. It is
 * faster and it is what most bootloaders do. It also means every pointer in
 * this file stops meaning what C thinks it means, and the compiler is emitting
 * 16-bit addressing on the assumption that it does. That trade is not worth
 * making for a copy that happens once at boot.
 */

struct desc {
	u16 limit;
	u16 base_lo;
	u8  base_mid;
	u8  access;
	u8  attr;
	u8  base_hi;
} __attribute__((packed));

/* The six descriptors AH=87h expects, in the order it expects them: null, the
 * table itself, source, destination, BIOS code segment, BIOS stack. Only the
 * middle two are ours to fill; the BIOS writes the ones it needs. */
static struct desc gdt87[6];

static void set_desc(struct desc *d, u32 base, u16 bytes)
{
	d->limit    = bytes - 1;
	d->base_lo  = (u16)base;
	d->base_mid = (u8)(base >> 16);
	d->access   = 0x93;		/* present, data, read-write */
	d->attr     = 0;
	d->base_hi  = (u8)(base >> 24);
}

/* Between any two physical addresses, in either direction. Both the load and
 * the read-back that checks it go through here, so there is one place where the
 * word count and the descriptor layout can be wrong. */
static int copy_phys(u32 dest, u32 src, u16 bytes)
{
	u16 ax = 0x8700;
	u16 cx;
	int failed, i;

	/* An odd byte count would lose its last byte to a word count that
	 * rounds down, so it is rounded up: the extra byte is inside the
	 * destination the caller has already reserved. */
	if (bytes & 1)
		bytes++;

	for (i = 0; i < (int)sizeof(gdt87); i++)
		((u8 *)gdt87)[i] = 0;

	set_desc(&gdt87[2], src, bytes);
	set_desc(&gdt87[3], dest, bytes);

	cx = bytes / 2;			/* words, not bytes */

	__asm__ __volatile__("int $0x15"
			     : "=@ccc"(failed), "+a"(ax)
			     : "c"(cx), "S"(gdt87)
			     : "memory");

	/* AH is the status: zero is success. Checked as well as the carry flag,
	 * because a BIOS reporting failure only in AH and not in the flag is
	 * within the letter of the specification. */
	return !failed && (ax >> 8) == 0;
}

static int copy_high(u32 dest, const void *src, u16 bytes)
{
	return copy_phys(dest, (u32)src, bytes);
}

/* --- reading the kernel ---------------------------------------------------- */

/* Eight sectors at a time, whatever the cluster size is.
 *
 * This was one whole cluster, in sixteen kilobytes of a sixty-four kilobyte
 * loader, and that was a convenience rather than a requirement: the file was
 * already read a sector at a time into it, and holding the whole cluster only
 * saved repeating the hash and the block move. The directory walker a few
 * hundred lines up states the principle -- *a sector at a time, so no buffer
 * has to be as large as a cluster* -- and this did not follow it.
 *
 * Four kilobytes is the floor rather than a guess: the page-table builder below
 * assembles a 4KB table in this same buffer, and a second one of its own would
 * put stage 2 back against the limit.
 *
 * **A ceiling was deleted here, not raised.** The old buffer refused any volume
 * whose clusters were bigger than it -- an EFI partition above about eight
 * gigabytes, by name. A cluster no longer has to fit in anything. */
#define CHUNK_MAX  4096
#define CHUNK_SECS (CHUNK_MAX / 512)
static u8 chunk_buf[CHUNK_MAX];

/* Hashed on the way past, not afterwards.
 *
 * The only moment each byte is reachable is while its cluster sits in the low
 * buffer: once it is above the first megabyte, real mode cannot read it back
 * without another BIOS call per chunk. So the digest is accumulated here, and
 * `ctx` is optional so that this function also serves reads that are not the
 * kernel.
 */
static u32 read_file(u8 drive, u32 cluster, u32 size, u32 dest,
		     struct sha256_ctx *ctx)
{
	u32 done = 0;

	while (cluster && done < size) {
		u32 base = cluster_lba(cluster);
		u8 s = 0;

		/* Through the cluster in chunks, so a volume with 32KB clusters
		 * reads the same way as one with 4KB and neither needs a buffer
		 * its own size. */
		while (s < sectors_per_cluster && done < size) {
			u8 take = (u8)(sectors_per_cluster - s);
			u32 want;
			u8 i;

			if (take > CHUNK_SECS)
				take = CHUNK_SECS;

			for (i = 0; i < take; i++)
				if (!disk_read(drive, base + s + i, 1,
					       &chunk_buf[(u32)i * 512]))
					return done;

			want = size - done;
			if (want > (u32)take * 512)
				want = (u32)take * 512;

			if (ctx)
				sha256_update(ctx, chunk_buf, want);

			if (!copy_high(dest + done, chunk_buf, (u16)want))
				return done;

			done += want;
			s = (u8)(s + take);
		}

		cluster = fat_next(drive, cluster);
	}

	return done;
}

/* A small file, into a buffer down here, rather than pushed above the megabyte
 * with the kernel.
 *
 * **Built unconditionally since 20 September**, and it used to be inside
 * `#ifdef RECONOS_KEY_PRESENT` because the signature was its only caller. The
 * command line is the second, and a keyless build needs it -- a keyless build
 * is what every machine in the verification rig boots.
 *
 * **It refuses rather than truncates**: `size > max` returns 0 and reads
 * nothing. That is the property the command line needs and it was already
 * here. */
static u32 read_small(u8 drive, u32 cluster, u32 size, u8 *out, u32 max)
{
	u32 done = 0;

	if (size > max)
		return 0;

	while (cluster && done < size) {
		u32 base = cluster_lba(cluster);
		u8 s = 0;

		while (s < sectors_per_cluster && done < size) {
			u8 take = (u8)(sectors_per_cluster - s);
			u32 want, i;
			u8 j;

			if (take > CHUNK_SECS)
				take = CHUNK_SECS;

			for (j = 0; j < take; j++)
				if (!disk_read(drive, base + s + j, 1,
					       &chunk_buf[(u32)j * 512]))
					return done;

			want = size - done;
			if (want > (u32)take * 512)
				want = (u32)take * 512;

			for (i = 0; i < want; i++)
				out[done + i] = chunk_buf[i];

			done += want;
			s = (u8)(s + take);
		}

		cluster = fat_next(drive, cluster);
	}

	return done;
}

#ifdef RECONOS_KEY_PRESENT

/* Built only when there is a key to check against: the buffer below exists to
 * serve the verifier, and a keyless build has no verifier. */
static u8 sig_buf[256];

#endif /* RECONOS_KEY_PRESENT */

/* --- the kernel command line ---------------------------------------------
 *
 * **This loader used to write an empty string into the handoff and never read
 * `\reconos\cmdline` at all.** (NW-013)
 *
 * That is not a missing nicety. `logport`, `verbose`, `noinit`, `recovery`,
 * `poweroff` and `restart` are all switches the kernel reads off this line, so
 * on a machine that boots through this loader **none of them could be asked
 * for**, however well everything under them worked. The one that stings is
 * `noinit`: `xhci.c` prints every port's `PORTSC` as the controller comes up,
 * so the diagnostic for a USB fault was unavailable on a machine with a USB
 * fault -- and the one that prompted this is `logport`, because a legacy-BIOS
 * server with no screen anybody stands in front of is exactly the machine whose
 * boot report needs to come back over the wire.
 *
 * **Found by the network session with a control**, which is what makes it a
 * result rather than a suspicion: one medium, one cmdline file, read back off
 * the image before booting, then booted twice. BIOS carried no command line;
 * UEFI carried `logport verbose` and the port listened. Without the UEFI half,
 * "no log port on BIOS" is equally well explained by a file written to the
 * wrong path.
 *
 * **Silent when nothing is wrong, loud when the file exists and did not
 * entirely take effect.** The same four states the UEFI loader now handles, and
 * deliberately the same, because two loaders that disagree about one medium
 * send the next person to whichever one is quiet:
 *
 *   absent               silent -- and normal. `make-medium.sh` omits the file
 *                        on purpose, so an `install-onto=` cannot ride in.
 *   read whole, applied  silent here. The kernel prints `command line : ...`
 *                        itself, and that is the better report: it says what
 *                        the kernel received rather than what this believed it
 *                        sent.
 *   too long             loud, and **nothing is passed.** Half of
 *                        `recovery logport` is a different instruction from
 *                        either word.
 *   short read           loud, and nothing is passed.
 */
static char cmdline_buf[128];

static void read_cmdline(u8 drive, u32 dir)
{
	u32 file, size = 0, got, i;

	cmdline_buf[0] = '\0';

	file = dir_find(drive, dir, "cmdline", &size);
	if (!file)
		return;			/* absent, which is normal */

	if (!size)
		return;			/* empty file, empty command line */

	/* The terminator needs a byte, so a file of exactly the buffer's size
	 * does not fit either. Checked against the directory entry's size
	 * rather than against what was read: a short read is the thing being
	 * guarded against, so it cannot also be the evidence. */
	if (size >= sizeof(cmdline_buf)) {
		print("cmdline: too long for this loader, none of it used\n");
		return;
	}

	got = read_small(drive, file, size, (u8 *)cmdline_buf,
			 sizeof(cmdline_buf) - 1);
	if (got != size) {
		cmdline_buf[0] = '\0';
		print("cmdline: would not read whole, none of it used\n");
		return;
	}

	cmdline_buf[size] = '\0';

	/* One line, and no line ending. A file edited on Windows ends CR LF,
	 * and a command line with a carriage return in it matches nothing --
	 * which presents as an option being ignored for no visible reason. The
	 * UEFI loader has stripped these since it was written; this is the
	 * same three lines, on purpose, rather than a second opinion. */
	for (i = 0; i < size; i++)
		if (cmdline_buf[i] == '\r' || cmdline_buf[i] == '\n') {
			cmdline_buf[i] = '\0';
			break;
		}
}

/* Reads back what was written, which is the only way to know it arrived.
 *
 * A copy that silently did nothing leaves whatever was at the destination
 * before -- and on a machine that has just been powered on, that is zeroes,
 * which is exactly what a failed read looks like from here. */
static int check_landed(u32 dest)
{
	if (!copy_phys((u32)sector, dest, 512))
		return 0;

	return sector[0] == 0x7F && sector[1] == 'E' && sector[2] == 'L' &&
	       sector[3] == 'F';
}

/* --- the signature ---------------------------------------------------------
 *
 * **A BIOS path that does not check the kernel's signature is the off-switch
 * the UEFI path deliberately does not have.** An attacker who can write to the
 * disk would not need to defeat the check in reconboot; they would only need to
 * make the machine take this path instead. So every claim in docs/INTEGRITY.md
 * is a claim about both loaders or about neither.
 *
 * The wording of the refusals is deliberately the same as the UEFI loader's. A
 * person who has met one of these messages once should not have to learn it
 * again because the firmware underneath was different.
 */

#define SIGNATURE_NAME KERNEL_NAME ".sig"

static struct sha256_ctx hash;

static int verify(struct sha256_ctx *ctx, u32 dir, u8 drive)
{
#ifndef RECONOS_KEY_PRESENT
	(void)ctx; (void)dir; (void)drive;

	/* Built without a key. Announced on every boot rather than silent,
	 * because a build that cannot check is a fact somebody should be told
	 * rather than a default they discover later. */
	print("signature: not checked (this loader was built without a key)\n");
	return 1;
#else
	UINT8 digest[32];
	u32 sig_cluster, sig_size = 0;

	sig_cluster = dir_find(drive, dir, SIGNATURE_NAME, &sig_size);
	if (!sig_cluster) {
		print("\nreconboot: this kernel is not signed, and this loader "
		      "only runs signed kernels.\n");
		return 0;
	}

	if (sig_size != sizeof(sig_buf) ||
	    read_small(drive, sig_cluster, sig_size, sig_buf,
		       sizeof(sig_buf)) != sig_size) {
		print("\nreconboot: the signature file is not 256 bytes.\n");
		return 0;
	}

	sha256_final(ctx, digest);

	if (!rsa2048_verify(reconos_signing_modulus, sig_buf, digest)) {
		print("\nreconboot: this kernel's signature does not match. It "
		      "has been changed since it was built, or it is not "
		      "ours.\n");
		return 0;
	}

	print("signature: good\n");
	return 1;
#endif
}

/* --- page tables -----------------------------------------------------------
 *
 * Four gigabytes, identity mapped with 2MB pages: one PML4, one PDPT, and four
 * page directories. Enough to reach the kernel, its handoff, and any
 * memory-mapped device below 4GB, and the kernel replaces all of it with its
 * own map at checkpoint 5 the moment it is running.
 *
 * They are built at 0x70000 -- above stage 2 and its data, below the 640KB
 * line, and in memory the handoff marks as the loader's so the kernel does not
 * hand it out before it has finished with it.
 */

/* Eight megabytes: above the kernel's image, and nowhere near anything else
 * this loader touches. **Not below the first megabyte**, which was the first
 * attempt and does not work: a pointer in stage 2 is an offset into the 64KB
 * real mode can address, so writing tables at 0x70000 through `*(u32 *)addr`
 * writes somewhere else entirely. The symptom was a page fault on the first
 * instruction after paging came on, with CR2 equal to that instruction's own
 * address -- the identity map simply was not there.
 *
 * The rule was already written at the top of this file. Following it needs the
 * tables built down here and moved up, the same way the kernel is. */
#define PT_BASE 0x00800000UL
#define PT_PRESENT 0x001
#define PT_WRITE   0x002
#define PT_HUGE    0x080

/* One table at a time, assembled in a buffer that is reachable and then block
 * moved to where the CPU will walk it. chunk_buf is reused: the kernel has
 * been read by the time this runs, and a second 4KB of bss would put stage 2
 * within a few hundred bytes of the limit the linker script enforces -- which
 * is not hypothetical, since adding the El Torito reader put it 208 bytes over
 * and stopped the build. */
static void put64(u8 *buf, u32 index, u64 value)
{
	u32 *p = (u32 *)(buf + index * 8);

	p[0] = (u32)value;
	p[1] = (u32)(value >> 32);
}

static void clear4k(u8 *buf)
{
	u32 i;

	for (i = 0; i < 1024; i++)
		((u32 *)buf)[i] = 0;
}

static u32 build_page_tables(void)
{
	u32 i, t;

	/* The top level: one entry, covering the first 512GB of address space,
	 * of which the four below describe the first four gigabytes. */
	clear4k(chunk_buf);
	put64(chunk_buf, 0, (u64)(PT_BASE + 0x1000) | PT_PRESENT | PT_WRITE);
	if (!copy_phys(PT_BASE, (u32)chunk_buf, 4096))
		return 0;

	clear4k(chunk_buf);
	for (i = 0; i < 4; i++)
		put64(chunk_buf, i,
		      (u64)(PT_BASE + 0x2000 + i * 0x1000) | PT_PRESENT |
		      PT_WRITE);
	if (!copy_phys(PT_BASE + 0x1000, (u32)chunk_buf, 4096))
		return 0;

	/* Four directories of 512 entries, each entry a 2MB page: four
	 * gigabytes, identity mapped. The kernel replaces all of it with its
	 * own map as soon as it is running. */
	for (t = 0; t < 4; t++) {
		clear4k(chunk_buf);
		for (i = 0; i < 512; i++)
			put64(chunk_buf, i,
			      ((u64)(t * 512 + i) << 21) | PT_PRESENT |
			      PT_WRITE | PT_HUGE);

		if (!copy_phys(PT_BASE + 0x2000 + t * 0x1000,
			       (u32)chunk_buf, 4096))
			return 0;
	}

	return PT_BASE;
}

/* --- placing the kernel ----------------------------------------------------
 *
 * The file was read to a staging address as it came off the disk, because it
 * had to be hashed before anything acted on it. What the CPU runs is not that
 * file: an ELF says where each of its pieces belongs, and the pieces are not
 * laid out in the file the way they are laid out in memory.
 *
 * So the program headers are read back down, and each loadable segment is
 * copied from the staging copy to the address the ELF names -- and the part of
 * a segment that is longer in memory than in the file is zeroed, because that
 * is the kernel's bss and it will read it before it writes it.
 */

#define KERNEL_STAGE 0x01000000UL	/* 16MB, clear of the kernel's own space */

struct elf64_header {
	u8  ident[16];
	u16 type;
	u16 machine;
	u32 version;
	u64 entry;
	u64 phoff;
	u64 shoff;
	u32 flags;
	u16 ehsize;
	u16 phentsize;
	u16 phnum;
	u16 shentsize;
	u16 shnum;
	u16 shstrndx;
} __attribute__((packed));

struct elf64_phdr {
	u32 type;
	u32 flags;
	u64 offset;
	u64 vaddr;
	u64 paddr;
	u64 filesz;
	u64 memsz;
	u64 align;
} __attribute__((packed));

#define PT_LOAD 1

static u8 hdr_buf[1024];

/* Copies between two physical addresses in 32KB pieces, since one BIOS block
 * move carries at most 64KB and the count is in words. */
static int copy_range(u32 dest, u32 src, u32 bytes)
{
	while (bytes) {
		u16 chunk = bytes > 32768 ? 32768 : (u16)bytes;

		if (!copy_phys(dest, src, chunk))
			return 0;
		dest += chunk;
		src += chunk;
		bytes -= chunk;
	}
	return 1;
}

static int zero_range(u32 dest, u32 bytes)
{
	u32 i;

	for (i = 0; i < sizeof(sector); i++)
		sector[i] = 0;

	while (bytes) {
		u16 chunk = bytes > sizeof(sector) ? sizeof(sector)
						   : (u16)bytes;

		if (!copy_phys(dest, (u32)sector, chunk))
			return 0;
		dest += chunk;
		bytes -= chunk;
	}
	return 1;
}

static int place_kernel(u64 *entry_out)
{
	struct elf64_header *eh = (struct elf64_header *)hdr_buf;
	u32 phoff, phnum, phentsize, i;

	if (!copy_phys((u32)hdr_buf, KERNEL_STAGE, sizeof(hdr_buf)))
		return 0;

	if (eh->ident[0] != 0x7F || eh->ident[1] != 'E' ||
	    eh->ident[2] != 'L' || eh->ident[3] != 'F')
		return 0;

	phoff     = (u32)eh->phoff;
	phnum     = eh->phnum;
	phentsize = eh->phentsize;

	if (!phnum || phentsize < sizeof(struct elf64_phdr) ||
	    (u32)phnum * phentsize > sizeof(hdr_buf))
		return 0;

	*entry_out = eh->entry;

	/* The program header table, read separately: it is not necessarily
	 * inside the first kilobyte the header came from. */
	if (!copy_phys((u32)hdr_buf, KERNEL_STAGE + phoff,
		       (u16)((u32)phnum * phentsize)))
		return 0;

	for (i = 0; i < phnum; i++) {
		struct elf64_phdr *ph =
			(struct elf64_phdr *)(hdr_buf + i * phentsize);

		if (ph->type != PT_LOAD || !ph->memsz)
			continue;

		if (ph->filesz &&
		    !copy_range((u32)ph->paddr, KERNEL_STAGE + (u32)ph->offset,
				(u32)ph->filesz))
			return 0;

		if (ph->memsz > ph->filesz &&
		    !zero_range((u32)ph->paddr + (u32)ph->filesz,
				(u32)(ph->memsz - ph->filesz)))
			return 0;
	}

	return 1;
}

/* --- the kernel's own entry point ------------------------------------------
 *
 * An ELF has one entry point and it is already spoken for by the 32-bit
 * trampoline that Multiboot2 and PVH arrive at. The kernel carries a small
 * header in a known section saying where a loader that has already reached long
 * mode should jump instead, and it is found by scanning for its magic -- the
 * same way the UEFI loader finds it.
 */
static int find_kernel_entry(u32 lowest, u32 highest, u64 *entry_out)
{
	/* Each read overlaps the last by the size of a header, so one straddling
	 * the seam between two reads is still whole in one of them. */
	const u32 step = sizeof(hdr_buf) - 32;
	u32 p = lowest;

	while (p + 24 <= highest) {
		u32 n = highest - p;
		u32 off;

		if (n > sizeof(hdr_buf))
			n = sizeof(hdr_buf);

		if (!copy_phys((u32)hdr_buf, p, (u16)n))
			return 0;

		for (off = 0; off + 24 <= n; off += 8) {
			u32 lo  = *(u32 *)&hdr_buf[off];
			u32 hi  = *(u32 *)&hdr_buf[off + 4];
			u32 ver = *(u32 *)&hdr_buf[off + 8];

			/* "RCNBOOT\0" little-endian, then version 1. */
			if (lo == 0x424E4352 && hi == 0x00544F4F && ver == 1) {
				*entry_out = *(u64 *)&hdr_buf[off + 16];
				return 1;
			}
		}

		if (n < sizeof(hdr_buf))
			break;
		p += step;
	}

	return 0;
}


/* --- VBE: asking the firmware for a framebuffer ----------------------------
 *
 * The last thing that needs the BIOS, and therefore the last thing done before
 * long mode. See the note above `vbe_pick` for which mode and why.
 */

struct vbe_info {
	char sig[4];		/* "VESA" */
	u16  version;
	u32  oem;
	u32  caps;
	u32  modes;		/* FAR pointer: segment in the high half */
	u16  memory_64k;
	u8   rest[492];
} __attribute__((packed));

struct vbe_mode {
	u16 attributes;		/* bit 4 graphics, bit 7 linear framebuffer */
	u8  win_a, win_b;
	u16 granularity, win_size;
	u16 seg_a, seg_b;
	u32 win_func;
	u16 pitch;
	u16 width, height;
	u8  w_char, y_char, planes, bpp;
	u8  banks, memory_model, bank_size, image_pages, reserved0;
	u8  red_mask, red_pos, green_mask, green_pos;
	u8  blue_mask, blue_pos, rsv_mask, rsv_pos;
	u8  directcolor;
	u32 framebuffer;	/* the physical address, which is the whole point */
	u32 off_screen;
	u16 off_screen_size;
	u8  rest[206];
} __attribute__((packed));

static struct vbe_info vinfo;
static struct vbe_mode vmode;

/* ES:DI has to point at the buffer, and the BIOS is free to return with ES
 * holding anything. Saved and restored, because the C around this assumes a
 * zero segment for every pointer it forms. */
static u16 vbe_query(u16 ax, u16 cx, void *buf)
{
	u16 ret;

	__asm__ __volatile__(
		"pushw %%es\n\t"
		"pushw %%ds\n\t"
		"popw %%es\n\t"		/* ES = DS = 0, which is where buf is */
		"int $0x10\n\t"
		"popw %%es"
		: "=a"(ret)
		: "a"(ax), "c"(cx), "D"(buf)
		: "cc", "memory");

	return ret;
}

static u16 vbe_set(u16 mode)
{
	u16 ret;

	__asm__ __volatile__("int $0x10"
			     : "=a"(ret)
			     : "a"((u16)0x4F02), "b"(mode)
			     : "cc", "memory");
	return ret;
}

/* One word from anywhere in the first megabyte.
 *
 * The mode list is reached through a far pointer and usually lives in the video
 * ROM around 0xC0000, which is past what a 16-bit offset can name. A segment
 * override is the only way to read it, and FS is used because nothing else in
 * this file touches it. */
static u16 peek16(u32 linear)
{
	u16 seg = (u16)(linear >> 4);
	u16 off = (u16)(linear & 0x0F);
	u16 v;

	__asm__ __volatile__(
		"pushw %%fs\n\t"
		"movw %[s], %%fs\n\t"
		"movw %%fs:(%%bx), %[v]\n\t"
		"popw %%fs"
		: [v] "=&r"(v)
		: [s] "r"(seg), "b"(off)
		: "memory");

	return v;
}

/* The largest mode that fits inside the console's window.
 *
 * Bounded rather than maximised: `fbcon` draws into 1920x1200 however large the
 * screen is (KF-203), so a bigger mode buys pixels nothing will use until there
 * is a compositor, and costs memory bandwidth on a machine old enough not to
 * have UEFI.
 *
 * Chosen by walking what this machine offers rather than asking for a size and
 * hoping: a BIOS with 1024x768 and no 1280x800 should give a screen rather than
 * nothing. Returns 0 when none of them will do, which is a fact about the
 * machine and not a failure.
 */
#define VBE_MAX_W 1920u
#define VBE_MAX_H 1200u

static u16 vbe_pick(u32 *base, u16 *w, u16 *h, u16 *pitch)
{
	u32 list;
	u16 best = 0;
	u32 best_area = 0;
	unsigned i;

	for (i = 0; i < sizeof(vinfo); i++)
		((u8 *)&vinfo)[i] = 0;

	vinfo.sig[0] = 'V'; vinfo.sig[1] = 'B';
	vinfo.sig[2] = 'E'; vinfo.sig[3] = '2';

	if (vbe_query(0x4F00, 0, &vinfo) != 0x004F)
		return 0;

	if (vinfo.sig[0] != 'V' || vinfo.sig[1] != 'E' ||
	    vinfo.sig[2] != 'S' || vinfo.sig[3] != 'A')
		return 0;

	list = ((vinfo.modes >> 16) << 4) + (vinfo.modes & 0xFFFF);

	for (i = 0; i < 512; i++) {
		u16 mode = peek16(list + i * 2);
		u32 area;

		if (mode == 0xFFFF)
			break;

		if (vbe_query(0x4F01, mode, &vmode) != 0x004F)
			continue;

		/* A linear framebuffer, graphics, and thirty-two bits. Each is
		 * a separate reason the mode is no use, and every one of them
		 * is true of modes a machine will happily offer. */
		if (!(vmode.attributes & (1u << 7)))
			continue;
		if (!(vmode.attributes & (1u << 4)))
			continue;
		if (vmode.bpp != 32)
			continue;
		if (!vmode.framebuffer)
			continue;

		if (vmode.width > VBE_MAX_W || vmode.height > VBE_MAX_H)
			continue;

		area = (u32)vmode.width * vmode.height;
		if (area <= best_area)
			continue;

		best      = mode;
		best_area = area;
		*base     = vmode.framebuffer;
		*w        = vmode.width;
		*h        = vmode.height;
		*pitch    = vmode.pitch;
	}

	return best;
}

/* Asks for a screen, and says what happened either way.
 *
 * Returns zero where there is none, which every caller already handles: the
 * kernel has booted with no framebuffer on every aarch64 path since it existed,
 * because AAVMF provides none. */
static u32 vbe_setup(u16 *w, u16 *h, u16 *pitch)
{
	u32 base = 0;
	u16 mode = vbe_pick(&base, w, h, pitch);

	if (!mode) {
		print("  framebuffer  : this firmware offers no linear "
		      "32-bit mode\n");
		return 0;
	}

	/* Bit 14 asks for the linear framebuffer rather than the banked
	 * window. Without it the mode is set and the address is useless. */
	if (vbe_set((u16)(mode | 0x4000)) != 0x004F) {
		print("  framebuffer  : the firmware refused the mode it "
		      "offered\n");
		return 0;
	}

	print("  framebuffer  : ");
	print_dec(*w);
	print("x");
	print_dec(*h);
	print(", pitch ");
	print_dec(*pitch);
	print(" BGRA\n");

	return base;
}

/* --- the handoff -----------------------------------------------------------
 *
 * The same structure the UEFI loader fills in, at a fixed address below the
 * first megabyte where the kernel can read it before it has a map of its own.
 *
 * The memory map is translated from E820's numbering into ReconBoot's, rather
 * than passed through: they agree on 1 and 2 and disagree on everything else,
 * and a map that is subtly wrong is the worst kind of wrong there is at this
 * point in a boot.
 */

/* Below the 64KB real mode addresses, because stage 2 writes this structure
 * through ordinary pointers -- the same rule the page tables above had to be
 * moved to obey, in the other direction. 0x4000 is clear of the interrupt
 * vector table and the BIOS data area at the bottom, and of the stack, stage 1
 * and stage 2 above.
 *
 * The kernel reads it after paging is on, where 0x4000 is mapped like
 * everything else in the first four gigabytes. */
#define HANDOFF_ADDR  0x00004000UL
#define REGIONS_ADDR  0x00004800UL

static u32 e820_to_reconboot(u32 type)
{
	switch (type) {
	case 1:  return RECONBOOT_MEM_USABLE;
	case 2:  return RECONBOOT_MEM_RESERVED;
	case 3:  return RECONBOOT_MEM_ACPI_RECLAIM;
	case 4:  return RECONBOOT_MEM_ACPI_NVS;
	case 5:  return RECONBOOT_MEM_BAD;
	/* A type this loader has never heard of is reserved, not usable.
	 * Guessing the other way hands the kernel memory somebody else owns. */
	default: return RECONBOOT_MEM_RESERVED;
	}
}

static void put_str(u8 *dst, const char *src, u32 max)
{
	u32 i = 0;

	while (src[i] && i + 1 < max) {
		dst[i] = (u8)src[i];
		i++;
	}
	while (i < max)
		dst[i++] = 0;
}

/* Where this loader found the volume it booted from, in blocks from the start
 * of the drive. Declared up here because build_handoff passes it to the kernel;
 * it is set once, by stage2_main, before anything reads it. */
static u64 esp_lba;

/* The structure is filled in through reconboot.h's own declaration rather than
 * by writing at computed offsets. An offset worked out by hand is a number that
 * goes wrong silently the first time somebody adds a field, and this structure
 * is explicitly designed to grow. */
static u32 build_handoff(u8 boot_drive)
{
	struct reconboot *h = (struct reconboot *)HANDOFF_ADDR;
	struct reconboot_region *r = (struct reconboot_region *)REGIONS_ADDR;
	u32 i, n = 0;

	for (i = 0; i < sizeof(*h); i++)
		((u8 *)h)[i] = 0;

	for (i = 0; i < e820_count; i++) {
		r[n].base     = e820[i].base;
		r[n].size     = e820[i].length;
		r[n].kind     = e820_to_reconboot(e820[i].type);
		r[n].reserved = 0;
		n++;
	}

	/* The loader's own working memory, told to the kernel rather than left
	 * for it to walk into. Two ranges, because they are nowhere near each
	 * other: everything real mode can address, and the page tables.
	 *
	 * **The page tables are still live when the kernel starts** -- it is
	 * running on them until it builds its own, so handing that memory out
	 * as usable would be handing out the map underneath its own feet. */
	r[n].base     = 0x00000000ULL;
	r[n].size     = 0x00010000ULL;	/* IVT, BDA, stack, stage 1, stage 2 */
	r[n].kind     = RECONBOOT_MEM_BOOTLOADER;
	r[n].reserved = 0;
	n++;

	r[n].base     = PT_BASE;
	r[n].size     = 0x00006000ULL;
	r[n].kind     = RECONBOOT_MEM_BOOTLOADER;
	r[n].reserved = 0;
	n++;

	/* Cleared before anything is written into it, and this is KF-225.
	 *
	 * `size` below says the loader wrote through the whole structure, and
	 * that is not a description -- it is what `RECONBOOT_HAS` reads to
	 * decide whether an appended field holds a value. This loader fills
	 * eleven fields of sixteen. The other five -- `acpi_rsdp`, `dtb`,
	 * `runtime_services`, `initrd_base`, `initrd_size` -- were never
	 * written and were never zeroed either: HANDOFF_ADDR is 0x4000, below
	 * stage 2 at 0x8000 and below the stack at 0x7000, so the entry stub's
	 * bss sweep does not reach it and nothing else does.
	 *
	 * The stub already says what that costs, about its own statics: *on
	 * this machine that is usually zero, which is the worst case -- it
	 * would work here and fail on a machine whose firmware used the memory
	 * for something.* The same sentence, one address lower.
	 *
	 * So the comment further down claiming zero means the firmware
	 * published no RSDP is true now. It was a hope before. */
	{
		u8 *p = (u8 *)h;
		u32 i;

		for (i = 0; i < sizeof(*h); i++)
			p[i] = 0;
	}

	h->magic        = RECONBOOT_MAGIC;
	h->version      = RECONBOOT_VERSION;
	h->size         = sizeof(*h);
	h->firmware     = RECONBOOT_FIRMWARE_BIOS;
	h->region_count = n;
	h->regions      = REGIONS_ADDR;

	/* The screen, if the firmware had one to give.
	 *
	 * Asked for here rather than earlier because INT 10h stops working the
	 * moment this loader leaves real mode, and this is the last place it
	 * still can. A machine that offers no linear 32-bit mode gets NONE,
	 * which is the honest answer and one the kernel has always handled --
	 * every aarch64 path boots that way, because AAVMF gives none either.
	 *
	 * BGRA because that is what every VBE direct-colour mode this loader
	 * will accept lays out, and the mode search refuses anything that is
	 * not 32 bits a pixel. */
	{
		u16 w = 0, ht = 0, pitch = 0;
		u32 fb = vbe_setup(&w, &ht, &pitch);

		if (fb) {
			h->framebuffer.base   = fb;
			h->framebuffer.size   = (u64)pitch * ht;
			h->framebuffer.width  = w;
			h->framebuffer.height = ht;
			h->framebuffer.pitch  = pitch;
			h->framebuffer.format = RECONBOOT_PIXEL_BGRA;
		} else {
			h->framebuffer.format = RECONBOOT_PIXEL_NONE;
		}
	}

	/* No RSDP and no device tree from here yet. Finding ACPI means scanning
	 * the EBDA and the region below 1MB for "RSD PTR ", which is its own
	 * piece of work; zero says the firmware did not publish one, which is
	 * true of everything this loader has looked at. */

	/* Which volume this kernel came off, which this loader has known all
	 * along and never passed on.
	 *
	 * `esp_lba` is the block it found the EFI partition at and prints on
	 * every boot; `boot_drive` is what the firmware left in DL and stage 1
	 * handed across. Between them they name a volume on this machine.
	 *
	 * No GUID. Reading one means reading the GPT partition entry array --
	 * another disk read, in a loader with eleven kilobytes of room -- and
	 * the pair above already answers the question on a machine that has
	 * one disk controller and one BIOS. The kernel is told there is no
	 * GUID rather than given a wrong one. */
	h->boot_part_lba = esp_lba;
	h->boot_disk     = boot_drive;

	put_str((u8 *)h->loader, "reconboot-bios", sizeof(h->loader));
	put_str((u8 *)h->cmdline, cmdline_buf, sizeof(h->cmdline));

	return HANDOFF_ADDR;
}

/* --- what there is to say so far ------------------------------------------ */

/* In longmode.S: the one part of stage 2 that cannot be C, because between
 * its first and last instruction the machine is not running the language. */
void enter_long_mode(u32 entry_lo, u32 entry_hi, u32 handoff, u32 pml4);


/* Where the FAT32 volume holding the kernel starts, and what kind of medium
 * this is.
 *
 * **A dispatch, not a fallback.** Asking the GPT reader first and trying El
 * Torito when it came back empty would also work today, and would be wrong the
 * first time a disk turns up with no EFI partition: it would go looking for a
 * boot catalogue on a hard disk and act on whatever those bytes happened to
 * say. The medium states which it is, and exactly one reader runs.
 */
static u64 find_boot_volume(u8 drive, const char **kind)
{
	if (is_iso9660(drive)) {
		*kind = "disc";
		return find_eltorito(drive);
	}

	*kind = "disk";
	return find_esp(drive);
}

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

	{
		const char *kind = "disk";

		esp_lba = find_boot_volume((u8)boot_drive, &kind);
		if (!esp_lba) {
			/* Said plainly, because on a machine where this
			 * happens the kernel is unreachable and the reason is
			 * worth more than a halt. */
			print("esp: none found on this ");
			print(kind);
			print("\n");
			goto done;
		}

		print("esp: ");
		print(kind);
		print(", block ");
		print_dec((u32)esp_lba);
		print("\n");
	}

	if (!fat_mount((u8)boot_drive, (u32)esp_lba)) {
		print("esp: not a FAT32 volume this loader can read\n");
		goto done;
	}

	{
		u32 dir, file, size = 0, got, handoff, pml4;
		u64 entry = 0, elf_entry = 0;

		dir = dir_find((u8)boot_drive, root_cluster, "reconos", 0);
		if (!dir) {
			print("kernel: no \\reconos directory on the ESP\n");
			goto done;
		}

		/* Before the kernel is loaded, because build_handoff runs
		 * after this block and passes on what this leaves behind. */
		read_cmdline((u8)boot_drive, dir);

		file = dir_find((u8)boot_drive, dir, KERNEL_NAME, &size);
		if (!file) {
			print("kernel: " KERNEL_NAME " is not there\n");
			goto done;
		}

		print("kernel: " KERNEL_NAME ", ");
		print_dec(size);
		print(" bytes\n");

		sha256_init(&hash);
		got = read_file((u8)boot_drive, file, size, KERNEL_STAGE,
				&hash);
		if (got != size) {
			print("kernel: only ");
			print_dec(got);
			print(" bytes reached memory\n");
			goto done;
		}

		/* Read back, because a copy that silently did nothing leaves
		 * whatever was at the destination -- and on a machine just
		 * powered on that is zeroes, which is what a failed read looks
		 * like from here too. */
		if (!check_landed(KERNEL_STAGE)) {
			print("kernel: what landed at 1 MB is not an ELF\n");
			goto done;
		}

		print("kernel: ");
		print_dec(got);
		print(" bytes staged, and it is an ELF\n");

		/* Before anything is placed where it will run. A signature
		 * checked after the kernel is in position is a signature
		 * checked after the damage. */
		if (!verify(&hash, dir, (u8)boot_drive))
			goto done;

		if (!place_kernel(&elf_entry)) {
			print("kernel: its program headers do not make sense\n");
			goto done;
		}

		/* The ELF's own entry point is the 32-bit trampoline, which is
		 * for the loaders that hand over before long mode. This loader
		 * arrives in long mode, so it jumps where the kernel's own
		 * header says instead -- the same header reconboot scans for.
		 */
		if (!find_kernel_entry(KERNEL_STAGE,
				       KERNEL_STAGE + 0x10000, &entry)) {
			print("kernel: no ReconBoot header, so there is nowhere "
			      "to jump that is not the 32-bit trampoline\n");
			goto done;
		}

		handoff = build_handoff((u8)boot_drive);
		pml4 = build_page_tables();

		if (!pml4) {
			print("page tables: the block move refused them\n");
			goto done;
		}

		print("handing over: entry 0x");
		print_hex8((u8)(entry >> 24));
		print_hex8((u8)(entry >> 16));
		print_hex8((u8)(entry >> 8));
		print_hex8((u8)entry);
		print("\n\n");

		enter_long_mode((u32)entry, (u32)(entry >> 32), handoff, pml4);

		/* enter_long_mode does not return. Reaching here is a fault in
		 * it, and saying so beats a blank screen. */
		print("reconboot: the switch to long mode returned\n");
	}

done:

	for (;;)
		__asm__ __volatile__("hlt");
}
