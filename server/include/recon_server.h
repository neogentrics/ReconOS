/*
 * The server role, and the names its parallels take.
 *
 * `docs/ROLES.md` sets the goal this header serves: on first boot a machine
 * looks at the network for another ReconOS in the same role, and finding one
 * offers to become a **parallel** of it -- clone the configuration, adapt it
 * to the hardware actually present. A parallel of `M16` calls itself `M17`.
 *
 * --- Why the naming is a module and not four lines at the call site ---
 *
 * Because it is the part that runs unattended. The clone happens on a machine
 * with no screen yet, answering a question the installer asked once, and the
 * name it picks is the name that machine answers to for the rest of its life.
 * A name chosen wrong is not a cosmetic fault: two machines that both believe
 * they are `M17` are two machines the network cannot tell apart.
 *
 * So every way of getting it wrong is refused here rather than guessed at,
 * and each refusal has a number a caller can report. The list is short on
 * purpose -- a name either belongs to a countable family or it does not.
 */

#ifndef RECON_SERVER_H
#define RECON_SERVER_H

#include <stddef.h>

/* The longest machine name this role will accept, including its terminator.
 *
 * 64 rather than the 253 a DNS name may reach, because this is a *host* name
 * and not a fully qualified one -- the label limit is 63 octets, and a name
 * that cannot be a single label cannot be the left-most part of the record
 * this machine will later publish for itself. Taking the DNS limit here would
 * accept names that DNS will refuse later, which is the same fault one layer
 * further on. */
#define RECON_NAME_MAX 64

/* How far `server_name_next_parallel` will count before it gives up.
 *
 * A bound rather than a loop to exhaustion: the caller hands in the names it
 * saw on the network, and a sweep that found a thousand consecutive members
 * of one family has found something other than a network worth joining. */
#define RECON_PARALLEL_SEARCH_MAX 1024

/* Why a name was refused. Zero is success, negative is a reason, and the
 * numbers are stable because they are reported by a machine with no screen. */
#define SERVER_OK            0
#define SERVER_ETOOLONG    (-1)	/* the name does not fit RECON_NAME_MAX */
#define SERVER_EEMPTY      (-2)	/* the name is empty */
#define SERVER_ECHAR       (-3)	/* a character a host name may not carry */
#define SERVER_EUNNUMBERED (-4)	/* it ends in no digit, so it names no family */
#define SERVER_EALLDIGITS  (-5)	/* it is only digits, so it has no family stem */
#define SERVER_ERANGE      (-6)	/* the next number does not fit */
#define SERVER_EEXHAUSTED  (-7)	/* every candidate in range is already taken */

/*
 * A name split into the parts a parallel needs.
 *
 * `digits` is kept because it is the difference between `srv008` and `srv8`.
 * A machine cloning `srv007` joins a family that writes its numbers three
 * wide, and a parallel that drops the padding has renamed the family rather
 * than joined it. The width is a floor and never a ceiling -- `M99` numbers
 * on to `M100` rather than refusing or wrapping to `M00`.
 */
struct name_family {
	char     stem[RECON_NAME_MAX];	/* "M" from "M16"; "srv" from "srv007" */
	unsigned number;		/* 16; 7 */
	unsigned digits;		/* 2; 3 -- as written, including padding */
};

/* Split a name into its family and its number.
 *
 * Refuses a name that names no family: `gateway` ends in no digit, and there
 * is no next `gateway`. That is not a failure of this function -- it is the
 * honest answer, and the caller's job is to ask the operator for a name
 * rather than to invent `gateway2`. */
int server_name_split(const char *name, struct name_family *into);

/* Write `family` numbered `number` into `into`.
 *
 * Refuses rather than truncates. A name cut short is a name that belongs to a
 * different machine. */
int server_name_format(const struct name_family *family, unsigned number,
                       char *into, size_t room);

/* The name a parallel of `peer` should take, given what is already on the wire.
 *
 * `taken` is every name discovery saw, `peer` included; `count` is how many.
 * The first number above the peer's that nobody answers to is the answer, so
 * a wire holding `M16` and `M17` gives `M18` rather than a collision.
 *
 * Comparison is case-insensitive, because a host name is, and a wire holding
 * `m17` must not hand out `M17`. */
int server_name_next_parallel(const char *peer,
                              const char *const *taken, size_t count,
                              char *into, size_t room);

#endif
