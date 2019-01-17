#ifndef _CLOCKSOURCE_INTERVAL_TIMER_H
#define _CLOCKSOURCE_INTERVAL_TIMER_H
/*
 * An interface for timers that can provide a periodic callback.
 */

#include <linux/of.h>
#include <linux/list.h>

struct interval_timer_block;
struct interval_timer;

struct interval_timer_block_ops {
	struct interval_timer * (*of_xlate)(struct interval_timer_block *,
					    const struct of_phandle_args *);
};

struct interval_timer_ops {
	int (*set_interval)(struct interval_timer *, uint64_t interval);
	int (*capture)(struct interval_timer *, uint64_t *count);
};

struct interval_timer_cb {
	struct list_head list;

	void (*func)(void *cb_arg);
	void *arg;
};

struct interval_timer_block
{
	struct list_head list;
	struct device_node *node;
	struct interval_timer_block_ops *ops;
};

struct interval_timer
{
	struct interval_timer_ops *ops;
	struct interval_timer_cb callbacks;
};

void interval_timer_block_init(struct interval_timer_block *itmr_block,
			       struct interval_timer_block_ops *ops);
void interval_timer_init(struct interval_timer *itmr,
			 struct interval_timer_ops *ops);

void interval_timer_block_register(struct interval_timer_block *itmr_block,
				   struct device_node *node);
void interval_timer_block_unregister(struct interval_timer_block *itmr_block);
struct interval_timer *interval_timer_lookup(const struct of_phandle_args *spec);

struct interval_timer_cb *interval_timer_subscribe(struct interval_timer *itmr,
					void (*func)(void *cb_arg), void *arg);
void interval_timer_unsubscribe(struct interval_timer_cb *cb);
void interval_timer_notify(struct interval_timer *itmr);

#endif // _CLOCKSOURCE_INTERVAL_TIMER_H

