#ifndef __HPSC_MSG_H
#define __HPSC_MSG_H

#define HPSC_MSG_SIZE 64

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

#endif /* __HPSC_MSG_H */
