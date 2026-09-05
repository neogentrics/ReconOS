/* reconboot -- the ReconOS UEFI bootloader.
 *
 * What it does, in order: says hello on whatever console the firmware gave us,
 * finds a framebuffer, opens the volume it was loaded from, reads the kernel
 * off it, places the kernel's segments where the ELF says they go, collects the
 * memory map, leaves UEFI, and jumps.
 *
 * The last two steps are the delicate ones and they are delicate together. See
 * the comment above exit_and_jump().
 */
#include "efi.h"
#include "reconboot.h"

#define KERNEL_PATH u"\\reconos\\kernel.elf"

#if defined(__x86_64__)
typedef void (*kernel_entry_fn)(struct reconboot *) __attribute__((sysv_abi));
#else
typedef void (*kernel_entry_fn)(struct reconboot *);
#endif

static EFI_SYSTEM_TABLE  *ST;
static EFI_BOOT_SERVICES *BS;
static EFI_HANDLE         IMAGE;

/* --- Console ------------------------------------------------------------
 *
 * UEFI speaks UTF-16. Everything here is ASCII, widened on the way out, because
 * a loader that needs a text encoding library has lost sight of its job. */

static void print(const char *s)
{
	CHAR16 buf[128];
	UINTN n = 0;

	while (*s) {
		if (*s == '\n')
			buf[n++] = u'\r';
		buf[n++] = (CHAR16)(unsigned char)*s++;

		if (n >= 120) {
			buf[n] = 0;
			ST->ConOut->OutputString(ST->ConOut, buf);
			n = 0;
		}
	}
	buf[n] = 0;
	ST->ConOut->OutputString(ST->ConOut, buf);
}

static void print_hex(UINT64 v)
{
	static const char digits[] = "0123456789abcdef";
	char buf[19];
	int i = 18;

	buf[i] = '\0';
	if (v == 0)
		buf[--i] = '0';
	while (v) {
		buf[--i] = digits[v & 0xf];
		v >>= 4;
	}
	print("0x");
	print(&buf[i]);
}

static void print_dec(UINT64 v)
{
	char buf[21];
	int i = 20;

	buf[i] = '\0';
	if (v == 0)
		buf[--i] = '0';
	while (v) {
		buf[--i] = (char)('0' + (v % 10));
		v /= 10;
	}
	print(&buf[i]);
}

/* A failure here cannot be recovered from and must not be silent: the machine
 * is about to look like it hung. Say what went wrong and stop. */
static void fail(const char *what, EFI_STATUS status)
{
	print("\nreconboot: ");
	print(what);
	print(" failed, status ");
	print_hex(status);
	print("\nHalted.\n");

	for (;;)
		BS->Stall(1000000);
}

/* --- Small helpers ------------------------------------------------------ */

static void copy(void *dst, const void *src, UINTN n)
{
	unsigned char *d = dst;
	const unsigned char *s = src;

	while (n--)
		*d++ = *s++;
}

static void zero(void *dst, UINTN n)
{
	unsigned char *d = dst;

	while (n--)
		*d++ = 0;
}

static bool guid_eq(const EFI_GUID *a, const EFI_GUID *b)
{
	const unsigned char *x = (const unsigned char *)a;
	const unsigned char *y = (const unsigned char *)b;

	for (UINTN i = 0; i < sizeof(EFI_GUID); i++)
		if (x[i] != y[i])
			return false;
	return true;
}

/* --- ELF ----------------------------------------------------------------
 *
 * Only what a loader needs: the program headers say which byte ranges of the
 * file belong at which physical addresses. Sections, symbols and relocations
 * are a linker's business and are not read.
 */

#define ELF_MAGIC 0x464C457FU	/* 0x7F 'E' 'L' 'F' */
#define PT_LOAD   1

struct elf64_header {
	uint32_t magic;
	uint8_t  class_, data, version, osabi, abiversion, pad[7];
	uint16_t type, machine;
	uint32_t version2;
	uint64_t entry, phoff, shoff;
	uint32_t flags;
	uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};

