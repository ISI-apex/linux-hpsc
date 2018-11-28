/*
 * HPSC userspace mailbox client.
 * Provides device files for applications at /dev/mbox/<instance>/mbox<num>.
 */
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/idr.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/mailbox_client.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#define HPSC_MBOX_DATA_REGS	16
#define MBOX_MAX_MSG_LEN	(HPSC_MBOX_DATA_REGS * 4) // each reg is 32-bit

#define DT_MBOXES_PROP  "mboxes"
#define DT_MBOX_NAMES_PROP  "mbox-names"
#define DT_MBOXES_CELLS "#mbox-cells"

#define MBOX_DEVICE_NAME "mbox"

struct mbox_chan_dev;

struct mbox_client_dev {
	struct device		*dev;
	struct mbox_chan_dev	*chans;
	int			num_chans;
	unsigned int		major_num;
	int			id;
};

/*
 * Locking must protect against the following:
 * First, interrupts from the mailbox API and user operations can race.
 * Second, a userspace app might share file descriptors between threads and race
 * on file operations.
 * Critically, we must protect against operations after a channel is closed.
 */
struct mbox_chan_dev {
	/* fixed fields */
	struct mbox_client_dev	*tdev;
	struct cdev		cdev;
	spinlock_t		lock;
	wait_queue_head_t       wq;
	unsigned int		index;

	/* dynamic fields */

	// mbox structs
	struct mbox_client	client;
	struct mbox_chan	*channel;
	// rx/tx buffer
	u32			message[HPSC_MBOX_DATA_REGS];
	// direction
	bool			incoming;
	// a received message is ready to be read
	bool			rx_msg_pending;
	// set when controller notifies us from its ACK ISR
	bool			send_ack;
	// status code controller gives us for the ACK
	int			send_rc;
};

// May be multiple mailbox instances
// Therefore, manage class at module init/exit, not device probe/remove
static struct class *class;
static DEFINE_IDA(mbox_ida);

static int hpsc_mbox_rx_ack(struct mbox_chan_dev *chan, int err)
{
	// 0 is an ACK, anything else is a NACK
	return mbox_send_message(chan->channel, err ? ERR_PTR(err) : NULL);
}

static void mbox_received(struct mbox_client *client, void *message)
{
	struct mbox_chan_dev *chan = container_of(client, struct mbox_chan_dev,
						  client);
	unsigned long flags;

	print_hex_dump_bytes("mailbox rcved", DUMP_PREFIX_ADDRESS, message,
			     MBOX_MAX_MSG_LEN);

	spin_lock_irqsave(&chan->lock, flags);
	if (chan->rx_msg_pending) {
		dev_err(chan->tdev->dev,
			"rx: dropped message: buffer full: %u\n", chan->index);
		// Send NACK since we're about to drop the message
		// Race with channel assignment in mbox_open: might still be ERR
		// But, we won't NACK the first message so channel should be OK
		if (unlikely(IS_ERR_OR_NULL(chan->channel)))
			// More than one message received before channel
			// assignment? Can't NACK, but it's the sender's fault
			dev_warn(chan->tdev->dev, "rx: can't NACK message\n");
		else
			hpsc_mbox_rx_ack(chan, -ENOBUFS);
	} else {
		memcpy(chan->message, message, MBOX_MAX_MSG_LEN);
		chan->rx_msg_pending = true;
		wake_up_interruptible(&chan->wq);
	}
	spin_unlock_irqrestore(&chan->lock, flags);
}

static void mbox_sent(struct mbox_client *client, void *message, int r)
{
	struct mbox_chan_dev *chan = container_of(client, struct mbox_chan_dev,
						  client);
	unsigned long flags;

	if (r)
		dev_warn(client->dev, "sent: got NACK: %u: %d\n",
			 chan->index, r);
	else
		dev_info(client->dev, "sent: got ACK: %u\n", chan->index);

	spin_lock_irqsave(&chan->lock, flags);
	if (unlikely(IS_ERR_OR_NULL(chan->channel))) {
		dev_warn(chan->tdev->dev,
			 "sent: dropped [N]ACK: mailbox closed: %u\n",
			 chan->index);
	} else {
		// multiple [N]ACKs shouldn't happen, but overwrite if they do
		chan->send_rc = r;
		chan->send_ack = true;
		wake_up_interruptible(&chan->wq);
	}
	spin_unlock_irqrestore(&chan->lock, flags);
}

static void mbox_client_init(struct mbox_client *cl, struct device *dev,
			     bool incoming)
{
	// explicit NULLs in case mbox is reused in different direction
	cl->dev			= dev;
	if (incoming) {
		cl->rx_callback	= mbox_received;
		cl->tx_done	= NULL;
	} else {
		cl->rx_callback	= NULL;
		cl->tx_done	= mbox_sent;
	}
	cl->tx_block		= false;
	cl->knows_txdone	= false;
}

