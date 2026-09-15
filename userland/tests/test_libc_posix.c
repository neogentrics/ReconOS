/*
 * The descriptor layer, against the host's.
 *
 * Every function in `libc/posix.c` is a translation, and the translation is
 * the whole risk. `RECON_O_READ` is 1 and `O_RDONLY` is 0 -- so a wrapper that
 * passed the flags through unchanged would open every read-only file for
 * writing, which builds, passes the first test, and destroys a file on the
 * fourth.
 *
 * So this suite does what the rest of the library's suites do: **it makes the
 * same calls twice**, once through ReconOS's wrappers and once through the
 * host's, on the same files, and requires the same answers. A number that is
 * wrong in a way nobody would think to check is wrong against the reference
 * immediately.
 *
 * Three things it checks that a comparison cannot:
 *
 * - **The flags really are translated**, by writing through a descriptor
 *   opened read-only and requiring it to fail. Two libraries that both got
 *   this wrong the same way would agree with each other.
 * - **`realpath` refuses to climb out of the root**, which is ReconOS's
 *   containment rule and not POSIX's behaviour, so there is nothing to compare
 *   it against.
 * - **The descriptor limit matches the kernel's**, read out of the kernel's
 *   own header while the suite runs.
 *
 * Run with: ./build/recon_libc_posix_tests
 */

#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ReconOS's, renamed by prefix.h when libc/ was compiled. */
int recon_libc_open(const char *path, int flags, ...);
int recon_libc_close(int fd);
long recon_libc_read(int fd, void *into, size_t length);
long recon_libc_write(int fd, const void *from, size_t length);
long long recon_libc_lseek(int fd, long long offset, int whence);
int recon_libc_mkdir(const char *path, unsigned int mode);
char *recon_libc_realpath(const char *path, char *into);
long recon_libc_sysconf(int name);

struct recon_dirent {
	unsigned long long d_ino;
	unsigned char d_type;
	char d_name[256];
};

void *recon_libc_opendir(const char *path);
struct recon_dirent *recon_libc_readdir(void *dir);
int recon_libc_closedir(void *dir);

extern int recon_errno;

/* The allocator the directory layer uses, and the memory it comes from. */
struct recon_memory_source {
	void *(*take)(size_t bytes);
	int (*give_back)(void *at, size_t bytes);
};
void recon_memory_from(const struct recon_memory_source *from);

static unsigned long checks;
static unsigned long failures;

static void ok(int condition, const char *what)
{
	checks++;

	if (!condition) {
		failures++;
		if (failures <= 25)
			printf("  FAIL  %s\n", what);
	}
}

/* --- somewhere to work ---------------------------------------------------- */

static char area[256];

static const char *in_area(const char *name)
{
	static char path[512];

	snprintf(path, sizeof(path), "%s/%s", area, name);
	return path;
}

static void make_area(void)
{
	snprintf(area, sizeof(area), "/tmp/recon-posix-%d", (int)getpid());

	if (mkdir(area, 0755) != 0) {
		printf("  could not make %s: %s\n", area, strerror(errno));
		exit(2);
	}
}

static void clear_area(void)
{
	DIR *d = opendir(area);
	struct dirent *e;

	if (!d)
		return;

	while ((e = readdir(d)) != NULL) {
		char path[512];

		if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
			continue;

		snprintf(path, sizeof(path), "%s/%s", area, e->d_name);
		if (rmdir(path) != 0)
			unlink(path);
	}

	closedir(d);
	rmdir(area);
}

/* --- files ---------------------------------------------------------------- */

static void reading_and_writing(void)
{
	static const char text[] = "the same bytes, through two libraries";
	const char *path = in_area("both.txt");
	char mine[128], theirs[128];
	long a, b;
	int fd;

	/* Written through ReconOS's wrappers. */
	fd = recon_libc_open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	ok(fd >= 0, "a file can be created for writing");

	if (fd < 0)
		return;

	a = recon_libc_write(fd, text, sizeof(text) - 1);
	ok(a == (long)sizeof(text) - 1, "and every byte written");
	ok(recon_libc_close(fd) == 0, "and closed");

	/* Read back through both, and the bytes must match. */
	fd = recon_libc_open(path, O_RDONLY);
	ok(fd >= 0, "and opened again for reading");
	a = recon_libc_read(fd, mine, sizeof(mine));
	recon_libc_close(fd);

	fd = open(path, O_RDONLY);
	b = read(fd, theirs, sizeof(theirs));
	close(fd);

	ok(a == b, "both libraries read the same number of bytes");
	ok(a > 0 && memcmp(mine, theirs, (size_t)a) == 0,
	   "and the same bytes");
	ok(a == (long)sizeof(text) - 1 &&
	   memcmp(mine, text, (size_t)a) == 0,
	   "which are the ones that were written");
}

