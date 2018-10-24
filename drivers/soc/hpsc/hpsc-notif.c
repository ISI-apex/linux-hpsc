#include <linux/err.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/types.h>
#include <linux/hpsc_notif.h>

// callback functions for message types
// TODO: hardcode entries once we work out message processing
static void (*notif_cbs[HPSC_NOTIF_TYPE_COUNT])(u8 type, void *msg, size_t sz);

static LIST_HEAD(notif_cons);
static DEFINE_MUTEX(con_mutex);

int hpsc_notif_handler_register(struct hpsc_notif_handler *h)
{
	if (!h || !h->name || !h->msg_sz || !h->send)
		return -EINVAL;
	pr_info("HPSC Notif: registering handler: %s\n", h->name);
	mutex_lock(&con_mutex);
	list_add_tail(&h->node, &notif_cons);
	mutex_unlock(&con_mutex);
	return 0;
}
EXPORT_SYMBOL_GPL(hpsc_notif_handler_register);

void hpsc_notif_handler_unregister(struct hpsc_notif_handler *h)
{
	if (!h)
		return;
	pr_info("HPSC Notif: unregistering handler: %s\n", h->name);
	mutex_lock(&con_mutex);
	list_del(&h->node);
	mutex_unlock(&con_mutex);
}
EXPORT_SYMBOL_GPL(hpsc_notif_handler_unregister);

static void dump_msg(u8 type, void *msg, size_t sz)
{
	print_hex_dump_bytes("HPSC Notif rx", DUMP_PREFIX_ADDRESS, msg, sz);
}

int hpsc_notif_recv(struct hpsc_notif_handler *h, void *msg)
{
	u8 type;
	if (!h)
		return -EINVAL;
	// TODO: lock and ensure the handler is in our list and lock so it
	// can't be removed while we process
	pr_info("HPSC Notif: receive from: %s\n", h->name);
	// first 4 bytes are reserved (byte 0 is the message type)
	type = ((u8*) msg)[0];
	if (type == HPSC_NOTIF_INVALID || type >= HPSC_NOTIF_TYPE_COUNT) {
		// unknown/invalid message type
		pr_err("HPSC Notif: invalid message type: %d\n", (int) type);
		return -EINVAL;
	}
	BUG_ON(notif_cbs[type] == NULL);
	notif_cbs[type](type, msg, h->msg_sz);
	return 0;
}
EXPORT_SYMBOL_GPL(hpsc_notif_recv);

// In the future, use mailbox if available, fall back on other handlers
// TODO: Support retries on EAGAIN (by some policy)
int hpsc_notif_send(void *msg, size_t sz)
{
	struct hpsc_notif_handler *h;
	int ret = -ENODEV;
	pr_debug("HPSC Notif: send\n");
	BUG_ON(!msg);
	if (sz != HPSC_NOTIF_MSG_SIZE) {
		pr_err("HPSC Notif: send: sz != HPSC_NOTIF_MSG_SIZE\n");
		return -EINVAL;
	}
	mutex_lock(&con_mutex);
	if (list_empty(&notif_cons))
		// ret is -ENODEV
		pr_err("HPSC Notif: send: no handlers available!\n");
	else
		list_for_each_entry(h, &notif_cons, node)
			// TODO: hack, need a better way to choose handler
			if (h->msg_sz == sz) {
				pr_info("HPSC Notif: send to: %s\n", h->name);
				ret = h->send(msg);
				break;
			}
	mutex_unlock(&con_mutex);
	pr_info("HPSC Notif: send: result = %d\n", ret);
	return ret;
}
EXPORT_SYMBOL_GPL(hpsc_notif_send);

static int __init hpsc_notif_init(void) {
	int i;
	for (i = 0; i < HPSC_NOTIF_TYPE_COUNT; i++)
		notif_cbs[i] = dump_msg;
	pr_info("HPSC Notification module loaded\n");
	return 0;
}

static void __exit hpsc_notif_exit(void) {
	pr_info("HPSC Notification module unloaded\n");
}

MODULE_DESCRIPTION("HPSC Notification module");
MODULE_AUTHOR("HPSC");
MODULE_LICENSE("GPL v2");

module_init(hpsc_notif_init);
module_exit(hpsc_notif_exit);