static int mbox_open(struct inode *inodep, struct file *filp)
{
	struct mbox_chan_dev *chan = container_of(inodep->i_cdev,
						  struct mbox_chan_dev, cdev);
	struct mbox_client_dev *tdev = chan->tdev;
	unsigned long flags;

	spin_lock_irqsave(&chan->lock, flags);
	if (chan->channel) {
		dev_info(tdev->dev, "open: mailbox already opened: %u\n",
			 chan->index);
		spin_unlock_irqrestore(&chan->lock, flags);
		return -EBUSY;
	}
	// reset status fields
	// We allow reading of outgoing mbox (to get the [N]ACK), but not
	// writing of incoming mbox.
	chan->incoming =  (filp->f_mode & FMODE_READ) &&
			 !(filp->f_mode & FMODE_WRITE);
	chan->rx_msg_pending = false;
	chan->send_rc = 0;
	chan->send_ack = false;
	mbox_client_init(&chan->client, tdev->dev, chan->incoming);
	// non-NULL to prevent race with above if statement before channel open
	chan->channel = ERR_PTR(-ENODEV);
	// release lock before requesting channel since mbox_request_channel
	// uses a mutex and might sleep
	spin_unlock_irqrestore(&chan->lock, flags);

	// open mailbox channel
	chan->channel = mbox_request_channel(&chan->client, chan->index);
	if (IS_ERR(chan->channel)) {
		dev_err(tdev->dev, "request for mbox channel failed: %u\n",
			chan->index);
		chan->channel = NULL;
		return -EIO;
	}
	filp->private_data = chan;
	return 0;
}

static int mbox_release(struct inode *inodep, struct file *filp)
{
	struct mbox_chan_dev *chan = filp->private_data;
	unsigned long flags;
	spin_lock_irqsave(&chan->lock, flags);
	if (unlikely(IS_ERR_OR_NULL(chan->channel))) {
		dev_warn(chan->tdev->dev,
			 "release: mailbox already closed: %u\n", chan->index);
	} else {
		if (chan->rx_msg_pending)
			// send NACK since we're about to drop the message
			hpsc_mbox_rx_ack(chan, -EPIPE);
		mbox_free_channel(chan->channel);
		chan->channel = NULL;
	}
	spin_unlock_irqrestore(&chan->lock, flags);
	return 0;
}

static ssize_t mbox_write(struct file *filp, const char __user *userbuf,
			  size_t count, loff_t *ppos)
{
	u32 msg[HPSC_MBOX_DATA_REGS];
	struct mbox_chan_dev *chan = filp->private_data;
	struct mbox_client_dev *tdev = chan->tdev;
	unsigned long flags;
	ssize_t ret;

	if (count > MBOX_MAX_MSG_LEN) {
		dev_err(tdev->dev, "message too long: %zd > %d\n", count,
			MBOX_MAX_MSG_LEN);
		return -EINVAL;
	}
	ret = copy_from_user(msg, userbuf, count);
	if (ret) {
		dev_err(tdev->dev, "failed to copy msg data from userspace\n");
		return -EFAULT;
	}
	print_hex_dump_bytes("mailbox send: ", DUMP_PREFIX_ADDRESS,
			     msg, MBOX_MAX_MSG_LEN);

	spin_lock_irqsave(&chan->lock, flags);
	if (unlikely(IS_ERR_OR_NULL(chan->channel))) {
		dev_err(tdev->dev, "write: mailbox closed: %u\n", chan->index);
		ret = -ENODEV;
		goto out;
	}

	chan->send_ack = false;
	chan->send_rc = 0;

	// Note: successful return here does not indicate successful receipt of
	//       sent message by the other end
	ret = mbox_send_message(chan->channel, msg);
	if (ret < 0) {
		dev_err(tdev->dev, "Failed to send message via mailbox\n");
		ret = -EIO;
	} else {
		ret = count;
	}

out:
	spin_unlock_irqrestore(&chan->lock, flags);
	return ret;
}

