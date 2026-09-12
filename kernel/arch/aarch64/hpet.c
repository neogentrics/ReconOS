/* There is no HPET on aarch64, and there is nothing missing.
 *
 * The HPET exists on x86 because the time stamp counter is not guaranteed to
 * tick at a fixed rate -- it can change with the clock speed, and a kernel that
 * wants a dependable clock needs somewhere else to look. ARM does not have that
 * problem: the architecture defines a generic timer with a frequency the
 * hardware reports in `CNTFRQ_EL0`, and it is already what `arch_monotonic_ns`
 * reads.
 *
 * So this file is not a stub in the sense of work postponed. It is the honest
 * answer on this architecture, written out rather than left to `#ifdef` at
 * every call site -- because a caller asking "is there a better clock here"
 * deserves a reply, and a build-time conditional is not one.
 */
#include <recon/kernel/hpet.h>
#include <recon/kernel/console.h>

void hpet_init(void) { }

bool hpet_present(void) { return false; }

u64 hpet_now_ns(void) { return 0; }

u32 hpet_period_fs(void) { return 0; }

void hpet_print_summary(void)
{
	kputs("  hpet         : not an ARM device -- the generic timer is "
	      "already fixed-rate\n");
}

bool hpet_self_test(void)
{
	/* Nothing to test, and saying so beats a silent pass. The generic
	 * timer is checked by the clock tests that already exist. */
	kputs("  hpet: not an ARM device, so there is nothing to check\n");
	return true;
}
