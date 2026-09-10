/* Memory two programs can both reach. See shm.c for what this is and is not.
 *
 * The lifetime rule, stated here because it is the only thing a caller has to
 * get right: `shm_attach` takes a reference on behalf of the address space, and
 * **the caller releases it when that space is torn down**. This layer does not
 * hook itself into address-space teardown, because a region attached to two
 * spaces would then be released twice by machinery neither space knows about.
 * Whoever attached it knows when it is done.
 */
#ifndef RECON_KERNEL_SHM_H
#define RECON_KERNEL_SHM_H

#include <recon/kernel/types.h>

struct addrspace;
struct shm;

/* A visible cap rather than a list, for the same reason the region table has
 * one: a number somebody can read beats a structure that grows to whatever a
 * caller asks for. 64 pages is 256 KB. */
#define SHM_PAGES_MAX 64

/* Pages nobody else has. Null if there is no memory, or if more was asked for
 * than this will hold -- refused rather than trimmed, because a program handed
 * a smaller region than it asked for writes past the end of it. */
struct shm *shm_create(u64 bytes);

struct shm *shm_hold(struct shm *s);
void shm_release(struct shm *s);
u64 shm_size(const struct shm *s);

/* Maps every page of it into `as` at `at`.
 *
 * On failure -- which means the page tables could not be built -- the space is
 * left with part of the region mapped and **must be discarded rather than
 * reused**. There is no unmap to undo it with, and the note in shm.c says why
 * one has not been invented for a path nothing has taken. */
bool shm_attach(struct addrspace *as, vaddr_t at, struct shm *s,
		unsigned flags);

void shm_print_summary(void);
bool shm_self_test(void);

#endif /* RECON_KERNEL_SHM_H */