struct elf64_phdr {
	uint32_t type, flags;
	uint64_t offset, vaddr, paddr, filesz, memsz, align;
};

/* --- Reading the kernel off the volume we booted from ------------------- */

static void *read_kernel(UINTN *size_out)
{
	EFI_GUID li_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
	EFI_GUID fs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
	EFI_GUID fi_guid = EFI_FILE_INFO_GUID;

	EFI_LOADED_IMAGE_PROTOCOL *li;
	EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs;
	EFI_FILE_PROTOCOL *root, *file;
	EFI_STATUS s;

	UINT8 info_buf[512];
	UINTN info_size = sizeof(info_buf);
	UINTN size;
	void *buf;

	/* Which volume did the firmware load *us* from? That is the one the
	 * kernel is on, and asking rather than searching means a machine with
	 * several ReconOS installations boots the one it was told to. */
	s = BS->HandleProtocol(IMAGE, &li_guid, (void **)&li);
	if (EFI_ERROR(s))
		fail("finding our own loaded image", s);

	s = BS->HandleProtocol(li->DeviceHandle, &fs_guid, (void **)&fs);
	if (EFI_ERROR(s))
		fail("opening the filesystem we were loaded from", s);

	s = fs->OpenVolume(fs, &root);
	if (EFI_ERROR(s))
		fail("opening the volume", s);

	s = root->Open(root, &file, (CHAR16 *)KERNEL_PATH, EFI_FILE_MODE_READ, 0);
	if (EFI_ERROR(s)) {
		print("\nreconboot: could not open \\reconos\\kernel.elf\n");
		fail("opening the kernel", s);
	}

	s = file->GetInfo(file, &fi_guid, &info_size, info_buf);
	if (EFI_ERROR(s))
		fail("asking how large the kernel is", s);

	size = (UINTN)((EFI_FILE_INFO *)info_buf)->FileSize;

	/* EfiLoaderData rather than EfiBootServicesData: this buffer has to
	 * survive ExitBootServices, and boot-services memory does not. */
	s = BS->AllocatePool(EfiLoaderData, size, &buf);
	if (EFI_ERROR(s))
		fail("finding room for the kernel", s);

	UINTN want = size;
	s = file->Read(file, &want, buf);
	if (EFI_ERROR(s))
		fail("reading the kernel", s);
	if (want != size)
		fail("reading the whole kernel", EFI_LOAD_ERROR);

	file->Close(file);
	root->Close(root);

	*size_out = size;
	return buf;
}

/* Places each loadable segment at the physical address the ELF names, and
 * zeroes the part of it that has no bytes in the file -- which is the kernel's
 * BSS, and which the kernel is entitled to find already clear.
 *
 * The pages are claimed from the firmware first. Without that, the kernel would
 * be written into memory UEFI still believes is free, and UEFI is still running
 * at this point. */
