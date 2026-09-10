/* A process: who is running, as opposed to what is running.
 *
 * A thread is a thing the scheduler runs. A process is a thing that *owns*
 * threads, has an identity, and can be held responsible -- and the kernel has
 * had the first and not the second since checkpoint 10.
 *
 * That absence is felt above the kernel rather than in it. The desktop's own
 * notes call the missing piece "the largest honest gap in the system": ReconOS
 * has accounts and roles and an administrator check, and every one of them is
 * enforced by ReconOS asking itself. A standard account cannot install a
 * program because the Control Panel declines to, not because anything stops it.
 * Nothing can stop it while the kernel has no idea who is asking.
 *
 * --- What this is ---
 *
 * The identity: a process table, a number that is never reused and never zero,
 * a parent, a user and a group, the threads that belong to it, and an exit
 * status somebody can collect. `SYS_GETPID` answers with a process rather than
 * a thread, which is what it always meant -- a thread number would be a
 * different answer for each thread of one program.
 *
 * And, since 9 September 2026, **an address space of its own**. See
 * addrspace.h. Two programs run at once with the same addresses meaning
 * different memory, which is the thing this file could not say before.
 *
 * --- A correction, kept because the mistake is the useful part ---
 *
 * This header used to end by explaining why an address space was hard: that on
 * x86_64 the kernel's identity mapping of its own image shared the first
 * page-map entry with everything user space uses, so giving each process its
 * own low half meant first deciding *where user space lives*.
 *
 * There is no such mapping. Both architectures tear the identity map down when
 * they build the real tables and say so in a comment on the line that does it.
 * The claim came from two stale comments elsewhere in `arch/x86_64/vm.c` and
 * was copied forward -- into this header, a board, an audit and a commit
 * message -- without once being checked against the code that builds the map.
 *
 * What was actually in the user half was one page of processor bring-up
 * scaffolding per architecture, and both are gone. The kernel now *reports*
 * what it owns down there on every boot rather than being reasoned about:
 * `vm_user_half_report` prints the ranges and how many of them are the
 * kernel's. It says zero.
 *
 * --- And nothing enforces the credentials ---
 *
 * A process carries a user and a group. No code consults them. That is the same
 * honest half-step as file modes, which are stored correctly and read by
 * nobody: the point of recording them now is that when enforcement arrives it
 * has something true to enforce, and nothing created in the meantime has to be
 * gone back over.
 *
 * What is still missing is a program *from a disk*. There is no ELF loader, so
 * what runs in an address space of its own is still a byte array compiled into
 * the kernel.
 */
#ifndef RECON_KERNEL_PROCESS_H
#define RECON_KERNEL_PROCESS_H

#include <recon/kernel/types.h>

struct thread;
struct personality;
struct addrspace;

#define PROCESS_MAX      32
#define PROCESS_NAME_MAX 24

/* The one identity that means "the kernel itself". Not zero, because zero is
 * what an uninitialised field holds and a credential that defaults to the most
 * privileged value is the wrong direction to fail in. */
#define UID_KERNEL 0
#define UID_NOBODY 65534

enum process_state {
	PROCESS_FREE = 0,
	PROCESS_RUNNING,

	/* Finished, and still in the table because its exit status has not been
	 * collected. A process nobody waits for stays here, which is a leak
	 * with a name rather than a mystery. */
	PROCESS_ENDED,
};

struct process {
	enum process_state state;

	u32 id;
	u32 parent;

	/* Credentials, recorded and not yet enforced. See the header. */
	u32 uid;
	u32 gid;

	unsigned threads;		/* how many are still alive */
	i64 exit_code;

	/* What its system call numbers mean. On the process rather than on the
	 * thread, because it is a property of the *program*, and every thread
	 * of one program means the same thing by call number one. */
	const struct personality *personality;

	/* What it can see. Null for a process that runs in the kernel's own
	 * map, which is every process the kernel makes for itself.
	 *
	 * On the process and not the thread, deliberately: threads of one
	 * program share memory, and that sharing is what distinguishes a thread
	 * from a process. Putting it here makes that true by construction
	 * rather than by every caller remembering to pass the same one. */
	struct addrspace *space;

	char name[PROCESS_NAME_MAX];
};

void process_init(void);

/* Creates a process owned by `parent`, or by the kernel when parent is zero.
 * Returns null when the table is full, which is refused rather than grown. */
struct process *process_create(const char *name, u32 parent, u32 uid, u32 gid);

/* The process a thread belongs to, or null for a kernel thread that belongs to
 * none. */
struct process *process_of(const struct thread *t);

/* Attaches a thread to a process and detaches it again. The count is what
 * decides when a process has ended: a process is over when its last thread is,
 * not when any particular one is. */
void process_attach(struct process *p, struct thread *t);
void process_thread_ended(struct thread *t, i64 code);

struct process *process_by_id(u32 id);
unsigned process_count(void);
struct process *process_at(unsigned index);

/* Collects an ended process's exit status and frees its slot. Returns false if
 * there is no such process or it has not ended. */
bool process_reap(u32 id, i64 *code);

/* Gives a process an address space, and takes a reference to it. The process
 * holds that reference until it is reaped, which is why an ended process's
 * tables survive until somebody collects its status: a thread of it may still
 * be unwinding on another processor. */
void process_set_space(struct process *p, struct addrspace *as);

void process_print_summary(void);
bool process_self_test(void);

#endif /* RECON_KERNEL_PROCESS_H */