static ssize_t mbox_read(struct file *filp, char __user *userbuf, size_t count,
			 loff_t *ppos)
{
	struct mbox_chan_dev *chan = filp->private_data;
	unsigned long flags;
	ssize_t ret;

	// This can race with channel state if user is behaving badly, but it
	// won't be catastrophic - we still synchronize chan state changes
	// At worst, we unnecessarily copy to userspace but still return an
	// error, so userspace only knowingly reads the data once
	if (chan->incoming && chan->rx_msg_pending)
		ret = simple_read_from_buffer(userbuf, count, ppos,
					      chan->message, MBOX_MAX_MSG_LEN);
	else if (!chan->incoming && chan->send_ack)
		// outgoing, return the ACK
		ret = simple_read_from_buffer(userbuf, count, ppos,
					      &chan->send_rc,
					      sizeof(chan->send_rc));
	else
		ret = -EAGAIN;

	if (ret >= 0) {
		spin_lock_irqsave(&chan->lock, flags);
		if (unlikely(IS_ERR_OR_NULL(chan->channel))) {
			dev_err(chan->tdev->dev, "read: mailbox closed: %u\n",
				chan->index);
			ret = -ENODEV;
		} else if (chan->incoming) {
			chan->rx_msg_pending = false;
			// Tell the controller to issue the ACK since userspace
			// has taken the message from the kernel buffer, so the
			// remote sender may send the next message
			hpsc_mbox_rx_ack(chan, 0);
		} else {
			chan->send_ack = false;
			chan->send_rc = 0;
		}
		spin_unlock_irqrestore(&chan->lock, flags);
	}
	return ret;
}

static unsigned int mbox_poll(struct file *filp, poll_table *wait)
{
	struct mbox_chan_dev *chan = filp->private_data;
	unsigned long flags;
	unsigned int rc = 0;

	dev_dbg(chan->tdev->dev, "poll\n");
	poll_wait(filp, &chan->wq, wait);

	spin_lock_irqsave(&chan->lock, flags);
	if (unlikely(IS_ERR_OR_NULL(chan->channel))) {
		dev_err(chan->tdev->dev, "poll: mailbox closed: %u\n",
			chan->index);
		goto out;
	}
	if (chan->rx_msg_pending || chan->send_ack)
		rc |= POLLIN | POLLRDNORM;
	if (!chan->send_ack)
		rc |= POLLOUT | POLLWRNORM;
out:
	spin_unlock_irqrestore(&chan->lock, flags);
	dev_dbg(chan->tdev->dev, "poll ret: %d\n", rc);
	return rc;
}

static const struct file_operations mbox_fops = {
	.owner		= THIS_MODULE,
	.write		= mbox_write,
	.read		= mbox_read,
	.poll		= mbox_poll,
	.open		= mbox_open,
	.release	= mbox_release,
};

static int mbox_chan_dev_init(struct mbox_chan_dev *chan,
			      struct mbox_client_dev *tdev, int minor,
			      const char *name)
{
	struct device *device;
	int rc;
	dev_t devno = MKDEV(tdev->major_num, minor);

	chan->tdev = tdev;
	chan->index = minor;
	spin_lock_init(&chan->lock);
	init_waitqueue_head(&chan->wq);
	cdev_init(&chan->cdev, &mbox_fops);

	rc = cdev_add(&chan->cdev, devno, 1);
	if (rc) {
		dev_err(tdev->dev, "failed to add cdev\n");
		return rc;
	}

	device = device_create(class, NULL, devno, NULL, "%s!%d!%s",
			       class->name, tdev->id, name);
	if (IS_ERR(device)) {
		dev_err(tdev->dev, "failed to create device\n");
		cdev_del(&chan->cdev);
		return PTR_ERR(device);
	}

	return 0;
}

static void mbox_chan_dev_destroy(struct mbox_chan_dev *chan)
{
	device_destroy(class, MKDEV(chan->tdev->major_num, chan->index));
	cdev_del(&chan->cdev);
}

static int mbox_create_dev_files(struct mbox_client_dev *tdev)
{
	struct device_node *dt_node = tdev->dev->of_node;
	struct property *names_prop;
	char devf_name[16];
	const char *fname = NULL;
	int i;
	int rc;

	names_prop = of_find_property(dt_node, DT_MBOX_NAMES_PROP, NULL);

	for (i = 0; i < tdev->num_chans; ++i) {
		if (names_prop) { // name from DT node
			// Advance the iterator over the names
			fname = of_prop_next_string(names_prop, fname);
			if (!fname) {
				dev_err(tdev->dev,
					"fewer items in property '%s' than in property '%s'\n",
					DT_MBOX_NAMES_PROP, DT_MBOXES_PROP);
				rc = -EFAULT;
				goto fail_dev;
			}

		} else { // index with a prefix
			rc = snprintf(devf_name, sizeof(devf_name), "mbox%u", i);
			if (rc < 0) {
				dev_err(tdev->dev,
					"failed to construct mbox device file name: rc %d\n",
					rc);
				rc = -EFAULT;
				goto fail_dev;
			}
			fname = devf_name;
		}

		rc = mbox_chan_dev_init(&tdev->chans[i], tdev, i, fname);
		if (rc) {
			dev_err(tdev->dev, "failed to construct mailbox device\n");
			goto fail_dev;
		}
	}

	return 0;
fail_dev:
	for (--i; i >= 0; --i) // i-th has failed, so no cleanup for i-th
		mbox_chan_dev_destroy(&tdev->chans[i]);
	return rc;
}