static uint64_t load_kernel(void *image, UINTN image_size, uint64_t *entry_out)
{
	struct elf64_header *eh = image;
	uint64_t lowest = ~0ULL, highest = 0;

	if (image_size < sizeof(*eh) || eh->magic != ELF_MAGIC)
		fail("recognising the kernel as an ELF", EFI_LOAD_ERROR);

	for (unsigned i = 0; i < eh->phnum; i++) {
		struct elf64_phdr *ph = (struct elf64_phdr *)
			((uint8_t *)image + eh->phoff + i * eh->phentsize);

		if (ph->type != PT_LOAD || ph->memsz == 0)
			continue;

		if (ph->paddr < lowest)
			lowest = ph->paddr;
		if (ph->paddr + ph->memsz > highest)
			highest = ph->paddr + ph->memsz;
	}

	if (highest == 0)
		fail("finding anything to load in the kernel", EFI_LOAD_ERROR);

	{
		EFI_PHYSICAL_ADDRESS at = lowest & ~0xFFFULL;
		UINTN pages = (UINTN)((highest - at + 0xFFF) / 0x1000);
		EFI_STATUS s = BS->AllocatePages(AllocateAddress, EfiLoaderData,
						 pages, &at);

		/* AllocateAddress can fail because the firmware has something of
		 * its own where the kernel wants to be. That is a real
		 * possibility on a machine we have never seen, and the fix is a
		 * relocatable kernel rather than a retry -- so it is reported
		 * precisely rather than worked around. */
		if (EFI_ERROR(s)) {
			print("\nreconboot: the kernel wants to live at ");
			print_hex(at);
			print(" and the firmware will not give it up.\n");
			fail("claiming the kernel's memory", s);
		}
	}

	for (unsigned i = 0; i < eh->phnum; i++) {
		struct elf64_phdr *ph = (struct elf64_phdr *)
			((uint8_t *)image + eh->phoff + i * eh->phentsize);

		if (ph->type != PT_LOAD || ph->memsz == 0)
			continue;

		copy((void *)(uintptr_t)ph->paddr,
		     (uint8_t *)image + ph->offset, ph->filesz);
		zero((void *)(uintptr_t)(ph->paddr + ph->filesz),
		     ph->memsz - ph->filesz);
	}

	/* Find where to jump. Not the ELF entry point -- that belongs to the
	 * 32-bit trampoline the other boot protocols arrive through. The kernel
	 * carries a header naming its 64-bit entry, and it is found by scanning
	 * the loaded image for the magic. */
	for (uint64_t p = lowest; p + sizeof(struct reconboot_kernel_header) <= highest;
	     p += 8) {
		struct reconboot_kernel_header *h =
			(struct reconboot_kernel_header *)(uintptr_t)p;

		if (h->magic == RECONBOOT_MAGIC && h->version == RECONBOOT_VERSION) {
			*entry_out = h->entry;
			return highest;
		}
	}

	print("\nreconboot: the kernel has no ReconBoot header, so there is\n"
	      "nowhere to jump that is not the 32-bit trampoline.\n");
	fail("finding the kernel's 64-bit entry point", EFI_LOAD_ERROR);
	return 0;
}

/* --- Framebuffer -------------------------------------------------------- */

static void find_framebuffer(struct reconboot_framebuffer *fb)
{
	EFI_GUID gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
	EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
	EFI_STATUS s;

	zero(fb, sizeof(*fb));
	fb->format = RECONBOOT_PIXEL_NONE;

	s = BS->LocateProtocol(&gop_guid, 0, (void **)&gop);
	if (EFI_ERROR(s)) {
		/* A serial-only machine, or one whose firmware offers no
		 * graphics. Not a failure: the kernel has a serial console and
		 * this is exactly what a framebuffer of zero width means. */
		print("  framebuffer  : none offered by this firmware\n");
		return;
	}

	fb->base   = gop->Mode->FrameBufferBase;
	fb->size   = gop->Mode->FrameBufferSize;
	fb->width  = gop->Mode->Info->HorizontalResolution;
	fb->height = gop->Mode->Info->VerticalResolution;

	/* Pitch in bytes, from pixels-per-scanline. The two differ whenever the
	 * firmware pads rows, which is common, and using width here instead is
	 * how a picture ends up sheared. */
	fb->pitch = gop->Mode->Info->PixelsPerScanLine * 4;

	switch (gop->Mode->Info->PixelFormat) {
	case PixelBlueGreenRedReserved8BitPerColor:
		fb->format = RECONBOOT_PIXEL_BGRA;
		break;
	case PixelRedGreenBlueReserved8BitPerColor:
		fb->format = RECONBOOT_PIXEL_RGBA;
		break;
	default:
		/* PixelBitMask and PixelBltOnly. The first needs the masks
		 * interpreting and the second has no linear framebuffer at all.
		 * Reported as unknown rather than guessed at: drawing into a
		 * buffer whose layout we assumed is how you get a display full
		 * of colourful noise. */
		fb->format = RECONBOOT_PIXEL_NONE;
		break;
	}

	print("  framebuffer  : ");
	print_dec(fb->width);
	print("x");
	print_dec(fb->height);
	print(" at ");
	print_hex(fb->base);
	print(", pitch ");
	print_dec(fb->pitch);
	print(fb->format == RECONBOOT_PIXEL_BGRA ? " BGRA\n" :
	      fb->format == RECONBOOT_PIXEL_RGBA ? " RGBA\n" :
	      " layout not understood\n");
}

