/*
 * Device files for timers that implement the interval timer interface
 *
 * This driver creates a character device file per each timer listed by
 * in the device tree node for this device (via phandle reference). The
 * timer must implement the interval timer interface (linux/interval_timer.h),
 * i.e. invoke a callback at a configurable interval.
 *
 * The semantics of the device files for the userspace are:
 * 	* read: return the current value of the timer counter as
 * 		a binary value (a 64-bit integer)
 * 	* write: set the timer interval (at which events are generated)
 * 		 as a binary value (a 64-bit integer)
 * 	* select/poll/epoll: block until the timer generates an event
 *
 * The read/write methods will return an error if the timer driver does not
 * support the respective functionality.
 *
 * NOTE: The functionality implemented here can't always be implemented in the
 * timer driver, because a timer driver might initialized before the class
 * subsystem is initialized, too early to create the class (and the dev files).
 * Clocksource drivers are not initialized (late) as a platform device due to
 * their need to setup per-cpu state. When the per-cpu state needs to be
 * initialized on each CPU via hotplug callbacks, it is problematic to get the
 * reference to the device object from those callbacks.
 */
#include <linux/module.h>
#include <linux/types.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include <linux/poll.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/of.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/platform_device.h>

#include "interval_timer.h"

#define DT_TIMERS_PROP "timers"
#define DT_NAME_PROP "devname"
#define DT_TIMER_CELLS "#timer-cells"

#define MAX_NAME_LEN 16

struct interval_dev_instance {
	struct device *dev;
	unsigned index;
	dev_t devno;
	struct interval_timer *itmr;
	struct interval_timer_cb *cb_handle;
	struct cdev cdev;
	wait_queue_head_t wq;
	bool event_pending;
};

struct interval_dev {
	struct device *dev;
	dev_t devno_major;
	unsigned num_instances;
	struct interval_dev_instance *instances;
};

static struct class interval_dev_class = {
	.name =		"interval_dev",
	.owner =	THIS_MODULE,
};

static void handle_timer_event(void *opaque)
{
	struct interval_dev_instance *instance = opaque;
	struct device *dev = instance->dev;

	dev_dbg(dev, "event from timer %u\n", instance->index);
	instance->event_pending = true;
	wake_up_interruptible(&instance->wq);
}

static int interval_dev_open(struct inode *inodep, struct file *filp)
{
	struct interval_dev_instance *instance =
		container_of(inodep->i_cdev, struct interval_dev_instance, cdev);
	filp->private_data = instance;
	return 0;
}

static int interval_dev_release(struct inode *inodep, struct file *filp)
{
	struct interval_dev_instance *instance = filp->private_data;
	struct interval_timer *itmr = instance->itmr;
	struct device *dev = instance->dev;

	dev_dbg(dev, "instance %d: release\n", instance->index);

	// Set to max to not create load on the system
	if (itmr->ops->set_interval)
		itmr->ops->set_interval(itmr, ~0ULL);
	return 0;
}

static ssize_t interval_dev_write(struct file *filp, const char __user *userbuf,
			  size_t count, loff_t *ppos)
{
	struct interval_dev_instance *instance = filp->private_data;
	struct interval_timer *itmr = instance->itmr;
	struct device *dev = instance->dev;
	uint64_t interval;
	int ret;

	dev_dbg(dev, "instance %d: write %lu bytes\n", instance->index, count);

	if (count != sizeof(interval))
	{
		dev_err(dev, "data written not of length %lu bytes\n",
			sizeof(interval));
		return -EINVAL;
	}
	ret = simple_write_to_buffer(&interval, sizeof(interval), ppos, userbuf, count);
	if (ret < 0) {
		dev_err(dev, "failed to copy msg data from userspace\n");
		return ret;
	}

	if (!itmr->ops->set_interval) {
		dev_dbg(dev, "timer device does not support set interval\n");
		return -ENOSYS;
	}
	ret = itmr->ops->set_interval(itmr, interval);
	if (ret) {
		dev_dbg(dev, "failed to set interval on timer: rc %d\n", ret);
		return ret;
	}
	return count;
}

