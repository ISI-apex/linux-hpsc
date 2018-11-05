#include <linux/delay.h>
#include <linux/err.h>
#include <linux/hpsc_msg.h>
#include <linux/hpsc_notif.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/notifier.h>

#define RETRIES_DEFAULT 10
static unsigned int retries = RETRIES_DEFAULT;
module_param(retries, uint, 0);
MODULE_PARM_DESC(retries,
	"Number of retry attempts, default=" __MODULE_STRING(RETRIES_DEFAULT));

#define RETRY_DELAY_US 100
static unsigned long retry_delay_us = RETRY_DELAY_US;
module_param(retry_delay_us, ulong, 0);
MODULE_PARM_DESC(retries,
	"Microsecond delay between retries, default="
	__MODULE_STRING(RETRY_DELAY_US));

static ATOMIC_NOTIFIER_HEAD(notif_handlers);


int hpsc_notif_register(struct notifier_block *nb)
{
	pr_info("hpsc-notif: registering handler type: %d\n", nb->priority);
	return atomic_notifier_chain_register(&notif_handlers, nb);
}
EXPORT_SYMBOL_GPL(hpsc_notif_register);

int hpsc_notif_unregister(struct notifier_block *nb)
{
	pr_info("hpsc-notif: unregistering handler type: %d\n", nb->priority);
	return atomic_notifier_chain_unregister(&notif_handlers, nb);
}
EXPORT_SYMBOL_GPL(hpsc_notif_unregister);

int hpsc_notif_recv(const void *msg, size_t sz)
{
	// We don't actually need any locking here, making it easy for message
	// processing to send response (or new) messages before returning here.
	pr_debug("hpsc-notif: receive\n");
	BUG_ON(sz != HPSC_MSG_SIZE);
	return hpsc_msg_process(msg, sz);
}
EXPORT_SYMBOL_GPL(hpsc_notif_recv);

int hpsc_notif_send(void *msg, size_t sz)
{
	unsigned int i;
	int nr_calls = 0;
	int ret;
	pr_debug("hpsc-notif: send\n");
	BUG_ON(sz != HPSC_MSG_SIZE);
	for (i = 0; i <= retries; i++) {
		ret = __atomic_notifier_call_chain(&notif_handlers, 0, msg, -1,
						   &nr_calls);
		if (ret == NOTIFY_STOP)
			// normal behavior
			return 0;
		if (!nr_calls) {
			pr_err("hpsc-notif: send: no handlers available!\n");
			ret = -ENODEV;
			break;
		}
		if (ret != (NOTIFY_STOP_MASK & EAGAIN)) {
			pr_err("hpsc-notif: send: failed: %d\n", ret);
			break;
		}
		if (i < retries) {
			pr_info("hpsc-notif: send: retry %u in %lu us...\n",
				i + 1, retry_delay_us);
			udelay(retry_delay_us);
		} else {
			pr_err("hpsc-notif: send: retries exhausted\n");
		}
	}
	return ret;
}
EXPORT_SYMBOL_GPL(hpsc_notif_send);

static int __init hpsc_notif_init(void)
{
	pr_info("hpsc-notif: init\n");
	return 0;
}

static void __exit hpsc_notif_exit(void)
{
	pr_info("hpsc-notif: exit\n");
}

MODULE_DESCRIPTION("HPSC notification module");
MODULE_AUTHOR("Connor Imes <cimes@isi.edu>");
MODULE_LICENSE("GPL v2");

module_init(hpsc_notif_init);
module_exit(hpsc_notif_exit);
