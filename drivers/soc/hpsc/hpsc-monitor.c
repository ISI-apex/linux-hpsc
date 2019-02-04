/*
 * Module for registering listeners into other parts of the kernel.
 *
 * Currently monitors:
 * -kernel oops
 * -system lifecycle
 */
#include <linux/kdebug.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/reboot.h>
#include <linux/watchdog.h>
#include <linux/watchdog_pretimeout_notifier.h>
#include "hpsc_msg.h"

static int hpsc_monitor_shutdown(struct notifier_block *nb,
				 unsigned long action, void *data)
{
	if (hpsc_msg_lifecycle(LIFECYCLE_DOWN, "%lu", action))
		return NOTIFY_BAD;
	return NOTIFY_OK;
}

static struct notifier_block hpsc_monitor_shutdown_nb = {
	.notifier_call = hpsc_monitor_shutdown
};

static int hpsc_monitor_die(struct notifier_block *nb, unsigned long action,
			    void *data)
{
	struct die_args *args = data;
	if (hpsc_msg_lifecycle(LIFECYCLE_DOWN, "%lu|%s|%ld|%d|%d",
			       action, args->str, args->err, args->trapnr,
			       args->signr))
		return NOTIFY_BAD;
	return NOTIFY_OK;
}

static struct notifier_block hpsc_monitor_die_nb = {
	.notifier_call = hpsc_monitor_die
};

static int hpsc_monitor_panic(struct notifier_block *nb, unsigned long action,
			      void *data)
{
	if (hpsc_msg_lifecycle(LIFECYCLE_DOWN, (char*) data))
		return NOTIFY_BAD;
	return NOTIFY_OK;
}

static struct notifier_block hpsc_monitor_panic_nb = {
	.notifier_call = hpsc_monitor_panic
};

static int hpsc_monitor_wdt(struct notifier_block *nb, unsigned long action,
			    void *data)
{
	hpsc_msg_wdt_timeout(action);
	pr_crit("hpsc_monitor_wdt: initiating poweroff\n");
	orderly_poweroff(true);
	// if we get this far, then poweroff failed
	return NOTIFY_BAD;
}

static struct notifier_block hpsc_monitor_wdt_nb = {
	.notifier_call = hpsc_monitor_wdt
};

static int hpsc_monitor_up(void)
{
	return hpsc_msg_lifecycle(LIFECYCLE_UP, NULL);
}

static int __init hpsc_monitor_init(void)
{
	pr_info("hpsc-monitor: init\n");
	// Note: Both the oops (die) and panic handlers may run - if this is a
	// problem, track an atomic status variable to only send one message
	// oops handler
	register_die_notifier(&hpsc_monitor_die_nb);
	// panic handler
	atomic_notifier_chain_register(&panic_notifier_list,
				       &hpsc_monitor_panic_nb);
	// failure is ok - the HW watchdog will reset us eventually
	if (watchdog_pretimeout_notifier_register(&hpsc_monitor_wdt_nb))
		pr_warn("hpsc-monitor: failed to register watchdog notifier - "
			"'CONFIG_WATCHDOG_PRETIMEOUT_DEFAULT_GOV_NOTIFIER' "
			"not set?");
#if 0	// test oops
	*(int*)0 = 0;
#endif
	// normal shutdown handlers
	register_reboot_notifier(&hpsc_monitor_shutdown_nb);
	register_restart_handler(&hpsc_monitor_shutdown_nb);
	// TODO: At this point, the system can't really be considered "up".
	// Is there a notifier we can listen on instead?
	hpsc_monitor_up();
	return 0;
}

static void __exit hpsc_monitor_exit(void)
{
	pr_info("hpsc-monitor: exit\n");
	watchdog_pretimeout_notifier_unregister(&hpsc_monitor_wdt_nb);
	unregister_restart_handler(&hpsc_monitor_shutdown_nb);
	unregister_reboot_notifier(&hpsc_monitor_shutdown_nb);
	unregister_die_notifier(&hpsc_monitor_die_nb);
}

MODULE_DESCRIPTION("HPSC kernel monitoring module");
MODULE_AUTHOR("Connor Imes <cimes@isi.edu>");
MODULE_LICENSE("GPL v2");

// module_init(hpsc_monitor_init);
late_initcall(hpsc_monitor_init);
module_exit(hpsc_monitor_exit);
