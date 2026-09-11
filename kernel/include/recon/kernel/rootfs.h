/* The one filesystem the kernel keeps mounted, and creating a file on it with
 * its permissions already set.
 *
 * Asked for by the desktop, at the top of docs/KERNEL-WANTS.md, and it is first
 * on that list because of what the gap costs rather than how hard it is:
 *
 *     recon_fs_write creates a file and writes it. There is no way to say what
 *     the file's mode should be, so the private key is written and *then*
 *     tightened with chmod. Between those two calls it is a private key
 *     readable by anything on the machine.
 *
 * The window is the whole bug. It is small, it is real, and every secret the
 * system writes from here on has it -- so it gets worse rather than better.
 *
 * --- Why this closes it completely rather than narrowing it ---
 *
 * ReconFS is copy-on-write and commits once. So the file is created *and*
 * filled *and* given its mode inside a single transaction, and the whole
 * transaction becomes real when one superblock write lands. There is no instant
 * at which the file exists with the wrong permissions, because there is no
 * instant at which it exists at all until it is finished.
 *
 * That is stronger than doing the two calls in the right order, and it holds
 * across a power cut: cut the machine between any two writes and the file is
 * not there. A half-made secret with open permissions is not a state this can
 * be left in.
 *
 * --- And the part that is honestly not done ---
 *
 * **Nothing enforces the mode.** It is stored, it is reported, and it survives
 * a remount -- and no code anywhere consults it before reading a file, because
 * the kernel has no idea who is asking. That needs the *next* entry on the
 * desktop's list, an identity the kernel enforces, and it is a larger piece.
 *
 * That is worth being exact about rather than letting "create with a mode"
 * imply more than it is. What this fixes is the window. A file created here has
 * the right permissions recorded from the first instant it exists, so that when
 * enforcement arrives it has something true to enforce -- and no volume written
 * in the meantime has to be gone back over.
 */
#ifndef RECON_KERNEL_ROOTFS_H
#define RECON_KERNEL_ROOTFS_H

#include <recon/kernel/reconfs.h>
#include <recon/kernel/types.h>

/* Looks for a ReconFS volume among the block devices and keeps the first one.
 * Called once at boot. A machine with none is ordinary -- the rig's disks are
 * blank and a machine booted from installation media has not got one yet -- so
 * this is not a failure, and every call below reports that there is no
 * filesystem rather than pretending. */
/* How much of a directory this kernel will read in one go. Generous enough
 * that nothing here has ever reached it, and a bound rather than no bound
 * because the alternative is an allocation sized by whatever is on the disk. */
#define RECONFS_LIST_MAX   256
#define RECONFS_LIST_BYTES 8192

void rootfs_init(void);

/* The mounted volume, or null. Callers must check: "there is no filesystem" and
 * "the operation failed" are different answers. */
struct reconfs *rootfs(void);

/* Creates `path` with `mode` and the contents given, in one commit.
 *
 * Refuses rather than truncating if the file already exists -- replacing a file
 * is a different operation with a different question attached ("did you mean to
 * lose what was there?"), and a create that silently overwrites is how a
 * key-generation routine run twice destroys the key that was working.
 *
 * The directories in the path must already exist. Making them implicitly would
 * mean a typo in a path silently building a tree.
 */
/* Replaces everything a file holds, keeping the file.
 *
 * **Separate from `rootfs_create_file`, and that is the whole point.** Create
 * refuses a name that exists, for a reason worth keeping: a create that
 * silently overwrites is how running a key-generation routine a second time
 * destroys the key that was working. This is the call that says *yes, replace
 * it*, and a caller has to mean it -- the same shape as `block_claim_raw`,
 * which describes itself as a declaration of intent to destroy a disk.
 *
 * Refused if the name does not exist. This replaces; it does not create, and a
 * caller that wanted either has to decide which.
 *
 * **The file keeps its dossier**, because it is the same file. That is what
 * makes it possible to say the contents changed and mean it -- under a block
 * number the object would simply become a different one, and nothing holding
 * the old number would ever learn anything. It is also why the page cache has
 * to be told, and this is the call that tells it.
 *
 * Whole-file, like the write underneath it: there is no way to change part of
 * a file, and under copy-on-write a partial write is barely cheaper anyway.
 */
enum reconfs_status rootfs_replace_file(const char *path, const void *data,
					u32 len);

enum reconfs_status rootfs_create_file(const char *path, u32 mode,
				       const void *data, u32 len);

/* Reads a whole file, and tells the caller the mode it was created with. Either
 * pointer may be null. */
/* The owner and group of a file, without reading it.
 *
 * Separate from rootfs_read_file because the caller that needs this needs it
 * *before* deciding whether to read at all -- asking for the contents and then
 * checking whether it was allowed to have them is not a permission check. */
/* `dossier` receives the number that names this object for as long as the
 * volume exists: allocated once, never reused, and unchanged when the object
 * moves -- which under copy-on-write it does on every write. That is what makes
 * it the only thing here a page cache can use as a key. A block number would be
 * a new key after every rewrite, which sounds like free invalidation and is
 * really a cache that never hits. */
enum reconfs_status rootfs_owner_of(const char *path, u32 *mode, u32 *uid,
				    u32 *gid, u64 *dossier);

enum reconfs_status rootfs_read_file(const char *path, void *out, u32 max,
				     u32 *got, u32 *mode);

/* Every name directly inside a directory on the mounted volume.
 *
 * `names` receives them NUL-terminated and back to back, and `*needed` the
 * bytes a whole listing takes -- whether or not it fitted. Nothing is written
 * unless all of it fits, which is `reconfs_list`'s rule carried up: half a
 * directory reported as a whole one is a caller that cannot tell.
 *
 * The permission check is the caller's, not this function's: this is the layer
 * that knows where a directory is, and identity is decided in one place. */
enum reconfs_status rootfs_list(const char *path, char *names, u64 names_len,
				u64 *needed, unsigned *count);

void rootfs_print_summary(void);
bool rootfs_self_test(void);

/* Looks again for a volume, then runs the test and prints its line. Called
 * after the filesystem battery, which on a machine with no installed volume is
 * the only thing that ever makes one. */
void rootfs_run(void);

#endif /* RECON_KERNEL_ROOTFS_H */
