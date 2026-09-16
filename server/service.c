/*
 * Starting things, polling them, and writing down what happened.
 *
 * See `service.h` for what this deliberately is not, and for why a crash loop
 * is worse than a stopped service.
 *
 * --- What was tried and rejected ---
 *
 * **Stopping the whole round when one service's poll reports a fault.** It is
 * the tidier loop and it means one broken service takes the rest down with it
 * -- so a machine whose DNS is failing also stops serving the console, which
 * is the page somebody would have used to find out that DNS is failing. Every
 * service is polled every round regardless of what the others said.
 *
 * **Counting a restart as the service having run.** An early version reset
 * `faults` on restart, on the theory that the new run starts clean. It does
 * not: the number a person needs is *how often has this broken*, and a counter
 * that forgets is a counter that says a service flapping every few seconds has
 * never faulted. Faults accumulate across restarts; `restarts` is its own
 * number beside it.
 */

#include "service.h"

int supervisor_add(struct supervisor *sup, const struct service *service)
{
	struct service_status *st;

	if (!sup || !service)
		return -1;

	/* A service with no `poll` can never report anything, so registering
	 * it would put a row on the dashboard that says `running` forever
	 * whatever happens to it. Refused rather than accepted quietly. */
	if (!service->poll)
		return -2;

	/* A service with no name cannot be named in a table or a log, and the
	 * whole point of a registry is being able to say which one. */
	if (!service->name || !service->name[0])
		return -3;

	if (sup->count >= SERVICES_MAX)
		return -4;

	sup->services[sup->count] = service;
	st = &sup->status[sup->count];
	st->state = SERVICE_STOPPED;
	st->last_reason = SERVICE_OK;
	st->polls = 0;
	st->faults = 0;
	st->restarts = 0;
	st->started_at = 0;
	sup->count++;
	return SERVICE_OK;
}

unsigned supervisor_start(struct supervisor *sup, unsigned long long now)
{
	unsigned i, running = 0;

	if (!sup)
		return 0;

	for (i = 0; i < sup->count; i++) {
		const struct service *s = sup->services[i];
		struct service_status *st = &sup->status[i];
		int rc = SERVICE_OK;

		if (st->state == SERVICE_RUNNING)
			continue;

		if (s->start)
			rc = s->start(s->ctx);

		st->last_reason = rc;
		if (rc != SERVICE_OK) {
			/* Never ran. Not the same as having stopped -- see the
			 * comment on SERVICE_REFUSED. */
			st->state = SERVICE_REFUSED;
			continue;
		}

		st->state = SERVICE_RUNNING;
		st->started_at = now;
		running++;
	}

	return running;
}

void supervisor_poll(struct supervisor *sup, unsigned long long now)
{
	unsigned i;

	if (!sup)
		return;

	for (i = 0; i < sup->count; i++) {
		const struct service *s = sup->services[i];
		struct service_status *st = &sup->status[i];
		int rc;

		/* Only what is running is polled. A refused or failed service
		 * is asked nothing, because its `poll` has no state to work
		 * from -- and asking it would produce a stream of identical
		 * faults that buries the first one. */
		if (st->state != SERVICE_RUNNING)
			continue;

		st->polls++;
		rc = s->poll(s->ctx);
		if (rc == SERVICE_OK)
			continue;

		st->last_reason = rc;
		st->faults++;

		/* Bounded, and the bound is the point. Past it the service
		 * stays failed with the reason it failed for, and a person
		 * decides what happens next. See `service.h`. */
		if (st->restarts >= SERVICE_RESTARTS_MAX) {
			if (s->stop)
				s->stop(s->ctx);
			st->state = SERVICE_FAILED;
			continue;
		}

		if (s->stop)
			s->stop(s->ctx);

		st->restarts++;

		rc = s->start ? s->start(s->ctx) : SERVICE_OK;
		if (rc != SERVICE_OK) {
			/* It faulted and then would not come back. Failed
			 * rather than refused: it had been running, and the
			 * distinction is what tells somebody whether this is a
			 * thing that never worked or a thing that stopped. */
			st->last_reason = rc;
			st->state = SERVICE_FAILED;
			continue;
		}

		st->started_at = now;
	}
}

void supervisor_stop(struct supervisor *sup)
{
	unsigned i;

	if (!sup)
		return;

	/* Backwards. The last thing started is the most likely to be built on
	 * the others, so it is the first that should let go. */
	for (i = sup->count; i > 0; i--) {
		const struct service *s = sup->services[i - 1];
		struct service_status *st = &sup->status[i - 1];

		if (st->state != SERVICE_RUNNING)
			continue;
		if (s->stop)
			s->stop(s->ctx);
		st->state = SERVICE_STOPPED;
	}
}

void supervisor_tally(const struct supervisor *sup, unsigned *running,
                      unsigned *failed, unsigned *refused)
{
	unsigned i, r = 0, f = 0, x = 0;

	if (sup) {
		for (i = 0; i < sup->count; i++) {
			switch (sup->status[i].state) {
			case SERVICE_RUNNING:  r++; break;
			case SERVICE_FAILED:   f++; break;
			case SERVICE_REFUSED:  x++; break;
			case SERVICE_STOPPED:  break;
			}
		}
	}

	if (running)
		*running = r;
	if (failed)
		*failed = f;
	if (refused)
		*refused = x;
}

const char *service_state_name(enum service_state state)
{
	switch (state) {
	case SERVICE_STOPPED: return "stopped";
	case SERVICE_RUNNING: return "running";
	case SERVICE_REFUSED: return "refused";
	case SERVICE_FAILED:  return "failed";
	}

	/* Unreachable while the enumeration is complete above, and written
	 * anyway: a state added later without a case here would otherwise
	 * return whatever was on the stack, and a table full of nonsense is
	 * harder to trace than a table saying it does not know. */
	return "unknown";
}
