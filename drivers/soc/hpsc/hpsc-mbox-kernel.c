/*
 * HPSC in-kernel mailbox client for exchanging systems messages.
 * Exactly two mailboxes are reserved in the device tree for this module.
 * The first is for outbound messages, the second is for inbound messages.
 */
#include <linux/device.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/mailbox_client.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include "hpsc_notif.h"

#define DT_MBOXES_PROP      "mboxes"
#define DT_MBOX_OUT         0
#define DT_MBOX_IN          1
#define DT_MBOXES_COUNT     2
#define DT_MBOXES_CELLS     "#mbox-cells"

#define HPSC_MBOX_CLIENT_KERNEL_MSG_LEN 64

struct mbox_client_dev {
    struct device *dev;
};

struct mbox_chan_dev {
    struct mbox_client_dev *tdev;
    struct mbox_client      client;
    spinlock_t              lock;
    struct mbox_chan       *channel;

    // Mailbox instance identifiers and config, stays constant
    unsigned                instance_idx;

    bool                    send_ack; // set when controller notifies us from its ACK ISR
    int                     send_rc;  // status code controller gives us for the ACK
};

static struct mbox_chan_dev *mbox_chan_dev_ar;
static struct hpsc_notif_handler *notif_h;

static void client_rx_callback(struct mbox_client *cl, void *message)
{
    struct mbox_chan_dev *cdev = container_of(cl, struct mbox_chan_dev, client);
    dev_info(cl->dev, "rx_callback\n");
    spin_lock(&cdev->lock);
    // handle message synchronously
    if (IS_ERR(cdev->channel)) {
        // This was a message that was pending when we opened the channel, but
        // we were forced to close it because of other channel failures.
        // Dump the message, but don't ACK it
        dev_err(cl->dev, "Pending message cannot be processed!\n");
        print_hex_dump_bytes("rx_callback", DUMP_PREFIX_ADDRESS, message,
                             HPSC_MBOX_CLIENT_KERNEL_MSG_LEN);
    } else {
        hpsc_notif_recv(notif_h, message);
        // NOTE: yes, this is abuse of the method, but otherwise we need to
        // add another method to the interface.
        // Tell the controller to issue the ACK.
        mbox_client_peek_data(cdev->channel);
    }
    spin_unlock(&cdev->lock);
}

static void client_tx_done(struct mbox_client *cl, void *message, int r)
{
    // received a [N]ACK from previous message
    struct mbox_chan_dev *cdev = container_of(cl, struct mbox_chan_dev, client);
    spin_lock(&cdev->lock);
    cdev->send_ack = true;
    cdev->send_rc = r;
    spin_unlock(&cdev->lock);
    if (r)
        dev_warn(cl->dev, "tx_done: got NACK%d\n", r);
    else
        dev_info(cl->dev, "tx_done: got ACK\n");
}

static int hpsc_mbox_client_kernel_send(void *msg)
{
    // send message synchronously
    struct mbox_chan_dev *cdev;
    int ret;
    pr_info("Mailbox client kernel module: send\n");
    cdev = &mbox_chan_dev_ar[DT_MBOX_OUT];
    spin_lock(&cdev->lock);
    if (!cdev->send_ack) {
        // previous message not yet ack'd
        ret = -EAGAIN;
        goto send_out;
    }
    ret = mbox_send_message(cdev->channel, msg);
    if (ret >= 0) {
        cdev->send_ack = false;
        cdev->send_rc = 0;
    }
send_out:
    spin_unlock(&cdev->lock);
    if (ret < 0)
        dev_err(cdev->tdev->dev, "Failed to send message via mailbox: %d\n", ret);
    return ret;
}

static void hpsc_mbox_client_init(struct mbox_client *cl, struct device *dev)
{
    cl->dev          = dev;
    cl->rx_callback  = client_rx_callback;
    cl->tx_done      = client_tx_done;
    cl->tx_block     = false;
    cl->knows_txdone = false;
}

