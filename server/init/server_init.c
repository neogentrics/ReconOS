/*
 * The first program on a machine whose role is **server**.
 *
 * `docs/ROLES.md`: the installer puts down the whole operating system every
 * time and the role is chosen on the first boot afterwards. So this is not a
 * different system from the workstation's `userland/init/recon_init.c` -- it
 * is the same system told what it is for. It draws with the same `screen.c`,
 * on purpose and at Joshua's instruction, so that a person who has seen one
 * ReconOS machine recognises the next one.
 *
 * What it does that the workstation's does not: **it serves.**
 *
 * --- What this program is actually here to prove ---
 *
 * The kernel session recorded, in its own words, that a socket descriptor
 * working with `read` and `write` is *argued, not measured*. Their self-test
 * cannot hold a descriptor -- a kernel thread has no process, so `fd_install`
 * fails there for sockets exactly as it does for pipes. `recon_init` answered
 * half of it: a descriptor can be held, closed once, refused twice, and a
 * listener says `EAGAIN` with nobody waiting.
 *
 * The half still open is whether **bytes move over an accepted connection**.
 * Nothing had ever done that. This program does it, and the line it prints on
 * the serial console is the measurement:
 *
 *     the web server: listening on :80 -- <n> requests served, <m> bytes out
 *
 * A number there that goes up is the first evidence that this kernel's sockets
 * carry traffic. A number that stays at zero, with a client that connected, is
 * a fault worth a KF entry.
 */

#include <recon.h>
#include <recon_machine.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "layout.h"
#include "screen.h"

#include "../http/serve.h"
#include "../include/recon_server.h"

/* Return codes, in the workstation's numbering so that a reader of one file
 * recognises them in the other. The server's own start at 70. */
#define WENT_WELL             0
#define NO_SCREEN_DESCRIBED  60
#define SCREEN_STRUCT_WRONG  61
#define FRAMEBUFFER_CLOSED   62
#define MAPPING_REFUSED      63
#define SCREEN_MAKES_NO_SENSE 64
#define MACHINE_UNREADABLE   65
#define NO_LISTENER          70

static void say(const char *text)
{
	recon_write(1, text, recon_strlen(text));
}

/* --- what this server answers with --------------------------------------- */

/*
 * The machine's own facts, gathered once and shown on every page.
 *
 * Kept in one place because the dashboard and the JSON endpoint must not be
 * able to disagree: two readers of `SYS_MACHINE` would eventually be two
 * answers, and a monitoring system that sees a different figure from the page
 * beside it has to be told which one to believe.
 */
struct server_facts {
	struct recon_machine machine;
	char                 name[RECON_NAME_MAX];
	unsigned long        served;
	unsigned long        bytes_out;
};

static struct server_facts FACTS;

static const char PAGE_HEAD[] =
	"<!doctype html>\n<meta charset=\"utf-8\">\n"
	"<title>ReconOS Server</title>\n"
	"<style>"
	"body{background:#141821;color:#d8dce6;font:15px/1.6 system-ui,sans-serif;"
	"margin:0;padding:40px}"
	"main{max-width:640px;margin:0 auto}"
	"h1{color:#4fb3a5;font-size:26px;margin:0 0 4px}"
	"p.sub{color:#737c90;margin:0 0 28px}"
	"table{border-collapse:collapse;width:100%}"
	"td{padding:8px 0;border-bottom:1px solid #2a3145}"
	"td:first-child{color:#737c90;width:40%}"
	"code{color:#4fb3a5}"
	"</style>\n<main>\n";

/* The dashboard. One page, real numbers, nothing invented.
 *
 * This is the console the docx calls the Server Manager dashboard, at the size
 * it can honestly be today: what this machine is, and what this server has
 * done. Every row is read from the kernel or counted here. */
static int handle_dashboard(const struct http_request *r, const char *body,
                            size_t body_len, struct http_response *out,
                            void *ctx)
{
	static char page[HTTP_RESPONSE_MAX];
	struct server_facts *f = (struct server_facts *)ctx;
	int n;

	(void)r; (void)body; (void)body_len;

	n = snprintf(page, sizeof(page),
	             "%s"
	             "<h1>%s</h1>\n"
	             "<p class=\"sub\">ReconOS, running its own kernel. "
	             "No Linux is underneath it.</p>\n"
	             "<table>\n"
	             "<tr><td>role</td><td>server</td></tr>\n"
	             "<tr><td>architecture</td><td>%s</td></tr>\n"
	             "<tr><td>processor</td><td>%s</td></tr>\n"
	             "<tr><td>processors</td><td>%u found, %u in use</td></tr>\n"
	             "<tr><td>memory</td><td>%llu MiB total, %llu MiB free</td></tr>\n"
	             "<tr><td>page size</td><td>%u KiB</td></tr>\n"
	             "<tr><td>requests served</td><td><code>%lu</code></td></tr>\n"
	             "<tr><td>bytes sent</td><td><code>%lu</code></td></tr>\n"
	             "</table>\n"
	             "<p class=\"sub\" style=\"margin-top:28px\">"
	             "Served by <code>server/http/</code> over the kernel's five "
	             "socket calls. This page is the first thing to move bytes "
	             "over an accepted connection on this system.</p>\n"
	             "</main>\n",
	             PAGE_HEAD,
	             f->name,
	             f->machine.architecture[0] ? f->machine.architecture
	                                        : "unnamed",
	             f->machine.cpu_model[0] ? f->machine.cpu_model
	                                     : f->machine.cpu_vendor,
	             f->machine.processors_found, f->machine.processors_online,
	             (unsigned long long)(f->machine.memory_bytes >> 20),
	             (unsigned long long)(f->machine.memory_free_bytes >> 20),
	             f->machine.page_size / 1024u,
	             f->served, f->bytes_out);

	if (n < 0 || (size_t)n >= sizeof(page))
		return HTTP_EBODY_LONG;

	http_response_simple(out, 200, "text/html; charset=utf-8", page,
	                     (size_t)n);
	return HTTP_OK;
}