/* --- Firmware tables ---------------------------------------------------- */

static void find_tables(struct reconboot *bi)
{
	EFI_GUID acpi20 = EFI_ACPI_20_TABLE_GUID;
	EFI_GUID acpi10 = EFI_ACPI_10_TABLE_GUID;
	EFI_GUID dtb    = EFI_DTB_TABLE_GUID;

	for (UINTN i = 0; i < ST->NumberOfTableEntries; i++) {
		EFI_CONFIGURATION_TABLE *t = &ST->ConfigurationTable[i];

		/* ACPI 2.0 wins over 1.0 where both are present, which is why
		 * it is not an else-if: a machine publishing both should be
		 * read through the newer one. */
		if (guid_eq(&t->VendorGuid, &acpi20))
			bi->acpi_rsdp = (uint64_t)(uintptr_t)t->VendorTable;
		else if (guid_eq(&t->VendorGuid, &acpi10) && !bi->acpi_rsdp)
			bi->acpi_rsdp = (uint64_t)(uintptr_t)t->VendorTable;
		else if (guid_eq(&t->VendorGuid, &dtb))
			bi->dtb = (uint64_t)(uintptr_t)t->VendorTable;
	}
}

/* --- Memory map, and leaving --------------------------------------------
 *
 * These are one operation, not two, and the reason is the map key.
 *
 * ExitBootServices only succeeds if the key it is given matches the current
 * state of the memory map. Anything that allocates -- including calling
 * GetMemoryMap itself, which may need a larger buffer -- changes the map and
 * invalidates the key. So the sequence is: allocate generously first, then get
 * the map, then immediately exit with the key it returned, with nothing in
 * between. If the exit fails anyway, the only correct response is to get the
 * map again and retry, because the firmware has changed something underneath.
 *
 * After ExitBootServices returns, none of BS is callable and print() no longer
 * works. Nothing below that line may fail.
 */
