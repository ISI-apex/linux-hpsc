/*
 * HPSC Notification module.
 * Allows exchanging systems-level messages with the Chiplet manager (TRCH).
 *
 * Selects between different delivery mechanisms like mailbox or shared memory.
 * For now, only mailbox is supported; messages must be kept at mailbox size.
 *
 * Bidirectional exchange mechanisms register themselves as handlers.
 * This allows the mechanisms to be added, removed, or reconfigured in a fault-
 * tolerant manner while always keeping this API available.
 */
#ifndef __HPSC_NOTIF_H
#define __HPSC_NOTIF_H

#define HPSC_NOTIF_MSG_SIZE 64

// Message type enumeration
typedef enum hpsc_notif_type {
	// Value 0 is reserved so empty messages can be recognized
	HPSC_NOTIF_INVALID = 0,
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
	HPSC_NOTIF_TYPE_COUNT
} hpsc_notif_type;

/**
 * Handlers are registered by modules that can exchange messages.
 * They are usually configured through the device tree.
 *
 * @name:   a unique identifier for debugging
 * @msg_sz: The message size this handler requires
 * @send:   A function pointer for sending data using this handler
 */
struct hpsc_notif_handler {
	const char* name;
	size_t msg_sz;
	int (*send)(void *msg);
	/* Internal to API */
	struct list_head node;
};

/**
 * Used by handlers to register themselves.
 *
 * @param h the handler
 * @return 0 on success, a negative error code otherwise
 */
int hpsc_notif_handler_register(struct hpsc_notif_handler *h);

/**
 * Used by handlers to unregister themselves.
 *
 * @param h the handler
 */
void hpsc_notif_handler_unregister(struct hpsc_notif_handler *h);

/**
 * Called by handlers when they receive messages.
 *
 * @param h the registered handler
 * @param msg the message of size h->msg_sz
 * @return 0 on success, a negative error code otherwise
 */
int hpsc_notif_recv(struct hpsc_notif_handler *h, void *msg);

/**
 * Used by kernel components to send a message to the Chiplet manager.
 * The first byte must be the message type, the following 3 bytes are reserved.
 * Message starts at the fifth byte, &msg[4].
 *
 * @param msg Pointer to the message buffer
 * @param sz Message size, currently must be HPSC_NOTIF_MSG_SIZE
 * @return 0 on success, a negative error code otherwise
 */
int hpsc_notif_send(void *msg, size_t sz);

#endif/* __HPSC_NOTIF_H */
