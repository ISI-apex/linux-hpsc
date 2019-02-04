/*
 * HPSC in-kernel shared memory module.
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

// TODO: This is an arbitrarily chosen value. Make configurable in DT?
static const unsigned long sleep_ms = 100;

// All subsystems must understand this structure and its protocol
struct hpsc_shmem_region {
	u8 data[HPSC_MSG_SIZE];
	u32 is_new;
};

struct hpsc_kshmem_dev {
	struct device			*dev;
	spinlock_t			lock;
	struct hpsc_shmem_region	*in;
	struct hpsc_shmem_region	*out;
	struct notifier_block		nb;
	struct task_struct		*t;
};

static int hpsc_kshmem_send(struct notifier_block *nb, unsigned long action,
			    void *msg)
{
	struct hpsc_kshmem_dev *tdev = container_of(nb, struct hpsc_kshmem_dev,
						    nb);
	int ret = NOTIFY_STOP;
	dev_info(tdev->dev, "send\n");
	spin_lock(&tdev->lock);
	if (tdev->out->is_new) {
		// a message is still waiting to be processed
		ret = NOTIFY_STOP_MASK | EAGAIN;
	} else {
		memcpy(&tdev->out->data, msg, HPSC_MSG_SIZE);
		tdev->out->is_new = 1;
	}
	spin_unlock(&tdev->lock);
	return ret;
}

static int hpsc_kshmem_recv(void *arg)
{
	struct hpsc_kshmem_dev *tdev = (struct hpsc_kshmem_dev *) arg;
	while (!kthread_should_stop()) {
		if (tdev->in->is_new) {
			dev_info(tdev->dev, "hpsc_kshmem_recv\n");
			// don't really care if processing fails...
			hpsc_notif_recv(tdev->in->data, HPSC_MSG_SIZE);
			tdev->in->is_new = 0;
		}
		msleep_interruptible(sleep_ms);
	}
	return 0;
}

static int hpsc_kshmem_parse_dt(struct hpsc_kshmem_dev *tdev, const char *name,
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

static int hpsc_kshmem_probe(struct platform_device *pdev)
{
	struct hpsc_kshmem_dev *tdev;
	int ret;

	dev_info(&pdev->dev, "probe\n");
	tdev = devm_kzalloc(&pdev->dev, sizeof(*tdev), GFP_KERNEL);
	if (!tdev)
		return -ENOMEM;
	tdev->dev = &pdev->dev;
	platform_set_drvdata(pdev, tdev);

	spin_lock_init(&tdev->lock);
	ret = hpsc_kshmem_parse_dt(tdev, "memory-region-in", &tdev->in) ||
	      hpsc_kshmem_parse_dt(tdev, "memory-region-out", &tdev->out);
	if (ret)
		return ret;

	// Must register with notif handler before starting the receiver thread.
	// Receiving messages can result in a synchronous reply, and we must be
	// registered for that reply to be sent.
	tdev->nb.notifier_call = hpsc_kshmem_send;
	tdev->nb.priority = HPSC_NOTIF_PRIORITY_SHMEM;
	hpsc_notif_register(&tdev->nb);
	tdev->t = kthread_run(hpsc_kshmem_recv, tdev, "hpsc_kshmem");
	if (IS_ERR(tdev->t)) {
		dev_err(tdev->dev, "kthread_run failed\n");
		hpsc_notif_unregister(&tdev->nb);
		return PTR_ERR(tdev->t);
	}

	return 0;
}

static int hpsc_kshmem_remove(struct platform_device *pdev)
{
	struct hpsc_kshmem_dev *tdev = platform_get_drvdata(pdev);
	int ret;
	dev_info(tdev->dev, "remove\n");
	ret = kthread_stop(tdev->t);
	hpsc_notif_unregister(&tdev->nb);
	return ret;
}

static const struct of_device_id hpsc_kshmem_match[] = {
	{ .compatible = "hpsc-kshmem" },
	{},
};

static struct platform_driver hpsc_kshmem_driver = {
	.driver = {
		.name = "hpsc_kshmem",
		.of_match_table = hpsc_kshmem_match,
	},
	.probe  = hpsc_kshmem_probe,
	.remove = hpsc_kshmem_remove,
};
module_platform_driver(hpsc_kshmem_driver);

MODULE_DESCRIPTION("HPSC shared memory in-kernel interface");
MODULE_AUTHOR("Connor Imes <cimes@isi.edu>");
MODULE_LICENSE("GPL v2");
