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
#include <linux/notifier.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include "hpsc_notif.h"

#define DT_MBOXES_PROP	"mboxes"
#define DT_MBOX_OUT	0
#define DT_MBOX_IN	1
#define DT_MBOXES_COUNT	2
#define DT_MBOXES_CELLS	"#mbox-cells"

#define HPSC_MBOX_MSG_LEN 64

struct mbox_chan_dev {
	struct mbox_client_dev	*tdev;
	struct mbox_client	client;
	spinlock_t		lock;
	struct mbox_chan	*channel;
	// set when controller notifies us from its ACK ISR
	bool			send_ack;
};

struct mbox_client_dev {
	struct mbox_chan_dev	chans[DT_MBOXES_COUNT];
	struct notifier_block	nb;
	struct device		*dev;
};


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
		hpsc_notif_recv(msg, HPSC_MBOX_MSG_LEN);
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

static int hpsc_mbox_kernel_send(struct notifier_block *nb,
				 unsigned long action, void *msg)
{
	// send message synchronously
	struct mbox_client_dev *tdev = container_of(nb, struct mbox_client_dev, nb);
	struct mbox_chan_dev *cdev = &tdev->chans[DT_MBOX_OUT];
	int ret;
	pr_info("HPSC mbox kernel: send\n");
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
		dev_err(tdev->dev, "Failed to send mailbox message: %d\n", ret);
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

	dev_info(&pdev->dev, "probe\n");

	// create/init client device
	tdev = devm_kzalloc(&pdev->dev, sizeof(*tdev), GFP_KERNEL);
	if (!tdev)
		return -ENOMEM;
	tdev->dev = &pdev->dev;
	platform_set_drvdata(pdev, tdev);

	// verify channel configuration in device tree, init chan array
	ret = hpsc_mbox_verify_chan_cfg(tdev);
	if (ret)
		return ret;
	for (i = 0; i < DT_MBOXES_COUNT; i++)
		hpsc_mbox_chan_dev_init(&tdev->chans[i], tdev);

	// We must delay processing of pending messages until we've registered
	// with notif handler, which requires all channels to be open.
	// First, lock each channel before opening them
	for (i = 0; i < DT_MBOXES_COUNT; i++) {
		cdev = &tdev->chans[i];
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
	tdev->nb.notifier_call = hpsc_mbox_kernel_send;
	tdev->nb.priority = HPSC_NOTIF_PRIORITY_MAILBOX;
	ret = hpsc_notif_register(&tdev->nb);
	BUG_ON(ret); // we should be the only mailbox handler
	// Finally, release the lock to start processing any pending messages
	for (i = 0; i < DT_MBOXES_COUNT; i++)
		spin_unlock(&tdev->chans[i].lock);

	dev_info(&pdev->dev, "registered\n");
#if 0
	u8 msg[HPSC_MBOX_MSG_LEN] = { 0x1 };
	ret = hpsc_mbox_kernel_send(&tdev->nb, 0, msg);
	pr_info("hpsc_mbox_kernel_send: %d\n", ret);
#endif
	return 0;

fail_channel:
	for (i--; i >= 0; i--) {
		cdev = &tdev->chans[i];
		mbox_free_channel(cdev->channel);
		cdev->channel = NULL;
		spin_unlock(&cdev->lock);
	}
	return ret;
}

static int hpsc_mbox_kernel_remove(struct platform_device *pdev)
{
	int i;
	struct mbox_client_dev *tdev = platform_get_drvdata(pdev);
	dev_info(&pdev->dev, "remove\n");
	// unregister with notification handler
	hpsc_notif_unregister(&tdev->nb);
	// close channels
	for (i = 0; i < DT_MBOXES_COUNT; i++)
		mbox_free_channel(tdev->chans[i].channel);
	// mbox_client_dev instance managed for us
	dev_info(&pdev->dev, "unregistered\n");
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
