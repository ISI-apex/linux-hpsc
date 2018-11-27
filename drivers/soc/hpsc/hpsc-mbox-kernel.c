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
	struct mbox_chan_dev *cdev = container_of(cl, struct mbox_chan_dev,
						  client);
	unsigned long flags;
	int ret;
	dev_info(cl->dev, "rx_callback\n");
	spin_lock_irqsave(&cdev->lock, flags);
	// handle message synchronously
	ret = hpsc_notif_recv(msg, HPSC_MBOX_MSG_LEN);
	// tell the controller to issue the ACK (NULL if !ret) or NACK
	mbox_send_message(cdev->channel, ERR_PTR(ret));
	spin_unlock_irqrestore(&cdev->lock, flags);
}

static void client_tx_done(struct mbox_client *cl, void *msg, int r)
{
	// received a [N]ACK from previous message
	struct mbox_chan_dev *cdev = container_of(cl, struct mbox_chan_dev,
						  client);
	unsigned long flags;
	spin_lock_irqsave(&cdev->lock, flags);
	cdev->send_ack = true;
	spin_unlock_irqrestore(&cdev->lock, flags);
	if (r)
		dev_warn(cl->dev, "tx_done: got NACK: %d\n", r);
	else
		dev_info(cl->dev, "tx_done: got ACK\n");
}

static int hpsc_mbox_kernel_send(struct notifier_block *nb,
				 unsigned long action, void *msg)
{
	// send message synchronously
	struct mbox_client_dev *tdev = container_of(nb, struct mbox_client_dev,
						    nb);
	struct mbox_chan_dev *cdev = &tdev->chans[DT_MBOX_OUT];
	unsigned long flags;
	int ret;
	dev_info(tdev->dev, "send\n");
	spin_lock_irqsave(&cdev->lock, flags);
	if (!cdev->send_ack) {
		// previous message not yet ack'd
		ret = NOTIFY_STOP_MASK | EAGAIN;
		goto send_out;
	}
	ret = mbox_send_message(cdev->channel, msg);
	if (ret >= 0) {
		cdev->send_ack = false;
		ret = NOTIFY_STOP;
	} else {
		dev_err(tdev->dev, "Failed to send mailbox message: %d\n", ret);
		// need the positive error code value
		ret = -ret;
	}
send_out:
	spin_unlock_irqrestore(&cdev->lock, flags);
	return ret;
}

static int hpsc_mbox_verify_chan_cfg(struct mbox_client_dev *tdev)
{
	// there must be exactly 2 channels - 1 out, 1 in
	int num_chans = of_count_phandle_with_args(tdev->dev->of_node,
						   DT_MBOXES_PROP,
						   DT_MBOXES_CELLS);
	if (num_chans != DT_MBOXES_COUNT) {
		dev_err(tdev->dev, "Num instances in '%s' property != %d: %d\n",
			DT_MBOXES_PROP, DT_MBOXES_COUNT, num_chans);
		return -EINVAL;
	}
	return 0;
}

static void hpsc_mbox_kernel_init(struct mbox_client *cl, struct device *dev,
				  bool incoming)
{
	cl->dev = dev;
	if (incoming)
		cl->rx_callback = client_rx_callback;
	else
		cl->tx_done = client_tx_done;
	cl->tx_block = false;
	cl->knows_txdone = false;
}

static void hpsc_mbox_chan_dev_init(struct mbox_chan_dev *cdev,
				    struct mbox_client_dev *tdev, bool incoming)
{
	cdev->tdev = tdev;
	hpsc_mbox_kernel_init(&cdev->client, tdev->dev, incoming);
	spin_lock_init(&cdev->lock);
	cdev->channel = NULL;
	cdev->send_ack = true;
}

static int hpsc_mbox_kernel_request_chan(struct mbox_chan_dev *cdev, int i)
{
	cdev->channel = mbox_request_channel(&cdev->client, i);
	if (IS_ERR(cdev->channel)) {
		dev_err(cdev->tdev->dev, "Channel request failed: %d\n", i);
		return PTR_ERR(cdev->channel);
	}
	return 0;
}

static int hpsc_mbox_kernel_probe(struct platform_device *pdev)
{
	struct mbox_client_dev *tdev;
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
		hpsc_mbox_chan_dev_init(&tdev->chans[i], tdev, i);

	// Can't use spin_lock_irqsave around mbox_request_channel since the
	// latter uses a mutex and might sleep.
	// So, we open the outbound channel first, register with notif handler,
	// then finally open the inbound channel. Inbound may quickly receive
	// a rx interrupt, but that's OK because outbound is registered and
	// ready to send any synchronous response.
	ret = hpsc_mbox_kernel_request_chan(&tdev->chans[DT_MBOX_OUT],
					    DT_MBOX_OUT);
	if (ret)
		return ret;
	tdev->nb.notifier_call = hpsc_mbox_kernel_send;
	tdev->nb.priority = HPSC_NOTIF_PRIORITY_MAILBOX;
	ret = hpsc_notif_register(&tdev->nb);
	BUG_ON(ret); // we should be the only mailbox handler
	ret = hpsc_mbox_kernel_request_chan(&tdev->chans[DT_MBOX_IN],
					    DT_MBOX_IN);
	if (ret) {
		mbox_free_channel(tdev->chans[DT_MBOX_OUT].channel);
		hpsc_notif_unregister(&tdev->nb);
		return ret;
	}

	return 0;
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
