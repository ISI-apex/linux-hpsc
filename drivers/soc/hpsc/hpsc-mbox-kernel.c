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

#define DT_MBOXES_PROP	"mboxes"
#define DT_MBOX_OUT	0
#define DT_MBOX_IN	1
#define DT_MBOXES_COUNT	2
#define DT_MBOXES_CELLS	"#mbox-cells"

#define HPSC_MBOX_MSG_LEN 64

struct mbox_client_dev {
	struct device *dev;
};

struct mbox_chan_dev {
	struct mbox_client_dev	*tdev;
	struct mbox_client	client;
	spinlock_t		lock;
	struct mbox_chan	*channel;
	// set when controller notifies us from its ACK ISR
	bool			send_ack;
};

static struct mbox_chan_dev *mbox_chan_dev_ar;
static struct hpsc_notif_handler *notif_h;

static void client_rx_callback(struct mbox_client *cl, void *msg)
{
	struct mbox_chan_dev *cdev = container_of(cl, struct mbox_chan_dev, client);
	dev_info(cl->dev, "rx_callback\n");
	spin_lock(&cdev->lock);
	// handle message synchronously
	if (IS_ERR(cdev->channel)) {
		// Note: Shouldn't happen currently, but in the future...?
		// Message that was pending when we opened the channel, but
		// we were forced to close it because of other channel failures.
		// Dump the message, but don't ACK it
		dev_err(cl->dev, "Pending message cannot be processed!\n");
		print_hex_dump_bytes("rx_callback", DUMP_PREFIX_ADDRESS, msg,
				     HPSC_MBOX_MSG_LEN);
	} else {
		hpsc_notif_recv(notif_h, msg, HPSC_MBOX_MSG_LEN);
		// NOTE: yes, this is abuse of the method, but otherwise we need to
		// add another method to the interface.
		// Tell the controller to issue the ACK.
		mbox_client_peek_data(cdev->channel);
	}
	spin_unlock(&cdev->lock);
}

static void client_tx_done(struct mbox_client *cl, void *msg, int r)
{
	// received a [N]ACK from previous message
	struct mbox_chan_dev *cdev = container_of(cl, struct mbox_chan_dev, client);
	spin_lock(&cdev->lock);
	cdev->send_ack = true;
	spin_unlock(&cdev->lock);
	if (r)
		dev_warn(cl->dev, "tx_done: got NACK%d\n", r);
	else
		dev_info(cl->dev, "tx_done: got ACK\n");
}

static int hpsc_mbox_kernel_send(void *msg)
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
	if (ret >= 0)
		cdev->send_ack = false;
	else
		dev_err(cdev->tdev->dev, "Failed to send mailbox message: %d\n",
			ret);
send_out:
	spin_unlock(&cdev->lock);
	return ret;
}

static int hpsc_mbox_verify_chan_cfg(struct mbox_client_dev *tdev)
{
	struct of_phandle_args spec;
	int num_chans;
	int i;
	// there must be exactly 2 channels - 1 out, 1 in
	num_chans = of_count_phandle_with_args(tdev->dev->of_node,
					       DT_MBOXES_PROP, DT_MBOXES_CELLS);
	if (num_chans != DT_MBOXES_COUNT) {
		dev_err(tdev->dev, "Num instances in '%s' property != %d: %d\n",
			DT_MBOXES_PROP, DT_MBOXES_COUNT, num_chans);
		return -EINVAL;
	}
	// check channel ordering - index 0 is outbound and index 1 is inbound
	for (i = 0; i < DT_MBOXES_COUNT; i++) {
		if (of_parse_phandle_with_args(tdev->dev->of_node,
					       DT_MBOXES_PROP, DT_MBOXES_CELLS,
					       i, &spec)) {
			dev_err(tdev->dev, "Can't parse '%s' property\n",
				DT_MBOXES_PROP);
			return -EINVAL;
		}
		of_node_put(spec.np);
		if (i != spec.args[1]) {
			dev_err(tdev->dev, "First '%s' entry must be outbound, second must be inbound\n",
				DT_MBOXES_PROP);
			return -EINVAL;
		}
	}
	return 0;
}

static void hpsc_mbox_notif_handler_init(struct hpsc_notif_handler *h)
{
	h->type = HPSC_NOTIF_HANDLER_MAILBOX;
	h->name = "HPSC In-kernel Mailbox Client";
	h->send = hpsc_mbox_kernel_send;
}

static void hpsc_mbox_kernel_init(struct mbox_client *cl, struct device *dev)
{
	cl->dev = dev;
	cl->rx_callback = client_rx_callback;
	cl->tx_done = client_tx_done;
	cl->tx_block = false;
	cl->knows_txdone = false;
}