/* The same facts as JSON, which is what turns this from a page into an API.
 *
 * Deliberately the same numbers from the same structure -- see `struct
 * server_facts`. A dashboard and an endpoint that read separately are a
 * dashboard and an endpoint that eventually disagree. */
static int handle_status(const struct http_request *r, const char *body,
                         size_t body_len, struct http_response *out, void *ctx)
{
	static char json[1024];
	struct server_facts *f = (struct server_facts *)ctx;
	int n;

	(void)r; (void)body; (void)body_len;

	n = snprintf(json, sizeof(json),
	             "{\"role\":\"server\","
	             "\"name\":\"%s\","
	             "\"architecture\":\"%s\","
	             "\"processors_found\":%u,"
	             "\"processors_online\":%u,"
	             "\"memory_bytes\":%llu,"
	             "\"memory_free_bytes\":%llu,"
	             "\"page_size\":%u,"
	             "\"requests_served\":%lu,"
	             "\"bytes_sent\":%lu}\n",
	             f->name,
	             f->machine.architecture[0] ? f->machine.architecture
	                                        : "unnamed",
	             f->machine.processors_found, f->machine.processors_online,
	             (unsigned long long)f->machine.memory_bytes,
	             (unsigned long long)f->machine.memory_free_bytes,
	             f->machine.page_size,
	             f->served, f->bytes_out);

	if (n < 0 || (size_t)n >= sizeof(json))
		return HTTP_EBODY_LONG;

	http_response_simple(out, 200, "application/json", json, (size_t)n);
	return HTTP_OK;
}

/* Enough of a health endpoint to be polled by something that is not a person.
 * Plain text and a fixed body, so that a checker can compare bytes rather than
 * parse. */
static int handle_health(const struct http_request *r, const char *body,
                         size_t body_len, struct http_response *out, void *ctx)
{
	(void)r; (void)body; (void)body_len; (void)ctx;
	http_response_simple(out, 200, "text/plain", "ok\n", 3);
	return HTTP_OK;
}

static const struct http_route ROUTES[] = {
	{ "GET", "/",             1, handle_dashboard },
	{ "GET", "/api/status",   1, handle_status },
	{ "GET", "/health",       1, handle_health },
};

/* --- the screen ----------------------------------------------------------- */

/*
 * Redraw with the current counts.
 *
 * Called after each connection so the screen in front of the machine is the
 * same truth as the page being served from it. Cheap enough at this scale, and
 * the alternative -- a screen drawn once at boot -- is a screen that is wrong
 * from the first request onward.
 */
static void draw(const struct recon_canvas *canvas,
                 struct recon_first_boot *facts, char *served_line,
                 size_t room)
{
	snprintf(served_line, room,
	         "The web server is listening on port 80. %lu served, %lu bytes out.",
	         FACTS.served, FACTS.bytes_out);
	recon_screen_draw(canvas, facts);
}

