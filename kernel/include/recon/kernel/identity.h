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

/* --- Capabilities ---------------------------------------------------------
 *
 * The answer to the paragraph above: `UID_KERNEL` may do everything, and that
 * is a blunt instrument. A capability is one of those powers, named and held
 * separately, so that a program can be allowed to do the one privileged thing
 * it needs and nothing else.
 *
 * --- They are dropped, never gained ---
 *
 * **This is the whole of the safety property.** A process may take a
 * capability out of its own set, and there is no call that puts one back. Not
 * "no call yet" -- there is nowhere for one to go, because a set that can be
 * regained protects nothing: anything that could re-take a power is a power
 * that was never given up.
 *
 * What that buys is the thing worth having. An installer holds `CAP_RAW_DISK`
 * while it writes a partition table and drops it the moment it is done, and a
 * bug in everything it does afterwards -- a corrupted path, a wild pointer, a
 * hostile file it was asked to copy -- cannot reach a disk. The window in which
 * the power exists is the window the programmer chose, not the lifetime of the
 * program.
 *
 * --- Only the ones something needs ---
 *
 * Three, because this kernel has three privileged acts and no more. The
 * temptation is to write the list a grown system ends up with; a capability
 * nothing checks is a promise nothing keeps, and the day something does need it
 * the name will be chosen by whoever needs it rather than guessed at here.
 *
 * --- How a process gets them ---
 *
 * From its parent, never more. A process made with `UID_KERNEL` starts with all
 * three, which is what keeps every existing caller working; one made with any
 * other identity starts with none. That is deliberately the same rule as
 * before, expressed in a form that can now be narrowed -- the point is not that
 * root has fewer powers today, it is that root can *give them up* and that
 * anything checking now asks a question with a real answer.
 *
 * The kernel itself -- a thread with no process at all -- holds everything, for
 * the reason in the paragraph above: it is the thing doing the enforcing.
 */

/* Ignore the permission bits on a file. What `uid == UID_KERNEL` used to mean
 * on its own, now separable from the identity that usually carries it. */
#define CAP_FILE_OVERRIDE  (1ull << 0)

/* Claim a whole disk and write over it. `block_claim_raw` calls itself a
 * declaration of intent to destroy a disk, and until now nothing asked who was
 * declaring it. */
#define CAP_RAW_DISK       (1ull << 1)

/* Stop the machine. */
#define CAP_SHUTDOWN       (1ull << 2)

#define CAP_ALL (CAP_FILE_OVERRIDE | CAP_RAW_DISK | CAP_SHUTDOWN)

/* Whether whoever is running holds every one of `caps`.
 *
 * Every one, not any: a caller asking for two powers needs both, and being
 * handed a yes because it held one of them is how a check becomes decoration.
 */
bool capable(u64 caps);

/* Gives them up, for the life of this process. There is no matching grant --
 * see the note above, which is the reason rather than an omission. */
void capability_drop(u64 caps);

/* What the running process still holds, for the summary and for a program that
 * wants to know what it has left. */
u64 capability_held(void);

/* The name of a single capability, for saying which one was missing. Null for
 * anything that is not exactly one of them, because "CAP_RAW_DISK|CAP_SHUTDOWN"
 * is not a name and a caller printing it would be inventing one. */
const char *capability_name(u64 cap);

void identity_print_summary(void);
bool identity_self_test(void);

#endif /* RECON_KERNEL_IDENTITY_H */
