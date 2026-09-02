/*
 * Process information from Linux's /proc. See include/recon_procinfo.h.
 */

#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <dirent.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> /* strcasecmp */
#include <unistd.h>

#include "recon_procinfo.h"

#define INITIAL_CAPACITY 256

struct recon_proc_snapshot {
    struct recon_process *processes;
    size_t count;
    size_t capacity;

    /* Previous sample, kept only to turn cumulative CPU time into a rate. */
    struct recon_process *previous;
    size_t previous_count;

    unsigned long long total_ticks_prev;
    unsigned long long idle_ticks_prev;
    double total_cpu_percent;

    size_t total_memory_kb;
    size_t used_memory_kb;

    long clock_ticks;
    long page_size_kb;
};

struct recon_proc_snapshot *recon_proc_snapshot_create(void) {
    struct recon_proc_snapshot *snapshot = calloc(1, sizeof(*snapshot));
    if (snapshot == NULL) {
        return NULL;
    }

    snapshot->processes = calloc(INITIAL_CAPACITY, sizeof(*snapshot->processes));
    if (snapshot->processes == NULL) {
        free(snapshot);
        return NULL;
    }
    snapshot->capacity = INITIAL_CAPACITY;

    snapshot->clock_ticks = sysconf(_SC_CLK_TCK);
    if (snapshot->clock_ticks <= 0) {
        snapshot->clock_ticks = 100;
    }
    snapshot->page_size_kb = sysconf(_SC_PAGESIZE) / 1024;
    if (snapshot->page_size_kb <= 0) {
        snapshot->page_size_kb = 4;
    }

    return snapshot;
}

void recon_proc_snapshot_destroy(struct recon_proc_snapshot *snapshot) {
    if (snapshot == NULL) {
        return;
    }
    free(snapshot->processes);
    free(snapshot->previous);
    free(snapshot);
}

/* Find a pid in the previous sample, so a rate can be computed. */
static const struct recon_process *find_previous(
        const struct recon_proc_snapshot *snapshot, pid_t pid) {
    for (size_t i = 0; i < snapshot->previous_count; i++) {
        if (snapshot->previous[i].pid == pid) {
            return &snapshot->previous[i];
        }
    }
    return NULL;
}

/*
 * Read one process from /proc/<pid>/stat.
 *
 * The process name sits in parentheses and may itself contain spaces or
 * parentheses, so the fields after it are found from the last ')' rather than
 * by splitting on whitespace.
 */
static bool read_process(struct recon_proc_snapshot *snapshot, pid_t pid,
        struct recon_process *out) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);

    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return false;
    }

    char line[2048];
    if (fgets(line, sizeof(line), f) == NULL) {
        fclose(f);
        return false;
    }
    fclose(f);

    char *name_start = strchr(line, '(');
    char *name_end = strrchr(line, ')');
    if (name_start == NULL || name_end == NULL || name_end <= name_start) {
        return false;
    }

    size_t name_len = (size_t)(name_end - name_start - 1);
    if (name_len >= RECON_PROC_NAME_MAX) {
        name_len = RECON_PROC_NAME_MAX - 1;
    }
    memcpy(out->name, name_start + 1, name_len);
    out->name[name_len] = '\0';

    /* Fields from 3 onwards: state, ppid, pgrp, session, tty, tpgid, flags,
     * minflt, cminflt, majflt, cmajflt, utime, stime, ... */
    char state = '?';
    unsigned long long utime = 0, stime = 0;
    long rss_pages = 0;

    int matched = sscanf(name_end + 2,
        "%c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu "
        "%*d %*d %*d %*d %*d %*d %*u %*u %ld",
        &state, &utime, &stime, &rss_pages);
    if (matched < 3) {
        return false;
    }

    out->pid = pid;
    out->state = state;
    out->cpu_ticks = utime + stime;
    out->memory_kb = rss_pages > 0
        ? (size_t)rss_pages * (size_t)snapshot->page_size_kb : 0;
    out->cpu_percent = 0.0;

    return true;
}

/* System-wide CPU busy percentage, from the aggregate line in /proc/stat. */
static void read_system_cpu(struct recon_proc_snapshot *snapshot) {
    FILE *f = fopen("/proc/stat", "r");
    if (f == NULL) {
        return;
    }

    char line[512];
    if (fgets(line, sizeof(line), f) != NULL && strncmp(line, "cpu ", 4) == 0) {
        unsigned long long values[10] = {0};
        int n = sscanf(line + 4, "%llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
            &values[0], &values[1], &values[2], &values[3], &values[4],
            &values[5], &values[6], &values[7], &values[8], &values[9]);

        unsigned long long total = 0;
        for (int i = 0; i < n; i++) {
            total += values[i];
        }
        /* Fields 4 and 5 are idle and iowait. */
        unsigned long long idle = values[3] + (n > 4 ? values[4] : 0);

        if (snapshot->total_ticks_prev != 0 && total > snapshot->total_ticks_prev) {
            unsigned long long total_delta = total - snapshot->total_ticks_prev;
            unsigned long long idle_delta = idle - snapshot->idle_ticks_prev;
            snapshot->total_cpu_percent =
                100.0 * (double)(total_delta - idle_delta) / (double)total_delta;
        }

        snapshot->total_ticks_prev = total;
        snapshot->idle_ticks_prev = idle;
    }

    fclose(f);
}

static void read_system_memory(struct recon_proc_snapshot *snapshot) {
    FILE *f = fopen("/proc/meminfo", "r");
    if (f == NULL) {
        return;
    }

    char line[256];
    size_t total = 0, available = 0;
    while (fgets(line, sizeof(line), f) != NULL) {
        unsigned long value;
        if (sscanf(line, "MemTotal: %lu kB", &value) == 1) {
            total = value;
        } else if (sscanf(line, "MemAvailable: %lu kB", &value) == 1) {
            available = value;
        }
    }
    fclose(f);

    snapshot->total_memory_kb = total;
    snapshot->used_memory_kb = total > available ? total - available : 0;
}