static int hpsc_mbox_client_kernel_probe(struct platform_device *pdev)
{
    struct mbox_client_dev *tdev;
    struct mbox_chan_dev *cdev;
    struct of_phandle_args spec;
    int num_chans;
    int i;
    int ret;

    dev_info(&pdev->dev, "Mailbox client kernel module: probe\n");

    tdev = devm_kzalloc(&pdev->dev, sizeof(*tdev), GFP_KERNEL);
    if (!tdev)
        return -ENOMEM;
    tdev->dev = &pdev->dev;
    platform_set_drvdata(pdev, tdev);

    mbox_chan_dev_ar = devm_kzalloc(&pdev->dev, DT_MBOXES_COUNT * sizeof(struct mbox_chan_dev), GFP_KERNEL);
    if (!mbox_chan_dev_ar)
        return -ENOMEM;

    notif_h = devm_kzalloc(&pdev->dev, sizeof(*notif_h), GFP_KERNEL);
    if (!notif_h)
        return -ENOMEM;
    notif_h->name = "HPSC Mailbox Client Kernel";
    notif_h->msg_sz = HPSC_MBOX_CLIENT_KERNEL_MSG_LEN;
    notif_h->send = hpsc_mbox_client_kernel_send;

    // there must be 2 and only 2 channels - 1 out, 1 in
    num_chans = of_count_phandle_with_args(tdev->dev->of_node, DT_MBOXES_PROP,
                                           DT_MBOXES_CELLS);
    if (num_chans != DT_MBOXES_COUNT) {
        // device tree not configured properly
        dev_err(tdev->dev, "Num instances in '%s' property != %d: %d\n",
                DT_MBOXES_PROP, DT_MBOXES_COUNT, num_chans);
        return -EINVAL;
    }

    // populate mbox_chan_dev_ar and open channels
    for (i = 0; i < DT_MBOXES_COUNT; i++) {
        cdev = &mbox_chan_dev_ar[i];
        // validate outbound and inbound mailbox ordering
        if (of_parse_phandle_with_args(tdev->dev->of_node, DT_MBOXES_PROP,
                                       DT_MBOXES_CELLS, i, &spec)) {
            dev_err(tdev->dev, "Can't parse '%s' property\n", DT_MBOXES_PROP);
            ret = -EINVAL;
            goto fail_channel;
        }
        if (i != spec.args[1]) {
            // device tree not configured properly
            // index 0 is mbox out and index 1 is mbox in
            dev_err(tdev->dev, "First '%s' entry must be outbound, second must be inbound\n",
                    DT_MBOXES_PROP);
            ret = -EINVAL;
            of_node_put(spec.np);
            goto fail_channel;
        }
        of_node_put(spec.np);
        cdev->instance_idx = i;
        cdev->tdev = tdev;
        hpsc_mbox_client_init(&cdev->client, tdev->dev);
        spin_lock_init(&cdev->lock);
        // lock to hold pending messages until we register notification handler
        spin_lock(&cdev->lock);
        cdev->channel = mbox_request_channel(&cdev->client, cdev->instance_idx);
        if (IS_ERR(cdev->channel)) {
            dev_err(tdev->dev, "Failed to request channel: %u\n",
                    cdev->instance_idx);
            ret = -EIO;
            spin_unlock(&cdev->lock);
            goto fail_channel;
        }
        cdev->send_ack = true;
        cdev->send_rc = 0;
    }

    ret = hpsc_notif_handler_register(notif_h);
    for (i = 0; i < DT_MBOXES_COUNT; i++)
        // now we can release the lock to receive pending messages
        spin_unlock(&mbox_chan_dev_ar[i].lock);
    BUG_ON(ret); // shouldn't fail if we setup notif_h correctly
    dev_info(&pdev->dev, "Mailbox client kernel module registered\n");
#if 0
    // send a test message to TRCH
    u32 msg[HPSC_MBOX_CLIENT_KERNEL_MSG_LEN / sizeof(u32)] = { 1, 42 }; // echo request
    ret = hpsc_notif_send(msg, sizeof(msg));
    dev_warn(tdev->dev, "Test message send: %d\n", ret);
#endif
    return 0;

fail_channel:
    for (i--; i >= 0; i--) {
        cdev = &mbox_chan_dev_ar[i];
        mbox_free_channel(cdev->channel);
        cdev->channel = NULL;
        // remember to release lock
        spin_unlock(&cdev->lock);
    }
    return ret;
}

static int hpsc_mbox_client_kernel_remove(struct platform_device *pdev)
{
    int i;
    dev_info(&pdev->dev, "Mailbox client kernel module: remove\n");
    hpsc_notif_handler_unregister(notif_h);
    // close channels
    for (i = 0; i < DT_MBOXES_COUNT; i++)
        mbox_free_channel(mbox_chan_dev_ar[i].channel);
    // mbox_chan_dev_ar and notif_h managed for us
    dev_info(&pdev->dev, "Mailbox client kernel module unregistered\n");
    return 0;
}

static const struct of_device_id mbox_test_match[] = {
    { .compatible = "hpsc-mbox-client-kernel" },
    {},
};

static struct platform_driver hpsc_mbox_client_kernel_driver = {
    .driver = {
        .name = "hpsc_mbox_client_kernel",
        .of_match_table = mbox_test_match,
    },
    .probe  = hpsc_mbox_client_kernel_probe,
    .remove = hpsc_mbox_client_kernel_remove,
};
module_platform_driver(hpsc_mbox_client_kernel_driver);

MODULE_DESCRIPTION("HPSC Mailbox client for in-kernel use");
MODULE_AUTHOR("HPSC");
MODULE_LICENSE("GPL v2");
