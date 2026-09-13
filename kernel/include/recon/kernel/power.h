/* Turning the machine off, and saying why when it cannot be.
 *
 * See power.c: it needs the FADT for where, the DSDT for what, and the
 * architecture for how -- three separate things, any of which a given machine
 * may not provide. Each of those is a different answer and they are not
 * collapsed into one failure, because they send whoever reads them somewhere
 * different.
 */
#ifndef RECON_KERNEL_POWER_H
#define RECON_KERNEL_POWER_H

#include <recon/kernel/types.h>

enum power_result {
	POWER_OK = 0,			/* never returned; the machine is off */
	POWER_NO_REGISTER,		/* the FADT named no control register */
	POWER_NO_SLEEP_STATE,		/* no _S5 in the machine's description */
	POWER_UNSUPPORTED,		/* this architecture cannot reach it */
	POWER_REFUSED,			/* it was told, and did not */
};

/* Wait for something to happen, with this processor's tick suspended where the
 * machine can do that.
 *
 * The idle loops call this instead of arch_wait_for_interrupt. It works out how
 * long there is until the next filed timer, bounds that, and hands it to the
 * architecture -- then catches the timer wheel up on the way back, because the
 * wheel is turned by the tick and the tick was off. */
void power_idle_wait(void);


/* How many times the tick was actually suspended. Zero on a machine that
 * cannot do it, which is a real answer rather than a failure. */
u64 power_idle_tickless(void);

bool power_idle_self_test(void);

/* Does not return on a machine that obeys. */
enum power_result power_off(void);

/* The same, with the reason printed. */
void power_off_or_say_why(void);

#endif /* RECON_KERNEL_POWER_H */