static void exit_and_jump(struct reconboot *bi, uint64_t entry)
{
	EFI_MEMORY_DESCRIPTOR *map = 0;
	UINTN map_size = 0, map_key = 0, desc_size = 0;
	UINT32 desc_version = 0;
	EFI_STATUS s;
	struct reconboot_region *regions;

	/* Ask how large the map is, then ask for more than that: getting the
	 * map is itself an allocation, and the map can grow between the two
	 * calls. Four extra descriptors is the conventional slack. */
	s = BS->GetMemoryMap(&map_size, 0, &map_key, &desc_size, &desc_version);
	if (s != EFI_BUFFER_TOO_SMALL)
		fail("asking how large the memory map is", s);

	map_size += desc_size * 8;

	s = BS->AllocatePool(EfiLoaderData, map_size, (void **)&map);
	if (EFI_ERROR(s))
		fail("finding room for the memory map", s);

	/* Room for the translated map, allocated *now*, because allocating it
	 * after the final GetMemoryMap would invalidate the key. */
	s = BS->AllocatePool(EfiLoaderData,
			     (map_size / desc_size) * sizeof(*regions),
			     (void **)&regions);
	if (EFI_ERROR(s))
		fail("finding room for the translated memory map", s);

	bi->regions = (uint64_t)(uintptr_t)regions;

	print("\nreconboot: leaving UEFI.\n");

	for (int attempt = 0; attempt < 4; attempt++) {
		UINTN this_size = map_size;

		s = BS->GetMemoryMap(&this_size, map, &map_key,
				     &desc_size, &desc_version);
		if (EFI_ERROR(s))
			fail("reading the memory map", s);

		/* Translate before exiting. After the exit there is no console
		 * to report a mistake on, so the work is done while there still
		 * is one -- even though it means the map is translated once per
		 * attempt. */
		bi->region_count = 0;
		for (UINTN off = 0; off < this_size; off += desc_size) {
			EFI_MEMORY_DESCRIPTOR *d =
				(EFI_MEMORY_DESCRIPTOR *)((uint8_t *)map + off);
			uint32_t kind;

			switch (d->Type) {
			case EfiConventionalMemory:
			case EfiBootServicesCode:
			case EfiBootServicesData:
				/* Boot services memory becomes ours the instant
				 * we exit, which is the whole point of exiting. */
				kind = RECONBOOT_MEM_USABLE;
				break;
			case EfiLoaderCode:
			case EfiLoaderData:
				/* Ours, and still in use: the kernel image and
				 * this structure are in here. Reclaimable once
				 * the kernel has finished reading it. */
				kind = RECONBOOT_MEM_BOOTLOADER;
				break;
			case EfiACPIReclaimMemory:
				kind = RECONBOOT_MEM_ACPI_RECLAIM;
				break;
			case EfiACPIMemoryNVS:
				kind = RECONBOOT_MEM_ACPI_NVS;
				break;
			case EfiUnusableMemory:
				kind = RECONBOOT_MEM_BAD;
				break;
			default:
				kind = RECONBOOT_MEM_RESERVED;
				break;
			}

			regions[bi->region_count].base = d->PhysicalStart;
			regions[bi->region_count].size = d->NumberOfPages * 0x1000;
			regions[bi->region_count].kind = kind;
			regions[bi->region_count].reserved = 0;
			bi->region_count++;
		}

		s = BS->ExitBootServices(IMAGE, map_key);
		if (!EFI_ERROR(s))
			break;

		/* The map changed underneath us. The key is stale; go round
		 * again. This genuinely happens on real firmware. */
		if (attempt == 3)
			fail("leaving UEFI", s);
	}

	/* No console, no allocator, no firmware services. Just the kernel.
	 *
	 * The cast is not decoration. This loader is compiled for the Microsoft
	 * ABI because that is what UEFI requires, and the kernel is compiled by
	 * gcc for the System V ABI. On x86_64 those disagree about where the
	 * first argument goes -- RCX against RDI -- so calling straight through
	 * would hand the kernel whatever happened to be in RDI. The handoff is
	 * declared System V because the kernel is the thing that has to live
	 * with it afterwards. On aarch64 both ABIs pass the first argument in
	 * x0 and there is nothing to reconcile. */
	((kernel_entry_fn)(uintptr_t)entry)(bi);

	/* Not reached. If it is, there is nothing left to report it with. */
	for (;;)
		__asm__ volatile("");
}

/* --- Entry -------------------------------------------------------------- */

static struct reconboot boot_info;

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *system_table)
{
	void *kernel_image;
	UINTN kernel_size;
	uint64_t entry = 0;

	IMAGE = image;
	ST = system_table;
	BS = system_table->BootServices;

	ST->ConOut->ClearScreen(ST->ConOut);
	print("reconboot -- the ReconOS bootloader\n\n");

	zero(&boot_info, sizeof(boot_info));
	boot_info.magic    = RECONBOOT_MAGIC;
	boot_info.version  = RECONBOOT_VERSION;
	boot_info.size     = sizeof(boot_info);
	boot_info.firmware = RECONBOOT_FIRMWARE_UEFI;
	copy(boot_info.loader, "reconboot", 10);

	find_framebuffer(&boot_info.framebuffer);
	find_tables(&boot_info);

	print("  acpi rsdp    : ");
	print_hex(boot_info.acpi_rsdp);
	print("\n  device tree  : ");
	print_hex(boot_info.dtb);
	print("\n");

	kernel_image = read_kernel(&kernel_size);
	print("  kernel       : ");
	print_dec(kernel_size);
	print(" bytes read\n");

	load_kernel(kernel_image, kernel_size, &entry);
	print("  entry        : ");
	print_hex(entry);
	print("\n");

	exit_and_jump(&boot_info, entry);
	return EFI_LOAD_ERROR;
}
