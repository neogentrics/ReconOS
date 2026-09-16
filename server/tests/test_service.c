/*
 * Services, their states, and the restart that has to stop.
 *
 * Pure: the services here are fakes whose behaviour the test sets, and the
 * clock is a number the test supplies. So a service can be run through a
 * hundred rounds, made to fail on the fourth, and checked -- in microseconds,
 * with nothing real running.
 *
 * **The case the file exists for is the crash loop.** A supervisor that
 * restarts whatever stops turns one crash into an endless one: the dashboard
 * says `running`, the log fills with identical lines nobody reads, and the
 * machine looks healthy while doing nothing. So the bound is checked, and so
 * is what is left behind when it is reached -- a state and a reason a person
 * can act on, rather than a service quietly going round again.
 *
 * **Watched failing first**, against a supervisor with the restart bound
 * removed and one that stopped the round at the first fault. The loop case and
 * the isolation case failed as they should.
 */

#include "../service.h"

#include <stdio.h>
#include <string.h>

static int failures;
static int checks;

static void ok(int cond, const char *what)
{
	checks++;
	if (!cond) {
		failures++;
		printf("  FAIL  %s\n", what);
	}
}

/* --- a service whose behaviour the test decides --------------------------- */

struct fake {
	int  start_gives;	/* what `start` returns */
	int  poll_gives;	/* what `poll` returns */
	int  fail_after;	/* polls before `poll` starts failing; -1 never */
	int  starts;		/* how many times start was called */
	int  stops;
	int  polls;
};

static int fake_start(void *ctx)
{
	struct fake *f = (struct fake *)ctx;

	f->starts++;
	return f->start_gives;
}

static int fake_poll(void *ctx)
{
	struct fake *f = (struct fake *)ctx;

	f->polls++;
	if (f->fail_after >= 0 && f->polls > f->fail_after)
		return f->poll_gives;
	return SERVICE_OK;
}

static void fake_stop(void *ctx)
{
	struct fake *f = (struct fake *)ctx;

	f->stops++;
}

static void reset(struct fake *f)
{
	memset(f, 0, sizeof(*f));
	f->fail_after = -1;
}

