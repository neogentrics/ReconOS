/*
 * Running process information.
 *
 * The only place in ReconOS that asks the kernel what is running. Today that
 * means reading Linux's /proc; when ReconOS has its own kernel, this is the
 * file that changes and nothing above it needs to.
 */

#ifndef RECON_PROCINFO_H
#define RECON_PROCINFO_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#define RECON_PROC_NAME_MAX 64

struct recon_process {
    pid_t pid;
    char name[RECON_PROC_NAME_MAX];
    char state; /* R running, S sleeping, D uninterruptible, Z zombie, T stopped */

    /*
     * A kernel worker rather than a program. ReconOS neither started these nor
     * can do anything useful with them, so the task manager hides them by
     * default.
     */
    bool kernel_thread;

    /* Session id. Clients ReconOS launched inherit its session, which is how
     * "started from this desktop" is told apart from the rest of the system. */
    int session;

    size_t memory_kb; /* resident set size */

    /* Share of one CPU since the previous sample, 0-100 per core. Zero on the
     * first sample, because a rate needs two readings to exist. */
    double cpu_percent;

    /* Cumulative CPU time, kept to compute the rate on the next sample. */
    unsigned long long cpu_ticks;
};

struct recon_proc_snapshot;

struct recon_proc_snapshot *recon_proc_snapshot_create(void);
void recon_proc_snapshot_destroy(struct recon_proc_snapshot *snapshot);

/*
 * Re-read the process table. CPU percentages are measured against the previous
 * call, so the interval between calls sets the averaging window.
 */
bool recon_proc_snapshot_refresh(struct recon_proc_snapshot *snapshot);

size_t recon_proc_count(const struct recon_proc_snapshot *snapshot);
const struct recon_process *recon_proc_at(const struct recon_proc_snapshot *snapshot,
    size_t index);

/* Sort in place. Descending, so the heaviest processes come first. */
void recon_proc_sort_by_cpu(struct recon_proc_snapshot *snapshot);
void recon_proc_sort_by_memory(struct recon_proc_snapshot *snapshot);
void recon_proc_sort_by_name(struct recon_proc_snapshot *snapshot);

/* System-wide totals from the most recent refresh. */
double recon_proc_total_cpu_percent(const struct recon_proc_snapshot *snapshot);
size_t recon_proc_total_memory_kb(const struct recon_proc_snapshot *snapshot);
size_t recon_proc_used_memory_kb(const struct recon_proc_snapshot *snapshot);

/*
 * Ask a process to exit, or force it to.
 *
 * Polite first: a terminate request lets a program save and close cleanly.
 * Forcing gives it no such chance, so it is a separate, deliberate call.
 */
bool recon_proc_terminate(pid_t pid);

/* --- The machine --- */

/*
 * What the processor calls itself, and how many of it there are.
 *
 * Read from the host, which is where it comes from until ReconOS has a kernel
 * of its own to ask. Said plainly on the page that shows it: this is the
 * machine ReconOS is running on, and reporting it as though ReconOS had found
 * it itself would be a claim about hardware nothing here has touched.
 *
 * Returns false when there is nothing to read, and leaves `out` empty.
 */
bool recon_proc_cpu_name(char *out, size_t size);
int recon_proc_cpu_cores(void);
bool recon_proc_kill(pid_t pid);

#endif
