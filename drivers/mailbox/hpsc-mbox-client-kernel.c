#include <linux/device.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/mailbox_client.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/hpsc_mbox_client_kernel.h>

#define DT_MBOXES_PROP      "mboxes"
#define DT_MBOX_OUT         0
#define DT_MBOX_IN          1
#define DT_MBOXES_COUNT     2
#define DT_MBOXES_CELLS     "#mbox-cells"

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
    void                    (*rx_cb)(void *msg);
};

static struct mbox_chan_dev *mbox_chan_dev_ar;

static void dump_msg(void *msg)
{
    print_hex_dump_bytes("mailbox received", DUMP_PREFIX_ADDRESS, msg,
                         HPSC_MBOX_CLIENT_KERNEL_MSG_LEN);
}

static void client_rx_callback(struct mbox_client *cl, void *message)
{
    struct mbox_chan_dev *cdev = container_of(cl, struct mbox_chan_dev, client);
    dev_info(cl->dev, "rx_callback\n");
    spin_lock(&cdev->lock);
    // handle message synchronously
    cdev->rx_cb(message);
    // Tell the controller to issue the ACK.
    // NOTE: yes, this is abuse of the method, but otherwise we need to
    // add another method to the interface.
    mbox_client_peek_data(cdev->channel);
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

int hpsc_mbox_client_kernel_open(void (*cb)(void *msg))
{
    struct mbox_chan_dev *cdev;
    int i;
    int ret;
    pr_info("Mailbox client kernel module: open\n");
    if (mbox_chan_dev_ar == NULL) // init failed
        return -ENODEV;
    if (cb != NULL)
        mbox_chan_dev_ar[DT_MBOX_IN].rx_cb = cb;
    // Need to synchronize channels here since module is already loaded
    // kernel threads could conceivably race
    for (i = 0; i < DT_MBOXES_COUNT; i++) {
        cdev = &mbox_chan_dev_ar[i];
        spin_lock(&cdev->lock);
        // open channels
        cdev->channel = mbox_request_channel(&cdev->client, cdev->instance_idx);
        spin_unlock(&cdev->lock);
        if (IS_ERR(cdev->channel)) {
            dev_err(cdev->tdev->dev, "Failed to request channel: %u\n",
                    cdev->instance_idx);
            ret = -EIO;
            goto fail_open;
        }
    }
    return 0;
fail_open:
    for (i--; i >= 0; i--) {
        cdev = &mbox_chan_dev_ar[i];
        spin_lock(&cdev->lock);
        mbox_free_channel(cdev->channel);
        cdev->channel = NULL;
        spin_unlock(&cdev->lock);
    }
    return ret;
}
EXPORT_SYMBOL_GPL(hpsc_mbox_client_kernel_open);

int hpsc_mbox_client_kernel_send(void *msg)
{
    // send message synchronously
    struct mbox_chan_dev *cdev;
    int ret;
    pr_info("Mailbox client kernel module: send\n");
    BUG_ON(msg == NULL);
    if (mbox_chan_dev_ar == NULL) // init failed
        return -ENODEV;
    cdev = &mbox_chan_dev_ar[DT_MBOX_OUT];
    spin_lock(&cdev->lock);
    if (IS_ERR(cdev->channel)) {
        ret = -ENODEV;
        goto send_out;
    }
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
EXPORT_SYMBOL_GPL(hpsc_mbox_client_kernel_send);

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
    if (mbox_chan_dev_ar == NULL)
        return -ENOMEM;

    // there must be 2 and only 2 channels - 1 out, 1 in
    num_chans = of_count_phandle_with_args(tdev->dev->of_node, DT_MBOXES_PROP,
                                           DT_MBOXES_CELLS);
    if (num_chans != DT_MBOXES_COUNT) {
        // device tree not configured properly
        dev_err(tdev->dev, "Num instances in '%s' property != %d: %d\n",
                DT_MBOXES_PROP, DT_MBOXES_COUNT, num_chans);
        return -EINVAL;
    }

    // populate mbox_chan_dev_ar
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
            goto fail_channel;
        }
        of_node_put(spec.np);
        cdev->instance_idx = i;
        cdev->tdev = tdev;
        hpsc_mbox_client_init(&cdev->client, tdev->dev);
        spin_lock_init(&cdev->lock);
        cdev->channel = NULL;
        cdev->send_ack = true;
        cdev->send_rc = 0;
        cdev->rx_cb = dump_msg;
    }

    dev_info(&pdev->dev, "Mailbox client kernel module registered\n");
    return 0;

fail_channel:
    for (i--; i >= 0; i--) {
        mbox_free_channel(mbox_chan_dev_ar[i].channel);
        mbox_chan_dev_ar[i].channel = NULL;
    }
    return ret;
}

static int hpsc_mbox_client_kernel_remove(struct platform_device *pdev)
{
    struct mbox_chan_dev *cdev;
    int i;
    dev_info(&pdev->dev, "Mailbox client kernel module: remove\n");
    // close channels
    for (i = 0; mbox_chan_dev_ar != NULL && i < DT_MBOXES_COUNT; i++) {
        cdev = &mbox_chan_dev_ar[i];
        // TODO: not 100% sure locking isn't needed...
        spin_lock(&cdev->lock);
        if (!IS_ERR(cdev->channel)) {
            mbox_free_channel(cdev->channel);
            cdev->channel = NULL;
        }
        spin_unlock(&cdev->lock);
    }
    // mbox_chan_dev_ar managed for us
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
