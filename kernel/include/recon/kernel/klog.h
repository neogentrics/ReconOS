/* Everything the kernel has said, kept.
 *
 * Output goes straight out and is gone. That is fine while somebody is watching
 * a serial port and useless afterwards: a fault that scrolled past, a message
 * from before a reset, anything a program wants to read back about the machine
 * it is running on. The audit has called this out as a gap since diagnostics
 * were first assessed, and it is the half of that row which is cheap.
 *
 * --- Where it hooks, and why there ---
 *
 * `kputc` is the one place every character goes through: the serial port and the
 * framebuffer are both reached from it, and every other printing function ends
 * up there. So the ring is written there and nothing has to remember to log.
 *
 * It takes no lock, and that is the property that makes this safe rather than a
 * new way to hang. `console.c` deliberately locks whole strings rather than
 * characters, and `panic` bypasses the lock entirely -- because a fault taken
 * while the console lock is held would turn a report into a hang. A ring that
 * took a lock in kputc would reintroduce exactly that, on the path a dying
 * machine uses.
 *
 * So the ring is written without one. Two processors printing at once can
 * interleave their characters in it, in the same way and for the same reason
 * they can interleave on the console. A garbled line can still be read; a
 * deadlock cannot.
 *
 * --- What it is not ---
 *
 * Not persistent. It is memory, so it goes when the power does -- reading it
 * back after a *reset* needs somewhere on a disk to put it, which is a different
 * piece of work with a different failure mode. This survives a panic, which is
 * the case that matters most often.
 *
 * Not filtered. There are no levels, no categories and no way to turn parts of
 * it off. Every one of those is a decision about what somebody will want to read
 * during an incident, made in advance, by somebody who is not them.
 */
#ifndef RECON_KERNEL_KLOG_H
#define RECON_KERNEL_KLOG_H

#include <recon/kernel/types.h>

/* Big enough to hold a whole boot's output on this kernel, which is a few
 * hundred lines. A visible number: a ring that silently wrapped in the middle of
 * the thing being investigated would be worse than no ring, because the reader
 * would not know it had. */
#define KLOG_BYTES (64u * 1024u)

/* One character. Called from kputc, and from nowhere else. */
void klog_putc(char c);

/* Copies the most recent `max` bytes out, oldest first, and returns how many.
 *
 * Oldest-first because that is reading order. A caller wanting only the end
 * takes the end of what it gets. */
u32 klog_read(void *out, u32 max);

/* How many bytes are held, and whether anything has been lost to the wrap.
 *
 * The second is the one that matters: a reader who does not know the log wrapped
 * will read the surviving half as though it were the whole story. */
u32 klog_held(void);
bool klog_wrapped(void);

void klog_print_summary(void);
bool klog_self_test(void);

#endif /* RECON_KERNEL_KLOG_H */
