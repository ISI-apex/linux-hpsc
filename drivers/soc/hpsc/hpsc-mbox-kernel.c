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
#define DT_MBOX_COUNT	2
#define DT_MBOXES_CELLS	"#mbox-cells"

#define HPSC_MBOX_MSG_LEN 64

struct mbox_chan_dev {
	struct mbox_client	cl;
	struct mbox_chan	*channel;
	atomic_t		send_ready;
};

struct mbox_client_dev {
	struct mbox_chan_dev	chans[DT_MBOX_COUNT];
	struct notifier_block	nb;
	struct device		*dev;
};

static void client_rx_callback(struct mbox_client *cl, void *msg)
{
	struct mbox_chan_dev *cdev = container_of(cl, struct mbox_chan_dev, cl);
	int ret;
	dev_info(cl->dev, "rx_callback\n");
	ret = hpsc_notif_recv(msg, HPSC_MBOX_MSG_LEN);
	// tell the controller to issue the ACK (NULL if !ret) or NACK
	mbox_send_message(cdev->channel, ERR_PTR(ret));
}

static void client_tx_done(struct mbox_client *cl, void *msg, int r)
{
	struct mbox_chan_dev *cdev = container_of(cl, struct mbox_chan_dev, cl);
	dev_info(cl->dev, "tx_done: got %sACK: %d\n", r ? "N" : "", r);
	atomic_set(&cdev->send_ready, true);
}

static int hpsc_mbox_kernel_send(struct notifier_block *nb,
				 unsigned long action, void *msg)
{
	struct mbox_client_dev *tdev = container_of(nb, struct mbox_client_dev,
						    nb);
	struct mbox_chan_dev *cdev = &tdev->chans[DT_MBOX_OUT];
	int ret;
	dev_info(tdev->dev, "send\n");
	if (!atomic_cmpxchg(&cdev->send_ready, true, false))
		// previous message not yet [N]ACK'd
		return NOTIFY_STOP_MASK | EAGAIN;
	ret = mbox_send_message(cdev->channel, msg);
	if (ret < 0) {
		dev_err(tdev->dev, "Failed to send mailbox message: %d\n", ret);
		atomic_set(&cdev->send_ready, true);
		// need the positive error code value
		return -ret;
	}
	return NOTIFY_STOP;
}

static int hpsc_mbox_verify_chan_cfg(struct mbox_client_dev *tdev)
{
	// there must be exactly 2 channels - 1 out, 1 in
	int num_chans = of_count_phandle_with_args(tdev->dev->of_node,
						   DT_MBOXES_PROP,
						   DT_MBOXES_CELLS);
	if (num_chans != DT_MBOX_COUNT) {
		dev_err(tdev->dev, "Num instances in '%s' property != %d: %d\n",
			DT_MBOXES_PROP, DT_MBOX_COUNT, num_chans);
		return -EINVAL;
	}
	return 0;
}

static void hpsc_mbox_client_init(struct mbox_client *cl, struct device *dev,
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

static int hpsc_mbox_chan_open(struct mbox_client_dev *tdev, int i)
{
	struct mbox_chan_dev *cdev = &tdev->chans[i];
	hpsc_mbox_client_init(&cdev->cl, tdev->dev, (bool) i);
	atomic_set(&cdev->send_ready, true);
	cdev->channel = mbox_request_channel(&cdev->cl, i);
	if (IS_ERR(cdev->channel)) {
		dev_err(tdev->dev, "Channel request failed: %d\n", i);
		return PTR_ERR(cdev->channel);
	}
	return 0;
}

static int hpsc_mbox_kernel_probe(struct platform_device *pdev)
{
	struct mbox_client_dev *tdev;
	int ret;
	dev_info(&pdev->dev, "probe\n");

	tdev = devm_kzalloc(&pdev->dev, sizeof(*tdev), GFP_KERNEL);
	if (!tdev)
		return -ENOMEM;
	tdev->dev = &pdev->dev;
	platform_set_drvdata(pdev, tdev);

	tdev->nb.notifier_call = hpsc_mbox_kernel_send;
	tdev->nb.priority = HPSC_NOTIF_PRIORITY_MAILBOX;

	// verify channel configuration in device tree
	ret = hpsc_mbox_verify_chan_cfg(tdev);
	if (ret)
		return ret;

	// Must open the outbound chan and register with notif handler before
	// opening the inbound chan, which may receive a rx interrupt on open
	// that results in a synchronous reply (outbound message).
	ret = hpsc_mbox_chan_open(tdev, DT_MBOX_OUT);
	if (ret)
		return ret;
	hpsc_notif_register(&tdev->nb);
	ret = hpsc_mbox_chan_open(tdev, DT_MBOX_IN);
	if (ret) {
		hpsc_notif_unregister(&tdev->nb);
		mbox_free_channel(tdev->chans[DT_MBOX_OUT].channel);
		return ret;
	}

	return 0;
}

static int hpsc_mbox_kernel_remove(struct platform_device *pdev)
{
	struct mbox_client_dev *tdev = platform_get_drvdata(pdev);
	int i;
	dev_info(&pdev->dev, "remove\n");
	hpsc_notif_unregister(&tdev->nb);
	for (i = 0; i < DT_MBOX_COUNT; i++)
		mbox_free_channel(tdev->chans[i].channel);
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
