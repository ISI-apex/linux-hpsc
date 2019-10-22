/*
 * Module for registering listeners into other parts of the kernel.
 *
 * Currently monitors:
 * -watchdog pretimeouts
 * -kernel panic
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
	static atomic_t is_in_poweroff = ATOMIC_INIT(0);
	hpsc_msg_wdt_timeout(action);
	if (atomic_cmpxchg(&is_in_poweroff, 0, 1)) {
		pr_crit("hpsc_monitor_wdt: poweroff already in progress\n");
	} else {
		pr_crit("hpsc_monitor_wdt: initiating poweroff\n");
		orderly_poweroff(true);
		// if we get this far, then poweroff failed
		// let another thread retry on timeout, or wait for HW WDT reset
		atomic_set(&is_in_poweroff, 1);
		return NOTIFY_BAD;
	}
	return NOTIFY_OK;
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
			"'CONFIG_WATCHDOG_PRETIMEOUT_GOV_NOTIFIER' not set?");
#if 0	// test oops
	*(int*)0 = 0;
#endif
	// normal shutdown handlers
	register_reboot_notifier(&hpsc_monitor_shutdown_nb);
	register_restart_handler(&hpsc_monitor_shutdown_nb);
	// as close as we can get to the system being "up"
	hpsc_monitor_up();
	return 0;
}

static void __exit hpsc_monitor_exit(void)
{
	pr_info("hpsc-monitor: exit\n");
	unregister_restart_handler(&hpsc_monitor_shutdown_nb);
	unregister_reboot_notifier(&hpsc_monitor_shutdown_nb);
	watchdog_pretimeout_notifier_unregister(&hpsc_monitor_wdt_nb);
	atomic_notifier_chain_unregister(&panic_notifier_list,
					 &hpsc_monitor_panic_nb);
	unregister_die_notifier(&hpsc_monitor_die_nb);
}

MODULE_DESCRIPTION("HPSC kernel monitoring module");
MODULE_AUTHOR("Connor Imes <cimes@isi.edu>");
MODULE_LICENSE("GPL v2");

late_initcall(hpsc_monitor_init);
module_exit(hpsc_monitor_exit);
