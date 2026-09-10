/* The ELF loader. See elf.h for what it refuses and why.
 *
 * The shape of this file is: check everything, then map. Two passes over the
 * program headers, and no page is allocated during the first. A loader that
 * maps as it validates leaves a half-built address space behind when it meets
 * the bad field, and the caller then has to know how much to undo.
 */
#include <recon/kernel/elf.h>

#include <recon/kernel/addrspace.h>
#include <recon/kernel/arch.h>
#include <recon/kernel/console.h>
#include <recon/kernel/heap.h>
#include <recon/kernel/kstring.h>
#include <recon/kernel/pmm.h>
#include <recon/kernel/user.h>
#include <recon/kernel/vm.h>

#define ELF_MAGIC   0x464C457FU		/* 0x7F 'E' 'L' 'F' */
#define ELFCLASS64  2
#define ELFDATA2LSB 1
#define ET_EXEC     2
#define PT_LOAD     1
#define PT_INTERP   3

#define PF_X 1
#define PF_W 2
#define PF_R 4

/* Eight is more than any program this kernel will load has needed, and a number
 * somebody can see beats a loop bounded by a field in the file. */
#define ELF_SEGMENTS_MAX 8

/* A ceiling on how much memory one program may ask for while being loaded. Not
 * a policy about program size -- it is the bound that stops a header claiming
 * four gigabytes and being handed it one page at a time until the machine has
 * none. Sixteen megabytes is far more than anything here and far less than the
 * machine. */
#define ELF_IMAGE_MAX (16u * 1024u * 1024u)

struct elf64_header {
	u32 magic;
	u8  class_, data, version, osabi, abiversion, pad[7];
	u16 type, machine;
	u32 version2;
	u64 entry, phoff, shoff;
	u32 flags;
	u16 ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};

struct elf64_phdr {
	u32 type, flags;
	u64 offset, vaddr, paddr, filesz, memsz, align;
};

const char *elf_why(enum elf_result r)
{
	switch (r) {
	case ELF_OK:			return "loaded";
	case ELF_TOO_SMALL:		return "the file is smaller than an ELF header";
	case ELF_NOT_AN_ELF:		return "the file does not begin with an ELF magic";
	case ELF_WRONG_CLASS:		return "not a 64-bit little-endian ELF";
	case ELF_WRONG_MACHINE:		return "built for a different architecture";
	case ELF_NOT_EXECUTABLE:	return "not an executable -- shared objects need relocating and nothing here does that";
	case ELF_BAD_PHDRS:		return "the program headers are not inside the file";
	case ELF_NO_SEGMENTS:		return "there is nothing in it to load";
	case ELF_TOO_MANY_SEGMENTS:	return "more loadable segments than this kernel will map";
	case ELF_WANTS_INTERPRETER:	return "it asks for a dynamic linker, and there is none";
	case ELF_SEGMENT_OUTSIDE:	return "a segment's bytes are not inside the file";
	case ELF_SEGMENT_TOO_BIG:	return "a segment asks for more memory than a program is given";
	case ELF_SEGMENT_NOT_USER:	return "a segment wants an address that is not the program's to name";
	case ELF_SEGMENT_UNALIGNED:	return "a segment's file offset and address disagree about where a page starts";
	case ELF_SEGMENT_OVERLAP:	return "two segments claim the same page";
	case ELF_WRITABLE_AND_EXEC:	return "a segment is both writable and executable";
	case ELF_ENTRY_NOT_IN_CODE:	return "the entry point is not inside anything executable";
	case ELF_NO_MEMORY:		return "the machine had no memory for it";
	}

	return "refused for a reason nobody named";
}

/* --- the arithmetic that has to not wrap ---------------------------------
 *
 * Every "is this inside that" question below is asked as a subtraction rather
 * than as an addition, because `start + length` is exactly how a range near the
 * top of the address space becomes a range near the bottom -- and then compares
 * as comfortably inside whatever it is being checked against. The file chooses
 * both numbers.
 */
