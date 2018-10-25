#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/mailbox_client.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/cdev.h>
#include <linux/poll.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#define HPSC_MBOX_DATA_REGS	16
#define MBOX_MAX_MSG_LEN	(HPSC_MBOX_DATA_REGS * 4) // each reg is 32-bit

#define DT_MBOXES_PROP  "mboxes"
#define DT_MBOX_NAMES_PROP  "mbox-names"
#define DT_MBOXES_CELLS "#mbox-cells"

#define MBOX_DEVICE_NAME "mbox"

struct mbox_client_dev {
	struct device		*dev;
};

struct mbox_chan_dev {
	struct mbox_client_dev	*tdev;
	struct cdev		cdev;
	struct mbox_client	client;
	struct mbox_chan	*channel;
	spinlock_t		lock;
	wait_queue_head_t       wq;

	// Mailbox instance identifiers and config, stays constant
	unsigned		instance_idx;
	bool			incoming;

	// receive or tx buffer
	// NOTE: could alloc on open to not spend heap mem on unused mboxes,
	//       don't bother for now since it's a small amount of mem
	u32			message[HPSC_MBOX_DATA_REGS];

	bool			rx_msg_pending; // a received message is ready to be read
	bool			send_ack; // set when controller notifies us from its ACK ISR
	int			send_rc;  // status code controller gives us for the ACK
};


static struct class *class;
static unsigned major_num = 0;
static int num_chans = 0;
static struct mbox_chan_dev *mbox_chan_dev_ar;


static void mbox_received(struct mbox_client *client, void *message)
{
	struct mbox_chan_dev *mbox_chan_dev = container_of(client, struct mbox_chan_dev, client);
	u32 *msg = message;
	unsigned i;

	spin_lock(&mbox_chan_dev->lock);
	if (mbox_chan_dev->rx_msg_pending) {
		dev_err(mbox_chan_dev->tdev->dev,
			"rx: dropped message: buffer full\n");
	} else {
		// can't memcpy, need 4-byte word reads
		for (i = 0; i < HPSC_MBOX_DATA_REGS; ++i)
			mbox_chan_dev->message[i] = msg[i];
		print_hex_dump_bytes("mailbox rcved", DUMP_PREFIX_ADDRESS,
				     mbox_chan_dev->message, MBOX_MAX_MSG_LEN);
		mbox_chan_dev->rx_msg_pending = true;
	}
	spin_unlock(&mbox_chan_dev->lock);

	if (mbox_chan_dev->rx_msg_pending)
	    wake_up_interruptible(&mbox_chan_dev->wq);
}

static void mbox_sent(struct mbox_client *client, void *message, int r)
{
	struct mbox_chan_dev *mbox_chan_dev = container_of(client, struct mbox_chan_dev, client);
	spin_lock(&mbox_chan_dev->lock);
	if (r)
		dev_warn(client->dev, "send: got NACK%d\n", r);
	else
		dev_info(client->dev, "sent: got ACK\n");
	mbox_chan_dev->send_rc = r;
	mbox_chan_dev->send_ack = true;
	spin_unlock(&mbox_chan_dev->lock);

	wake_up_interruptible(&mbox_chan_dev->wq);
}

