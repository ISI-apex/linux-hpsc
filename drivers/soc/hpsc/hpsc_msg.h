#ifndef __HPSC_MSG_H
#define __HPSC_MSG_H

#include <linux/types.h>

#define HPSC_MSG_SIZE 64

#define HPSC_MSG_DEFINE(name) unsigned char name[HPSC_MSG_SIZE] = {0}

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
	// an enumerated/predefined action
	ACTION,
	// enum counter
	HPSC_MSG_TYPE_COUNT
};

/**
 * Send a message that a watchdog timed out.
 *
 * @param cpu The CPU whose watchdog timed out
 */
void hpsc_msg_wdt_timeout(unsigned int cpu);

/**
 * Process a received messaged. Should only be called by hpsc-notif.
 *
 * @param msg The received message
 * @param sz The size of the message
 * @return 0 on success, a negative error code otherwise
 */
int hpsc_msg_process(const void *msg, size_t sz);

#endif /* __HPSC_MSG_H */
