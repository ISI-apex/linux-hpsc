#include <linux/err.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/hpsc_msg.h>
#include <linux/hpsc_notif.h>

// Messages may be sent or received during interrupts or other critical times
// where sleeping isn't allowed. Therefore, we must use a spinlock, not a mutex.
static DEFINE_SPINLOCK(notif_lock);
static struct hpsc_notif_handler *handlers[HPSC_NOTIF_HANDLER_COUNT];

int hpsc_notif_handler_register(struct hpsc_notif_handler *h)
{
	int ret = 0;
	if (!h || h->type >= HPSC_NOTIF_HANDLER_COUNT || !h->name || !h->send)
		return -EINVAL;
	pr_info("HPSC Notif: registering handler: %s\n", h->name);
	spin_lock(&notif_lock);
	if (handlers[h->type])
		// device of this type already registered
		ret = -EBUSY;
	else
		handlers[h->type] = h;
	spin_unlock(&notif_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(hpsc_notif_handler_register);

void hpsc_notif_handler_unregister(struct hpsc_notif_handler *h)
{
	if (!h || h->type >= HPSC_NOTIF_HANDLER_COUNT)
		return;
	pr_info("HPSC Notif: unregistering handler: %s\n", h->name);
	spin_lock(&notif_lock);
	handlers[h->type] = NULL;
	spin_unlock(&notif_lock);
}
EXPORT_SYMBOL_GPL(hpsc_notif_handler_unregister);

// TODO: Fall back on other handlers on send error?
// TODO: Support retries on ret == -EAGAIN (by some policy)
// Callers are responsible for holding the spinlock
static int __hpsc_notif_send(void *msg)
{
	struct hpsc_notif_handler *h;
	int i;
	BUG_ON(!msg);
	for (i = 0; i < HPSC_NOTIF_HANDLER_COUNT; i++) {
		// handlers are ordered by preference
		h = handlers[i];
		if (h) {
			pr_info("HPSC Notif: send to: %s\n", h->name);
			return h->send(h, msg);
		}
	}
	pr_err("HPSC Notif: send: no matching handlers!\n");
	return -ENODEV;
}

static void msgcpy(void *dest, void *src, size_t sz)
{
	// Note: can't use memcpy if mailbox was source
	size_t i;
	for (i = 0; i < sz; i++)
		((u8 *)dest)[i] = ((u8 *)src)[i];
}

static int hpsc_msg_nop(enum hpsc_msg_type t, void *msg, size_t sz, void *res)
{
	BUG_ON(t != NOP);
	pr_info("HPSC Notif: received NOP\n");
	return 0;
}

static int hpsc_msg_ping(enum hpsc_msg_type t, void *msg, size_t sz, void *res)
{
	BUG_ON(t != PING);
	pr_info("HPSC Notif: received PING, replying with PONG\n");
	// reply with pong and echo payload back
	((unsigned char*)res)[0] = (unsigned char) PONG;
	msgcpy(res + 1, msg + 1, sz - 1);
	return 1;
}

static int hpsc_msg_pong(enum hpsc_msg_type t, void *msg, size_t sz, void *res)
{
	BUG_ON(t != PONG);
	pr_info("HPSC Notif: received PONG\n");
	return 0;
}

static int hpsc_msg_drop(enum hpsc_msg_type t, void *msg, size_t sz, void *res)
{
	pr_warn("HPSC Notif: Unsupported/unimplemented message type: %d\n", t);
	print_hex_dump_bytes("HPSC Notif rx", DUMP_PREFIX_ADDRESS, msg, sz);
	return 0;
}

/**
 * Callback functions for message types.
 *
 * @param t Message type
 * @param msg Message pointer
 * @param sz Message size
 * @param res An available response buffer
 * @return a positive value to send a response, a negative value on error,
 *         0 otherwise
 */
static int (* const notif_cbs[HPSC_MSG_TYPE_COUNT])(enum hpsc_msg_type t,
						    void *msg, size_t sz,
						    void *res) = {
	hpsc_msg_nop,		// NOP
	hpsc_msg_ping,		// PING
	hpsc_msg_pong,		// PONG
	hpsc_msg_drop,		// READ_VALUE
	hpsc_msg_drop,		// WRITE_VALUE
	hpsc_msg_drop,		// READ_FILE
	hpsc_msg_drop,		// WRITE_FILE
	hpsc_msg_drop,		// READ_PROP
	hpsc_msg_drop,		// WRITE_PROP
	hpsc_msg_drop,		// READ_ADDR
	hpsc_msg_drop,		// WRITE_ADDR
	hpsc_msg_drop,		// WATCHDOG_TIMEOUT
	hpsc_msg_drop,		// FAULT
	hpsc_msg_drop,		// ACTION
};

int hpsc_notif_recv(struct hpsc_notif_handler *h, void *msg, size_t sz)
{
	u8 res[HPSC_MSG_SIZE];
	enum hpsc_msg_type t;
	int ret;
	BUG_ON(!h);
	BUG_ON(sz != HPSC_MSG_SIZE);
	// TODO: lock and ensure the handler is in our list and lock so it
	// can't be removed while we process?
	pr_info("HPSC Notif: receive from: %s\n", h->name);
	// first 4 bytes are reserved (byte 0 is the message type)
	t = (enum hpsc_msg_type) ((u8*) msg)[0];
	if (t >= HPSC_MSG_TYPE_COUNT) {
		// unknown/invalid message type
		pr_err("HPSC Notif: invalid message type: %d\n", t);
		return -EINVAL;
	}
	ret = notif_cbs[t](t, msg, sz, res);
	if (ret > 0) {
		pr_info("HPSC Notif: sending response\n");
		ret = __hpsc_notif_send(res);
		if (ret)
			pr_err("HPSC Notif: sending response failed\n");
	} else if (ret < 0) {
		pr_err("HPSC Notif: failed to process message\n");
	}
	return ret;
}
EXPORT_SYMBOL_GPL(hpsc_notif_recv);

int hpsc_notif_send(void *msg, size_t sz)
{
	int ret;
	pr_debug("HPSC Notif: send\n");
	BUG_ON(!msg);
	BUG_ON(sz != HPSC_MSG_SIZE);
	spin_lock(&notif_lock);
	ret = __hpsc_notif_send(msg);
	spin_unlock(&notif_lock);
	pr_info("HPSC Notif: send: result = %d\n", ret);
	return ret;
}
EXPORT_SYMBOL_GPL(hpsc_notif_send);

static int __init hpsc_notif_init(void) {
	int i;
	for (i = 0; i < HPSC_MSG_TYPE_COUNT; i++)
		BUG_ON(notif_cbs[i] == NULL);
	pr_info("HPSC Notification module loaded\n");
	return 0;
}

static void __exit hpsc_notif_exit(void) {
	pr_info("HPSC Notification module unloaded\n");
}

MODULE_DESCRIPTION("HPSC Notification module");
MODULE_AUTHOR("Connor Imes <cimes@isi.edu>");
MODULE_LICENSE("GPL v2");

module_init(hpsc_notif_init);
module_exit(hpsc_notif_exit);