static bool inside_file(u64 offset, u64 length, u64 file_len)
{
	if (offset > file_len)
		return false;

	return length <= file_len - offset;
}

/* The half of the address space a program owns.
 *
 * The first page is excluded deliberately: it is never mapped, so that a null
 * pointer in a program faults the way it does in the kernel. A program that
 * asked to be loaded there would take that away from itself and would also be
 * the shape of a file trying to make a null dereference succeed. */
static bool inside_user(u64 va, u64 length)
{
	if (va < PAGE_SIZE || va >= USER_LIMIT)
		return false;

	return length <= USER_LIMIT - va;
}

static u64 page_down(u64 v) { return v & ~(u64)(PAGE_SIZE - 1); }
static u64 page_up(u64 v)   { return (v + PAGE_SIZE - 1) & ~(u64)(PAGE_SIZE - 1); }

static unsigned vm_flags_of(u32 p_flags)
{
	unsigned f = VM_USER;

	if (p_flags & PF_R)
		f |= VM_READ;
	if (p_flags & PF_W)
		f |= VM_WRITE;
	if (p_flags & PF_X)
		f |= VM_EXEC;

	return f;
}

/* --- pass one: is this a program at all ---------------------------------- */

struct segment {
	u64 file_offset;
	u64 file_bytes;
	u64 va_first;		/* page-aligned, inclusive */
	u64 va_last;		/* page-aligned, exclusive */
	u64 va_exact;		/* where the segment actually starts */
	u32 flags;
};

static enum elf_result inspect(const void *image, u64 len,
			       struct segment *segs, unsigned *count_out,
			       u64 *entry_out)
{
	const struct elf64_header *eh = image;
	unsigned count = 0;
	u64 phdr_bytes;
	unsigned i;

	if (len < sizeof(*eh))
		return ELF_TOO_SMALL;

	if (eh->magic != ELF_MAGIC)
		return ELF_NOT_AN_ELF;

	if (eh->class_ != ELFCLASS64 || eh->data != ELFDATA2LSB)
		return ELF_WRONG_CLASS;

	/* Asked of the architecture rather than written here, so that core/
	 * keeps not knowing which machine it is on. */
	if (eh->machine != arch_elf_machine())
		return ELF_WRONG_MACHINE;

	if (eh->type != ET_EXEC)
		return ELF_NOT_EXECUTABLE;

	/* The header says how big its own entries are. Trusting that and then
	 * indexing by it is how a file chooses where this reads from. */
	if (eh->phentsize != sizeof(struct elf64_phdr) || eh->phnum == 0)
		return ELF_BAD_PHDRS;

	phdr_bytes = (u64)eh->phnum * sizeof(struct elf64_phdr);
	if (!inside_file(eh->phoff, phdr_bytes, len))
		return ELF_BAD_PHDRS;

	for (i = 0; i < eh->phnum; i++) {
		const struct elf64_phdr *ph = (const struct elf64_phdr *)
			((const u8 *)image + eh->phoff +
			 (u64)i * sizeof(struct elf64_phdr));
		struct segment *s;
		unsigned j;

		if (ph->type == PT_INTERP)
			return ELF_WANTS_INTERPRETER;

		if (ph->type != PT_LOAD || ph->memsz == 0)
			continue;

		if (count == ELF_SEGMENTS_MAX)
			return ELF_TOO_MANY_SEGMENTS;

		if (ph->filesz > ph->memsz)
			return ELF_SEGMENT_OUTSIDE;

		if (ph->memsz > ELF_IMAGE_MAX)
			return ELF_SEGMENT_TOO_BIG;

		if (!inside_file(ph->offset, ph->filesz, len))
			return ELF_SEGMENT_OUTSIDE;

		if (!inside_user(ph->vaddr, ph->memsz))
			return ELF_SEGMENT_NOT_USER;

		/* A page is copied whole, so the byte the segment starts at
		 * must sit at the same place within a page in the file as it
		 * does in memory. Anything else needs a partial-page copy that
		 * nothing here asks for, and doing it wrong shifts a program's
		 * code by a few bytes -- which runs, for a while. */
		if ((ph->vaddr & (PAGE_SIZE - 1)) != (ph->offset & (PAGE_SIZE - 1)))
			return ELF_SEGMENT_UNALIGNED;

		if ((ph->flags & PF_W) && (ph->flags & PF_X))
			return ELF_WRITABLE_AND_EXEC;

		s = &segs[count];
		s->file_offset = ph->offset;
		s->file_bytes  = ph->filesz;
		s->va_exact    = ph->vaddr;
		s->va_first    = page_down(ph->vaddr);
		s->va_last     = page_up(ph->vaddr + ph->memsz);
		s->flags       = ph->flags;

		/* Pairwise, because the list is at most eight long and a
		 * sorted sweep would need the file to be sorted -- which is
		 * one more thing to be true of an input. */
		for (j = 0; j < count; j++)
			if (s->va_first < segs[j].va_last &&
			    segs[j].va_first < s->va_last)
				return ELF_SEGMENT_OVERLAP;

		count++;
	}

	if (count == 0)
		return ELF_NO_SEGMENTS;

	/* The entry has to be somewhere the processor may execute. A file that
	 * points it into its own writable data is asking the kernel to start it
	 * on a page it can rewrite. */
	{
		bool ok = false;

		for (i = 0; i < count; i++)
			if ((segs[i].flags & PF_X) &&
			    eh->entry >= segs[i].va_exact &&
			    eh->entry < segs[i].va_last) {
				ok = true;
				break;
			}

		if (!ok)
			return ELF_ENTRY_NOT_IN_CODE;
	}

	*count_out = count;
	*entry_out = eh->entry;
	return ELF_OK;
}

