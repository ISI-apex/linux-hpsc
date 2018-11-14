/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Notify registered listeners on watchdog pretimeout.
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/watchdog.h>
#include <linux/watchdog_pretimeout_notifier.h>
#include "watchdog_pretimeout.h"

static ATOMIC_NOTIFIER_HEAD(pretimeout_notifiers);

int watchdog_pretimeout_notifier_register(struct notifier_block *nb)
{
	return atomic_notifier_chain_register(&pretimeout_notifiers, nb);
}

int watchdog_pretimeout_notifier_unregister(struct notifier_block *nb)
{
	return atomic_notifier_chain_unregister(&pretimeout_notifiers, nb);
}

static void pretimeout_notifier(struct watchdog_device *wdd)
{
	atomic_notifier_call_chain(&pretimeout_notifiers, wdd->id, wdd);
}

static struct watchdog_governor watchdog_gov_notifier = {
	.name		= "notifier",
	.pretimeout	= pretimeout_notifier,
};

static int __init watchdog_gov_notifier_init(void)
{
	return watchdog_register_governor(&watchdog_gov_notifier);
}

static void __exit watchdog_gov_notifier_exit(void)
{
	watchdog_unregister_governor(&watchdog_gov_notifier);
}

module_init(watchdog_gov_notifier_init);
module_exit(watchdog_gov_notifier_exit);

MODULE_AUTHOR("Connor Imes <cimes@isi.edu>");
MODULE_DESCRIPTION("Notifier watchdog pretimeout governor");
MODULE_LICENSE("GPL");
