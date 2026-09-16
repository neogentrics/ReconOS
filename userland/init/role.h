/*
 * Which of the five systems this program is the first program of.
 *
 * --- why this file exists ------------------------------------------------
 *
 * `recon_init.c` was written as *the* first program ReconOS runs, and from
 * inside this half of the project that is what it looks like. Under
 * `docs/ROLES.md` it is not: a role is chosen at first boot -- server,
 * firewall, workstation, thin client, NAS -- out of one install, one kernel and
 * one image, and the kernel starts whichever first program that role names.
 * This is the workstation's.
 *
 * The kernel session raised it while there was still almost nothing built on
 * the assumption, which is the cheap moment to raise it: *"a rename is cheap
 * today and a structural change in a month."* They are right, and this is the
 * half of it that can be done without breaking a machine that boots.
 *
 * --- what is deliberately NOT renamed yet --------------------------------
 *
 * The installed path is still `/System/init.elf`, and the medium still carries
 * `/reconos/init.elf`.
 *
 * **That name is a contract between two halves and only one of them is here.**
 * `scripts/make-medium.sh` and `scripts/install-then-boot-test.sh` write it;
 * the kernel's loader reads it, falls back to its built-in copy, and says which
 * it used. Renaming the writing side alone produces an installed disk that
 * boots the copy inside the kernel while reporting that it did -- which is not
 * a failure anybody would see, and is the worst kind.
 *
 * So the rename is the kernel session's to lead and mine to follow in the same
 * hour. `kernel/Makefile` names the sources under `../userland/init/`
 * directly, so the directory is theirs to move too.
 *
 * What this file does today is make the program stop calling itself *the*
 * system, so that nothing further gets built on the assumption that there is
 * exactly one.
 */

#ifndef RECON_ROLE_H_INCLUDED
#define RECON_ROLE_H_INCLUDED

/*
 * The five, as docs/ROLES.md has them.
 *
 * Listed rather than left implicit because the point of the exercise is that
 * a reader of this program can see it is one of several. Four of them have no
 * first program written yet, and saying so here is more honest than a comment
 * somewhere that this is "the" init.
 */
#define RECON_ROLE_SERVER       "server"
#define RECON_ROLE_FIREWALL     "firewall"
#define RECON_ROLE_WORKSTATION  "workstation"
#define RECON_ROLE_THIN_CLIENT  "thin client"
#define RECON_ROLE_NAS          "nas"

/*
 * What this program is.
 *
 * A constant rather than something read at run time, because it is not a
 * decision this program makes -- the role is chosen at first boot, remembered
 * on the EFI partition, and applied by the kernel deciding which program to
 * start. By the time this one is running the question is already answered, and
 * the answer is the reason it was the one that ran.
 */
#define RECON_ROLE RECON_ROLE_WORKSTATION

#endif /* RECON_ROLE_H_INCLUDED */
