/*
 * HPSC messaging interface.
 * Provides helper functions to send different types of messages.
 * Also provides a callback function for processing received messages.
 */
#ifndef __HPSC_MSG_H
#define __HPSC_MSG_H

#include <linux/types.h>

#define HPSC_MSG_SIZE 64
#define HPSC_MSG_PAYLOAD_OFFSET 4
#define HPSC_MSG_PAYLOAD_SIZE (HPSC_MSG_SIZE - 4)

#define HPSC_MSG_DEFINE(name) u8 name[HPSC_MSG_SIZE] = {0}

// Message type enumeration
enum hpsc_msg_type {
	// Value 0 is reserved so empty messages can be recognized
	NOP = 0,
	// test messages
	PING,
	PONG,
	// responses - payload contains ID of the response being acknowledged
	READ_VALUE,
	WRITE_STATUS,
	// general operations
	READ_FILE,
	WRITE_FILE,
	READ_PROP,
	WRITE_PROP,
	READ_ADDR,
	WRITE_ADDR,
	// notifications
	WATCHDOG_TIMEOUT,
	FAULT,
	LIFECYCLE,
	// an enumerated/predefined action
	ACTION,
	// enum counter
	HPSC_MSG_TYPE_COUNT
};

enum hpsc_msg_lifecycle_status {
	LIFECYCLE_UP,
	LIFECYCLE_DOWN
};

// info is for debugging, use real data types if we need more detail
struct hpsc_msg_lifeycle_payload {
	u32 status;
	char info[HPSC_MSG_PAYLOAD_SIZE - sizeof(u32)];
};

/**
 * Send a message that a watchdog timed out.
 *
 * @param cpu The CPU whose watchdog timed out
 * @return 0 on success, a negative error code otherwise
 */
int hpsc_msg_wdt_timeout(unsigned int cpu);

/**
 * Send a message indicating a change in lifecycle
 *
 * @param status The new status
 * @param fmt Optional extra info
 * @return 0 on success, a negative error code otherwise
 */
int hpsc_msg_lifecycle(enum hpsc_msg_lifecycle_status status, const char *fmt, ...);

/**
 * Process a received messaged. Should only be called by hpsc-notif.
 *
 * @param msg The received message
 * @param sz The size of the message
 * @return 0 on success, a negative error code otherwise
 */
int hpsc_msg_process(const void *msg, size_t sz);

#endif /* __HPSC_MSG_H */
