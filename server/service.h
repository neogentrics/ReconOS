/*
 * Services, and keeping them running.
 *
 * --- What this is not, said first because the name promises more ---
 *
 * **This does not supervise processes, because nothing on this system can
 * start one.** `docs/KERNEL-WANTS.md` carries the entry: there is no call in
 * user mode that launches a program, so a supervisor in the ordinary sense --
 * one that forks a daemon, watches it die and forks it again -- cannot be
 * written here and is not what this is.
 *
 * What it is: **a registry of things this one process does, and their state.**
 * A service is a few functions and some context. The supervisor starts them,
 * polls them in turn, and records what happened. The web server is one; the
 * discovery sweep will be another; DNS and DHCP will be two more when the
 * datagram call exists.
 *
 * That is cooperative multiplexing inside a single process, and it is not a
 * lesser version of process supervision waiting to be upgraded. It is what
 * this system can actually do, and it is what the Services and Daemon
 * Inspector in the architecture document will read. When a program can be
 * started, this grows a second kind of service rather than being replaced.
 *
 * --- Why there is no unbounded restart ---
 *
 * A supervisor that restarts anything that stops is a supervisor that turns a
 * crash into a **crash loop**, and a crash loop is worse than a stopped
 * service: the dashboard says `running`, the log fills with identical lines
 * nobody reads, and the machine looks healthy while doing nothing.
 *
 * So a service is restarted at most `SERVICE_RESTARTS_MAX` times, and then it
 * stays failed with the reason it failed for. A person decides what happens
 * next, which is the correct owner of that decision.
 */

#ifndef RECON_SERVICE_H
#define RECON_SERVICE_H

#define SERVICES_MAX          16
#define SERVICE_RESTARTS_MAX   3

/* What a service's own functions answer. Zero is well; anything negative is a
 * reason, and the number is the service's own -- the supervisor stores it and
 * shows it without interpreting it. */
#define SERVICE_OK 0

enum service_state {
	/* Registered and not started. */
	SERVICE_STOPPED = 0,

	/* Started, and its last poll said nothing was wrong. */
	SERVICE_RUNNING,

	/* **It never started.** Distinguished from FAILED on purpose: a
	 * service that refused to start has done nothing, while one that
	 * failed later may have done a great deal first. Reading a listener
	 * that could not bind as "it stopped" would send somebody looking for
	 * what changed. */
	SERVICE_REFUSED,

	/* It was running and its poll reported a fault, and it has been
	 * restarted as often as it may be. */
	SERVICE_FAILED
};

struct service {
	const char *name;	/* short, for a table: "web", "discovery" */
	const char *what;	/* one line a person reads: what it is for */

	/* Bring it up. SERVICE_OK, or a negative reason. A service with no
	 * start is one that needs no preparation, which is legal. */
	int (*start)(void *ctx);

	/* Called on every round. Must return promptly: this is cooperative,
	 * and a poll that blocks stops every other service on the machine.
	 * SERVICE_OK, or a negative reason. */
	int (*poll)(void *ctx);

	/* Let go of whatever start took. May be NULL. */
	void (*stop)(void *ctx);

	void *ctx;
};

struct service_status {
	enum service_state state;
	int           last_reason;	/* the number its own function gave */
	unsigned long polls;		/* how many rounds it has been asked */
	unsigned long faults;		/* how many times a poll reported one */
	unsigned long restarts;
	unsigned long long started_at;	/* the clock the caller supplied */
};

struct supervisor {
	const struct service *services[SERVICES_MAX];
	struct service_status status[SERVICES_MAX];
	unsigned count;
};

/* Register a service. Returns SERVICE_OK, or negative when there is no room or
 * the service is not usable -- one with no `poll` cannot be supervised, and is
 * refused rather than registered and never asked anything. */
int supervisor_add(struct supervisor *sup, const struct service *service);

/*
 * Start everything registered.
 *
 * Returns how many are running. A service that refuses to start does not stop
 * the others: a machine whose DNS will not bind should still serve its
 * console, which is how somebody finds out that DNS will not bind.
 */
unsigned supervisor_start(struct supervisor *sup, unsigned long long now);

/*
 * One round: poll every running service once.
 *
 * `now` is supplied rather than read, so this file has no clock in it and the
 * suite can run a service through a hundred rounds without waiting for any of
 * them.
 */
void supervisor_poll(struct supervisor *sup, unsigned long long now);

/* Stop everything, in reverse order of registration -- the last thing started
 * is the most likely to depend on the others. */
void supervisor_stop(struct supervisor *sup);

/* How many are in each state, for a caller that wants a summary rather than a
 * table. Any pointer may be NULL. */
void supervisor_tally(const struct supervisor *sup, unsigned *running,
                      unsigned *failed, unsigned *refused);

/* The word for a state, for a table or a log. Never NULL. */
const char *service_state_name(enum service_state state);

#endif
