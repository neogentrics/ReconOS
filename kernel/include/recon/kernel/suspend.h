/* Suspend: stopping the machine in a way it can come back from.
 *
 * Checkpoint 24's other half. The tick half made an idle processor stop waking
 * up; this is the machine stopping altogether, with the memory kept alive and
 * everything else switched off.
 *
 * --- Why this is a registry and not a function ------------------------------
 *
 * Turning the machine off is one write to one register. Suspending it is not,
 * and the difference is the only thing that matters here: **every device loses
 * power, and every one of them has to be able to come back.** A disk controller
 * resumed into an unknown state is a kernel that reads the wrong blocks; a
 * network card is a machine that looks alive and answers nothing.
 *
 * So the interesting question is never "can we write the sleep value" -- the
 * machine declares that and this kernel can read it. It is "what is in this
 * machine that would not survive", and that is a question about what happens to
 * be plugged in, which nothing can answer at compile time.
 *
 * --- The rule this is built on ---------------------------------------------
 *
 * **A driver that has not said it can come back is assumed not to be able to,
 * and is named.** Not assumed fine, not skipped quietly. A suspend that returns
 * to a machine whose xHCI controller is in whatever state firmware left it is
 * worse than a machine that stayed awake, because the second one works.
 *
 * That is why `suspend_declare` takes an `ops` that may be null. Null is the
 * honest state for most drivers today, it is the default, and it is *visible*:
 * the boot summary lists what would not come back, and `suspend_to_ram` refuses
 * while that list is not empty.
 *
 * --- Order, and the part that is easy to get wrong --------------------------
 *
 * Quiesced in reverse order of declaration and resumed in declaration order,
 * like a stack, because a block device declared after the controller it sits on
 * has to stop first and start last.
 *
 * **And a quiesce that fails partway must put back what it already stopped.**
 * A refused suspend that leaves half the machine stopped is far worse than one
 * that never started -- the caller is told "no" and the disk is off. That
 * unwinding is the substance of this file and is what its self-test spends its
 * time on.
 */
#ifndef RECON_KERNEL_SUSPEND_H
#define RECON_KERNEL_SUSPEND_H

#include <recon/kernel/types.h>

/* How many devices may declare themselves. A visible cap rather than a list
 * that grows to whatever a bad bus walk asks for -- the same reasoning as the
 * block table's. */
#define SUSPEND_MAX 32

struct suspend_ops {
	/* Stop it: finish or abandon what is outstanding, mask its interrupt,
	 * and leave it somewhere `resume` can bring it back from.
	 *
	 * **False refuses the whole suspend**, and everything already quiesced
	 * is resumed before the refusal is returned. A driver that cannot stop
	 * safely right now -- a write in flight it cannot abandon -- says so
	 * here and the machine stays awake, which is the right outcome. */
	bool (*quiesce)(void);

	/* Bring it back, on a machine where firmware has just re-initialised
	 * everything it felt like and left the rest.
	 *
	 * False means the machine woke *without* this device. That is reported
	 * and never hidden: a resumed kernel that quietly lost its disk is the
	 * failure this whole arrangement exists to prevent. */
	bool (*resume)(void);
};

/* A device that is present and will have to survive a suspend.
 *
 * Called by the driver when it takes the device on, not at build time, because
 * what is in the machine is not known until the bus is walked.
 *
 * `ops` may be null, and for most drivers today it is. That is not a gap in
 * this interface -- it is the gap being *recorded*, so that the list of things
 * that cannot come back is generated from the machine rather than remembered
 * by a person.
 */
void suspend_declare(const char *name, const struct suspend_ops *ops);

/* Whether every declared device can come back.
 *
 * False while anything declared has no ops. `why` is filled with the names that
 * cannot, so the refusal says which device rather than that something did. */
bool suspend_ready(char *why, u64 len);

enum suspend_result {
	SUSPEND_OK = 0,		/* it slept, and this is the far side */
	SUSPEND_NOT_DECLARED,	/* a device here cannot come back -- see suspend_ready */
	SUSPEND_NO_STATE,	/* the machine declares no _S3 */
	SUSPEND_NO_REGISTER,	/* the FADT named no control register */
	SUSPEND_UNSUPPORTED,	/* this architecture has no way to reach it */
	SUSPEND_REFUSED,	/* a driver would not stop; nothing was changed */
	SUSPEND_NO_WAKE,	/* nothing could have woken it, so it was not tried */
};

/* Stop the machine, keeping memory alive.
 *
 * Returns on the far side of a successful suspend, which is the one thing that
 * makes this different from `power_off` -- and why every device in the machine
 * has an opinion about it. */
enum suspend_result suspend_to_ram(void);

/* What the boot summary says: how many declared, and which cannot come back. */
void suspend_print_summary(void);

bool suspend_self_test(void);

#endif /* RECON_KERNEL_SUSPEND_H */
