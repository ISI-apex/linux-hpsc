/*
 * Module for registering listeners into other parts of the kernel.
 *
 * Currently monitors:
 * -kernel oops
 * -system lifecycle
 */
#include <linux/hpsc_msg.h>
#include <linux/kdebug.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/reboot.h>

#define LIFECYCLE_INFO_SIZE FIELD_SIZEOF(struct hpsc_msg_lifeycle_payload, info)

static int hpsc_monitor_shutdown(struct notifier_block *nb,
				 unsigned long action, void *data)
{
	char info[LIFECYCLE_INFO_SIZE];
	snprintf(info, sizeof(info), "%lu", action);
	if (hpsc_msg_lifecycle(LIFECYCLE_DOWN, info))
		return NOTIFY_BAD;
	return NOTIFY_OK;
}

static struct notifier_block hpsc_monitor_shutdown_nb = {
	.notifier_call = hpsc_monitor_shutdown
};

static int hpsc_monitor_die(struct notifier_block *nb, unsigned long action,
			    void *data)
{
	char info[LIFECYCLE_INFO_SIZE];
	struct die_args *args = data;
	snprintf(info, sizeof(info), "%lu|%s|%ld|%d|%d",
		 action, args->str, args->err, args->trapnr, args->signr);
	if (hpsc_msg_lifecycle(LIFECYCLE_DOWN, args->str))
		return NOTIFY_BAD;
	return NOTIFY_OK;
}

static struct notifier_block hpsc_monitor_die_nb = {
	.notifier_call = hpsc_monitor_die
};

static int hpsc_monitor_up(void)
{
	return hpsc_msg_lifecycle(LIFECYCLE_UP, NULL);
}

static int __init hpsc_monitor_init(void)
{
	pr_info("hpsc-monitor: init\n");
	// oops handler
	register_die_notifier(&hpsc_monitor_die_nb);
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
