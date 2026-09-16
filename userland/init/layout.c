/*
 * The shape a ReconOS volume has.
 *
 * Every path here is one the desktop already names. `include/recon_fs.h` has
 * carried these as constants since v0.1.0 -- `RECON_DIR_SYSTEM`,
 * `RECON_DIR_SYSTEM_THEMES` and the rest -- and until now there was no way to
 * make a single one of them: the installer formatted the System partition and
 * wrote nothing into it, so a freshly installed machine booted to a volume
 * with nothing at its root. The first-boot screen said so, in those words.
 *
 * They are written out here rather than included from `recon_fs.h` because
 * that header is the compositor's and reaches for `<time.h>` and `<stdbool.h>`
 * and a filesystem interface none of which exist for a program on this kernel.
 * **The duplication is the point of contact between the two halves**, so it is
 * checked rather than trusted: `recon_init_layout_tests` reads
 * `include/recon_fs.h` and requires every `RECON_DIR_` constant in it to be in
 * this list, and every entry in this list to be one of those constants.
 */

#include "layout.h"

/*
 * Parents before children, and that ordering is load-bearing rather than
 * tidy.
 *
 * `SYS_MKDIR` makes one directory and refuses if its parent is not there --
 * deliberately, because making parents implicitly means a typo in a path
 * silently builds a tree. So `/System` has to come before `/System/Apps`, and
 * the suite checks that this list satisfies that rather than leaving it to
 * whoever adds the next line.
 */
const char *const RECON_LAYOUT[] = {
	"/System",		/* ReconOS itself */
	"/System/Apps",		/* its own applications */
	"/System/Config",	/* settings */
	"/System/Themes",	/* skins */
	"/System/Icons",	/* icon sets */

	/*
	 * The machine's own fonts.
	 *
	 * There is no /usr/share/fonts here, which is the whole reason this
	 * entry exists: a desktop with no font draws nothing a person can read.
	 * A medium carries DejaVu Sans and Sans Mono and the installer writes
	 * them here.
	 */
	"/System/Fonts",
	"/System/Modules",	/* the .rex and .rts a session loads */
	"/System/Logs",		/* including the boot report */

	"/Apps",		/* installed applications, kept apart so a
				 * full disk of programs cannot stop the
				 * system booting */
	"/Users",		/* documents, including the desktop */
	"/Temp",		/* scratch, not preserved */

	(const char *)0,
};

/*
 * Every directory gets 0755: readable and enterable by anyone, writable by the
 * owner.
 *
 * Not a choice made here so much as one deferred. The kernel records a user
 * and a group on every process and enforces file permissions against them, and
 * the desktop has accounts of its own, and **the two have never been
 * introduced** -- which is its own entry in `docs/KERNEL-WANTS.md` and the
 * largest honest gap in the system. Until they are, every one of these belongs
 * to uid 0 and a mode that is anything other than "the system owns it, anyone
 * may read it" would be a claim this system cannot back.
 *
 * `/Users` and `/Temp` are the two that will change first.
 */
#define LAYOUT_MODE 0755u

int recon_layout_count(void)
{
	int n = 0;

	while (RECON_LAYOUT[n] != (const char *)0) {
		n++;
	}
	return n;
}

int recon_layout_build(recon_make_directory make, long already_there,
		       struct recon_layout_report *report)
{
	int i;
	int present = 0;

	if (report != (struct recon_layout_report *)0) {
		report->made = 0;
		report->already = 0;
		report->refused = 0;
		report->first_refused = (const char *)0;
		report->first_reason = 0;
	}
	if (make == (recon_make_directory)0) {
		return 0;
	}

	for (i = 0; RECON_LAYOUT[i] != (const char *)0; i++) {
		long answer = make(RECON_LAYOUT[i], LAYOUT_MODE);

		if (answer == 0) {
			present++;
			if (report) {
				report->made++;
			}
			continue;
		}

		/*
		 * Already there is not a failure. A second boot has to be a
		 * no-op, and a first boot that was interrupted half way has to
		 * be able to finish -- which is the same requirement and the
		 * reason this is counted rather than ignored: "9 already, 1
		 * made" is a machine that was interrupted, and that is worth
		 * being able to see.
		 */
		if (answer == already_there) {
			present++;
			if (report) {
				report->already++;
			}
			continue;
		}

		/*
		 * **Carries on rather than stopping.**
		 *
		 * The obvious thing is to give up at the first refusal, and it
		 * is wrong here: the directories are independent of each other
		 * except for the parent rule, so one that cannot be made says
		 * nothing about the next. Stopping would turn a read-only
		 * `/Users` into a machine with no `/Temp` either, and the
		 * report would name the first fault while hiding nine more.
		 *
		 * A refusal that *does* matter -- `/System` itself -- takes its
		 * children down with it anyway, because they have no parent,
		 * and the count at the end says so plainly.
		 */
		if (report) {
			report->refused++;
			if (report->first_refused == (const char *)0) {
				report->first_refused = RECON_LAYOUT[i];
				report->first_reason = answer;
			}
		}
	}

	return present;
}
