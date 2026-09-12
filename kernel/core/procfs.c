/* What the kernel knows about itself, reachable by opening a file.
 *
 * The third filesystem behind `struct file_ops`, and it is here for a reason
 * the desktop wrote down before this existed: `docs/KERNEL-WANTS.md` asks for
 * **machine facts**, and says the desktop reads them out of the host's
 * `/proc` and `/sys` today. This is the beginning of the answer, in ReconOS's
 * own terms rather than Linux's.
 *
 * --- A file here is generated, and generated *once*, at open ---
 *
 * Every entry is produced into a buffer when the file is opened, and reads are
 * served from that buffer. Not because it is simpler -- it costs an allocation
 * a read-on-demand version would not need -- but because it is the difference
 * between a snapshot and a smear.
 *
 * A program that reads `meminfo` in two goes with a page fault in between must
 * not get the first half of one machine and the second half of another. Free
 * pages move constantly; uptime moves every tick. Generating at read would hand
 * back a file that never existed at any instant, and the failure looks like
 * arithmetic that does not add up rather than like a filesystem problem.
 *
 * --- What is here, and what is deliberately not ---
 *
 *   version   what `uname` would want: the kernel, its version, the machine.
 *   uptime    seconds since boot, from the monotonic clock.
 *   meminfo   physical pages: total, free, used.
 *   cpuinfo   what checkpoint 3 asked the processor and got back.
 *   self      the process doing the reading.
 *
 * `self` is the one that earns the filesystem. Every other entry could have
 * been a system call returning a struct; `self` answers *differently depending
 * on who opened it*, which is a property a path has and a global does not.
 *
 * **No per-process directories** -- no `/proc/<id>/`. Listing every process's
 * innards by path is a design decision about what one program may learn about
 * another, and this kernel has an identity system that would have to be
 * consulted for each one. Building the directory first and the permission check
 * afterwards is how /proc became the hole it is on other systems.
 *
 * **Read-only, and creating is refused rather than ignored.** Nothing here is a
 * setting. A writable /proc is a control surface wearing a filesystem's clothes,
 * and the moment one entry accepts a write the next one is expected to.
 */
#include <recon/kernel/vfs.h>
#include <recon/kernel/user.h>
#include <recon/kernel/kstring.h>
#include <recon/kernel/heap.h>
#include <recon/kernel/pmm.h>
#include <recon/kernel/time.h>
#include <recon/kernel/cpu.h>
#include <recon/kernel/arch.h>
#include <recon/kernel/sched.h>
#include <recon/kernel/process.h>
#include <recon/kernel/console.h>
#include <recon/kernel/boot.h>

#ifndef RECONOS_KERNEL_VERSION
#define RECONOS_KERNEL_VERSION "0.0.0"
#endif

/* Generous for what is written into it, and bounded because this is a kernel.
 * A generated file that does not fit is truncated *and says so* -- see fill. */
#define PROC_MAX 1024

struct proc_file {
	char text[PROC_MAX];
	u64 len;
	u64 pos;
};

/* --- writing into one, without a printf that can run off the end ---------- */

struct out {
	char *at;
	u64 left;
	bool overflowed;
};

static void put(struct out *o, const char *s)
{
	while (*s) {
		if (!o->left) {
			o->overflowed = true;
			return;
		}
		*o->at++ = *s++;
		o->left--;
	}
}

static void put_u64(struct out *o, u64 v)
{
	char tmp[24];
	unsigned n = 0;

	if (!v) {
		put(o, "0");
		return;
	}

	while (v && n < sizeof(tmp)) {
		tmp[n++] = (char)('0' + (v % 10));
		v /= 10;
	}

	while (n) {
		char one[2];

		one[0] = tmp[--n];
		one[1] = 0;
		put(o, one);
	}
}

/* --- the entries ---------------------------------------------------------- */

static void gen_version(struct out *o)
{
	put(o, "ReconOS ");
	put(o, RECONOS_KERNEL_VERSION);
	put(o, "\narchitecture ");
	put(o, arch_name());
	put(o, "\nfirmware ");
	put(o, boot_firmware_name(boot_info()->firmware));
	put(o, "\nloader ");
	put(o, boot_info()->loader ? boot_info()->loader : "none");
	put(o, "\n");
}