static void hpsc_mbox_chan_dev_init(struct mbox_chan_dev *cdev,
				    struct mbox_client_dev *tdev)
{
	cdev->tdev = tdev;
	hpsc_mbox_kernel_init(&cdev->client, tdev->dev);
	spin_lock_init(&cdev->lock);
	cdev->channel = NULL;
	cdev->send_ack = true;
}

static int hpsc_mbox_kernel_probe(struct platform_device *pdev)
{
	struct mbox_client_dev *tdev;
	struct mbox_chan_dev *cdev;
	int i;
	int ret;

	dev_info(&pdev->dev, "Mailbox client kernel module: probe\n");

	// create/init client device
	tdev = devm_kzalloc(&pdev->dev, sizeof(*tdev), GFP_KERNEL);
	if (!tdev)
		return -ENOMEM;
	tdev->dev = &pdev->dev;
	platform_set_drvdata(pdev, tdev);

	// verify channel configuration in device tree, create/init chan array
	ret = hpsc_mbox_verify_chan_cfg(tdev);
	if (ret)
		return ret;
	mbox_chan_dev_ar = devm_kzalloc(&pdev->dev, DT_MBOXES_COUNT * sizeof(struct mbox_chan_dev), GFP_KERNEL);
	if (!mbox_chan_dev_ar)
		return -ENOMEM;
	for (i = 0; i < DT_MBOXES_COUNT; i++)
		hpsc_mbox_chan_dev_init(&mbox_chan_dev_ar[i], tdev);

	// create/init notifier handler
	notif_h = devm_kzalloc(&pdev->dev, sizeof(*notif_h), GFP_KERNEL);
	if (!notif_h)
		return -ENOMEM;
	hpsc_mbox_notif_handler_init(notif_h);

	// We must delay processing of pending messages until we've registered
	// with notif handler, which requires all channels to be open.
	// First, lock each channel before opening them
	for (i = 0; i < DT_MBOXES_COUNT; i++) {
		cdev = &mbox_chan_dev_ar[i];
		spin_lock(&cdev->lock);
		cdev->channel = mbox_request_channel(&cdev->client, i);
		if (IS_ERR(cdev->channel)) {
			dev_err(tdev->dev, "Channel request failed: %u\n", i);
			ret = PTR_ERR(cdev->channel);
			spin_unlock(&cdev->lock);
			goto fail_channel;
		}
	}
	// Now register with notification handler
	ret = hpsc_notif_handler_register(notif_h);
	BUG_ON(ret); // shouldn't fail if we setup notif_h correctly
	// Finally, release the lock to start processing any pending messages
	for (i = 0; i < DT_MBOXES_COUNT; i++)
		spin_unlock(&mbox_chan_dev_ar[i].lock);

	dev_info(&pdev->dev, "Mailbox client kernel module registered\n");
#if 0
	u8 msg[HPSC_MBOX_MSG_LEN] = { 0x1 };
	ret = hpsc_mbox_kernel_send(msg);
	pr_info("hpsc_mbox_kernel_send: %d\n", ret);
#endif
	return 0;

fail_channel:
	for (i--; i >= 0; i--) {
		cdev = &mbox_chan_dev_ar[i];
		mbox_free_channel(cdev->channel);
		cdev->channel = NULL;
		spin_unlock(&cdev->lock);
	}
	return ret;
}

static int hpsc_mbox_kernel_remove(struct platform_device *pdev)
{
	int i;
	dev_info(&pdev->dev, "Mailbox client kernel module: remove\n");
	// unregister with notification handler
	hpsc_notif_handler_unregister(notif_h);
	// close channels
	for (i = 0; i < DT_MBOXES_COUNT; i++)
		mbox_free_channel(mbox_chan_dev_ar[i].channel);
	// mbox_chan_dev_ar and notif_h managed for us
	dev_info(&pdev->dev, "Mailbox client kernel module unregistered\n");
	return 0;
}

static const struct of_device_id hpsc_mbox_kernel_match[] = {
	{ .compatible = "hpsc-mbox-kernel" },
	{},
};

static struct platform_driver hpsc_mbox_kernel_driver = {
	.driver = {
		.name = "hpsc_mbox_kernel",
		.of_match_table = hpsc_mbox_kernel_match,
	},
	.probe  = hpsc_mbox_kernel_probe,
	.remove = hpsc_mbox_kernel_remove,
};
module_platform_driver(hpsc_mbox_kernel_driver);

MODULE_DESCRIPTION("HPSC mailbox in-kernel interface");
MODULE_AUTHOR("Connor Imes <cimes@isi.edu>");
MODULE_AUTHOR("Alexei Colin <acolin@isi.edu>");
MODULE_LICENSE("GPL v2");