/* --- pass two: put it there ----------------------------------------------
 *
 * Page at a time, and each page is filled before it is mapped. Filling it
 * afterwards would mean writing through the program's own mapping, which is
 * read-only for its code -- so the kernel would either have to map it writable
 * first and change its mind, or write through a second mapping. Filling the
 * page while it is still only the kernel's is simpler and has no window where
 * a program's code is writable.
 */
static enum elf_result place(struct addrspace *as, const void *image,
			     const struct segment *segs, unsigned count)
{
	unsigned i;

	for (i = 0; i < count; i++) {
		const struct segment *s = &segs[i];
		unsigned flags = vm_flags_of(s->flags);
		u64 va;

		for (va = s->va_first; va < s->va_last; va += PAGE_SIZE) {
			paddr_t page = pmm_alloc_page();
			u8 *at;

			if (!page)
				return ELF_NO_MEMORY;

			at = phys_to_virt(page);

			/* Cleared first, and every page. What is not in the
			 * file is the program's .bss, which it is entitled to
			 * find zero -- and what is in neither is a page that
			 * would otherwise arrive holding whatever the last
			 * owner of it wrote. */
			kmemset(at, 0, PAGE_SIZE);

			/* The part of this page the file has bytes for, as an
			 * intersection rather than a special case per page:
			 * the first page of a segment may start part way in,
			 * the last may end part way through, and a one-page
			 * segment is both at once. */
			{
				u64 want_from = s->va_exact;
				u64 want_to   = s->va_exact + s->file_bytes;
				u64 from = want_from > va ? want_from : va;
				u64 to   = want_to < va + PAGE_SIZE
					   ? want_to : va + PAGE_SIZE;

				if (to > from)
					kmemcpy(at + (from - va),
						(const u8 *)image +
						s->file_offset +
						(from - s->va_exact),
						(size_t)(to - from));
			}

			if (!addrspace_map(as, va, page, PAGE_SIZE, flags)) {
				pmm_free_page(page);
				return ELF_NO_MEMORY;
			}
		}
	}

	return ELF_OK;
}