int main(void)
{
	printf("services, their states, and the restart that has to stop\n");

	/* --- registration refuses what it cannot supervise --------------------- */
	{
		struct supervisor sup;
		struct fake f;
		struct service good, no_poll, no_name;

		memset(&sup, 0, sizeof(sup));
		reset(&f);

		good.name = "web"; good.what = "serves the console";
		good.start = fake_start; good.poll = fake_poll;
		good.stop = fake_stop; good.ctx = &f;

		no_poll = good; no_poll.poll = 0;
		no_name = good; no_name.name = "";

		ok(supervisor_add(&sup, &good) == SERVICE_OK,
		   "a usable service registers");
		ok(supervisor_add(&sup, &no_poll) != SERVICE_OK,
		   "one with no poll is refused -- it could only ever say running");
		ok(supervisor_add(&sup, &no_name) != SERVICE_OK,
		   "one with no name is refused -- a registry must be able to say which");
		ok(sup.count == 1, "and neither was registered");
	}

	/* --- starting, and a refusal that does not stop the others ------------- */
	{
		struct supervisor sup;
		struct fake bad, good;
		struct service a, b;
		unsigned running = 0, failed = 0, refused = 0;

		memset(&sup, 0, sizeof(sup));
		reset(&bad);
		reset(&good);
		bad.start_gives = -7;		/* will not come up */

		a.name = "dns"; a.what = "names"; a.start = fake_start;
		a.poll = fake_poll; a.stop = fake_stop; a.ctx = &bad;
		b.name = "web"; b.what = "console"; b.start = fake_start;
		b.poll = fake_poll; b.stop = fake_stop; b.ctx = &good;

		supervisor_add(&sup, &a);
		supervisor_add(&sup, &b);

		ok(supervisor_start(&sup, 1000) == 1, "one of two came up");
		ok(sup.status[0].state == SERVICE_REFUSED,
		   "the one that would not start is refused, not failed");
		ok(sup.status[0].last_reason == -7,
		   "and its own reason is kept, not translated");

		/* The point of the case: the console still runs. It is the
		 * page somebody would use to find out that DNS did not. */
		ok(sup.status[1].state == SERVICE_RUNNING,
		   "the other started anyway");
		ok(sup.status[1].started_at == 1000, "and recorded when");

		supervisor_tally(&sup, &running, &failed, &refused);
		ok(running == 1 && failed == 0 && refused == 1,
		   "the tally agrees");
	}

	/* --- polling ------------------------------------------------------------ */
	{
		struct supervisor sup;
		struct fake f;
		struct service s;
		int i;

		memset(&sup, 0, sizeof(sup));
		reset(&f);
		s.name = "web"; s.what = "console"; s.start = fake_start;
		s.poll = fake_poll; s.stop = fake_stop; s.ctx = &f;

		supervisor_add(&sup, &s);
		supervisor_start(&sup, 0);
		for (i = 0; i < 10; i++)
			supervisor_poll(&sup, (unsigned long long)i);

		ok(f.polls == 10, "a running service is polled every round");
		ok(sup.status[0].polls == 10, "and the count agrees");
		ok(sup.status[0].faults == 0, "with nothing reported");
		ok(sup.status[0].state == SERVICE_RUNNING, "and it is running");
	}

	/* A refused service is never polled. Asking it would produce a stream
	 * of identical faults that buries the first one. */
	{
		struct supervisor sup;
		struct fake f;
		struct service s;
		int i;

		memset(&sup, 0, sizeof(sup));
		reset(&f);
		f.start_gives = -1;
		s.name = "x"; s.what = "x"; s.start = fake_start;
		s.poll = fake_poll; s.stop = fake_stop; s.ctx = &f;

		supervisor_add(&sup, &s);
		supervisor_start(&sup, 0);
		for (i = 0; i < 5; i++)
			supervisor_poll(&sup, 0);

		ok(f.polls == 0, "a refused service is asked nothing");
	}

	/* --- the crash loop, which is what all of this is for -------------------
	 *
	 * A service that fails on every poll. Without a bound it restarts
	 * forever and the dashboard goes on saying `running`. */
	{
		struct supervisor sup;
		struct fake f;
		struct service s;
		int i;

		memset(&sup, 0, sizeof(sup));
		reset(&f);
		f.fail_after = 0;	/* fails on its very first poll */
		f.poll_gives = -42;

		s.name = "flapper"; s.what = "fails immediately";
		s.start = fake_start; s.poll = fake_poll;
		s.stop = fake_stop; s.ctx = &f;

		supervisor_add(&sup, &s);
		supervisor_start(&sup, 0);
		for (i = 0; i < 50; i++)
			supervisor_poll(&sup, (unsigned long long)i);

		ok(sup.status[0].restarts == SERVICE_RESTARTS_MAX,
		   "it is restarted a bounded number of times");
		ok(f.starts == 1 + SERVICE_RESTARTS_MAX,
		   "which is one start plus that many restarts, and no more");
		ok(sup.status[0].state == SERVICE_FAILED,
		   "and then it stays failed rather than going round again");
		ok(sup.status[0].last_reason == -42,
		   "holding the reason it failed for, so somebody can act on it");

		/* Fifty rounds, and it was polled four times. A supervisor
		 * without the bound would have polled it fifty. */
		ok(f.polls == 1 + SERVICE_RESTARTS_MAX,
		   "a failed service stops consuming rounds");

		/* The number a person needs is how often it broke, across the
		 * whole life of the service and not since the last restart. */
		ok(sup.status[0].faults == 1 + SERVICE_RESTARTS_MAX,
		   "faults accumulate across restarts rather than resetting");
	}

	/* --- a service that recovers -------------------------------------------- */
	{
		struct supervisor sup;
		struct fake f;
		struct service s;
		int i;

		memset(&sup, 0, sizeof(sup));
		reset(&f);
		f.fail_after = 2;	/* two good polls, then it faults once */
		f.poll_gives = -5;

		s.name = "wobbly"; s.what = "fails once";
		s.start = fake_start; s.poll = fake_poll;
		s.stop = fake_stop; s.ctx = &f;

		supervisor_add(&sup, &s);
		supervisor_start(&sup, 0);

		/* `fake_poll` counts across restarts, so after the third poll
		 * it keeps failing -- which is the crash-loop path again. What
		 * this case pins down is the restart itself. */
		for (i = 0; i < 3; i++)
			supervisor_poll(&sup, 100 + (unsigned long long)i);

		ok(sup.status[0].restarts == 1, "one fault, one restart");
		ok(f.stops == 1, "and it was stopped before being started again");
		ok(sup.status[0].started_at == 102,
		   "the start time is the restart's, not the first start's");
		ok(sup.status[0].state == SERVICE_RUNNING,
		   "and it is running again");
	}

	/* --- one broken service does not stop the others ------------------------
	 *
	 * The isolation case. A round that gave up at the first fault would
	 * leave the healthy service unpolled, so a failure in one would stop
	 * the console that reports it. */
	{
		struct supervisor sup;
		struct fake broken, fine;
		struct service a, b;
		int i;

		memset(&sup, 0, sizeof(sup));
		reset(&broken);
		reset(&fine);
		broken.fail_after = 0;
		broken.poll_gives = -9;

		a.name = "broken"; a.what = "fails"; a.start = fake_start;
		a.poll = fake_poll; a.stop = fake_stop; a.ctx = &broken;
		b.name = "web"; b.what = "console"; b.start = fake_start;
		b.poll = fake_poll; b.stop = fake_stop; b.ctx = &fine;

		supervisor_add(&sup, &a);
		supervisor_add(&sup, &b);
		supervisor_start(&sup, 0);
		for (i = 0; i < 10; i++)
			supervisor_poll(&sup, (unsigned long long)i);

		ok(sup.status[0].state == SERVICE_FAILED, "the broken one failed");
		ok(fine.polls == 10,
		   "and the healthy one was polled every round regardless");
		ok(sup.status[1].state == SERVICE_RUNNING,
		   "and is still running");
	}

	/* --- stopping ----------------------------------------------------------- */
	{
		struct supervisor sup;
		struct fake f1, f2;
		struct service a, b;

		memset(&sup, 0, sizeof(sup));
		reset(&f1);
		reset(&f2);
		a.name = "first"; a.what = "x"; a.start = fake_start;
		a.poll = fake_poll; a.stop = fake_stop; a.ctx = &f1;
		b.name = "second"; b.what = "x"; b.start = fake_start;
		b.poll = fake_poll; b.stop = fake_stop; b.ctx = &f2;

		supervisor_add(&sup, &a);
		supervisor_add(&sup, &b);
		supervisor_start(&sup, 0);
		supervisor_stop(&sup);

		ok(f1.stops == 1 && f2.stops == 1, "everything running was stopped");
		ok(sup.status[0].state == SERVICE_STOPPED
		   && sup.status[1].state == SERVICE_STOPPED,
		   "and both say so");

		supervisor_stop(&sup);
		ok(f1.stops == 1, "stopping twice does not stop it twice");
	}

	/* --- room ---------------------------------------------------------------- */
	{
		struct supervisor sup;
		struct fake f;
		struct service s;
		int i, refused_at = -1;

		memset(&sup, 0, sizeof(sup));
		reset(&f);
		s.name = "x"; s.what = "x"; s.start = fake_start;
		s.poll = fake_poll; s.stop = fake_stop; s.ctx = &f;

		for (i = 0; i < SERVICES_MAX + 4; i++)
			if (supervisor_add(&sup, &s) != SERVICE_OK
			    && refused_at < 0)
				refused_at = i;

		ok(refused_at == SERVICES_MAX,
		   "registration refuses past its bound rather than overrunning");
		ok(sup.count == SERVICES_MAX, "and the count stops there");
	}

	/* --- names --------------------------------------------------------------- */
	ok(strcmp(service_state_name(SERVICE_RUNNING), "running") == 0, "running");
	ok(strcmp(service_state_name(SERVICE_REFUSED), "refused") == 0, "refused");
	ok(strcmp(service_state_name(SERVICE_FAILED), "failed") == 0, "failed");
	ok(strcmp(service_state_name(SERVICE_STOPPED), "stopped") == 0, "stopped");

	printf("  %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
