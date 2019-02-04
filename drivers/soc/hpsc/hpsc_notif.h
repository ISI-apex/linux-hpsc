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
 * Sending and receiving are performed in atomic contexts.
 */
#ifndef __HPSC_NOTIF_H
#define __HPSC_NOTIF_H

#include <linux/notifier.h>

/**
 * Higher-priority notifiers are attempted first
 */
enum hpsc_notif_priority {
	HPSC_NOTIF_PRIORITY_SHMEM,
	HPSC_NOTIF_PRIORITY_MAILBOX
};

/**
 * Register a notifier handler which runs in an atomic context.
 * The notifier_block's priority should be set relative to other handlers.
 * On success, handlers should return NOTIFY_STOP so only the highest priority
 * notifier is executed.
 * On failure, they handlers return (NOTIFY_STOP_MASK | EAGAIN) if a retry is
 * should be attempted, otherwise return a positive value error code so other
 * handlers can be tried.
 *
 * @param nb The notifier block
 * @return 0 on success, a negative error code otherwise
 */
int hpsc_notif_register(struct notifier_block *nb);

/**
 * Unregister a notifier handler.
 *
 * @param nb The notifier block
 * @return 0 on success, a negative error code otherwise
 */
int hpsc_notif_unregister(struct notifier_block *nb);

/**
 * Called by handlers when they receive messages.
 * Runs in an atomic context.
 *
 * @param msg The message
 * @param sz Message size, currently must be HPSC_MSG_SIZE
 * @return 0 on success, a negative error code otherwise
 */
int hpsc_notif_recv(const void *msg, size_t sz);

/**
 * Send a message to the Chiplet manager in an atomic context.
 * The first byte must be the message type, the following 3 bytes are reserved.
 * Message starts at the fifth byte, &msg[4].
 *
 * @param msg The message
 * @param sz Message size, currently must be HPSC_MSG_SIZE
 * @return 0 on success, a negative error code otherwise
 */
int hpsc_notif_send(void *msg, size_t sz);

#endif/* __HPSC_NOTIF_H */
