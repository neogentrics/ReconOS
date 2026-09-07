/* Shared between the loader's own files, and nowhere else.
 *
 * `efi.h` is the firmware's interface and `reconboot.h` is the contract with
 * the kernel. This is neither: it is the seam inside one program, split across
 * files only because finding other operating systems is a different job from
 * loading ours, and a file that does both is a file nobody reads.
 */
#ifndef RECONBOOT_INTERNAL_H
#define RECONBOOT_INTERNAL_H

#include "efi.h"

/* Boot services, captured at entry. A pointer rather than a copy because the
 * firmware's table is the firmware's. */
extern EFI_BOOT_SERVICES *BS;
extern EFI_SYSTEM_TABLE *ST;

void print(const char *s);
void print_hex(UINT64 v);
void print_dec(UINT64 v);

/* SHA-256 of a whole message. Written out rather than borrowed -- see
 * sha256.c for why that is the safe choice for a verifier specifically. */
void sha256(const void *data, UINTN len, UINT8 out[32]);
BOOLEAN sha256_self_test(void);

/* Verifies a 256-byte PKCS#1 v1.5 signature over a SHA-256 digest, against a
 * 256-byte modulus with exponent 65537. Everything it touches is public, which
 * is why writing it out is defensible here -- see rsa.c. */
BOOLEAN rsa2048_verify(const UINT8 *modulus, const UINT8 *sig,
		       const UINT8 digest[32]);

/* Builds a device path naming a *file on a device*, by copying the device's own
 * path and appending a file node to it.
 *
 * This is what chain-loading needs and it cannot be borrowed: the firmware
 * gives us a path to the disk, and `LoadImage` wants a path to the file. The
 * two are the same chain with one more node on the end.
 *
 * The result is allocated from boot services memory and belongs to the caller.
 */
EFI_DEVICE_PATH_PROTOCOL *file_device_path(const EFI_DEVICE_PATH_PROTOCOL *dev,
					   const CHAR16 *file);

/* Looks at every filesystem the firmware can see and records the operating
 * systems found on them. `exclude` is the handle we were loaded from: offering
 * "boot the medium you are already booted from" is not a choice, it is a loop.
 *
 * Returns how many were found. */
unsigned menu_discover(EFI_HANDLE exclude);

void menu_print(void);

/* Offers the choice for `seconds`, and returns the index chosen or -1 for
 * "start ReconOS". Bounded, because a loader that waits for a keypress is a
 * machine that does not come back from a power cut. */
int menu_choose(unsigned seconds);

/* Starts one, and does not return if it works. */
BOOLEAN menu_boot(unsigned index, EFI_HANDLE self);

#endif /* RECONBOOT_INTERNAL_H */