static int mbox_open(struct inode *inodep, struct file *filp)
{
	struct mbox_chan_dev *mbox_chan_dev;
	struct mbox_client_dev *tdev;
	struct of_phandle_args spec;
	int ret;

	unsigned int major = imajor(inodep);
	unsigned int minor = iminor(inodep);

	if (major != major_num || minor >= num_chans)
		return -ENODEV;

	mbox_chan_dev = &mbox_chan_dev_ar[minor];
	tdev = mbox_chan_dev->tdev;

	spin_lock(&mbox_chan_dev->lock);

	if (mbox_chan_dev->channel) {
		dev_info(tdev->dev, "mailbox %u already claimed\n",
			 mbox_chan_dev->instance_idx);
		ret = -EBUSY;
		goto out;
	}

	// only one thread will make it here
	filp->private_data = mbox_chan_dev;

	// Yes, framework also parses this prop, but we need the metadata about
	// the direction of the channel here, and we can't get it through the
	// interface into the framework This is a violation of encapsulation,
	// as well as duplication of the parsing code, i.e. convention for
	// mbox-cells meaning, between here and of_xlate callback in the
	// controller (we can invoke that callback from here, but it's not
	// exposing the metadata).
	//
	// If we want to make the direction dynamic, determined by file open
	// mode, we have the opposite problem: can't pass the direction to
	// the common mailbox framework, without modifying that interface.
	if (of_parse_phandle_with_args(tdev->dev->of_node,
				       DT_MBOXES_PROP, DT_MBOXES_CELLS,
				       mbox_chan_dev->instance_idx, &spec)) {
		dev_err(tdev->dev, "%s: can't parse '%s' property\n",
			__func__, DT_MBOXES_PROP);
		ret = -EINVAL;
		goto out;
	}
	// NOTE: protocol also in of_xlate in mbox controller
	mbox_chan_dev->incoming = spec.args[1];
	of_node_put(spec.np);

	// We allow reading of outgoing mbox (to get the [N]ACK), but not
	// writing of incoming mbox.
	if (mbox_chan_dev->incoming && filp->f_mode & FMODE_WRITE) {
		dev_err(tdev->dev, "mailbox test: file access mode disagrees with spec in DT node\n");
		ret = -EINVAL;
		goto out;
	}

	dev_dbg(tdev->dev, "mbox_chan_dev: %p\n", mbox_chan_dev);
	mbox_chan_dev->channel = mbox_request_channel(&mbox_chan_dev->client,
						      mbox_chan_dev->instance_idx);
	if (IS_ERR(mbox_chan_dev->channel)) {
		dev_err(tdev->dev, "request for mbox channel idx %u failed\n",
			mbox_chan_dev->instance_idx);
		mbox_chan_dev->channel = NULL;
		ret = -EIO;
		goto out;
	}

	ret = 0;

out:
	spin_unlock(&mbox_chan_dev->lock);
	return ret;
}

static int mbox_release(struct inode *inodep, struct file *filp)
{
	struct mbox_chan_dev *mbox_chan_dev = filp->private_data;
	// bad user might share FD among threads
	spin_lock(&mbox_chan_dev->lock);
	if (mbox_chan_dev->channel) {
		mbox_free_channel(mbox_chan_dev->channel);
		mbox_chan_dev->channel = NULL;
	}
	spin_unlock(&mbox_chan_dev->lock);
	return 0;
}

static ssize_t mbox_write(struct file *filp, const char __user *userbuf,
			  size_t count, loff_t *ppos)
{
	struct mbox_chan_dev *mbox_chan_dev = filp->private_data;
	struct mbox_client_dev *tdev = mbox_chan_dev->tdev;
	int ret;

	// bad user might share FD among threads
	spin_lock(&mbox_chan_dev->lock);

	BUG_ON(!mbox_chan_dev->channel);
	// file mode should not call this func
	// TODO: unless we add getting ACK through read of !incoming mbox
	BUG_ON(mbox_chan_dev->incoming);

	if (count > MBOX_MAX_MSG_LEN) {
		dev_err(tdev->dev, "message too long: %zd > %d\n", count,
			MBOX_MAX_MSG_LEN);
		ret = -EINVAL;
		goto out;
	}

	ret = copy_from_user(mbox_chan_dev->message, userbuf, count);
	if (ret) {
		dev_err(tdev->dev, "failed to copy msg data from userspace\n");
		ret = -EFAULT;
		goto out;
	}

	print_hex_dump_bytes("mailbox send: ", DUMP_PREFIX_ADDRESS,
			     mbox_chan_dev->message, MBOX_MAX_MSG_LEN);

	mbox_chan_dev->send_ack = false;
	mbox_chan_dev->send_rc = 0;

	ret = mbox_send_message(mbox_chan_dev->channel, mbox_chan_dev->message);
	if (ret < 0) {
		dev_err(tdev->dev, "Failed to send message via mailbox\n");
		ret = -EIO;
		goto out;
	}

	// Note: successful return here does not indicate successful receipt of
	//       sent message by the other end
out:
	spin_unlock(&mbox_chan_dev->lock);
	return ret < 0 ? ret : count;
}

