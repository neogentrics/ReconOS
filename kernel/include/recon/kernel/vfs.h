/* An open thing, and a small number that names it.
 *
 * Until now the kernel had files and no way to *hold* one. `rootfs_create_file`
 * takes a path and a whole file's contents and returns; `rootfs_read_file`
 * takes a path and gives back the whole file. Both are complete operations on a
 * name. Neither is a file you can keep, read a bit of, write a bit more to, and
 * hand to something else.
 *
 * That gap is why five separate rows of the architecture audit have said "waits
 * on: descriptors". Pipes are two ends of something held open. A program loaded
 * from a volume is a file read a piece at a time. File-backed mapping is a file
 * held while its pages are faulted in. None of them is about ReconFS.
 *
 * --- What makes this a VFS and not ReconFS wearing a hat ---
 *
 * Nothing above this line names a filesystem. A `struct file` is a pointer to a
 * table of four functions and some private state, and the three things that
 * implement it today -- the console, a file on the mounted volume, and a pipe --
 * have nothing in common except that table. The console has no filesystem
 * underneath it at all, which is the point: if the interface only fitted
 * ReconFS it would not fit the console either.
 *
 * The test for that is not that it compiles. It is that `sys_read` and
 * `sys_write` contain no `if` about what kind of thing they are talking to.
 *
 * --- What it deliberately is not, yet ---
 *
 * There is no path resolution here beyond handing the string to the mounted
 * volume, no mount table, no dentry cache, and no inode shared between two open
 * files of the same name. Two opens of one path produce two independent files,
 * and a write through one is not visible through the other until both are
 * closed. That is stated rather than discovered, and it is the next thing to
 * fix -- but it needs a filesystem that can read at an offset, which ReconFS
 * cannot yet.
 */
#ifndef RECON_KERNEL_VFS_H
#define RECON_KERNEL_VFS_H

#include <recon/kernel/types.h>
#include <recon/kernel/process.h>

struct file;
struct process;

/* The longest path this layer will carry. Long enough for the trees the
 * installer makes, and a fixed size on purpose: a path is copied out of a
 * program at a length the program chose, and a cap checked once is easier
 * to be sure of than a length carried through four functions. */
#define VFS_PATH_MAX 128

/* How a program asks to open something. Bit flags, because a path can be
 * opened for more than one of them at once. */
#define OPEN_READ	(1u << 0)
#define OPEN_WRITE	(1u << 1)
#define OPEN_CREATE	(1u << 2)	/* it must not already exist */

/* The opposite, and a separate flag rather than a relaxation of the one above.
 *
 * `OPEN_CREATE` means *it must not already exist*, and that is a promise worth
 * keeping: a create that silently overwrites is how running a key-generation
 * routine a second time destroys the key that was working. Making it replace
 * when the name happens to be taken would turn a guarantee into a default, and
 * the test that noticed is the one asserting the guarantee.
 *
 * So replacing is its own request. The file must already exist -- this is not
 * "create or replace", because a caller that would accept either has to decide
 * which it meant. */
#define OPEN_REPLACE	(1u << 3)	/* it must exist, and its contents go */

/* Where a seek is measured from. */
#define SEEK_START	0
#define SEEK_HERE	1
#define SEEK_END	2

/* The whole interface. Four functions, and a file that cannot do one of them
 * leaves it null rather than providing a stub that returns an error -- so
 * "cannot read" is answered in one place instead of in every implementation.
 *
 * Byte counts are returned as i64: negative is one of the SYS_E codes, and the
 * error travels back to the program unchanged rather than being flattened into
 * "it did not work".
 */
struct file_ops {
	/* Reads into `out`, from the file's own position, and advances it.
	 * Returns 0 at the end. Null if this thing cannot be read. */
	i64 (*read)(struct file *f, void *out, u64 len);

	/* Writes from `in`, at the file's own position, and advances it.
	 * Null if this thing cannot be written. */
	i64 (*write)(struct file *f, const void *in, u64 len);

