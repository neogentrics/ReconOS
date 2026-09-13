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

static int disk_read(u8 drive, u64 lba, u16 count, void *buf)
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

/* One cluster at a time. Sixteen kilobytes is the largest cluster this will
 * accept, which covers an EFI partition up to about eight gigabytes; anything
 * larger is refused by name rather than read incorrectly. The buffer has to
 * live below 0x10000 with everything else, which is what sets the ceiling. */
#define CLUSTER_MAX 16384
static u8 cluster_buf[CLUSTER_MAX];

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
	u32 cluster_bytes = (u32)sectors_per_cluster * 512;
	u32 done = 0;

	if (cluster_bytes > CLUSTER_MAX)
		return 0;

	while (cluster && done < size) {
		u32 want = size - done;
		u8 s;

		if (want > cluster_bytes)
			want = cluster_bytes;

		for (s = 0; s < sectors_per_cluster; s++)
			if (!disk_read(drive, cluster_lba(cluster) + s, 1,
				       &cluster_buf[(u32)s * 512]))
				return done;

		if (ctx)
			sha256_update(ctx, cluster_buf, want);

		if (!copy_high(dest + done, cluster_buf, (u16)want))
			return done;

		done += want;
		cluster = fat_next(drive, cluster);
	}

	return done;
}

#ifdef RECONOS_KEY_PRESENT

/* Built only when there is a key to check against: this and the buffer below
 * exist to serve the verifier, and a keyless build has no verifier. */

/* A small file, into a buffer down here. The signature is 256 bytes and has to
 * be read where it can be looked at, rather than pushed above the megabyte with
 * the kernel. */
static u32 read_small(u8 drive, u32 cluster, u32 size, u8 *out, u32 max)
{
	u32 cluster_bytes = (u32)sectors_per_cluster * 512;
	u32 done = 0;

	if (size > max || cluster_bytes > CLUSTER_MAX)
		return 0;

	while (cluster && done < size) {
		u32 want = size - done;
		u32 i;
		u8 s;

		if (want > cluster_bytes)
			want = cluster_bytes;

		for (s = 0; s < sectors_per_cluster; s++)
			if (!disk_read(drive, cluster_lba(cluster) + s, 1,
				       &cluster_buf[(u32)s * 512]))
				return done;

		for (i = 0; i < want; i++)
			out[done + i] = cluster_buf[i];

		done += want;
		cluster = fat_next(drive, cluster);
	}

	return done;
}

static u8 sig_buf[256];

#endif /* RECONOS_KEY_PRESENT */

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
 * moved to where the CPU will walk it. cluster_buf is reused: the kernel has
 * been read by the time this runs, and a second 4KB of bss would put stage 2
 * within a few hundred bytes of the limit the linker script enforces. */
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
	clear4k(cluster_buf);
	put64(cluster_buf, 0, (u64)(PT_BASE + 0x1000) | PT_PRESENT | PT_WRITE);
	if (!copy_phys(PT_BASE, (u32)cluster_buf, 4096))
		return 0;

	clear4k(cluster_buf);
	for (i = 0; i < 4; i++)
		put64(cluster_buf, i,
		      (u64)(PT_BASE + 0x2000 + i * 0x1000) | PT_PRESENT |
		      PT_WRITE);
	if (!copy_phys(PT_BASE + 0x1000, (u32)cluster_buf, 4096))
		return 0;

	/* Four directories of 512 entries, each entry a 2MB page: four
	 * gigabytes, identity mapped. The kernel replaces all of it with its
	 * own map as soon as it is running. */
	for (t = 0; t < 4; t++) {
		clear4k(cluster_buf);
		for (i = 0; i < 512; i++)
			put64(cluster_buf, i,
			      ((u64)(t * 512 + i) << 21) | PT_PRESENT |
			      PT_WRITE | PT_HUGE);

		if (!copy_phys(PT_BASE + 0x2000 + t * 0x1000,
			       (u32)cluster_buf, 4096))
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

/* The structure is filled in through reconboot.h's own declaration rather than
 * by writing at computed offsets. An offset worked out by hand is a number that
 * goes wrong silently the first time somebody adds a field, and this structure
 * is explicitly designed to grow. */
static u32 build_handoff(void)
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

	h->magic        = RECONBOOT_MAGIC;
	h->version      = RECONBOOT_VERSION;
	h->size         = sizeof(*h);
	h->firmware     = RECONBOOT_FIRMWARE_BIOS;
	h->region_count = n;
	h->regions      = REGIONS_ADDR;

	/* No framebuffer. VBE could provide one and does not yet, and a
	 * framebuffer this loader has not set up is one the kernel must not be
	 * told about: NONE is the honest answer, and the kernel already handles
	 * it because AAVMF gives none either. */
	h->framebuffer.format = RECONBOOT_PIXEL_NONE;

	/* No RSDP and no device tree from here yet. Finding ACPI means scanning
	 * the EBDA and the region below 1MB for "RSD PTR ", which is its own
	 * piece of work; zero says the firmware did not publish one, which is
	 * true of everything this loader has looked at. */

	put_str((u8 *)h->loader, "reconboot-bios", sizeof(h->loader));
	put_str((u8 *)h->cmdline, "", sizeof(h->cmdline));

	return HANDOFF_ADDR;
}

/* --- what there is to say so far ------------------------------------------ */

/* In longmode.S: the one part of stage 2 that cannot be C, because between
 * its first and last instruction the machine is not running the language. */
void enter_long_mode(u32 entry_lo, u32 entry_hi, u32 handoff, u32 pml4);

static u64 esp_lba;

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

	esp_lba = find_esp((u8)boot_drive);
	if (!esp_lba) {
		/* Said plainly, because on a machine where this happens the
		 * kernel is unreachable and the reason is worth more than a
		 * halt. */
		print("esp: none found -- no EFI partition on this disk\n");
		goto done;
	}

	print("esp: block ");
	print_dec((u32)esp_lba);
	print("\n");

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

		handoff = build_handoff();
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
