/* Measuring what a flush is worth. See core/durability.c.
 *
 * Runs only when `durability=<device>` is on the command line, and writes to
 * every block of that device. There is no version of this that is safe to run
 * by default, which is why it is opt-in rather than a self-test.
 */
#ifndef RECON_KERNEL_DURABILITY_H
#define RECON_KERNEL_DURABILITY_H

void durability_run(void);

#endif /* RECON_KERNEL_DURABILITY_H */