	/* Moves the position. Null for a thing with no position at all -- a
	 * console, a pipe -- which is a different answer from "seek failed".
	 * Returns the new position. */
	i64 (*seek)(struct file *f, i64 offset, unsigned from);

	/* The last reference has gone.
	 *
	 * **This is where a buffered write reports failure**, and callers must
	 * not treat it as a formality. A file being written to the mounted
	 * volume holds its contents in memory until it is closed, so the commit
	 * -- and every way a commit can fail -- happens here and nowhere
	 * earlier. A close whose result is thrown away is a write whose result
	 * was thrown away.
	 */
	i64 (*close)(struct file *f);

	/* What this file *is*, as a number two different opens agree on, or
	 * zero for "cannot say".
	 *
	 * Only the page cache asks, and only so that two mappings of one file
	 * share its pages instead of each reading their own copy. A pointer
	 * will not do: open twice and there are two of them.
	 *
	 * Zero is the honest answer for most things here and is not a failure
	 * -- a console has no identity, a pipe has no contents to share, and a
	 * filesystem whose names move underneath it should say zero rather
	 * than a number that will later mean something else. Saying zero costs
	 * a private page per fault, which is what every mapping used to get.
	 */
	u64 (*identity)(struct file *f);

	/* Puts `len` bytes back at `offset`, without a position and without a
	 * later commit.
	 *
	 * Its **presence is the promise** that a write through a shared mapping
	 * reaches the file, and that is why it is a separate operation rather
	 * than a use of `write`: every filesystem here has `write`, and on the
	 * volume a write is buffered until the close, where the commit answers
	 * ERR_EXISTS and changes nothing. A shared mapping there would take
	 * writes and lose them, which is worse than not offering one.
	 *
	 * So a filesystem that cannot keep the promise does not implement this,
	 * and a shared writable mapping of its files is refused rather than
	 * accepted and quietly dropped. */
	i64 (*write_at)(struct file *f, u64 offset, const void *in, u64 len);


	/* For the summary and for tests: what kind of thing this is. Not used
	 * to decide anything. */
	const char *name;
};

struct file {
	const struct file_ops *ops;

	/* How many descriptors, in however many processes, point at this. A
	 * file outlives the descriptor that made it whenever a second one is
	 * taken -- which is what makes a pipe possible and what dup will
	 * mean. */
	unsigned refs;

	unsigned flags;		/* the OPEN_ bits it was opened with */
	u64 pos;

	void *private;		/* the implementation's own state */
};

/* --- holding one --------------------------------------------------------- */

/* Takes a reference. Returns the same file, for use in an expression. */
struct file *file_hold(struct file *f);

/* Drops one. The last drop calls `close` and frees it, and **returns what close
 * returned** -- which for a buffered write is the only report there is. */
i64 file_release(struct file *f);

/* --- the descriptor table ------------------------------------------------ */

/* Per process, and shared by its threads: two threads of one program that both
 * read descriptor 3 mean the same open file, which is the whole reason a
 * descriptor belongs to a process rather than to a thread.
 *
 * PROCESS_FDS_MAX is stated in process.h, which is where the array is. */

/* Installs `f` at the lowest free number and takes a reference. Returns the
 * number, or a negative error. On failure the caller still owns `f`. */
int fd_install(struct process *p, struct file *f);

/* The file a number names, with a reference taken, or null. The reference is
 * what makes it safe to use after the table lock is dropped -- another thread
 * closing that descriptor cannot free it underneath. Release it when done. */
struct file *fd_get(struct process *p, int fd);

/* Removes a number from the table and drops its reference. Returns what the
 * close returned, or SYS_EINVAL if the number named nothing. */
i64 fd_close(struct process *p, int fd);

/* Every descriptor of a process that has ended. Called once, by whoever notices
 * the last thread has gone. */
void fd_close_all(struct process *p);

/* Gives a new process its first three descriptors: nothing to read on 0, the
 * console on 1 and 2. */
void fd_open_standard(struct process *p);

/* --- what can be opened -------------------------------------------------- */

/* The console, as a file. No filesystem underneath it, which is why it is the
 * proof that this interface is not shaped around one. */
struct file *file_open_console(void);