static ssize_t mbox_read(struct file *filp, char __user *userbuf, size_t count,
			 loff_t *ppos)
{
	struct mbox_chan_dev *mbox_chan_dev = filp->private_data;
	int ret;

	// bad user might share FD among threads
	spin_lock(&mbox_chan_dev->lock);

	if (mbox_chan_dev->incoming) {

		if (!mbox_chan_dev->rx_msg_pending) {
			ret = -EAGAIN;
			goto out;
		}

		ret = simple_read_from_buffer(userbuf, MBOX_MAX_MSG_LEN, ppos,
					      mbox_chan_dev->message,
					      MBOX_MAX_MSG_LEN);
		mbox_chan_dev->rx_msg_pending = false;

		// Tell the controller to issue the ACK, since userspace has
		// taken the message from the kernel, so the remote sender may send
		// the next message, with the guarantee that we have an empty buffer
		// to accept it (since we have a buffer of size 1 message only).
		// NOTE: yes, this is abuse of the method, but otherwise we need to
		// add another method to the interface.
		mbox_client_peek_data(mbox_chan_dev->channel);

	} else { // outgoing, return the ACK

		if (!mbox_chan_dev->send_ack) {
			ret = -EAGAIN;
			goto out;
		}

		ret = simple_read_from_buffer(userbuf, MBOX_MAX_MSG_LEN, ppos,
					      &mbox_chan_dev->send_rc,
					      sizeof(mbox_chan_dev->send_rc));

		// If we clear here, then userspace can only fetch [N]ACK once
		mbox_chan_dev->send_ack = false;
		mbox_chan_dev->send_rc = 0;
	}
out:
	spin_unlock(&mbox_chan_dev->lock);
	return ret;
}

static unsigned int mbox_poll(struct file *filp, poll_table *wait)
{
	struct mbox_chan_dev *mbox_chan_dev = filp->private_data;
        unsigned int rc = 0;

        dev_err(mbox_chan_dev->tdev->dev, "poll\n");

        poll_wait(filp, &mbox_chan_dev->wq, wait);

        if (mbox_chan_dev->rx_msg_pending || mbox_chan_dev->send_ack)
                rc |= POLLIN | POLLRDNORM;
        if (!mbox_chan_dev->send_ack)
                rc |= POLLOUT | POLLWRNORM;

        dev_err(mbox_chan_dev->tdev->dev, "poll ret: %d\n", rc);
        return rc;
}

static const struct file_operations mbox_fops = {
	.owner		= THIS_MODULE,
	.write		= mbox_write,
	.read		= mbox_read,
	.poll           = mbox_poll,
	.open		= mbox_open,
	.release	= mbox_release,
};

static void mbox_client_init(struct mbox_client *cl, struct device *dev)
{
	cl->dev			= dev;
	cl->rx_callback		= mbox_received;
	cl->tx_done		= mbox_sent;
	cl->tx_block		= false;
	cl->knows_txdone	= false;
}

static int mbox_device_create(struct mbox_chan_dev *mbox_chan_dev,
			      struct mbox_client_dev *tdev, int major,
			      int minor, struct class *class, const char *name)
{
	struct device *device;
	int rc;
	dev_t devno = MKDEV(major, minor);

	mbox_chan_dev->tdev = tdev;
	mbox_chan_dev->instance_idx = minor;
	mbox_client_init(&mbox_chan_dev->client, tdev->dev);
	spin_lock_init(&mbox_chan_dev->lock);
	init_waitqueue_head(&mbox_chan_dev->wq);
	cdev_init(&mbox_chan_dev->cdev, &mbox_fops);

	rc = cdev_add(&mbox_chan_dev->cdev, devno, 1);
	if (rc) {
		dev_err(tdev->dev, "%s: failed to add cdev\n", __func__);
		return rc;
	}

	device = device_create(class, /* parent */ NULL, devno, NULL, name);
	if (IS_ERR(device)) {
		rc = PTR_ERR(device);
		dev_err(tdev->dev, "%s: failed to create device\n", __func__);
		cdev_del(&mbox_chan_dev->cdev);
		return rc;
	}

	return 0;
}

static void mbox_device_destroy(struct mbox_chan_dev *mbox_chan_dev, int major,
				int minor, struct class *class)
{
	device_destroy(class, MKDEV(major, minor));
	cdev_del(&mbox_chan_dev->cdev);
}

static int mbox_create_dev_files(struct platform_device *pdev,
				 struct mbox_client_dev *tdev)
{
	struct device_node *dt_node = tdev->dev->of_node;
	struct property *names_prop;
	char devf_name[16];
	const char *mbox_name = NULL;
	const char *fname;
	int i;
	int ret, rc;
	dev_t dev;

	names_prop = of_find_property(dt_node, DT_MBOX_NAMES_PROP, NULL);
	if (names_prop) {
		mbox_name = of_prop_next_string(names_prop, NULL);
		if (!mbox_name)
			dev_err(&pdev->dev,
				"%s: no values in '%s' prop string list\n",
				__func__, DT_MBOX_NAMES_PROP);
	} else {
		dev_err(&pdev->dev,
			"%s: no '%s' property, not creating named device files\n",
			__func__, DT_MBOX_NAMES_PROP);
	}

