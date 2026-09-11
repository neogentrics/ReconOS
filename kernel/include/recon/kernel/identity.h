/* Who is asking, and whether they may.
 *
 * Files have carried a mode, a uid and a gid since the filesystem was written,
 * and **nothing has ever read them**. The audit has said so in the same words
 * for as long as it has existed, and the desktop's own notes call it the
 * largest honest gap in the system: a standard account cannot install a program
 * because the Control Panel declines to, not because anything stops it.
 *
 * This is the thing that stops it.
 *
 * --- The policy is here, and only here ---
 *
 * Every filesystem knows facts this file does not -- where an inode lives, what
 * a device node means, whether a pipe has ends. None of them decides *policy*.
 * They supply the mode, the owner and the group; the answer comes from here.
 *
 * That is not tidiness. A decision made in three filesystems is three
 * decisions, and the day they disagree is the day a file is readable through
 * one path and not another. The VFS already refuses to let `sys_read` contain
 * an `if` about what kind of thing it is talking to, and this is the same line
 * drawn one level up.
 *
 * --- The first matching class decides ---
 *
 * This is the part with a wrong answer that looks obviously right.
 *
 * A mode has three sets of bits: owner, group, other. The tempting reading is
 * "may they do it under *any* of the classes they belong to" -- an or. That is
 * wrong, and it is wrong in the direction that grants access.
 *
 * Mode `0004` on a file you own says: **the owner may not read it**, and
 * everybody else may. The digits are owner, group, other in that order, so
 * this one is `0` for the owner and `4` for everybody else -- a strange thing
 * to write, a perfectly legal one, and how a file is hidden from its owner
 * while staying readable to a service. Under an or, the owner reads it: the
 * exact opposite of what the mode says.
 *
 * So the classes are tried in order and the first one that *matches* answers,
 * whether the answer is yes or no. A later class cannot grant what an earlier
 * one refused.
 *
 * --- The kernel is not subject to this ---
 *
 * A thread with no process is the kernel itself, and it is allowed everything.
 *
 * Not an oversight and not a shortcut: the check exists to constrain
 * *programs*, and the kernel is the thing enforcing it. A kernel that had to
 * ask permission to read its own filesystem would need an identity of its own
 * to ask with, which is a second account that can do everything -- the thing
 * this is supposed to prevent, wearing a hat.
 *
 * `UID_KERNEL` is zero and is allowed everything for the same reason every
 * system that has tried the alternative has gone back: without it, one wrong
 * mode locks the machine out of its own files with no way back in. That is a
 * real cost and it is written down rather than hidden -- a *capability* system
 * is the answer to it, and that is the next row on this page.
 *
 * --- What is not checked yet ---
 *
 * **Directories.** On a system with directory permissions, reaching
 * `/a/b/file` requires the right to traverse `/a` and `/a/b`. Nothing here
 * checks that, so a file is reachable by its path whatever the directories
 * above it say. Said out loud because a reader would reasonably assume
 * otherwise, and because it is the half that makes a permission system
 * airtight rather than advisory.
 *
 * **set-user-id**, and the sticky bit. Both are ways of saying "run as somebody
 * else", and neither has anything to run yet.
 */
#ifndef RECON_KERNEL_IDENTITY_H
#define RECON_KERNEL_IDENTITY_H

#include <recon/kernel/types.h>

/* The permission bits, named. Octal, because that is how a mode is written
 * everywhere else and translating in your head at each site is how one gets
 * typed wrong. */
#define PERM_OTHER_EXEC   0001
#define PERM_OTHER_WRITE  0002
#define PERM_OTHER_READ   0004
#define PERM_GROUP_EXEC   0010
#define PERM_GROUP_WRITE  0020
#define PERM_GROUP_READ   0040
#define PERM_OWNER_EXEC   0100
#define PERM_OWNER_WRITE  0200
#define PERM_OWNER_READ   0400

/* What is being asked for. Separate from the OPEN_ flags because those are
 * about a descriptor and these are about an act -- and because a caller that
 * is not opening anything, like an exec, still has to ask. */
enum access_want {
	ACCESS_READ  = 1,
	ACCESS_WRITE = 2,
	ACCESS_EXEC  = 4,
};

/* Whoever is running on this processor right now.
 *
 * `UID_KERNEL` when there is no process -- a kernel thread, or anything running
 * before the first program exists. See the note above about why that is allowed
 * everything. */
u32 identity_uid(void);
u32 identity_gid(void);

/* Whether the current identity may do `want` to a thing owned by `owner` and
 * `group` with permission bits `mode`.
 *
 * The first matching class answers. Read the note above before changing this:
 * the obvious simplification is an or across the classes, and it grants access
 * the mode refuses. */
bool identity_may(u32 mode, u32 owner, u32 group, enum access_want want);

/* The same question asked on behalf of a named identity rather than the running
 * one. For tests, and for anything that later has to answer "could *they*" --
 * which is what a file manager showing a padlock is asking. */
bool identity_may_as(u32 uid, u32 gid, u32 mode, u32 owner, u32 group,
		     enum access_want want);

/* What the OPEN_ flags of a request amount to, as an act. A request to open for
 * both reading and writing needs both, and a request for neither still needs to
 * be able to look at the thing. */
enum access_want identity_want_from_open(unsigned open_flags);

/* Opens a real file as somebody who is not the kernel, and says whether it was
 * refused. Does nothing when there is no filesystem, which is most machines.
 *
 * Not a self-test, because it needs something the self-tests do not have: a
 * mounted volume, which is made later in the boot. */
void identity_run(void);

void identity_print_summary(void);
bool identity_self_test(void);

#endif /* RECON_KERNEL_IDENTITY_H */
