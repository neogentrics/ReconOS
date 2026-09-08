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

/* And the handoff, because the declarations below name its framebuffer. A
 * header that uses a type should be the one that asks for it: leaving that to
 * whoever includes this works until a new file includes it in a different
 * order, which is what happened the first time gfx.c was compiled. */
#include "reconboot.h"

/* Boot services, captured at entry. A pointer rather than a copy because the
 * firmware's table is the firmware's. */
extern EFI_BOOT_SERVICES *BS;
extern EFI_SYSTEM_TABLE *ST;

void print(const char *s);
void print_hex(UINT64 v);
void print_dec(UINT64 v);

/* SHA-256 and RSA-2048 verification live in crypto.h, because the BIOS loader
 * compiles the same two sources for real mode and needs those declarations
 * without the rest of this file. */
#include "crypto.h"

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
int menu_choose(unsigned seconds,
		const struct reconboot_framebuffer *fb);

/* Starts one, and does not return if it works. */
BOOLEAN menu_boot(unsigned index, EFI_HANDLE self);

/* Is this entry the recovery environment rather than a system on the disk?
 *
 * Recovery is this loader starting our own kernel with a different command
 * line, so it is not chain-loaded and menu_boot refuses it. The caller must ask
 * this first. */
BOOLEAN menu_is_recovery(unsigned index);

/* --- The menu, drawn ------------------------------------------------------
 *
 * An attempt, with the text menu behind it. Every reason to say no -- no
 * framebuffer, a layout we cannot name, a pitch that cannot hold a row, a
 * screen too small -- means the text menu runs instead, and none of them is an
 * error. Drawing into a framebuffer we have misread is worse than not drawing:
 * it writes past the end of every row, and on a device mapping that is somebody
 * else's registers.
 *
 * The fallback is exercised on every aarch64 boot in the matrix, because AAVMF
 * provides no framebuffer at all. */
BOOLEAN gfx_available(const struct reconboot_framebuffer *fb);
void gfx_menu_draw(unsigned count, const char *const *labels,
		   unsigned selected, unsigned seconds);
void gfx_report(BOOLEAN used);

/* How many entries the menu will hold. Shared so the graphical renderer can
 * size a screen against it without a second, drifting number. */
#define MENU_MAX 8

#endif /* RECONBOOT_INTERNAL_H */
