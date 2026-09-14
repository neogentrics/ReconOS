/*
 * The shape a ReconOS volume has, and laying it down.
 *
 * --- Why this is in userland and not in the kernel ---
 *
 * The kernel knows about files and directories. It does not know what a theme
 * is, or that settings live apart from icons, or that a user's documents are
 * kept away from an application's. Putting `/System/Themes` in `core/` would
 * be the kernel holding an opinion about the desktop, which is the line this
 * project draws everywhere else -- the same line that keeps a partition table
 * in the kernel and what a partition *means* out of it.
 *
 * So the first program lays out the volume. That is what an init does.
 *
 * --- Why it takes a function rather than calling one ---
 *
 * `recon_layout_build` is handed the thing that makes a directory. On the
 * machine that is `SYS_MKDIR`; in the suite it is a fake that records what it
 * was asked for and can be told to fail. So the *decisions* -- what gets made,
 * in what order, what counts as already done, what counts as a refusal worth
 * stopping for -- are checked on the host in a millisecond, with no volume, no
 * kernel and no disk.
 *
 * Same split as `screen.c`, for the same reason: the part that can be wrong is
 * the part that should be cheap to run.
 */

#ifndef RECON_INIT_LAYOUT_H
#define RECON_INIT_LAYOUT_H

/*
 * Makes one directory. Answers 0, or a negative reason.
 *
 * The one reason that is not a failure is "it is already there", which the
 * caller below has to know about by value rather than by name -- so it is
 * passed in too, rather than this file reaching for a kernel header it has no
 * other use for.
 */
typedef long (*recon_make_directory)(const char *path, unsigned long mode);

struct recon_layout_report {
	int made;		/* directories that did not exist and now do */
	int already;		/* directories that were already there */
	int refused;		/* directories that could not be made */

	/* The first refusal, kept so a caller can say which one rather than
	 * only how many. A count with no name is a number somebody has to go
	 * and find the meaning of. */
	const char *first_refused;
	long first_reason;
};

/*
 * Lay the whole layout down, parents before children.
 *
 * `already_there` is the value `make` answers when the directory exists --
 * SYS_EEXIST on the machine. It is **not** counted as a refusal: a second boot
 * has to be a no-op, and a first boot that half-completed has to be able to
 * finish.
 *
 * Returns the number of directories that are now present, made or otherwise,
 * which is what a caller wants to compare against the number it expected.
 */
int recon_layout_build(recon_make_directory make, long already_there,
		       struct recon_layout_report *report);

/* The layout itself, in the order it has to be made, ending with a null.
 * Exposed so the suite can check the ordering rather than infer it from
 * behaviour. */
extern const char *const RECON_LAYOUT[];

/* And how many there are, so a caller can say "eleven of eleven" without
 * walking the list twice. */
int recon_layout_count(void);

#endif /* RECON_INIT_LAYOUT_H */
