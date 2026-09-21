/* The boot log, offered over TCP to anybody who connects.
 *
 * **Why this exists.** The kernel already writes its log to the medium it
 * booted from (`klog_save_to_medium`, KF-222), and on the one machine that
 * needed it most that failed -- because the medium is USB and USB is the
 * broken thing (KF-256). A machine whose fault prevents recording the
 * evidence of that fault is a machine you diagnose by photographing a panel,
 * and this project has now done that four times.
 *
 * The network does not depend on the disk. That is the whole idea.
 *
 * --- What it deliberately is not ------------------------------------------
 *
 * **It is not a shell, and the design is what guarantees that rather than the
 * intention.** Nothing here ever reads from the socket. There is no parser, no
 * command table, and no code path from a received byte to anything at all --
 * so there is nothing for a caller to say. A remote console is a
 * remote-code-execution service with a friendly name; this is a file being
 * read aloud.
 *
 * **It is not SSH and should not become it.** SSH is key exchange, a cipher, a
 * MAC, a packet protocol, user authentication and channel multiplexing. This
 * kernel has SHA-256 and RSA verification for signed kernels and nothing else.
 * Anything that wants the log privately can carry it over a link that is
 * already private, which on this network means a machine on the same wire.
 *
 * **It is off unless asked for, and it says so when it is on.** Off by
 * default, enabled by `logport` on the kernel command line -- the same
 * mechanism as `noinit`, which means enabling it is writing a file to the
 * medium rather than building a different kernel. And the boot report names
 * the port whenever it is listening, because a listening socket nobody
 * remembers opening is exactly the thing a kernel must not have.
 */
#ifndef RECON_KERNEL_LOGPORT_H
#define RECON_KERNEL_LOGPORT_H

#include <recon/kernel/types.h>

/* The port. Chosen rather than defaulted, and the reasoning is short:
 *
 *   **not 23.** Telnet. A port number is a promise about a protocol and that
 *   one promises a login.
 *   **not 514.** Syslog, which is a format this does not speak.
 *   **not 80.** The server role serves HTTP on a ReconOS machine already, and
 *   two things answering the same port is a fault waiting for a role change.
 *
 * 4919 is 0x1337 and means nothing, which is the point: a caller has to be
 * told, so nothing finds this by scanning for a service it recognises. */
#define LOGPORT_PORT 4919

/* Starts listening, if the command line asked for it.
 *
 * Returns false when it was not asked for -- which is the normal case and not
 * a failure -- and also when it was asked for and could not start. The boot
 * report distinguishes the two; a caller here does not need to. */
bool logport_start(void);

/* What to print in the boot report. Prints nothing at all when the port was
 * never asked for, so an ordinary boot is unchanged. */
void logport_print_summary(void);

#endif /* RECON_KERNEL_LOGPORT_H */