	mbox_chan_dev_ar = devm_kzalloc(&pdev->dev, num_chans * sizeof(struct mbox_chan_dev), GFP_KERNEL);
	if (mbox_chan_dev_ar == NULL) {
		dev_err(&pdev->dev, "failed to alloc mailbox instance state\n");
		rc = -ENOMEM;
		goto fail_devf;
	}

	rc = alloc_chrdev_region(&dev, /* baseminor */ 0, num_chans,
				 MBOX_DEVICE_NAME);
	if (rc < 0) {
		dev_err(&pdev->dev, "failed to alloc chrdev region\n");
		rc = -EFAULT;
		goto fail_region;
	}
	major_num = MAJOR(dev);

	class = class_create(THIS_MODULE, MBOX_DEVICE_NAME);
	if (IS_ERR(class)) {
		dev_err(&pdev->dev, "failed to alloc chrdev region\n");
		rc = -EFAULT;
		goto fail_class;
	}

	for (i = 0; i < num_chans; ++i) {
		if (names_prop && mbox_name) { // name from DT node
			fname = mbox_name;

		// Advance the iterator over the names
		mbox_name = of_prop_next_string(names_prop, mbox_name);
		if (!mbox_name && i < num_chans - 1) {
			dev_err(&pdev->dev,
				"fewer items in property '%s' than in property '%s'\n",
				DT_MBOX_NAMES_PROP, DT_MBOXES_PROP);
			rc = -EFAULT;
			goto fail_dev;
		}

		} else { // index with a prefix
			ret = snprintf(devf_name, sizeof(devf_name), "mbox%u", i);
			if (ret < 0 || ret >= sizeof(devf_name)) {
				dev_err(&pdev->dev,
					"failed to construct mbox device file name: rc %d\n",
					ret);
				rc = -EFAULT;
				goto fail_dev;
			}
			fname = devf_name;
		}

		rc = mbox_device_create(&mbox_chan_dev_ar[i], tdev, major_num,
					i, class, fname);
		if (rc) {
			dev_err(&pdev->dev, "failed to construct mailbox device\n");
			goto fail_dev;
		}
	}

	return 0;
fail_dev:
	for (--i; i >= 0; --i) // i-th has failed, so no cleanup for i-th
		mbox_device_destroy(&mbox_chan_dev_ar[i], major_num, i, class);
	class_destroy(class);
fail_class:
	unregister_chrdev_region(MKDEV(major_num, 0), num_chans);
fail_region:
	kfree(mbox_chan_dev_ar);
fail_devf:
	return rc;
}

static int mbox_test_probe(struct platform_device *pdev)
{
	struct mbox_client_dev *tdev;
	int ret;

	dev_info(&pdev->dev, "mailbox test: probe\n");

	tdev = devm_kzalloc(&pdev->dev, sizeof(*tdev), GFP_KERNEL);
	if (!tdev) {
		dev_err(&pdev->dev, "mailbox test: no mem\n");
		return -ENOMEM;
	}

	tdev->dev = &pdev->dev;
	platform_set_drvdata(pdev, tdev);

	num_chans = of_count_phandle_with_args(tdev->dev->of_node,
					       DT_MBOXES_PROP, DT_MBOXES_CELLS);
	dev_warn(tdev->dev, "%s: num instances in '%s' property: %d\n",
		 __func__, DT_MBOXES_PROP, num_chans);
	if (num_chans < 0)
		return -EINVAL;

	ret = mbox_create_dev_files(pdev, tdev);
	if (ret)
		return ret;

	dev_info(&pdev->dev, "Successfully registered\n");
	return 0;
}

static int mbox_test_remove(struct platform_device *pdev)
{
	int i;

	for (i = num_chans - 1; i >= 0; --i)
		mbox_device_destroy(&mbox_chan_dev_ar[i], major_num, i, class);

	class_destroy(class);
	unregister_chrdev_region(MKDEV(major_num, 0), num_chans);

	// mbox_chan_dev deallocated by device manager

	return 0;
}

static const struct of_device_id mbox_test_match[] = {
	{ .compatible = "mailbox-client-userspace" },
	{},
};

static struct platform_driver mbox_test_driver = {
	.driver = {
		.name = "mailbox_client_userspace",
		.of_match_table = mbox_test_match,
	},
	.probe  = mbox_test_probe,
	.remove = mbox_test_remove,
};
module_platform_driver(mbox_test_driver);

MODULE_DESCRIPTION("HPSC Mailbox client with interface to user-space");
MODULE_AUTHOR("HPSC");
MODULE_LICENSE("GPL v2");
