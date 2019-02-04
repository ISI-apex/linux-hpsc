#include <linux/err.h>
#include <linux/hpsc_msg.h>
#include <linux/hpsc_notif.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/spinlock.h>

// Messages may be sent or received during interrupts or other critical times
// when sleeping isn't allowed. Therefore, we must use a spinlock, not a mutex.
static DEFINE_SPINLOCK(notif_lock);
static struct notifier_block *handlers[HPSC_NOTIF_PRIORITY_COUNT];

int hpsc_notif_register(struct notifier_block *nb)
{
	int ret = 0;
	pr_info("hpsc-notif: registering handler type: %d\n", nb->priority);
	BUG_ON(nb->priority < 0 || nb->priority >= HPSC_NOTIF_PRIORITY_COUNT);
	spin_lock(&notif_lock);
	if (handlers[nb->priority])
		// device of this type already registered
		ret = -EBUSY;
	else
		handlers[nb->priority] = nb;
	spin_unlock(&notif_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(hpsc_notif_register);

void hpsc_notif_unregister(struct notifier_block *nb)
{
	pr_info("hpsc-notif: unregistering handler type: %d\n", nb->priority);
	BUG_ON(nb->priority < 0 || nb->priority >= HPSC_NOTIF_PRIORITY_COUNT);
	spin_lock(&notif_lock);
	if (handlers[nb->priority] == nb)
		handlers[nb->priority] = NULL;
	else
		pr_warn("hpsc-notif: handler does not match type\n");
	spin_unlock(&notif_lock);
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

// TODO: Fall back on other handlers on send error?
// TODO: Support retries on ret == -EAGAIN (by some policy)
int hpsc_notif_send(void *msg, size_t sz)
{
	struct notifier_block *nb;
	int i;
	int ret;
	pr_debug("hpsc-notif: send\n");
	BUG_ON(sz != HPSC_MSG_SIZE);
	spin_lock(&notif_lock);
	for (i = 0; i < HPSC_NOTIF_PRIORITY_COUNT; i++) {
		// handlers are ordered by preference
		nb = handlers[i];
		if (nb) {
			ret = nb->notifier_call(nb, 0, msg);
			pr_info("hpsc-notif: send: result = %d\n", ret);
			goto out;
		}
	}
	pr_err("hpsc-notif: send: no handlers available!\n");
	ret = -ENODEV;
out:
	spin_unlock(&notif_lock);
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
