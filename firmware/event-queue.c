#include <stdlib.h>
#include <errno.h>
#include <util/atomic.h>

#include "event-queue.h"
#include "utils.h"
#include "sleep.h"
#include "timer.h"
#include "gpio.h"

struct event {
	struct event *next;

	const void *source;
	uint8_t id;
	union event_val val;
};

#define MAX_PENDING_EVENTS 8

static struct event * volatile events;
static struct event * volatile events_tail;
static struct event events_storage[MAX_PENDING_EVENTS];

static struct event_handler * volatile handlers;

static uint8_t life_gpio;

int8_t event_handler_add(struct event_handler *hdlr)
{
	if (!hdlr || !hdlr->source || !hdlr->handler)
		return -EINVAL;

	ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
		hdlr->next = handlers;
		handlers = hdlr;
	}

	return 0;
}

int8_t event_handler_remove(struct event_handler *hdlr)
{
	int8_t ret = -ENOENT;

	if (!hdlr)
		return -EINVAL;

	ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
		if (hdlr == handlers) {
			handlers = hdlr->next;
			ret = 0;
		} else {
			struct event_handler *h;

			for (h = handlers ; h->next ; h = h->next)
				if (h->next == hdlr) {
					h->next = hdlr->next;
					ret = 0;
					break;
				}
		}
		hdlr->next = NULL;
	}

	return ret;
}

int8_t event_add(const void *source, uint8_t id, union event_val val)
{
	struct event *ev = NULL;
	uint8_t i;

	/* The source is mendatory */
	if (!source)
		return -EINVAL;

	ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
		/* Find a free event in the storage */
		for (i = 0; i < ARRAY_SIZE(events_storage); i++)
			if (!events_storage[i].source) {
				ev = &events_storage[i];
				break;
			}
		if (ev) {
			/* Fill the event */
			ev->next = NULL;
			ev->source = source;
			ev->id = id;
			ev->val = val;

			/* Add it to the tail */
			if (events_tail)
				events_tail->next = ev;
			else
				events = ev;
			events_tail = ev;
		}
	}

	return ev ? 0 : -ENOMEM;
}

int8_t event_remove(const void *source, uint8_t id)
{
	struct event *ev, *prev, *next;

	/* The source is mendatory */
	if (!source)
		return -EINVAL;

	ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
		for (ev = events, prev = NULL; ev ; ev = next) {
			next = ev->next;
			if (ev->source != source || ev->id != id) {
				prev = ev;
				continue;
			}
			if (prev)
				prev->next = next;
			else
				events = next;
			if (!next)
				events_tail = prev;
			ev->next = NULL;
			ev->source = NULL;
		}
	}

	return 0;
}

static void event_run_handlers(struct event *ev)
{
	static struct event_handler *hdlr;

	for (hdlr = handlers; hdlr ; hdlr = hdlr->next) {
		if (hdlr->source != ev->source)
			continue;
		if (hdlr->mask && (ev->id & hdlr->mask) != hdlr->id)
			continue;
		hdlr->handler(ev->id, ev->val, hdlr->context);
	}
}

static void event_loop_run_once(void)
{
	struct event *ev;

	/* Get the list's head */
	ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
		if ((ev = events)) {
			/* Remove it from the queue */
			events = events->next;
			if (!events)
				events_tail = NULL;
			ev->next = NULL;
		}
	}

	/* Run all the handlers */
	if (ev) {
		event_run_handlers(ev);
		ev->source = NULL;
	}
}

void _sleep_prepare(void)
{
	timers_sleep();
	gpio_set_value(life_gpio, 0);
}

void _sleep_finish(void)
{
	gpio_set_value(life_gpio, 1);
	timers_wakeup();
}

void event_loop_run(uint8_t gpio)
{
	life_gpio = gpio;
	gpio_direction_output(life_gpio, 1);
	while (1) {
		event_loop_run_once();
		/* Sleep if no event is pending */
		sleep_if(!events);
	}
	gpio_set_value(life_gpio, 0);
}
