/* Time, and the first interrupt the kernel actually services.
 *
 * --- Two clocks, because they answer different questions ---
 *
 * MONOTONIC never goes backwards and has no relationship to the calendar. It is
 * what durations are measured with, what timeouts are compared against, and
 * what a scheduler runs on. It starts at zero when the kernel starts.
 *
 * WALL CLOCK is what a person reads. It jumps -- at boot when a time source is
 * first believed, when somebody corrects it, when a network says otherwise --
 * and anything that measures an interval with it will eventually measure a
 * negative one.
 *
 * Confusing the two is a classic and expensive fault, which is why they are two
 * functions with two names rather than one with a flag. The desktop already
 * needs both: its clock shows wall time, and its network reachability probe
 * times out on a duration.
 *
 * --- And a tick ---
 *
 * The first hardware interrupt the kernel arranges, handles, and acknowledges.
 * Everything about preemption depends on it: without a periodic interrupt, a
 * program that does not yield runs forever.
 *
 * It is also the first place the project's anti-bloat claim is testable rather
 * than argued. A tick that fires a hundred times a second on an idle machine is
 * a hundred wakeups a second doing nothing, and that shows up as a laptop that
 * runs warm. What is here is a fixed tick, which is the simple correct thing;
 * making it stop when there is nothing to wake for belongs with the scheduler,
 * and is noted in docs/KERNEL.md rather than forgotten.
 */
#ifndef RECON_KERNEL_TIME_H
#define RECON_KERNEL_TIME_H

#include <recon/kernel/types.h>

#define TIME_TICK_HZ 100

void time_init(void);

/* Nanoseconds since the kernel started. Never decreases. */
u64 time_monotonic_ns(void);

/* Nanoseconds since 1970-01-01 UTC, or 0 if no source of the date was found --
 * which is a real answer on a machine with no clock, and better than a
 * plausible wrong one. */
u64 time_wall_ns(void);

/* How many times the tick has fired. */
u64 time_ticks(void);

/* Called by the architecture from its timer interrupt. */
void time_tick(void);

void time_print_summary(void);
bool time_self_test(void);

#endif /* RECON_KERNEL_TIME_H */
