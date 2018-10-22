/*
 * HPSC in-kernel mailbox client for exchanging systems messages.
 * Exactly two mailboxes are reserved in the device tree for this module.
 * The first is for outbound messages, the second is for inbound messages.
 * If the module cannot initialize the mailbox channels, global functions
 * return -ENODEV.
 */
#ifndef __HPSC_MBOX_CLIENT_KERNEL_H
#define __HPSC_MBOX_CLIENT_KERNEL_H

#define HPSC_MBOX_CLIENT_KERNEL_MSG_LEN 64

/**
 * Open mailbox channels.
 * Registers callback function to receive messages from the Chiplet manager.
 * The msg pointer size is HPSC_MBOX_CLIENT_KERNEL_MSG_LEN.
 *
 * @param cb
 * @return 0 on success, a negative error code on failure.
 */
int hpsc_mbox_client_kernel_open(void (*cb)(void *msg));

/**
 * Send a message to the Chiplet manager.
 * If the mailbox is currently full, returns -EAGAIN.
 *
 * @param msg must be of size HPSC_MBOX_CLIENT_KERNEL_MSG_LEN
 * @return 0 on success, a negative error code on failure.
 */
int hpsc_mbox_client_kernel_send(void *msg);

#endif /* __HPSC_MBOX_CLIENT_KERNEL_H */