/* A path on the mounted volume. Returns null and sets `*error` to a SYS_E code,
 * so "there is no filesystem" stays distinguishable from "no such file". */
struct file *file_open_path(const char *path, unsigned flags, u32 mode,
			    i64 *error);

/* Counted where the system calls are, because that is where a read means a
 * program asked for one -- the implementations read each other constantly. */
/* Builds a file around an implementation the VFS does not itself provide.
 *
 * Exported so that a pipe -- and whatever comes after it -- lives in its own
 * file rather than inside this one. An interface whose implementations all have
 * to be written in the same source file as the interface is not open.
 *
 * The file starts with one reference, which the caller owns. */
struct file *file_new_external(const struct file_ops *ops, unsigned flags,
			       void *private);

/* Both ends of a pipe. Either both come back or neither does. */
bool pipe_create(struct file **read_end, struct file **write_end);
void pipe_print_summary(void);
bool pipe_self_test(void);

/* --- listing ---------------------------------------------------------------
 *
 * Every name directly inside a directory, written into `names` NUL-terminated
 * and back to back.
 *
 * **Whole listing or nothing**, which is the shape `reconfs_list` already chose
 * and for the reason it gives: a caller handed the first half of a directory
 * with a success status has no way to know. So the return is always the number
 * of bytes a complete listing needs, and the caller compares it against the
 * buffer it offered -- more than that means nothing was written and the answer
 * is how much to come back with. Asking with a length of zero is therefore how
 * a caller finds out the size, and costs nothing but the walk.
 *
 * Negative is an error, by the same convention as everything else here.
 *
 * --- Names, and not yet anything else ---
 *
 * Not the kind, not the size, not the owner -- and that is a decision rather
 * than an oversight. `reconfs_list` is the call with the guarantee that matters
 * (a listing taken in one step cannot observe a change part-way through) and it
 * reports names and inode blocks. Types live in the directory entries it walks
 * past. Getting them out means either widening that call or reading each inode
 * afterwards, and the second gives up exactly the property that made the first
 * one worth using.
 *
 * A kind byte per entry is the obvious next field and the first caller that
 * needs to tell a folder from a file is what should decide its shape. */
i64 file_list_path(const char *path, char *names, u64 names_len,
		   unsigned *count);

void vfs_note_read(void);
void vfs_note_write(void);

/* The devices filesystem: /dev, with no disk under it. Its own file because a
 * second implementation written inside the first one proves nothing. */
struct file *devfs_open(const char *rest, unsigned flags, u32 mode, i64 *error);

/* /proc -- what the kernel knows about itself, reachable by path.
 *
 * Every entry is generated at open and read from that snapshot, so a file read
 * in two goes cannot be half of one machine and half of another. Read-only:
 * nothing here is a setting. */
struct file *procfs_open(const char *rest, unsigned flags, u32 mode,
			 i64 *error);
i64 procfs_list(const char *rest, char *names, u64 names_len,
		unsigned *count);
void procfs_print_summary(void);
bool procfs_self_test(void);
i64 devfs_list(const char *rest, char *names, u64 names_len,
		unsigned *count);
void devfs_print_summary(void);

/* Files that live in memory, mounted at /tmp. A flat namespace and no
 * removal, both stated in ramfs.c rather than discovered. */
struct file *ramfs_open(const char *rest, unsigned flags, u32 mode,
			i64 *error);
i64 ramfs_list(const char *rest, char *names, u64 names_len,
		unsigned *count);
void ramfs_print_summary(void);
bool ramfs_self_test(void);
bool devfs_self_test(void);

void vfs_print_summary(void);
bool vfs_self_test(void);

/* That a listing is whole or absent, and never half. */
bool vfs_list_self_test(void);

/* The same interface over the mounted volume. Separate because it needs a
 * formatted disk, which most machines in the rig do not have until the
 * filesystem battery has made one. */
bool vfs_file_self_test(void);

/* A mapping whose pages are filled from a file. Needs a volume for the same
 * reason the row above does. */
bool vfs_mmap_self_test(void);

#endif /* RECON_KERNEL_VFS_H */