enum elf_result elf_load(struct addrspace *as, const void *image, u64 len,
			 u64 *entry_out)
{
	struct segment segs[ELF_SEGMENTS_MAX];
	unsigned count = 0;
	u64 entry = 0;
	enum elf_result r;

	if (!as || !image)
		return ELF_TOO_SMALL;

	r = inspect(image, len, segs, &count, &entry);
	if (r != ELF_OK)
		return r;

	r = place(as, image, segs, count);
	if (r != ELF_OK)
		return r;

	if (entry_out)
		*entry_out = entry;

	return ELF_OK;
}

/* --- the self-test -------------------------------------------------------
 *
 * The interesting property is not that a good file loads. That is proved every
 * boot by a program printing a line and reporting on its own segments, which is
 * a far better test than anything in here.
 *
 * What this proves is the other half: that each refusal is *reachable*, and
 * that each one is reached by the fault it is for. A loader that answered
 * "refused" to everything would pass a test that only asked whether it
 * complained -- which is the same mistake the FAT32 reader's damage tests were
 * written to avoid, and it is worth making again here because the cost of
 * getting it wrong is a program that runs when it should not.
 *
 * So each case takes a *copy of the real file*, changes one field, and requires
 * the specific refusal for that field. Then the last case loads the copy with
 * nothing changed and requires it to be accepted -- without that one, a loader
 * that had simply stopped working would pass every case above it.
 *
 * And after every refusal the address space is required to be *empty*. Refusing
 * a file half way through mapping it leaves a space holding part of a program,
 * and the caller's response to a refusal is to throw the space away -- so a
 * partial map is a program that could be run by something that did not throw it
 * away carefully enough.
 */

/* Offsets into the ELF header, by name, so the damage below reads as what it
 * is rather than as arithmetic. */
#define EH_CLASS    4
#define EH_TYPE     16
#define EH_MACHINE  18
#define EH_ENTRY    24
#define EH_PHOFF    32
#define EH_PHENTSZ  54
#define EH_PHNUM    56

struct damage {
	const char      *what;		/* said aloud when it fails */
	enum elf_result  expect;
	u64              at;		/* byte offset to change, or 0 for none */
	unsigned         width;		/* 1, 2, 4 or 8 */
	u64              to;
	u64              shorten_to;	/* 0 to keep the whole file */
};

static void poke(u8 *p, u64 at, unsigned width, u64 value)
{
	unsigned i;

	for (i = 0; i < width; i++)
		p[at + i] = (u8)(value >> (8 * i));
}

/* Where the first and second program headers live in this particular file.
 * Read out of the header rather than assumed, because the linker decides them
 * and a hard-coded 64 would quietly stop pointing at a program header the day
 * it decided otherwise. */
static u64 phdr_at(const u8 *image, unsigned index)
{
	const struct elf64_header *eh = (const struct elf64_header *)image;

	return eh->phoff + (u64)index * sizeof(struct elf64_phdr);
}

