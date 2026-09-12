/* The High Precision Event Timer.
 *
 * A counter in the chipset rather than in the processor core: fixed rate, the
 * same on every processor, and unaffected by power management. It exists here
 * for the machines where the time stamp counter is not invariant, which is the
 * one case `arch_monotonic_ns` cannot serve well.
 *
 * Absent on aarch64, which has a generic timer the architecture guarantees --
 * the calls are still there and answer honestly, so nothing above has to ask
 * which machine it is on.
 */
#ifndef RECON_KERNEL_HPET_H
#define RECON_KERNEL_HPET_H

#include <recon/kernel/types.h>

void hpet_init(void);

/* Whether there is one, and whether its counter was seen to move. False on a
 * machine with no HPET and on one whose HPET is mapped but dead. */
bool hpet_present(void);

/* Nanoseconds since hpet_init. Zero when there is none. */
u64 hpet_now_ns(void);

/* The period of one tick in femtoseconds, as the hardware reports it. Zero
 * when there is none. */
u32 hpet_period_fs(void);

void hpet_print_summary(void);
bool hpet_self_test(void);

#endif
