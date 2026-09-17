/*
 * The first ReconOS program written in C.
 *
 * Everything that has run in user mode on this kernel until now has been
 * assembly. The desktop is ninety thousand lines of C, so the question this
 * program exists to answer is not "can a program draw" -- checkpoint 21
 * answered that -- but **"can a program written in the language the desktop is
 * written in be compiled, loaded, and put pixels on the panel"**. If it can,
 * the distance between the desktop and this kernel stops being a category
 * difference and becomes a list of missing system calls.
 *
 * It is written to *fail* if any part of the path is broken, and to fail with
 * a different number for each part, because a test that only checks for zero
 * also passes when the program never ran.
 *
 *   - it asks the screen's size before assuming one, and refuses a kernel
 *     whose description is a different size from the structure it was built
 *     against -- which is the failure that would otherwise read every field
 *     one slot out;
 *   - it draws through `pitch` and never through `width * 4`, and puts a
 *     marker in each corner at a position computed from pitch, so a program
 *     that assumed the stride would put at least two of them in the wrong
 *     place;
 *   - it reads every marker back, because a write that goes somewhere is
 *     indistinguishable from a write that goes nowhere unless somebody looks;
 *   - it uses a local array, a loop and a struct -- the things a C compiler
 *     needs a stack and a data segment for, and the things assembly did not
 *     prove.
 */

#include <recon.h>

/* The exit codes. 55 for success, and not 0, for the reason hello.S gives:
 * the kernel reads this out of a field that is zero before the program runs
 * and zero if it never reached its exit call, so a test that treats zero as
 * success passes in both of the cases it exists to catch. */
#define OK              55
#define BAD_SCREEN_SIZE 60	/* the kernel's fb_info is not ours */
#define BAD_SCREEN_CALL 61	/* SYS_SCREEN refused */
#define BAD_OPEN        62	/* /dev/fb0 would not open */
#define BAD_MAP         63	/* SYS_MAP refused */
#define BAD_GEOMETRY    64	/* the numbers do not describe a screen */
#define BAD_READBACK    65	/* a pixel did not come back */
#define BAD_PRESENT     66	/* SYS_PRESENT refused */
#define BAD_PRESENT_REFUSAL 67	/* SYS_PRESENT accepted what it must refuse */

/*
 * The two the compiler is allowed to call without being asked.
 *
 * Even at `-ffreestanding`, GCC may turn a fill loop into `memset` and a copy
 * loop into `memcpy`. Without these the program fails to *link*, a long way
 * from the loop that caused it.
 */
void *memset(void *to, int value, unsigned long length)
{
	unsigned char *p = to;

	while (length-- > 0) {
		*p++ = (unsigned char)value;
	}
	return to;
}

void *memcpy(void *to, const void *from, unsigned long length)
{
	unsigned char *d = to;
	const unsigned char *s = from;

	while (length-- > 0) {
		*d++ = *s++;
	}
	return to;
}

/*
 * One pixel, addressed the only way that is correct on real hardware.
 *
 * `row * pitch` and not `row * width * 4`. On this emulator the two are equal,
 * which is exactly why a program that gets it wrong passes here and shears on
 * a laptop -- so the distinction is made in the one function that addresses a
 * pixel, and nothing else in this program is allowed to compute an offset.
 */
static inline u32 *at(unsigned char *base, u32 pitch, u32 x, u32 y)
{
	return (u32 *)(base + (u64)y * pitch + (u64)x * 4);
}

/* A colour, in the order the framebuffer stores one. 0x00RRGGBB is what the
 * kernel's own test writes and what the console draws with, so this program
 * uses the same and does not try to interpret `format` -- a program that
 * guessed at a format it had not been taught would draw something wrong and
 * call it a pass. */
#define RGB(r, g, b) (((u32)(r) << 16) | ((u32)(g) << 8) | (u32)(b))

