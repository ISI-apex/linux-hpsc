/*
 * HPSC userspace mailbox client.
 * Provides device files for applications at /dev/mbox/<instance>/mbox<num>.
 */
#include <linux/atomic.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/mailbox_client.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/slab.h>
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
	unsigned		major_num;
	unsigned		instance;
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

// May be multiple mailbox instances
// Therefore, manage class at module init/exit, not device probe/remove
static struct class *class;
static atomic_t num_clients = ATOMIC_INIT(0);

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
		dev_warn(client->dev, "sent: got NACK%d\n", r);
	else
		dev_info(client->dev, "sent: got ACK\n");
	mbox_chan_dev->send_rc = r;
	mbox_chan_dev->send_ack = true;
	spin_unlock(&mbox_chan_dev->lock);

	wake_up_interruptible(&mbox_chan_dev->wq);
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
	struct mbox_chan_dev *mbox_chan_dev = container_of(inodep->i_cdev,
							   struct mbox_chan_dev,
							   cdev);
	struct mbox_client_dev *tdev = mbox_chan_dev->tdev;
	unsigned int major = imajor(inodep);
	unsigned int minor = iminor(inodep);
	int ret;

	if (major != tdev->major_num || minor >= tdev->num_chans)
		return -ENODEV;

	spin_lock(&mbox_chan_dev->lock);

	if (mbox_chan_dev->channel) {
		dev_info(tdev->dev, "mailbox %u already claimed\n",
			 mbox_chan_dev->instance_idx);
		ret = -EBUSY;
		goto out;
	}

	// only one thread will make it here
	filp->private_data = mbox_chan_dev;

	// We allow reading of outgoing mbox (to get the [N]ACK), but not
	// writing of incoming mbox.
	mbox_chan_dev->incoming =  (filp->f_mode & FMODE_READ) &&
				  !(filp->f_mode & FMODE_WRITE);

	mbox_client_init(&mbox_chan_dev->client, tdev->dev,
			 mbox_chan_dev->incoming);

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
		mbox_send_message(mbox_chan_dev->channel, NULL);

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

        dev_info(mbox_chan_dev->tdev->dev, "poll\n");

        poll_wait(filp, &mbox_chan_dev->wq, wait);

        if (mbox_chan_dev->rx_msg_pending || mbox_chan_dev->send_ack)
                rc |= POLLIN | POLLRDNORM;
        if (!mbox_chan_dev->send_ack)
                rc |= POLLOUT | POLLWRNORM;

        dev_info(mbox_chan_dev->tdev->dev, "poll ret: %d\n", rc);
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

static int mbox_chan_dev_init(struct mbox_chan_dev *mbox_chan_dev,
			      struct mbox_client_dev *tdev, int minor,
			      const char *name)
{
	struct device *device;
	int rc;
	dev_t devno = MKDEV(tdev->major_num, minor);

	mbox_chan_dev->tdev = tdev;
	mbox_chan_dev->instance_idx = minor;
	spin_lock_init(&mbox_chan_dev->lock);
	init_waitqueue_head(&mbox_chan_dev->wq);
	cdev_init(&mbox_chan_dev->cdev, &mbox_fops);

	rc = cdev_add(&mbox_chan_dev->cdev, devno, 1);
	if (rc) {
		dev_err(tdev->dev, "%s: failed to add cdev\n", __func__);
		return rc;
	}

	device = device_create(class, NULL, devno, NULL, "%s!%d!%s",
			       class->name, tdev->instance, name);
	if (IS_ERR(device)) {
		dev_err(tdev->dev, "%s: failed to create device\n", __func__);
		cdev_del(&mbox_chan_dev->cdev);
		return PTR_ERR(device);
	}

	return 0;
}

static void mbox_chan_dev_destroy(struct mbox_chan_dev *chan)
{
	device_destroy(class, MKDEV(chan->tdev->major_num, chan->instance_idx));
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
	if (!names_prop) 
		dev_dbg(tdev->dev,
			"%s: no '%s' property, not creating named device files\n",
			__func__, DT_MBOX_NAMES_PROP);


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
			if (rc < 0 || rc >= sizeof(devf_name)) {
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
	dev_info(tdev->dev, "%s: num instances in '%s' property: %d\n",
		 __func__, DT_MBOXES_PROP, tdev->num_chans);
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

	tdev->instance = atomic_inc_return(&num_clients) - 1;
	ret = mbox_create_dev_files(tdev);
	if (ret) {
		atomic_dec(&num_clients);
		unregister_chrdev_region(MKDEV(tdev->major_num, 0), tdev->num_chans);
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

	dev_info(&pdev->dev, "registered\n");
	return 0;
}

static int hpsc_mbox_userspace_remove(struct platform_device *pdev)
{
	struct mbox_client_dev *tdev = platform_get_drvdata(pdev);
	int i;

	dev_info(&pdev->dev, "remove\n");

	for (i = tdev->num_chans - 1; i >= 0; --i)
		mbox_chan_dev_destroy(&tdev->chans[i]);

	atomic_dec(&num_clients);
	unregister_chrdev_region(MKDEV(tdev->major_num, 0), tdev->num_chans);

	dev_info(&pdev->dev, "unregistered\n");
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
}

MODULE_DESCRIPTION("HPSC mailbox userspace interface");
MODULE_AUTHOR("Alexei Colin <acolin@isi.edu>");
MODULE_AUTHOR("Connor Imes <cimes@isi.edu>");
MODULE_LICENSE("GPL v2");

module_init(hpsc_mbox_userspace_init);
module_exit(hpsc_mbox_userspace_exit);
