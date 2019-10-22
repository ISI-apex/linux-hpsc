/*
 * A backend transport for the kernel messaging interface implemented using
 * shared memory regions.
 *
 * Memory regions should be reserved physical addresses with fixed size.
 */
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>
#include "hpsc_msg.h"
#include "hpsc_notif.h"

// All subsystems must understand this structure and its protocol
#define HPSC_SHMEM_STATUS_BIT_NEW 0x01
#define HPSC_SHMEM_STATUS_BIT_ACK 0x02
struct hpsc_shmem_region {
	u8 data[HPSC_MSG_SIZE];
	u32 status;
};

struct hpsc_msg_tp_shmem_dev {
	struct device			*dev;
	spinlock_t			lock;
	struct hpsc_shmem_region	*in;
	struct hpsc_shmem_region	*out;
	struct notifier_block		nb;
	struct task_struct		*t;
	unsigned int			poll_interval_ms;
};

static bool is_new(struct hpsc_shmem_region *reg)
{
	return reg->status & HPSC_SHMEM_STATUS_BIT_NEW;
}

static int hpsc_msg_tp_shmem_send(struct notifier_block *nb, unsigned long action,
			    void *msg)
{
	struct hpsc_msg_tp_shmem_dev *tdev = container_of(nb, struct hpsc_msg_tp_shmem_dev,
						    nb);
	int ret = NOTIFY_STOP;
	dev_info(tdev->dev, "send\n");
	spin_lock(&tdev->lock);
	if (is_new(tdev->out)) {
		// a message is still waiting to be processed
		ret = NOTIFY_STOP_MASK | EAGAIN;
	} else {
		memcpy(&tdev->out->data, msg, HPSC_MSG_SIZE);
		tdev->out->status |= HPSC_SHMEM_STATUS_BIT_NEW;
	}
	spin_unlock(&tdev->lock);
	return ret;
}

static int hpsc_msg_tp_shmem_recv(void *arg)
{
	struct hpsc_msg_tp_shmem_dev *tdev = (struct hpsc_msg_tp_shmem_dev *) arg;
	while (!kthread_should_stop()) {
		if (is_new(tdev->in)) {
			dev_info(tdev->dev, "hpsc_msg_tp_shmem_recv\n");
			// don't really care if processing fails...
			hpsc_notif_recv(tdev->in->data, HPSC_MSG_SIZE);
			tdev->in->status &= ~HPSC_SHMEM_STATUS_BIT_NEW;
			tdev->in->status |= HPSC_SHMEM_STATUS_BIT_ACK;
		}
		msleep_interruptible(tdev->poll_interval_ms);
	}
	return 0;
}

static int hpsc_msg_tp_shmem_parse_dt_mreg(struct hpsc_msg_tp_shmem_dev *tdev,
				     const char *name,
				     struct hpsc_shmem_region **reg)
{
	struct device_node *np;
	struct resource res;
	resource_size_t size;
	void *vaddr;
	int ret;
	// get memory region from DT
	np = of_parse_phandle(tdev->dev->of_node, name, 0);
	if (!np) {
		dev_err(tdev->dev, "no DT '%s' property\n", name);
		return -EFAULT;
	}
	ret = of_address_to_resource(np, 0, &res);
	if (ret) {
		dev_err(tdev->dev, "no address for DT '%s'\n", name);
		return ret;
	}
	// parse and map into kernel virtual memory
	size = resource_size(&res);
	if (size < sizeof(struct hpsc_shmem_region)) {
		dev_err(tdev->dev, "size of DT '%s' is too small\n", name);
		return -ENOMEM;
	}
	// use writecombine flag to prevent caching
	vaddr = devm_memremap(tdev->dev, res.start, size, MEMREMAP_WC);
	if (!vaddr) {
		dev_err(tdev->dev, "devm_memremap failed\n");
		return -ENOMEM;
	}
	*reg = (struct hpsc_shmem_region *) vaddr;
	return 0;
}

static int hpsc_msg_tp_shmem_parse_dt(struct hpsc_msg_tp_shmem_dev *tdev)
{
	// get interval for polling inbound region
	int ret = of_property_read_u32(tdev->dev->of_node, "poll-interval-ms",
				       &tdev->poll_interval_ms);
	if (ret) {
		dev_err(tdev->dev, "invalid DT 'poll-interval-ms' value\n");
		return ret;
	}
	// now parse memory regions
	return hpsc_msg_tp_shmem_parse_dt_mreg(tdev, "memory-region-in", &tdev->in) ||
	       hpsc_msg_tp_shmem_parse_dt_mreg(tdev, "memory-region-out", &tdev->out);
}

static int hpsc_msg_tp_shmem_probe(struct platform_device *pdev)
{
	struct hpsc_msg_tp_shmem_dev *tdev;
	int ret;

	dev_info(&pdev->dev, "probe\n");
	tdev = devm_kzalloc(&pdev->dev, sizeof(*tdev), GFP_KERNEL);
	if (!tdev)
		return -ENOMEM;
	tdev->dev = &pdev->dev;
	platform_set_drvdata(pdev, tdev);

	spin_lock_init(&tdev->lock);
	ret = hpsc_msg_tp_shmem_parse_dt(tdev);
	if (ret)
		return ret;

	// Must register with notif handler before starting the receiver thread.
	// Receiving messages can result in a synchronous reply, and we must be
	// registered for that reply to be sent.
	tdev->nb.notifier_call = hpsc_msg_tp_shmem_send;
	tdev->nb.priority = HPSC_NOTIF_PRIORITY_SHMEM;
	hpsc_notif_register(&tdev->nb);
	tdev->t = kthread_run(hpsc_msg_tp_shmem_recv, tdev, "hpsc_msg_tp_shmem");
	if (IS_ERR(tdev->t)) {
		dev_err(tdev->dev, "kthread_run failed\n");
		hpsc_notif_unregister(&tdev->nb);
		return PTR_ERR(tdev->t);
	}

	return 0;
}

static int hpsc_msg_tp_shmem_remove(struct platform_device *pdev)
{
	struct hpsc_msg_tp_shmem_dev *tdev = platform_get_drvdata(pdev);
	int ret;
	dev_info(tdev->dev, "remove\n");
	ret = kthread_stop(tdev->t);
	hpsc_notif_unregister(&tdev->nb);
	return ret;
}

static const struct of_device_id hpsc_msg_tp_shmem_match[] = {
	{ .compatible = "hpsc-msg-transport,shmem" },
	{},
};

static struct platform_driver hpsc_msg_tp_shmem_driver = {
	.driver = {
		.name = "hpsc_msg_tp_shmem",
		.of_match_table = hpsc_msg_tp_shmem_match,
	},
	.probe  = hpsc_msg_tp_shmem_probe,
	.remove = hpsc_msg_tp_shmem_remove,
};
module_platform_driver(hpsc_msg_tp_shmem_driver);

MODULE_DESCRIPTION("Shared Memory transport for "
		   "HPSC kernel messaging interface");
MODULE_AUTHOR("Connor Imes <cimes@isi.edu>");
MODULE_LICENSE("GPL v2");