static void gen_uptime(struct out *o)
{
	u64 ns = time_monotonic_ns();

	/* Seconds and milliseconds, separately, rather than a decimal this
	 * kernel would have to format. A reader wanting one number can add
	 * them; a reader wanting seconds stops at the first field. */
	put_u64(o, ns / 1000000000ull);
	put(o, " seconds\n");
	put_u64(o, (ns / 1000000ull) % 1000ull);
	put(o, " milliseconds\n");
}

static void gen_meminfo(struct out *o)
{
	u64 total = (u64)pmm_total_pages();
	u64 free = (u64)pmm_free_page_count();

	put(o, "pages total ");
	put_u64(o, total);
	put(o, "\npages free ");
	put_u64(o, free);
	put(o, "\npages used ");
	put_u64(o, total >= free ? total - free : 0);
	put(o, "\npage size ");
	put_u64(o, (u64)PAGE_SIZE);
	put(o, "\n");
}

static void gen_cpuinfo(struct out *o)
{
	struct cpu_caps c;

	kmemset(&c, 0, sizeof(c));
	arch_cpu_caps(&c);

	put(o, "vendor ");
	put(o, c.vendor[0] ? c.vendor : "unknown");
	put(o, "\ncores ");

	/* Zero means the processor declined to say, and that is not one. It is
	 * reported as what it is rather than rounded up, for the same reason
	 * the struct's own comment gives. */
	if (c.cores)
		put_u64(o, (u64)c.cores);
	else
		put(o, "not reported");

	put(o, "\nthreads ");

	if (c.threads)
		put_u64(o, (u64)c.threads);
	else
		put(o, "not reported");

	put(o, "\nextensions ");
	put(o, c.extensions[0] ? c.extensions : "none reported");
	put(o, "\n");
}

static void gen_self(struct out *o)
{
	struct thread *t = sched_current();
	struct process *p = t ? process_of(t) : NULL;

	/* Nothing is running that belongs to a process. That happens on the
	 * boot thread before the first program exists, and it is a fact rather
	 * than a failure -- so it is said rather than faked with zeroes. */
	if (!p) {
		put(o, "no process -- this was opened by the kernel itself\n");
		return;
	}

	put(o, "id ");
	put_u64(o, (u64)p->id);
	put(o, "\nparent ");
	put_u64(o, (u64)p->parent);
	put(o, "\nuid ");
	put_u64(o, (u64)p->uid);
	put(o, "\ngid ");
	put_u64(o, (u64)p->gid);
	put(o, "\nname ");
	put(o, p->name[0] ? p->name : "unnamed");
	put(o, "\n");
}

struct proc_entry {
	const char *name;
	void (*gen)(struct out *o);
};

static const struct proc_entry entries[] = {
	{ "version", gen_version },
	{ "uptime",  gen_uptime  },
	{ "meminfo", gen_meminfo },
	{ "cpuinfo", gen_cpuinfo },
	{ "self",    gen_self    },
};

#define PROC_ENTRIES (sizeof(entries) / sizeof(entries[0]))

/* --- the file ------------------------------------------------------------- */

static i64 proc_read(struct file *f, void *out, u64 len)
{
	struct proc_file *pf = f ? (struct proc_file *)f->private : NULL;
	u64 left;

	if (!pf || !out)
		return SYS_EFAULT;

	if (pf->pos >= pf->len)
		return 0;

	/* Subtraction, not addition. `pos + len` past the end of a u64 compares
	 * as comfortably inside, which is the shape every range check in this
	 * kernel is written to avoid. */
	left = pf->len - pf->pos;

	if (len > left)
		len = left;

	kmemcpy(out, pf->text + pf->pos, (size_t)len);
	pf->pos += len;

	return (i64)len;
}

static i64 proc_write(struct file *f, const void *in, u64 len)
{
	/* Refused, and refused here rather than by being absent. A file_ops
	 * with no write reports "this filesystem cannot write"; this reports
	 * "this file is not a thing you write to", which is the true sentence
	 * and the one a program can act on. */
	return SYS_EPERM;
}