/*
 * The flags are really translated.
 *
 * Nothing in a comparison can catch this: if both libraries opened a
 * read-only file for writing, they would agree. So the check is a behaviour
 * rather than an answer -- a write through a read-only descriptor has to
 * fail.
 */
static void flags_are_translated(void)
{
	const char *path = in_area("readonly.txt");
	int fd;
	long n;

	fd = recon_libc_open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd >= 0) {
		recon_libc_write(fd, "x", 1);
		recon_libc_close(fd);
	}

	fd = recon_libc_open(path, O_RDONLY);
	ok(fd >= 0, "a file opens read-only");

	if (fd < 0)
		return;

	n = recon_libc_write(fd, "should not land", 15);
	ok(n < 0, "and writing through it is refused");
	ok(recon_errno != 0, "with errno set");

	recon_libc_close(fd);

	/* And the same the other way, because a translation can be wrong in
	 * one direction only. */
	fd = recon_libc_open(path, O_WRONLY);
	ok(fd >= 0, "the same file opens write-only");

	if (fd >= 0) {
		char into[4];

		n = recon_libc_read(fd, into, sizeof(into));
		ok(n < 0, "and reading through it is refused");
		recon_libc_close(fd);
	}
}

static void seeking(void)
{
	const char *path = in_area("seek.txt");
	int mine, theirs;
	long long a, b;

	mine = recon_libc_open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	recon_libc_write(mine, "0123456789", 10);
	recon_libc_close(mine);

	mine = recon_libc_open(path, O_RDONLY);
	theirs = open(path, O_RDONLY);

	a = recon_libc_lseek(mine, 4, SEEK_SET);
	b = lseek(theirs, 4, SEEK_SET);
	ok(a == b, "seeking from the start agrees");

	a = recon_libc_lseek(mine, 2, SEEK_CUR);
	b = lseek(theirs, 2, SEEK_CUR);
	ok(a == b, "and from where it is");

	a = recon_libc_lseek(mine, 0, SEEK_END);
	b = lseek(theirs, 0, SEEK_END);
	ok(a == b && a == 10, "and from the end, which is the length");

	/* A whence that means nothing. Both must refuse rather than pick one. */
	a = recon_libc_lseek(mine, 0, 99);
	ok(a < 0, "an unknown whence is refused");

	recon_libc_close(mine);
	close(theirs);
}

static void failures_carry_a_reason(void)
{
	int fd;

	recon_errno = 0;
	fd = recon_libc_open(in_area("no-such-file"), O_RDONLY);

	ok(fd < 0, "a file that is not there does not open");
	ok(recon_errno == ENOENT, "and errno says which");
	ok(strcmp(strerror(ENOENT), "No such file or directory") == 0,
	   "which reads the way the host's does");

	recon_errno = 0;
	ok(recon_libc_close(4242) < 0, "a descriptor that names nothing");
	ok(recon_errno == EBADF, "is EBADF");
}

/* --- directories ---------------------------------------------------------- */

static int names_from_reconos(const char *path, char out[][256], int most)
{
	void *d = recon_libc_opendir(path);
	struct recon_dirent *e;
	int n = 0;

	if (!d)
		return -1;

	while ((e = recon_libc_readdir(d)) != NULL && n < most) {
		snprintf(out[n], 256, "%s", e->d_name);
		n++;
	}

	recon_libc_closedir(d);
	return n;
}

static int names_from_host(const char *path, char out[][256], int most)
{
	DIR *d = opendir(path);
	struct dirent *e;
	int n = 0;

	if (!d)
		return -1;

	while ((e = readdir(d)) != NULL && n < most) {
		if (strcmp(e->d_name, ".") == 0 ||
		    strcmp(e->d_name, "..") == 0)
			continue;
		snprintf(out[n], 256, "%s", e->d_name);
		n++;
	}

	closedir(d);
	return n;
}

static int by_name(const void *a, const void *b)
{
	return strcmp((const char *)a, (const char *)b);
}

