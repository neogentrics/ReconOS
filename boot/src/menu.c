/* What else is on this machine, and letting somebody choose it.
 *
 * ReconOS installs beside whatever was already there. That promise is worthless
 * if the machine then only boots ReconOS -- **installing beside Windows and
 * then being unable to reach Windows is not a partial success, it is a machine
 * somebody has lost the use of.**
 *
 * Under UEFI this is more tractable than it sounds. Another operating system's
 * loader is an ordinary `.EFI` file on an EFI System Partition, and starting it
 * is loading and running it -- the same operation the firmware performed on us.
 * No emulation, no patching, no knowledge of what the other system is.
 *
 * --- Found, not configured --------------------------------------------------
 *
 * There is no list of installed systems anywhere. A configuration file would
 * have to be kept correct as somebody installs and removes systems, and a stale
 * one is worse than none: it offers a choice that does not work, on the one
 * screen where a person has no way to investigate.
 *
 * So the disks are looked at, every time. What is there is what is offered.
 *
 * --- What it does not do ----------------------------------------------------
 *
 * It does not touch the other system's files, its boot variables, or its
 * partition. Chain-loading is the *least* invasive way to do this: the
 * alternative -- registering ourselves as the firmware's default and promising
 * to hand control on -- means editing NVRAM entries that somebody else's
 * updater also edits, and losing that argument means a machine that boots to
 * nothing.
 */
#include "efi.h"
#include "reconboot.h"
#include "boot_internal.h"

#define MENU_MAX 8

struct menu_entry {
	CHAR16 path[64];
	char label[48];
	EFI_HANDLE device;
};

static struct menu_entry entries[MENU_MAX];
static unsigned entry_count;

/* The loaders worth looking for, and what to call them.
 *
 * Named rather than discovered, because a `.EFI` file on an ESP is not
 * necessarily a bootable system -- firmware updates, diagnostic tools and
 * vendor utilities all live there, and offering somebody a menu of things that
 * are not operating systems is worse than offering a short one.
 *
 * Our own comes first, because a machine with ReconOS installed should start
 * ReconOS without anybody having to be present.
 */
static const struct {
	const CHAR16 *path;
	const char *label;
} known[] = {
	{ u"\\EFI\\ReconOS\\BOOTX64.EFI",           "ReconOS" },
	{ u"\\EFI\\Microsoft\\Boot\\bootmgfw.efi",  "Windows" },
	{ u"\\EFI\\ubuntu\\shimx64.efi",            "Ubuntu" },
	{ u"\\EFI\\ubuntu\\grubx64.efi",            "Ubuntu" },
	{ u"\\EFI\\debian\\grubx64.efi",            "Debian" },
	{ u"\\EFI\\fedora\\shimx64.efi",            "Fedora" },
	{ u"\\EFI\\arch\\grubx64.efi",              "Arch Linux" },

	/* An installed macOS, on a Mac. Apple's firmware is present in that
	 * case and boot.efi does the rest; on a PC this file is not there and
	 * nothing is offered, which is the correct outcome rather than a
	 * missing feature. See docs/KERNEL.md. */
	{ u"\\System\\Library\\CoreServices\\boot.efi", "macOS" },
};

static void copy_path(CHAR16 *dst, const CHAR16 *src, unsigned max)
{
	unsigned i = 0;

	while (src[i] && i + 1 < max) {
		dst[i] = src[i];
		i++;
	}
	dst[i] = 0;
}

static void copy_label(char *dst, const char *src, unsigned max)
{
	unsigned i = 0;

	while (src[i] && i + 1 < max) {
		dst[i] = src[i];
		i++;
	}
	dst[i] = 0;
}

/* Is this system already listed on this disk?
 *
 * The rule is one entry per *system*, not one per disk. The first version broke
 * out of the search after the first match on a volume, which reads like the
 * same rule and is not: a dual-boot machine keeps Windows and Linux on **one**
 * EFI partition, so it found Windows and stopped, and the Linux the person
 * actually wanted was never offered.
 *
 * The duplicate that needed avoiding is narrower than that. Ubuntu ships both a
 * shim and a GRUB and both open successfully; listing the same name twice
 * teaches somebody the menu is unreliable, which costs more than the missing
 * entry it was meant to prevent.
 */
static BOOLEAN already_listed(EFI_HANDLE device, const char *label)
{
	unsigned i, k;

	for (i = 0; i < entry_count; i++) {
		if (entries[i].device != device)
			continue;

		for (k = 0; entries[i].label[k] && label[k]; k++)
			if (entries[i].label[k] != label[k])
				break;

		if (!entries[i].label[k] && !label[k])
			return TRUE;
	}

	return FALSE;
}

/* Looks at every filesystem the firmware can see, for every loader we know.
 *
 * `exclude` is the handle we were loaded from -- an install medium, usually.
 * Offering "boot the stick you are already booted from" is not a choice, it is
 * a loop.
 */