static i64 proc_close(struct file *f)
{
	if (f && f->private) {
		kfree(f->private);
		f->private = NULL;
	}

	return SYS_OK;
}

static const struct file_ops proc_ops = {
	.read  = proc_read,
	.write = proc_write,
	.close = proc_close,
};

static bool is_the_root(const char *rest)
{
	return !rest || !*rest || (rest[0] == '/' && !rest[1]);
}

struct file *procfs_open(const char *rest, unsigned flags, u32 mode, i64 *error)
{
	unsigned i;

	*error = SYS_ENOENT;

	if (flags & (OPEN_CREATE | OPEN_REPLACE)) {
		*error = SYS_EPERM;
		return NULL;
	}

	if (flags & OPEN_WRITE) {
		*error = SYS_EPERM;
		return NULL;
	}

	for (i = 0; i < PROC_ENTRIES; i++) {
		const struct proc_entry *e = &entries[i];
		size_t n = kstrlen(e->name);
		struct proc_file *pf;
		struct file *f;
		struct out o;

		/* The whole name, terminator included, so "uptimes" does not
		 * match "uptime". */
		if (kstrlen(rest) != n || kmemcmp(rest, e->name, n) != 0)
			continue;

		pf = kmalloc(sizeof(*pf));

		if (!pf) {
			*error = SYS_ENOMEM;
			return NULL;
		}

		kmemset(pf, 0, sizeof(*pf));

		o.at = pf->text;
		o.left = sizeof(pf->text) - 1;
		o.overflowed = false;

		e->gen(&o);

		/* Truncated, and it says so inside the file. A generated file
		 * quietly cut short is a file whose last line is half a fact,
		 * and nothing downstream can tell that from the whole thing. */
		if (o.overflowed) {
			const char *cut = "\n-- truncated --\n";
			size_t cn = kstrlen(cut);

			if (sizeof(pf->text) > cn + 1)
				kmemcpy(pf->text + sizeof(pf->text) - 1 - cn,
					cut, cn);
		}

		pf->len = (u64)kstrlen(pf->text);
		pf->pos = 0;

		f = file_new_external(&proc_ops, flags, pf);

		if (!f) {
			kfree(pf);
			*error = SYS_ENOMEM;
			return NULL;
		}

		*error = SYS_OK;
		return f;
	}

	return NULL;
}

static void emit(const char *name, char *names, u64 names_len, u64 *needed,
		 unsigned *count)
{
	size_t n = kstrlen(name) + 1;

	if (*needed + n <= names_len && names)
		kmemcpy(names + *needed, name, n);

	*needed += n;

	if (count)
		(*count)++;
}

i64 procfs_list(const char *rest, char *names, u64 names_len, unsigned *count)
{
	u64 needed = 0;
	unsigned i;

	if (!is_the_root(rest))
		return SYS_ENOENT;

	for (i = 0; i < PROC_ENTRIES; i++)
		emit(entries[i].name, names, names_len, &needed, count);

	/* Whole or absent, never half -- the rule `reconfs_list` chose and the
	 * reason it gave. The answer is the size of the complete listing
	 * whether or not it fitted. */
	if (needed > names_len && count)
		*count = 0;

	return (i64)needed;
}

void procfs_print_summary(void)
{
	unsigned i;

	kprintf("  /proc        : ");

	for (i = 0; i < PROC_ENTRIES; i++)
		kprintf("%s%s", i ? ", " : "", entries[i].name);

	kprintf("\n");
}

/* --- the self-test --------------------------------------------------------
 *
 * Four claims, and three of them are about what this filesystem *will not* do.
 * That is deliberate: reading a generated file and finding bytes in it proves
 * very little, because a file that generated the wrong thing also has bytes in
 * it.
 *
 * The one that matters most is the snapshot. `uptime` is the only entry here
 * that is guaranteed to change between two opens, so it is what can show that
 * a file does not move under a reader -- read it in two halves with a delay
 * across the split and the second half must still belong to the first open.
 */