static void listing_a_directory(void)
{
	static char mine[64][256];
	static char theirs[64][256];
	const char *sub = in_area("adir");
	int a, b, i;

	ok(recon_libc_mkdir(sub, 0755) == 0, "a directory can be made");

	/* Something to find in it. */
	for (i = 0; i < 5; i++) {
		char path[512];
		int fd;

		snprintf(path, sizeof(path), "%s/file-%d", sub, i);
		fd = recon_libc_open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (fd >= 0) {
			recon_libc_write(fd, "x", 1);
			recon_libc_close(fd);
		}
	}

	a = names_from_reconos(sub, mine, 64);
	b = names_from_host(sub, theirs, 64);

	ok(a == 5, "five names come back");
	ok(a == b, "and both libraries find the same number");

	if (a > 0 && a == b) {
		int same = 1;

		qsort(mine, (size_t)a, 256, by_name);
		qsort(theirs, (size_t)b, 256, by_name);

		for (i = 0; i < a; i++) {
			if (strcmp(mine[i], theirs[i]) != 0)
				same = 0;
		}

		ok(same, "and the same names");
	}

	/* Reading it again gives the same answer -- a cursor left somewhere by
	 * the first walk would show up here and nowhere else. */
	a = names_from_reconos(sub, mine, 64);
	ok(a == 5, "and reading it a second time finds them all again");

	/* An empty one. The kernel answers zero bytes, and the layer must
	 * treat that as a directory with nothing in it rather than a
	 * failure -- which is the easiest thing in this file to get wrong. */
	{
		const char *empty = in_area("empty");

		ok(recon_libc_mkdir(empty, 0755) == 0, "an empty directory");
		a = names_from_reconos(empty, mine, 64);
		ok(a == 0, "reads as no names, not as a failure");
	}

	recon_errno = 0;
	ok(recon_libc_opendir(in_area("not-there")) == NULL,
	   "a directory that is not there does not open");
	ok(recon_errno == ENOENT, "and says so");
}

/* --- paths ---------------------------------------------------------------- */

static void resolving_paths(void)
{
	char into[512];

	struct {
		const char *given;
		const char *want;
	} cases[] = {
		{ "/System",			"/System" },
		{ "/System/",			"/System" },
		{ "//System//Apps//",		"/System/Apps" },
		{ "/System/./Apps",		"/System/Apps" },
		{ "/System/Apps/..",		"/System" },
		{ "/System/Apps/../Config",	"/System/Config" },
		{ "/",				"/" },
		{ "/.",				"/" },
		{ "///",			"/" },

		/* The containment rule, and the reason this cannot be
		 * compared against the host: ReconOS has one root and a path
		 * may not climb above it. The host's realpath would answer
		 * with something outside. */
		{ "/..",			"/" },
		{ "/../..",			"/" },
		{ "/System/../../Users",	"/Users" },
	};
	size_t i;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		char what[256];
		char *got = recon_libc_realpath(cases[i].given, into);

		snprintf(what, sizeof(what), "%s resolves to %s, not %s",
			 cases[i].given, cases[i].want, got ? got : "(null)");

		ok(got && strcmp(got, cases[i].want) == 0, what);
	}

	/* Relative paths are refused rather than resolved, because there is no
	 * working directory to resolve them against. */
	recon_errno = 0;
	ok(recon_libc_realpath("System/Apps", into) == NULL,
	   "a relative path is refused");
	ok(recon_errno == EINVAL, "with EINVAL, since there is no elsewhere");
}

/* --- what the machine says ------------------------------------------------ */

static int kernel_fd_limit(void)
{
	FILE *f = fopen("kernel/include/recon/kernel/process.h", "r");
	char line[256];
	int value = -1;

	if (!f) {
		ok(0, "kernel/include/recon/kernel/process.h could not be "
		      "read -- run this from the top of the repository");
		return -1;
	}

	while (fgets(line, sizeof(line), f)) {
		int n;

		if (sscanf(line, "#define PROCESS_FDS_MAX %d", &n) == 1) {
			value = n;
			break;
		}
	}

	fclose(f);
	return value;
}

static void machine_facts(void)
{
	long page = recon_libc_sysconf(_SC_PAGESIZE);
	long most = recon_libc_sysconf(_SC_OPEN_MAX);
	int kernel = kernel_fd_limit();
	char what[200];

	ok(page == sysconf(_SC_PAGESIZE),
	   "the page size agrees with the host's");
	ok(page > 0 && (page & (page - 1)) == 0,
	   "and is a power of two, which every page size is");

	snprintf(what, sizeof(what),
		 "the descriptor limit is %ld here and %d in the kernel",
		 most, kernel);
	ok(kernel > 0 && most == kernel, what);

	recon_errno = 0;
	ok(recon_libc_sysconf(4242) == -1, "a question it cannot answer");
	ok(recon_errno == EINVAL, "is EINVAL rather than a made-up number");
}

/* --- the allocator the directory layer needs ------------------------------ */

static void *take(size_t bytes)
{
	return malloc(bytes);
}

static int give_back(void *at, size_t bytes)
{
	(void)bytes;
	free(at);
	return 0;
}

static const struct recon_memory_source host_memory = { take, give_back };

int main(void)
{
	printf("The descriptor layer\n\n");

	/* `opendir` allocates. On the host the simplest source is the host's
	 * own allocator, which keeps this suite about the descriptor layer
	 * rather than about the one underneath it. */
	recon_memory_from(&host_memory);

	make_area();

	reading_and_writing();
	flags_are_translated();
	seeking();
	failures_carry_a_reason();
	listing_a_directory();
	resolving_paths();
	machine_facts();

	clear_area();

	printf("\n%lu checks, %lu failures\n", checks, failures);
	return failures ? 1 : 0;
}