int main(void)
{
	struct recon_screen screen;
	struct recon_canvas canvas;
	struct recon_first_boot facts;
	struct http_site site;
	char kernel_line[96];
	char processors_line[64];
	char memory_line[96];
	char display_line[96];
	char served_line[140];
	char address_line[120];
	char line[200];
	i64 answer, fd, mapped;
	int listener;

	/* --- the screen, exactly as the workstation does it ------------------ */

	answer = recon_screen(&screen, sizeof(screen));
	if (answer < 0)
		return NO_SCREEN_DESCRIBED;
	if ((u64)answer != sizeof(screen))
		return SCREEN_STRUCT_WRONG;
	if (screen.width == 0 || screen.height == 0 ||
	    screen.pitch < screen.width * 4u)
		return SCREEN_MAKES_NO_SENSE;

	fd = recon_open("/dev/fb0", 8, OPEN_WRITE | OPEN_READ, 0);
	if (fd < 0)
		return FRAMEBUFFER_CLOSED;

	mapped = recon_map((int)fd, screen.bytes);
	if (mapped < 0)
		return MAPPING_REFUSED;

	/* --- the machine ------------------------------------------------------ */

	memset(&FACTS, 0, sizeof(FACTS));
	FACTS.machine.size = (u32)sizeof(FACTS.machine);
	answer = recon_machine_facts(&FACTS.machine, sizeof(FACTS.machine));
	if (answer < 0)
		return MACHINE_UNREADABLE;

	/* This machine's name.
	 *
	 * Hard-coded for now, and deliberately a name that *belongs to a
	 * family* so that the parallel naming in `server/identity.c` has
	 * something real to work from the moment discovery exists. When a peer
	 * can be found on the wire, this is the line that changes: the name
	 * comes from `server_name_next_parallel` instead of from here. */
	snprintf(FACTS.name, sizeof(FACTS.name), "M16");

	snprintf(kernel_line, sizeof(kernel_line), "kernel on %s, %u KiB pages",
	         FACTS.machine.architecture[0] ? FACTS.machine.architecture
	                                       : "an unnamed architecture",
	         FACTS.machine.page_size / 1024u);
	snprintf(processors_line, sizeof(processors_line), "%u found, %u in use",
	         FACTS.machine.processors_found, FACTS.machine.processors_online);
	snprintf(memory_line, sizeof(memory_line), "%llu MiB, %llu MiB free",
	         (unsigned long long)(FACTS.machine.memory_bytes >> 20),
	         (unsigned long long)(FACTS.machine.memory_free_bytes >> 20));
	snprintf(display_line, sizeof(display_line), "%u x %u, %u bytes a row",
	         screen.width, screen.height, screen.pitch);

	/* --- the listener, before anything is drawn about it ------------------ */

	listener = http_listen(80);
	if (listener < 0) {
		/* Said plainly rather than drawn over. A server role whose
		 * server did not start is the one fact worth stopping for. */
		say("  the web server: could not listen on :80\n");
		snprintf(address_line, sizeof(address_line),
		         "The web server could not open port 80.");
	} else {
		snprintf(address_line, sizeof(address_line),
		         "Serving on port 80. Try / and /api/status.");
	}

	site.routes = ROUTES;
	site.route_count = sizeof(ROUTES) / sizeof(ROUTES[0]);
	site.ctx = &FACTS;
	site.server_name = "ReconOS";
	site.bytes_sent = &FACTS.bytes_out;

	/* --- the first screen -------------------------------------------------- */

	memset(&facts, 0, sizeof(facts));
	facts.version = "ReconOS -- server";
	facts.kernel = kernel_line;
	facts.processors = processors_line;
	facts.memory = memory_line;
	facts.cpu = FACTS.machine.cpu_model[0] ? FACTS.machine.cpu_model
	                                       : FACTS.machine.cpu_vendor;
	facts.display = display_line;
	facts.storage = "not yet read on this role";

	facts.notes[0] = address_line;
	facts.notes[1] = served_line;
	facts.notes[2] = "This machine is running its own kernel.";
	facts.notes[3] = "No Linux is underneath it.";
	facts.notes[4] = "";
	facts.notes[5] = "DNS and DHCP wait on a datagram system call.";

	canvas.pixels = (unsigned char *)(unsigned long)mapped;
	canvas.width = screen.width;
	canvas.height = screen.height;
	canvas.pitch = screen.pitch;

	draw(&canvas, &facts, served_line, sizeof(served_line));

	snprintf(line, sizeof(line),
	         "  the web server: listening on :80 -- %lu requests served,"
	         " %lu bytes out\n", FACTS.served, FACTS.bytes_out);
	say(line);

	if (listener < 0)
		return NO_LISTENER;

	/* --- serve ------------------------------------------------------------- */

	/*
	 * The loop, and why it yields.
	 *
	 * `accept` answers `EAGAIN` with nobody waiting -- there is no wait
	 * queue on a listener -- so this polls. `recon_yield` rather than a
	 * bare spin, for the reason the workstation's idle loop gives: a
	 * processor held at a hundred percent to wait for a connection is a
	 * fan running and an hour less battery.
	 *
	 * When the kernel can report readiness on more than a listener, this
	 * is the loop that changes, and `docs/WEB.md` carries the row.
	 */
	for (;;) {
		int rc = http_serve_once(listener, &site);

		if (rc < 0) {
			say("  the web server: the listener failed\n");
			return NO_LISTENER;
		}
		if (rc == 0) {
			recon_yield();
			continue;
		}

		FACTS.served++;

		/* Reported on every request, because this line is the
		 * measurement the whole exercise exists for. */
		snprintf(line, sizeof(line),
		         "  the web server: %lu served\n", FACTS.served);
		say(line);

		draw(&canvas, &facts, served_line, sizeof(served_line));
	}

	return WENT_WELL;
}