static ssize_t interval_dev_read(struct file *filp, char __user *userbuf, size_t count,
			 loff_t *ppos)
{
	struct interval_dev_instance *instance = filp->private_data;
	struct interval_timer *itmr = instance->itmr;
	struct device *dev = instance->dev;
	uint64_t counter;
	int ret;

	dev_dbg(dev, "instance %d: read %lu bytes\n", instance->index, count);

	if (!itmr->ops->capture) {
		dev_dbg(dev, "timer device does not support capture\n");
		return -ENOSYS;
	}

	ret = itmr->ops->capture(itmr, &counter);
	if (ret) {
		dev_dbg(dev, "timer device capture failed\n");
		return ret;
	}
	return simple_read_from_buffer(userbuf, count, ppos,
					&counter, sizeof(counter));
}

static unsigned int interval_dev_poll(struct file *filp, poll_table *wait)
{
	struct interval_dev_instance *instance = filp->private_data;
	struct device *dev = instance->dev;
	unsigned int rc = 0;

	// Not accessing the HW (and also sleeping), so not restricting to a CPU

	dev_dbg(dev, "instance %d: poll waiting\n", instance->index);
	poll_wait(filp, &instance->wq, wait);

	if (instance->event_pending) {
		rc |= POLLIN | POLLRDNORM;
		instance->event_pending = false;
	}
	dev_dbg(dev, "instance %d: poll ret: %d\n", instance->index, rc);
	return rc;
}

static const struct file_operations interval_dev_fops = {
	.owner		= THIS_MODULE,
	.open		= interval_dev_open,
	.write		= interval_dev_write,
	.read		= interval_dev_read,
	.poll		= interval_dev_poll,
	.release	= interval_dev_release,
};

static int create_dev_file(struct interval_dev *idev,
			   const char *name, unsigned i)
{
	int ret = 0;
	struct device *dev = idev->dev;
	struct interval_dev_instance *instance = &idev->instances[i];
	char name_fmt[MAX_NAME_LEN + 2];
	dev_t devno = MKDEV(MAJOR(idev->devno_major), i);

	dev_info(dev, "creating device file %d:%d\n",
		 MAJOR(devno), MINOR(devno));

	ret = snprintf(name_fmt, sizeof(name_fmt), "%s%%d", name);
	if (ret < 0 || ret != strlen(name) + 2) {
		dev_err(dev, "failed to compose name string: rc %d\n", ret);
		return -ENAMETOOLONG;
	}

	// TODO: add as an array in probe? difficulty is mapping from filep to
	// instance ptr is via cdev (does filep/inode have the minor number info?)
	instance->devno = devno;
	cdev_init(&instance->cdev, &interval_dev_fops);
	instance->cdev.owner = interval_dev_fops.owner;
	ret = cdev_add(&instance->cdev, devno, /* count */ 1);
	if (ret) {
		dev_err(dev, "failed to add char device %d:%d: rc %d\n",
			MAJOR(devno), MINOR(devno), ret);
		return ret;
	}

	instance->dev = device_create(&interval_dev_class, NULL, devno,
				      NULL, name_fmt, i);
	if (!instance->dev) {
		dev_err(dev, "failed to create device %d:%d\n",
			MAJOR(devno), MINOR(devno));
		cdev_del(&instance->cdev);
		return -EFAULT;
	}
	return 0;
}

static void destroy_dev_file(struct interval_dev_instance *instance)
{
	struct device *dev = instance->dev;

	dev_info(dev, "destroying dev file for instance %u\n", instance->index);

	device_destroy(&interval_dev_class, instance->devno);
	cdev_del(&instance->cdev);
}

static void cleanup_instance(struct interval_dev_instance *instance)
{
	destroy_dev_file(instance);
	interval_timer_unsubscribe(instance->cb_handle);
}