bool elf_self_test(void)
{
	const u64 len = user_elf_image_len;
	u8 *copy = kmalloc((size_t)len);
	bool ok = true;
	unsigned i;

	/* Offsets within a program header. */
	const u64 p0 = phdr_at(user_elf_image, 0);
	const u64 p1 = phdr_at(user_elf_image, 1);
	const u64 PH_FLAGS = 4, PH_OFFSET = 8, PH_VADDR = 16, PH_FILESZ = 32;

	const struct damage cases[] = {
	  { "a file with no ELF magic",          ELF_NOT_AN_ELF,
	    0,             4, 0x11223344, 0 },
	  { "a 32-bit ELF",                      ELF_WRONG_CLASS,
	    EH_CLASS,      1, 1, 0 },
	  { "an ELF for another architecture",   ELF_WRONG_MACHINE,
	    EH_MACHINE,    2, 0x00F3, 0 },
	  { "a shared object rather than an executable", ELF_NOT_EXECUTABLE,
	    EH_TYPE,       2, 3, 0 },
	  { "program headers outside the file",  ELF_BAD_PHDRS,
	    EH_PHOFF,      8, 0xFFFFFFF0ULL, 0 },
	  { "a program header size the loader does not use", ELF_BAD_PHDRS,
	    EH_PHENTSZ,    2, 32, 0 },
	  { "no program headers at all",         ELF_BAD_PHDRS,
	    EH_PHNUM,      2, 0, 0 },
	  { "a segment whose bytes run past the end of the file",
	    ELF_SEGMENT_OUTSIDE,
	    p0 + PH_FILESZ, 8, 0x100000, 0 },
	  { "a segment whose offset plus length wraps around",
	    ELF_SEGMENT_OUTSIDE,
	    p0 + PH_OFFSET, 8, 0xFFFFFFFFFFFFF000ULL, 0 },
	  { "a segment that wants to live in the kernel's half",
	    ELF_SEGMENT_NOT_USER,
	    p0 + PH_VADDR, 8, 0xFFFFFFFF80000000ULL, 0 },
	  { "a segment that wants the never-mapped first page",
	    ELF_SEGMENT_NOT_USER,
	    p0 + PH_VADDR, 8, 0, 0 },
	  { "a segment whose address and file offset disagree about a page",
	    ELF_SEGMENT_UNALIGNED,
	    p0 + PH_VADDR, 8, 0x400008, 0 },
	  { "a segment that is writable and executable",
	    ELF_WRITABLE_AND_EXEC,
	    p1 + PH_FLAGS, 4, 7, 0 },
	  { "two segments claiming the same page",
	    ELF_SEGMENT_OVERLAP,
	    p1 + PH_VADDR, 8, 0x400000, 0 },
	  { "an entry point that is not in anything executable",
	    ELF_ENTRY_NOT_IN_CODE,
	    EH_ENTRY,      8, 0x401000, 0 },
	  { "a file too short to hold a header", ELF_TOO_SMALL,
	    0,             0, 0, 8 },

	  /* The control. Without it every case above passes on a loader that
	   * has stopped accepting anything at all. */
	  { "the file with nothing changed",     ELF_OK,
	    0,             0, 0, 0 },
	};

	if (!copy) {
		kputs("  elf: no memory to make a copy of the program\n");
		return false;
	}

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		const struct damage *d = &cases[i];
		struct addrspace *as = addrspace_create();
		u64 use = d->shorten_to ? d->shorten_to : len;
		enum elf_result got;

		if (!as) {
			kputs("  elf: no address space to test loading into\n");
			ok = false;
			break;
		}

			kmemcpy(copy, user_elf_image, (size_t)len);

		/* The offsets are read out of the file, so a damaged one could
		 * put this write past the copy -- which would corrupt the heap
		 * and be blamed on whatever used it next. Bounded here rather
		 * than trusted, in the one place in this file that writes. */
		if (d->width) {
			if (d->at + d->width > len) {
				kprintf("  elf: the case \"%s\" would write "
					"past the end of the copy\n", d->what);
				ok = false;
				addrspace_release(as);
				continue;
			}
			poke(copy, d->at, d->width, d->to);
		}

		got = elf_load(as, copy, use, NULL);

		if (got != d->expect) {
			kprintf("  elf: %s should have been \"%s\" and was "
				"\"%s\"\n", d->what, elf_why(d->expect),
				elf_why(got));
			ok = false;
		}

		/* Nothing may be left behind by a refusal. */
		if (got != ELF_OK && as->mapped_bytes != 0) {
			kprintf("  elf: %s was refused and still mapped %lu "
				"bytes\n", d->what,
				(unsigned long)as->mapped_bytes);
			ok = false;
		}

		addrspace_release(as);
	}

	kfree(copy);
	return ok;
}
