#include <linux/hpsc_msg.h>
#include <linux/hpsc_notif.h>
#include <linux/kernel.h>
#include <linux/types.h>

static int msg_send(enum hpsc_msg_type t, const void *payload, size_t psz)
{
	// create message buffer, populate it, then send
	HPSC_MSG_DEFINE(msg);
	BUG_ON(psz > HPSC_MSG_PAYLOAD_SIZE);
	msg[0] = t;
	if (payload)
		memcpy(&msg[HPSC_MSG_PAYLOAD_OFFSET], payload, psz);
	print_hex_dump_bytes("msg_send", DUMP_PREFIX_ADDRESS, msg,
			     HPSC_MSG_SIZE);
	return hpsc_notif_send(msg, sizeof(msg));
}

int hpsc_msg_wdt_timeout(unsigned int cpu)
{
	// payload is the ID of the CPU that timed out
	pr_info("hpsc_msg_wdt_timeout: %u\n", cpu);
	return msg_send(WATCHDOG_TIMEOUT, &cpu, sizeof(cpu));
}
EXPORT_SYMBOL_GPL(hpsc_msg_wdt_timeout);

#define LIFECYCLE_INFO_SIZE FIELD_SIZEOF(struct hpsc_msg_lifeycle_payload, info)
int hpsc_msg_lifecycle(enum hpsc_msg_lifecycle_status status, const char *fmt, ...)
{
	// payload is the status enumeration and a string of debug data
	va_list args;
	struct hpsc_msg_lifeycle_payload p = {
		.status = status,
		.info = {0}
	};
	if (fmt) {
		va_start(args, fmt);
		vsnprintf(p.info, LIFECYCLE_INFO_SIZE - 1, fmt, args);
		va_end(args);
	}
	pr_info("hpsc_msg_lifecycle: %d: %s\n", p.status, p.info);
	return msg_send(LIFECYCLE, &p, sizeof(p));
}
EXPORT_SYMBOL_GPL(hpsc_msg_lifecycle);

/*
 * The remainder of this file is for processing received messages.
 */

static void msgcpy(unsigned char *dest, const unsigned char *src)
{
	// Note: can't use memcpy if mailbox was source; copy a word at a time
	size_t i;
	BUG_ON(HPSC_MSG_SIZE % sizeof(u32) != 0);
	for (i = 0; i < HPSC_MSG_SIZE / sizeof(u32); i++)
		((u32 *)dest)[i] = ((u32 *)src)[i];
}

static int msg_cb_nop(const unsigned char *msg)
{
	pr_info("hpsc-msg: received NOP\n");
	return 0;
}

static int msg_cb_ping(const unsigned char *msg)
{
	HPSC_MSG_DEFINE(res);
	pr_info("hpsc-msg: received PING, replying with PONG\n");
	// reply with pong and echo payload back
	msgcpy(res, msg);
	res[0] = PONG;
	hpsc_notif_send(res, sizeof(res));
	return 0;
}

static int msg_cb_pong(const unsigned char *msg)
{
	pr_info("hpsc-msg: received PONG\n");
	return 0;
}

static int msg_cb_drop(const unsigned char *msg)
{
	pr_warn("hpsc-msg: Unsupported/unimplemented type: %d\n", (int) msg[0]);
	print_hex_dump_bytes("hpsc-msg", DUMP_PREFIX_ADDRESS, msg,
			     HPSC_MSG_SIZE);
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
static int (* const msg_cbs[HPSC_MSG_TYPE_COUNT])(const unsigned char *msg) = {
	msg_cb_nop,		// NOP
	msg_cb_ping,		// PING
	msg_cb_pong,		// PONG
	msg_cb_drop,		// READ_VALUE
	msg_cb_drop,		// WRITE_VALUE
	msg_cb_drop,		// READ_FILE
	msg_cb_drop,		// WRITE_FILE
	msg_cb_drop,		// READ_PROP
	msg_cb_drop,		// WRITE_PROP
	msg_cb_drop,		// READ_ADDR
	msg_cb_drop,		// WRITE_ADDR
	msg_cb_drop,		// WATCHDOG_TIMEOUT
	msg_cb_drop,		// FAULT
	msg_cb_drop,		// LIFECYCLE
	msg_cb_drop,		// ACTION
};

int hpsc_msg_process(const void *msg, size_t sz)
{
	// first 4 bytes are reserved (byte 0 is the message type)
	int t = ((const unsigned char*) msg)[0];
	BUG_ON(sz != HPSC_MSG_SIZE);
	if (t < 0 || t >= HPSC_MSG_TYPE_COUNT) {
		// unknown/invalid message type
		pr_err("hpsc-msg: invalid message type: %d\n", t);
		return -EINVAL;
	}
	return msg_cbs[t](msg);
}
EXPORT_SYMBOL_GPL(hpsc_msg_process);
