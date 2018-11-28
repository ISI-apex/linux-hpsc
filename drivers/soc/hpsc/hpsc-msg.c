#include <linux/kernel.h>
#include <linux/types.h>
#include "hpsc_msg.h"
#include "hpsc_notif.h"

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

static int msg_cb_nop(const u8 *msg)
{
	pr_info("hpsc-msg: received NOP\n");
	return 0;
}

static int msg_cb_ping(const u8 *msg)
{
	HPSC_MSG_DEFINE(res);
	pr_info("hpsc-msg: received PING, replying with PONG\n");
	// reply with pong and echo payload back
	memcpy(res, msg, HPSC_MSG_SIZE);
	res[0] = PONG;
	hpsc_notif_send(res, sizeof(res));
	return 0;
}

static int msg_cb_pong(const u8 *msg)
{
	pr_info("hpsc-msg: received PONG\n");
	return 0;
}

static int msg_cb_drop(const u8 *msg)
{
	pr_warn("hpsc-msg: Unsupported/unimplemented type: %x\n", msg[0]);
	return 0;
}

/**
 * Callback functions for message types.
 *
 * @param msg Message pointer
 * @return 0 on success, a negative value on error
 */
static int (* const msg_cbs[HPSC_MSG_TYPE_COUNT])(const u8 *msg) = {
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
	u8 t = ((const u8*) msg)[0];
	BUG_ON(sz != HPSC_MSG_SIZE);
	print_hex_dump_bytes("hpsc_msg_process", DUMP_PREFIX_ADDRESS, msg, sz);
	if (t >= HPSC_MSG_TYPE_COUNT) {
		pr_err("hpsc-msg: invalid message type: %x\n", t);
		return -EINVAL;
	}
	return msg_cbs[t](msg);
}
EXPORT_SYMBOL_GPL(hpsc_msg_process);
