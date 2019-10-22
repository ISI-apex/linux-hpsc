/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Register/unregister notifiers to be used on watchdog pretimeout when the
 * pretimeout_notifier governor is used.
 *
 * Connor Imes <cimes@isi.edu>
 */
 
#ifndef _LINUX_WATCHDOG_PRETIMEOUT_NOTIFIER_H
#define _LINUX_WATCHDOG_PRETIMEOUT_NOTIFIER_H

#include <linux/kernel.h>
#include <linux/notifier.h>

#if IS_ENABLED(CONFIG_WATCHDOG_PRETIMEOUT_GOV_NOTIFIER)

int watchdog_pretimeout_notifier_register(struct notifier_block *nb);

int watchdog_pretimeout_notifier_unregister(struct notifier_block *nb);

#else

static inline
int watchdog_pretimeout_notifier_register(struct notifier_block *nb)
{
	return -ENODEV;
}

static inline
int watchdog_pretimeout_notifier_unregister(struct notifier_block *nb)
{
	return 0;
}

#endif /* IS_ENABLED(CONFIG_WATCHDOG_PRETIMEOUT_GOV_NOTIFIER) */

#endif /* _LINUX_WATCHDOG_PRETIMEOUT_NOTIFIER_H */