bool procfs_self_test(void)
{
	bool ok = true;
	i64 err = 0;
	struct file *f;
	char a[64], b[64];
	i64 n1, n2;

	/* 1. It is there, and it is not empty. */
	f = file_open_path("/proc/version", OPEN_READ, 0, &err);

	if (!f) {
		kprintf("  procfs: /proc/version would not open (%ld)\n",
			(long)err);
		return false;
	}

	n1 = f->ops->read(f, a, sizeof(a) - 1);
	file_release(f);

	if (n1 <= 0) {
		kputs("  procfs: /proc/version read back nothing\n");
		ok = false;
	}

	/* 2. A name that does not exist is refused, and refused as absent
	 *    rather than as forbidden -- the two send a caller different
	 *    places. */
	f = file_open_path("/proc/uptimes", OPEN_READ, 0, &err);

	if (f) {
		kputs("  procfs: /proc/uptimes opened, so the name match is a "
		      "prefix match\n");
		file_release(f);
		ok = false;
	} else if (err != SYS_ENOENT) {
		kprintf("  procfs: a missing name answered %ld rather than "
			"ENOENT\n", (long)err);
		ok = false;
	}

	/* 3. It cannot be written to or created. Checked at open, where a
	 *    program can still do something else about it. */
	f = file_open_path("/proc/version", OPEN_WRITE, 0, &err);

	if (f) {
		kputs("  procfs: /proc/version opened for writing\n");
		file_release(f);
		ok = false;
	}

	f = file_open_path("/proc/anything", OPEN_WRITE | OPEN_CREATE, 0600,
			   &err);

	if (f) {
		kputs("  procfs: a file was created in /proc\n");
		file_release(f);
		ok = false;
	}

	/* 4. A file does not move under a reader.
	 *
	 * Read four bytes, spin long enough that the monotonic clock has
	 * certainly advanced, then read the rest. If the content were produced
	 * per read rather than per open, the second half would come from a
	 * later uptime and the two halves would not join.
	 *
	 * Compared against the whole file read in one go from a *second* open,
	 * which is the only way to know what the join should have been. */
	f = file_open_path("/proc/uptime", OPEN_READ, 0, &err);

	if (!f) {
		kprintf("  procfs: /proc/uptime would not open (%ld)\n",
			(long)err);
		return false;
	}

	n1 = f->ops->read(f, a, 4);

	/* Wait until the clock has actually moved, rather than spinning a
	 * number of times and hoping.
	 *
	 * The first version counted iterations behind a compiler barrier,
	 * which is inline assembly -- and `core/` may not contain any, because
	 * that is the rule keeping a third architecture four files rather than
	 * a rewrite. `make check-portable` caught it, which is what it is for.
	 *
	 * Waiting on the monotonic clock is better than portable: this test is
	 * *about* uptime changing between two reads, so waiting for the thing
	 * the test depends on is more honest than waiting for a proxy. A count
	 * that is enough on one machine is not enough on a faster one. */
	{
		u64 until = time_monotonic_ns() + 2000000ull;	/* 2ms */

		while (time_monotonic_ns() < until)
			sched_yield();
	}

	n2 = f->ops->read(f, a + (n1 > 0 ? n1 : 0), sizeof(a) - 1 - 4);
	file_release(f);

	if (n1 <= 0 || n2 <= 0) {
		kputs("  procfs: /proc/uptime could not be read in two "
		      "halves\n");
		ok = false;
	} else {
		u64 total = (u64)n1 + (u64)n2;

		a[total] = 0;

		/* The same file, opened again and read whole. It is a *later*
		 * snapshot, so the two are not required to be equal -- what is
		 * required is that the first one is internally consistent,
		 * which is what reading it in halves tests. A smear shows up
		 * as a length that does not match a single read of the same
		 * file. */
		f = file_open_path("/proc/uptime", OPEN_READ, 0, &err);

		if (f) {
			i64 whole = f->ops->read(f, b, sizeof(b) - 1);

			file_release(f);

			if (whole > 0) {
				b[whole] = 0;

				if ((u64)whole != total) {
					kprintf("  procfs: uptime read in two "
						"goes gave %lu bytes and read "
						"whole gave %ld -- the file "
						"moved under the reader\n",
						(unsigned long)total,
						(long)whole);
					ok = false;
				}
			}
		}
	}

	return ok;
}