unsigned menu_discover(EFI_HANDLE exclude)
{
	EFI_GUID fs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
	EFI_HANDLE *handles = 0;
	UINTN size = 0;
	EFI_STATUS s;
	UINTN count, h, k;

	entry_count = 0;

	s = BS->LocateHandle(ByProtocol, &fs_guid, 0, &size, 0);
	if (s != EFI_BUFFER_TOO_SMALL)
		return 0;

	s = BS->AllocatePool(EfiLoaderData, size, (void **)&handles);
	if (EFI_ERROR(s))
		return 0;

	s = BS->LocateHandle(ByProtocol, &fs_guid, 0, &size, handles);
	if (EFI_ERROR(s)) {
		BS->FreePool(handles);
		return 0;
	}

	count = size / sizeof(EFI_HANDLE);

	for (h = 0; h < count && entry_count < MENU_MAX; h++) {
		EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs;
		EFI_FILE_PROTOCOL *root;

		if (handles[h] == exclude)
			continue;

		if (EFI_ERROR(BS->HandleProtocol(handles[h], &fs_guid,
						 (void **)&fs)))
			continue;

		if (EFI_ERROR(fs->OpenVolume(fs, &root)))
			continue;

		for (k = 0; k < sizeof(known) / sizeof(known[0]); k++) {
			EFI_FILE_PROTOCOL *f;

			if (entry_count >= MENU_MAX)
				break;

			if (EFI_ERROR(root->Open(root, &f,
						 (CHAR16 *)known[k].path,
						 EFI_FILE_MODE_READ, 0)))
				continue;

			f->Close(f);

			if (already_listed(handles[h], known[k].label))
				continue;

			copy_path(entries[entry_count].path, known[k].path, 64);
			copy_label(entries[entry_count].label, known[k].label,
				   48);
			entries[entry_count].device = handles[h];
			entry_count++;
		}
	}

	BS->FreePool(handles);
	return entry_count;
}

void menu_print(void)
{
	unsigned i;

	if (!entry_count)
		return;

	print("\nOther systems on this machine:\n");

	for (i = 0; i < entry_count; i++) {
		print("  ");
		print_dec(i + 1);
		print(". ");
		print(entries[i].label);
		print("\n");
	}
}

/* Offers the choice, and takes the default if nobody is there.
 *
 * **The default matters more than the menu does.** A machine that waits for a
 * keypress is a machine that does not come back from a power cut, because
 * nobody was there to press anything -- and the most likely reader of this menu
 * is a machine restarting at three in the morning with nobody in the room. So
 * the wait is bounded, short, and ends in ReconOS starting.
 *
 * Any key stops the countdown; a digit chooses. Stopping on any key rather than
 * only on a digit is deliberate: somebody who reaches for the keyboard has
 * decided to intervene, and losing the menu because they pressed the wrong
 * thing first would be the sort of thing that makes people distrust it.
 *
 * Returns the index chosen, or -1 for "carry on and start ReconOS".
 */
int menu_choose(unsigned seconds)
{
	EFI_INPUT_KEY key;
	unsigned left = seconds * 10;		/* tenths */
	BOOLEAN paused = FALSE;

	if (!entry_count)
		return -1;

	/* A machine with no console at all -- a server on a serial line the
	 * firmware did not wire up, an appliance with no keyboard. There is
	 * nobody to offer a choice to, and dereferencing this to find that
	 * out would end the boot rather than the menu.
	 *
	 * The default is the right answer here rather than a fallback: a
	 * machine nobody can talk to should start the system it has. */
	if (!ST->ConIn)
		return -1;

	print("\n  ReconOS starts on its own; press a number to choose "
	      "something else\n");

	for (;;) {
		EFI_STATUS s = ST->ConIn->ReadKeyStroke(ST->ConIn, &key);

		if (!EFI_ERROR(s)) {
			if (key.UnicodeChar >= '1' &&
			    key.UnicodeChar <= '9') {
				unsigned pick = (unsigned)(key.UnicodeChar - '1');

				if (pick < entry_count)
					return (int)pick;

				/* A number nobody offered. Ignored rather than
				 * treated as the default, because acting on a
				 * key somebody pressed by mistake is worse
				 * than doing nothing. */
				continue;
			}

			if (key.UnicodeChar == '\r' || key.UnicodeChar == '\n')
				return -1;	/* Enter: get on with it */

			/* Anything else: they are here, so stop the clock and
			 * wait for them to decide. */
			if (!paused) {
				paused = TRUE;
				print("  waiting -- press a number, or Enter "
				      "for ReconOS\n");
			}
			continue;
		}

		if (paused)
			continue;

		if (!left--)
			return -1;

		BS->Stall(100000);		/* a tenth of a second */
	}
}

/* Starts one of them, and does not come back if it works.
 *
 * The other system is loaded by the firmware exactly as we were, with its own
 * device handle so that its loader finds its own files. It is given no
 * arguments: what a bootloader expects on its command line is its own business,
 * and guessing produces a system that starts strangely rather than one that
 * does not start.
 */
BOOLEAN menu_boot(unsigned index, EFI_HANDLE self)
{
	EFI_DEVICE_PATH_PROTOCOL *path;
	EFI_GUID dp_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
	EFI_HANDLE image = 0;
	EFI_STATUS s;

	if (index >= entry_count)
		return FALSE;

	s = BS->HandleProtocol(entries[index].device, &dp_guid, (void **)&path);
	if (EFI_ERROR(s))
		return FALSE;

	path = file_device_path(path, entries[index].path);
	if (!path)
		return FALSE;

	s = BS->LoadImage(FALSE, self, path, 0, 0, &image);
	if (EFI_ERROR(s))
		return FALSE;

	print("\nreconboot: starting ");
	print(entries[index].label);
	print("\n");

	s = BS->StartImage(image, 0, 0);

	/* Only reached if the other system's loader returned, which means it
	 * declined to boot. Saying so beats a blank screen: the machine is
	 * still ours and the menu is still there. */
	print("reconboot: ");
	print(entries[index].label);
	print(" returned without starting\n");

	BS->UnloadImage(image);
	return FALSE;
}
