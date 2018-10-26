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

enum hpsc_notif_handler_type {
	HPSC_NOTIF_HANDLER_MAILBOX,
	HPSC_NOTIF_HANDLER_COUNT
};

/**
 * Handlers are registered by modules that can exchange messages.
 * They are usually configured through the device tree.
 *
 * @type:   The handler type - only one of each is allowed to be registered
 * @name:   A unique identifier for debugging
 * @send:   A function pointer for sending data using this handler
 */
struct hpsc_notif_handler {
	enum hpsc_notif_handler_type type;
	const char* name;
	int (*send)(void *msg);
};

/**
 * Used by handlers to register themselves.
 *
 * @param h The handler
 * @return 0 on success, a negative error code otherwise
 */
int hpsc_notif_handler_register(struct hpsc_notif_handler *h);

/**
 * Used by handlers to unregister themselves.
 *
 * @param h The registered handler
 */
void hpsc_notif_handler_unregister(struct hpsc_notif_handler *h);

/**
 * Called by handlers when they receive messages.
 *
 * @param h The registered handler
 * @param msg The message
 * @param sz Message size, currently must be HPSC_MSG_SIZE
 * @return 0 on success, a negative error code otherwise
 */
int hpsc_notif_recv(struct hpsc_notif_handler *h, void *msg, size_t sz);

/**
 * Send a message to the Chiplet manager.
 * The first byte must be the message type, the following 3 bytes are reserved.
 * Message starts at the fifth byte, &msg[4].
 *
 * @param msg The message
 * @param sz Message size, currently must be HPSC_MSG_SIZE
 * @return 0 on success, a negative error code otherwise
 */
int hpsc_notif_send(void *msg, size_t sz);

#endif/* __HPSC_NOTIF_H */