static int mbox_client_dev_init(struct mbox_client_dev *tdev,
				struct platform_device *pdev)
{
	dev_t dev;
	int ret;

	tdev->dev = &pdev->dev;
	platform_set_drvdata(pdev, tdev);

	tdev->num_chans = of_count_phandle_with_args(tdev->dev->of_node,
						     DT_MBOXES_PROP,
						     DT_MBOXES_CELLS);
	dev_info(tdev->dev, "num instances in '%s' property: %d\n",
		 DT_MBOXES_PROP, tdev->num_chans);
	if (tdev->num_chans < 0)
		return -EINVAL;

	tdev->chans = devm_kcalloc(&pdev->dev, tdev->num_chans,
				   sizeof(*tdev->chans), GFP_KERNEL);
	if (!tdev->chans)
		return -ENOMEM;

	ret = alloc_chrdev_region(&dev, 0, tdev->num_chans, MBOX_DEVICE_NAME);
	if (ret < 0) {
		dev_err(tdev->dev, "failed to alloc chrdev region\n");
		return ret;
	}
	tdev->major_num = MAJOR(dev);

	tdev->id = ida_simple_get(&mbox_ida, 0, 0, GFP_KERNEL);
	ret = mbox_create_dev_files(tdev);
	if (ret) {
		ida_simple_remove(&mbox_ida, tdev->id);
		unregister_chrdev_region(MKDEV(tdev->major_num, 0),
					 tdev->num_chans);
		return ret;
	}

	return 0;
}

static int hpsc_mbox_userspace_probe(struct platform_device *pdev)
{
	struct mbox_client_dev *tdev;
	int ret;

	dev_info(&pdev->dev, "probe\n");

	tdev = devm_kzalloc(&pdev->dev, sizeof(*tdev), GFP_KERNEL);
	if (!tdev)
		return -ENOMEM;

	ret = mbox_client_dev_init(tdev, pdev);
	if (ret)
		return ret;

	return 0;
}

static int hpsc_mbox_userspace_remove(struct platform_device *pdev)
{
	struct mbox_client_dev *tdev = platform_get_drvdata(pdev);
	int i;

	dev_info(&pdev->dev, "remove\n");

	for (i = tdev->num_chans - 1; i >= 0; --i)
		mbox_chan_dev_destroy(&tdev->chans[i]);

	ida_simple_remove(&mbox_ida, tdev->id);
	unregister_chrdev_region(MKDEV(tdev->major_num, 0), tdev->num_chans);

	return 0;
}

static const struct of_device_id hpsc_mbox_userspace_match[] = {
	{ .compatible = "hpsc-mbox-userspace" },
	{},
};

static struct platform_driver hpsc_mbox_userspace_driver = {
	.driver = {
		.name = "hpsc_mbox_userspace",
		.of_match_table = hpsc_mbox_userspace_match,
	},
	.probe  = hpsc_mbox_userspace_probe,
	.remove = hpsc_mbox_userspace_remove,
};

static int __init hpsc_mbox_userspace_init(void)
{
	int ret;
	pr_info("hpsc-mbox-userspace: init\n");
	class = class_create(THIS_MODULE, MBOX_DEVICE_NAME);
	if (IS_ERR(class)) {
		pr_err("hpsc-mbox-userspace: failed to create %s class\n",
		       MBOX_DEVICE_NAME);
		return PTR_ERR(class);
	}
	ret = platform_driver_register(&hpsc_mbox_userspace_driver);
	if (ret) {
		pr_err("hpsc-mbox-userspace: failed to register driver\n");
		class_destroy(class);
	}
	return ret;
}

static void __exit hpsc_mbox_userspace_exit(void)
{
	pr_info("hpsc-mbox-userspace: exit\n");
	platform_driver_unregister(&hpsc_mbox_userspace_driver);
	class_destroy(class);
	ida_destroy(&mbox_ida);
}

MODULE_DESCRIPTION("HPSC mailbox userspace interface");
MODULE_AUTHOR("Alexei Colin <acolin@isi.edu>");
MODULE_AUTHOR("Connor Imes <cimes@isi.edu>");
MODULE_LICENSE("GPL v2");

module_init(hpsc_mbox_userspace_init);
module_exit(hpsc_mbox_userspace_exit);
