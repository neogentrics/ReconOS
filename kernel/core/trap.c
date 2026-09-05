#include <recon/kernel/trap.h>
#include <recon/kernel/console.h>

volatile bool  trap_expecting;
volatile bool  trap_caught;
volatile void *trap_recovery;

/* The address to fault on. Volatile, and read rather than written as a literal:
 * given a constant the compiler knows this is a store to nothing, says so, and
 * would be within its rights to delete it. A self-test optimised away is a
 * self-test that passes without testing. */
volatile uintptr_t trap_test_address = 16;

/* How many times the handler caught during one test. One is correct; more means
 * the resume did not resume. Without this the kernel spins forever retaking the
 * same fault, which from outside is indistinguishable from a hang with no
 * cause -- and which is exactly what it did while this was being written. */
volatile unsigned trap_catches;

bool trap_self_test(void)
{
	trap_caught = false;
	trap_catches = 0;

	if (!trap_provoke_fault())
		return false;

	if (!trap_caught) {
		kputs("  trap: the write did not fault, so page zero is mapped\n");
		return false;
	}

	if (trap_catches != 1) {
		kprintf("  trap: caught %u times for one fault -- the resume did not resume\n",
			trap_catches);
		return false;
	}

	return true;
}

void trap_note_catch(void)
{
	trap_catches++;
}