bool recon_proc_snapshot_refresh(struct recon_proc_snapshot *snapshot) {
    if (snapshot == NULL) {
        return false;
    }

    /* Keep the previous sample; CPU usage is a rate, not a reading. */
    free(snapshot->previous);
    snapshot->previous = snapshot->processes;
    snapshot->previous_count = snapshot->count;

    snapshot->processes = calloc(snapshot->capacity, sizeof(*snapshot->processes));
    if (snapshot->processes == NULL) {
        snapshot->processes = snapshot->previous;
        snapshot->previous = NULL;
        snapshot->previous_count = 0;
        return false;
    }
    snapshot->count = 0;

    unsigned long long elapsed_ticks = snapshot->total_ticks_prev;
    read_system_cpu(snapshot);
    elapsed_ticks = snapshot->total_ticks_prev - elapsed_ticks;

    read_system_memory(snapshot);

    DIR *proc = opendir("/proc");
    if (proc == NULL) {
        return false;
    }

    struct dirent *entry;
    while ((entry = readdir(proc)) != NULL) {
        if (!isdigit((unsigned char)entry->d_name[0])) {
            continue;
        }

        if (snapshot->count >= snapshot->capacity) {
            size_t capacity = snapshot->capacity * 2;
            struct recon_process *grown =
                realloc(snapshot->processes, capacity * sizeof(*grown));
            if (grown == NULL) {
                break;
            }
            snapshot->processes = grown;
            snapshot->capacity = capacity;
        }

        pid_t pid = (pid_t)atoi(entry->d_name);
        struct recon_process *out = &snapshot->processes[snapshot->count];
        memset(out, 0, sizeof(*out));

        if (!read_process(snapshot, pid, out)) {
            continue;
        }

        /* Turn cumulative CPU time into a share of the elapsed period. */
        const struct recon_process *prev = find_previous(snapshot, pid);
        if (prev != NULL && elapsed_ticks > 0 && out->cpu_ticks >= prev->cpu_ticks) {
            unsigned long long delta = out->cpu_ticks - prev->cpu_ticks;
            out->cpu_percent = 100.0 * (double)delta / (double)elapsed_ticks;
        }

        snapshot->count++;
    }

    closedir(proc);
    return true;
}

size_t recon_proc_count(const struct recon_proc_snapshot *snapshot) {
    return snapshot != NULL ? snapshot->count : 0;
}

const struct recon_process *recon_proc_at(const struct recon_proc_snapshot *snapshot,
        size_t index) {
    if (snapshot == NULL || index >= snapshot->count) {
        return NULL;
    }
    return &snapshot->processes[index];
}

double recon_proc_total_cpu_percent(const struct recon_proc_snapshot *snapshot) {
    return snapshot != NULL ? snapshot->total_cpu_percent : 0.0;
}

size_t recon_proc_total_memory_kb(const struct recon_proc_snapshot *snapshot) {
    return snapshot != NULL ? snapshot->total_memory_kb : 0;
}

size_t recon_proc_used_memory_kb(const struct recon_proc_snapshot *snapshot) {
    return snapshot != NULL ? snapshot->used_memory_kb : 0;
}

/* --- Sorting --- */

static int compare_cpu(const void *a, const void *b) {
    const struct recon_process *pa = a, *pb = b;
    if (pa->cpu_percent < pb->cpu_percent) return 1;
    if (pa->cpu_percent > pb->cpu_percent) return -1;
    /* Equal usage is common, especially at idle; fall back to memory so the
     * order does not shuffle between refreshes. */
    if (pa->memory_kb < pb->memory_kb) return 1;
    if (pa->memory_kb > pb->memory_kb) return -1;
    return (pa->pid > pb->pid) - (pa->pid < pb->pid);
}

static int compare_memory(const void *a, const void *b) {
    const struct recon_process *pa = a, *pb = b;
    if (pa->memory_kb < pb->memory_kb) return 1;
    if (pa->memory_kb > pb->memory_kb) return -1;
    return (pa->pid > pb->pid) - (pa->pid < pb->pid);
}

static int compare_name(const void *a, const void *b) {
    const struct recon_process *pa = a, *pb = b;
    int result = strcasecmp(pa->name, pb->name);
    if (result != 0) {
        return result;
    }
    return (pa->pid > pb->pid) - (pa->pid < pb->pid);
}

void recon_proc_sort_by_cpu(struct recon_proc_snapshot *snapshot) {
    if (snapshot != NULL) {
        qsort(snapshot->processes, snapshot->count,
            sizeof(*snapshot->processes), compare_cpu);
    }
}

void recon_proc_sort_by_memory(struct recon_proc_snapshot *snapshot) {
    if (snapshot != NULL) {
        qsort(snapshot->processes, snapshot->count,
            sizeof(*snapshot->processes), compare_memory);
    }
}

void recon_proc_sort_by_name(struct recon_proc_snapshot *snapshot) {
    if (snapshot != NULL) {
        qsort(snapshot->processes, snapshot->count,
            sizeof(*snapshot->processes), compare_name);
    }
}

/* --- Ending processes --- */

bool recon_proc_terminate(pid_t pid) {
    if (pid <= 1) {
        /* Refuse pid 1: on Linux that is init, and killing it takes the
         * machine down. */
        return false;
    }
    return kill(pid, SIGTERM) == 0;
}

bool recon_proc_kill(pid_t pid) {
    if (pid <= 1) {
        return false;
    }
    return kill(pid, SIGKILL) == 0;
}