int main(void)
{
	struct recon_screen screen;
	i64 needed;
	i64 fd;
	i64 mapped;
	unsigned char *fb;

	/*
	 * How big the kernel's description is, before asking for it.
	 *
	 * A length of zero is the kernel's documented way to be told the size.
	 * If it wants more room than this program set aside, the structure the
	 * two were built against differ -- and reading it anyway would give
	 * every field after the first difference a value belonging to another.
	 */
	needed = recon_screen(0, 0);
	if (needed < 0) {
		return BAD_SCREEN_CALL;
	}
	if ((u64)needed != sizeof(screen)) {
		return BAD_SCREEN_SIZE;
	}

	memset(&screen, 0, sizeof(screen));
	if (recon_screen(&screen, sizeof(screen)) < 0) {
		return BAD_SCREEN_CALL;
	}

	/*
	 * Refuse a description that does not describe a screen.
	 *
	 * `pitch` is at least four bytes a pixel or the rows overlap; `bytes`
	 * is exactly pitch times height or the program does not know where the
	 * screen ends. Both are the kernel's own promises, and a program that
	 * took them on trust and drew past the end would fault in a place that
	 * looks like the mapping being wrong.
	 */
	if (screen.width == 0 || screen.height == 0 ||
	    screen.pitch < screen.width * 4 ||
	    screen.bytes != (u64)screen.pitch * screen.height) {
		return BAD_GEOMETRY;
	}

	/*
	 * OPEN_READ | OPEN_WRITE, and both halves are needed.
	 *
	 * Read as well as write, because this program reads its own markers
	 * back -- and a mapping of a file opened write-only is a mapping it
	 * cannot check. The first version passed no flags at all, which is a
	 * request to open a file for nothing, and the kernel refused it
	 * exactly as it should have.
	 */
	fd = recon_open_path("/dev/fb0", OPEN_READ | OPEN_WRITE);
	if (fd < 0) {
		return BAD_OPEN;
	}

	mapped = recon_map((int)fd, screen.bytes);
	if (mapped < 0) {
		recon_close((int)fd);
		return BAD_MAP;
	}
	fb = (unsigned char *)(u64)mapped;

	/*
	 * --- What it draws ---
	 *
	 * Not a test pattern for its own sake. This is the desktop's own
	 * furniture at its simplest: a ground, a bar along the bottom where the
	 * taskbar lives, and a rule above it. The first time ReconOS pixels
	 * appear on ReconOS's own kernel, they may as well be ReconOS's.
	 */
	{
		const u32 ground   = RGB(0x2B, 0x33, 0x42);
		const u32 bar      = RGB(0x7A, 0x14, 0x2B);
		const u32 rule     = RGB(0xE5, 0xDF, 0xE7);
		const u32 marker   = RGB(0x00, 0xFF, 0x00);

		u32 bar_height = screen.height / 18;
		u32 x, y;

		if (bar_height < 8) {
			bar_height = 8;
		}

		for (y = 0; y < screen.height; y++) {
			u32 colour = ground;

			if (y >= screen.height - bar_height) {
				colour = bar;
			} else if (y == screen.height - bar_height - 1) {
				colour = rule;
			}

			for (x = 0; x < screen.width; x++) {
				*at(fb, screen.pitch, x, y) = colour;
			}
		}

		/*
		 * A marker in each corner, one pixel in from the edge.
		 *
		 * These are the check. Every one of them is placed through
		 * `pitch`, and the two on the right-hand side are the ones a
		 * program that assumed `width * 4` would put somewhere else --
		 * on a screen whose rows are padded, the top-right marker lands
		 * short of the edge and the bottom-right lands on a different
		 * row entirely.
		 */
		*at(fb, screen.pitch, 1, 1) = marker;
		*at(fb, screen.pitch, screen.width - 2, 1) = marker;
		*at(fb, screen.pitch, 1, screen.height - 2) = marker;
		*at(fb, screen.pitch, screen.width - 2, screen.height - 2) =
			marker;

		/*
		 * And read them back. A store to a mapping that is not really
		 * the screen succeeds exactly as one to a mapping that is, and
		 * nothing about the call would have said so.
		 *
		 * Write-combining memory is allowed to hold a store in a buffer
		 * for a while, but a read of the same address from the same
		 * processor sees it -- the buffer is not a cache that another
		 * agent can be ahead of.
		 */
		if (*at(fb, screen.pitch, 1, 1) != marker ||
		    *at(fb, screen.pitch, screen.width - 2, 1) != marker ||
		    *at(fb, screen.pitch, 1, screen.height - 2) != marker ||
		    *at(fb, screen.pitch, screen.width - 2,
			screen.height - 2) != marker) {
			recon_close((int)fd);
			return BAD_READBACK;
		}

		/* And one pixel of the ground, to prove the fill happened and
		 * not only the four stores after it. */
		if (*at(fb, screen.pitch, screen.width / 2,
			screen.height / 3) != ground) {
			recon_close((int)fd);
			return BAD_READBACK;
		}
	}

	/* **What it must refuse, asked from ring 3 through the real boundary.**
	 *
	 * Each of these is one of the four decisions the call's shape was ruled
	 * on, and each is checked here rather than in a kernel self-test because
	 * the thing worth testing is the syscall entry -- the argument order, the
	 * descriptor check and the range arithmetic -- not a static function
	 * called from inside the file that defines it.
	 */
	{
		/* A descriptor that is not the screen. 999 was never opened, so
		 * this is the ordinary bad-descriptor case. */
		if (recon_present(999, 0, 0, 1, 1) != SYS_EBADF)
			return BAD_PRESENT_REFUSAL;

		/* **A descriptor that is open and is not a framebuffer.**
		 *
		 * Descriptor 1 is the console, which this program has been
		 * writing to all along. It is a real open file with no `map`, so
		 * it is the case that separates "is this a descriptor" from "is
		 * this the screen" -- and a kernel that only checked the first
		 * would present pixels for a program that had never mapped
		 * anything. */
		if (recon_present(1, 0, 0, 1, 1) != SYS_EBADF)
			return BAD_PRESENT_REFUSAL;

		/* Zero width. There is no whole-screen sentinel precisely so
		 * that this is refused rather than silently granted the most
		 * expensive call in the interface. */
		if (recon_present((int)fd, 0, 0, 0, 1) != SYS_EINVAL)
			return BAD_PRESENT_REFUSAL;

		if (recon_present((int)fd, 0, 0, 1, 0) != SYS_EINVAL)
			return BAD_PRESENT_REFUSAL;

		/* Past the right edge, and past the bottom. Refused rather than
		 * clamped -- the same answer SYS_MAP gives a length longer than
		 * the file. */
		if (recon_present((int)fd, screen.width, 0, 1, 1) != SYS_EINVAL)
			return BAD_PRESENT_REFUSAL;

		if (recon_present((int)fd, 0, 0, screen.width + 1, 1)
		    != SYS_EINVAL)
			return BAD_PRESENT_REFUSAL;

		if (recon_present((int)fd, 0, screen.height, 1, 1) != SYS_EINVAL)
			return BAD_PRESENT_REFUSAL;

		if (recon_present((int)fd, 0, 0, 1, screen.height + 1)
		    != SYS_EINVAL)
			return BAD_PRESENT_REFUSAL;

		/* **The one that catches an addition that wraps.** A width of
		 * nearly 2^32 added to an origin of zero is past the screen; a
		 * kernel that computed `x + w` in a narrow type and compared the
		 * result would find it comfortably inside. */
		if (recon_present((int)fd, 1, 0, 0xFFFFFFFFu, 1) != SYS_EINVAL)
			return BAD_PRESENT_REFUSAL;

		/* And one that must be *accepted*, so the refusals above are
		 * known to be refusing something rather than everything. */
		if (recon_present((int)fd, 0, 0, 1, 1) != SYS_OK)
			return BAD_PRESENT;
	}

	/* **And ask for it to be shown, which is not the same as drawing it.**
	 *
	 * Every read-back above passed on a virtio-gpu whose screen was
	 * entirely black (GX-003): those pages are ordinary memory, so reading
	 * back what was just written proves the store landed and nothing about
	 * whether anybody can see it. This is the call that makes the
	 * difference, and it is made unconditionally -- on a display that scans
	 * itself out it costs one system call and does nothing, and a program
	 * that branched on which sort it had would be wrong on one of them.
	 *
	 * The whole screen, spelled with the width and height SYS_SCREEN gave
	 * us, because there is no zero-means-everything. */
	if (recon_present((int)fd, 0, 0, screen.width, screen.height) != SYS_OK) {
		recon_close((int)fd);
		return BAD_PRESENT;
	}

	recon_say(1, "paint: a C program drew on the screen and asked for it "
			 "to be shown\n");
	recon_close((int)fd);
	return OK;
}
