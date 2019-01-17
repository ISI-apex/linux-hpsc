#include <linux/slab.h>
#include "interval_timer.h"

static struct interval_timer_block itmr_blocks = {
	.list = LIST_HEAD_INIT(itmr_blocks.list),
};

void interval_timer_block_init(struct interval_timer_block *itmr_block,
			       struct interval_timer_block_ops *ops)
{
	BUG_ON(!ops->of_xlate);
	itmr_block->ops = ops;
	itmr_block->node = NULL;
	INIT_LIST_HEAD(&itmr_block->list);
}

void interval_timer_init(struct interval_timer *itmr,
			 struct interval_timer_ops *ops)
{
	itmr->ops = ops;
	INIT_LIST_HEAD(&itmr->callbacks.list);
}

void interval_timer_block_register(struct interval_timer_block *itmr_block,
				   struct device_node *node)
{
	itmr_block->node = node;
	list_add_tail(&itmr_block->list, &itmr_blocks.list);
}

void interval_timer_block_unregister(struct interval_timer_block *itmr_block)
{
	list_del(&itmr_block->list);
	itmr_block->node = NULL;
}

struct interval_timer *interval_timer_lookup(const struct of_phandle_args *spec)
{
	// TODO: lock against unregister? or assume this is for init code only?
	struct interval_timer_block *itmr_block;
	list_for_each_entry(itmr_block, &itmr_blocks.list, list) {
		if (itmr_block->node == spec->np)
			return itmr_block->ops->of_xlate(itmr_block, spec);
	}
	return NULL;
}

struct interval_timer_cb *interval_timer_subscribe(struct interval_timer *itmr,
					void (*func)(void *cb_arg), void *arg)
{
	struct interval_timer_cb *cb;

	cb = kmalloc(sizeof(struct interval_timer_cb), GFP_KERNEL);
	if (!cb)
		return NULL;
	cb->func = func;
	cb->arg = arg;
	list_add_tail(&cb->list, &itmr->callbacks.list);
	return cb;
}

void interval_timer_unsubscribe(struct interval_timer_cb *cb)
{
	list_del(&cb->list);
	kfree(cb);
}

void interval_timer_notify(struct interval_timer *itmr)
{
	// TODO: lock from unsubscribe (is there a way to call the cb unlocked, tho?)
	struct interval_timer_cb *cb;
	list_for_each_entry(cb, &itmr->callbacks.list, list) {
		BUG_ON(!cb->func);
		cb->func(cb->arg);
	}
}