static int interval_dev_probe(struct platform_device *pdev)
{
	int ret = 0;
	struct device *dev = &pdev->dev;
	struct device_node *np = pdev->dev.of_node;
	struct interval_dev *idev;
	struct interval_dev_instance *instance;
	struct interval_timer *itmr;
	struct of_phandle_args spec;
	const char *name;
	unsigned num_instances;
	int i;

	dev_info(dev, "probe\n");

	idev = devm_kzalloc(dev, sizeof(*idev), GFP_KERNEL);
	if (!idev) {
		dev_err(dev, "failed to alloc memory for device\n");
		return -ENOMEM;
	}
	idev->dev = dev;

	num_instances = of_count_phandle_with_args(np,
				DT_TIMERS_PROP, DT_TIMER_CELLS);
	dev_info(dev, "num timers in '%s' property: %d\n",
		 DT_TIMERS_PROP, num_instances);
	if (num_instances < 0) {
		dev_err(dev, "failed to count values in timers array\n");
		return -EINVAL;
	}

	ret = of_property_read_string(np, DT_NAME_PROP, &name);
	if (ret < 0) {
		dev_err(dev, "failed to read '%s' property\n", DT_NAME_PROP);
		return ret;
	}

	idev->num_instances = num_instances;
	idev->instances = devm_kzalloc(dev,
		num_instances * sizeof(idev->instances[0]), GFP_KERNEL);
	if (!idev->instances) {
		dev_err(dev, "failed to alloc memory for instances\n");
		return -ENOMEM;
	}

	ret = alloc_chrdev_region(&idev->devno_major, 0, num_instances, name);
	if (ret < 0) {
		dev_err(dev, "unable to allocate char dev region\n");
		return -EFAULT;
	}

	for (i = 0; i < num_instances; ++i) {
		instance = &idev->instances[i];
		instance->index = i;
		instance->event_pending = false;
		init_waitqueue_head(&instance->wq);

		ret = of_parse_phandle_with_args(np,
				DT_TIMERS_PROP, DT_TIMER_CELLS, i, &spec);
		if (ret) {
			dev_err(dev, "unable to parse phandle %d in prop '%s': rc %d\n",
				i, DT_TIMERS_PROP, ret);
			ret = -ENODEV;
			goto fail;
		}
		itmr = interval_timer_lookup(&spec);
		if (!itmr) {
			dev_err(dev, "failed to resolve phandle %d in prop '%s'\n",
				i, DT_TIMERS_PROP);
			ret = -ENODEV;
			of_node_put(spec.np);
			goto fail;
		}
		of_node_put(spec.np);

		instance->itmr = itmr;
		instance->cb_handle = interval_timer_subscribe(itmr,
						handle_timer_event, instance);
		if (!instance->cb_handle) {
			dev_err(dev, "failed to subscribe to timer %d\n", i);
			ret = -EFAULT;
			goto fail;
		}

		ret = create_dev_file(idev, name, i);
		if (ret) {
			dev_err(dev, "failed to create dev file for timer %d\n", i);
			goto fail;
		}
	}
	return 0;
fail:
	for (--i; i >= 0; --i) // skip the failing iteration
		cleanup_instance(&idev->instances[i]);
	unregister_chrdev_region(idev->devno_major, num_instances);
	return ret;
}

static int interval_dev_remove(struct platform_device *pdev)
{
	struct interval_dev *idev = platform_get_drvdata(pdev);
	struct device *dev = idev->dev;
	int i;

	dev_info(dev, "remove\n");

	for (i = idev->num_instances; i >= 0; --i)
		cleanup_instance(&idev->instances[i]);
	unregister_chrdev_region(idev->devno_major, idev->num_instances);
	return 0;
}

static const struct of_device_id interval_dev_match[] = {
	{ .compatible = "interval-dev" },
	{},
};

static struct platform_driver interval_dev_driver = {
	.driver = {
		.name = "interval-dev",
		.of_match_table = interval_dev_match,
	},
	.probe  = interval_dev_probe,
	.remove = interval_dev_remove,
};

static int __init interval_dev_init(void)
{
	int ret;

	pr_info("interval-dev: load\n");

	ret = class_register(&interval_dev_class);
	if (ret < 0) {
		pr_err("interval-dev: failed to create class\n");
		return ret;
	}

	ret = platform_driver_register(&interval_dev_driver);
	if (ret) {
		pr_err("interval-dev: failed to register driver\n");
		class_destroy(&interval_dev_class);
		return ret;
	}

	return 0;
}

static void __exit interval_dev_exit(void)
{
	pr_info("interval-dev: unload\n");
	platform_driver_unregister(&interval_dev_driver);
	class_unregister(&interval_dev_class);
}

MODULE_DESCRIPTION("Device file interface to interval timers");
MODULE_AUTHOR("Alexei Colin <acolin@isi.edu>");
MODULE_LICENSE("GPL v2");

module_init(interval_dev_init);
module_exit(interval_dev_exit);
