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

#include <linux/notifier.h>

enum hpsc_notif_priority {
	HPSC_NOTIF_PRIORITY_MAILBOX,
	HPSC_NOTIF_PRIORITY_COUNT
};

/**
 * Register a notifier handler.
 * The notifier_block's priority must be set using hpsc_notif_priority.
 * There can be only one of each priority at a time.
 *
 * @param nb The notifier block
 * @return 0 on success, a negative error code otherwise
 */
int hpsc_notif_register(struct notifier_block *nb);

/**
 * Unregister a notifier handler.
 * The notifier_block's priority must be set using hpsc_notif_priority.
 *
 * @param nb The notifier block
 */
void hpsc_notif_unregister(struct notifier_block *nb);

/**
 * Called by handlers when they receive messages.
 *
 * @param msg The message
 * @param sz Message size, currently must be HPSC_MSG_SIZE
 * @return 0 on success, a negative error code otherwise
 */
int hpsc_notif_recv(const void *msg, size_t sz);

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
