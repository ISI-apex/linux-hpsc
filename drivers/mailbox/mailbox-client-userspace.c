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
#include <linux/uaccess.h>
#include <linux/cdev.h>

#define HPSC_MBOX_DATA_REGS     16
#define MBOX_MAX_MSG_LEN	HPSC_MBOX_DATA_REGS * 4 // each reg is 32-bit

#define DT_MBOXES_PROP  "mboxes"
#define DT_MBOX_NAMES_PROP  "mbox-names"
#define DT_MBOXES_CELLS "#mbox-cells"

#define MBOX_DEVICE_NAME "mbox"

static struct class *class;
static unsigned major_num = 0;
static int num_instances = 0;

static struct devf_data *devf_data_ar;

struct mbox_test_device {
	struct device		*dev;
	spinlock_t		lock;
};

struct devf_data {
	struct device		*dev;
        struct cdev             cdev;
        struct mbox_test_device *tdev;
        struct mbox_client      client;
	spinlock_t		lock;
	struct mbox_chan	*channel;

        // Mailbox instance identifiers and config, stays constant
        unsigned instance_idx;
        bool incoming;

        // receive or tx buffer
        // NOTE: could alloc on open to not spend heap mem on used mboxes,
        //       don't bother for now since it's a small amount of mem
	uint32_t                message[HPSC_MBOX_DATA_REGS];

        bool                    rx_msg;
        bool                    send_ack; // set when controller notifies us from its ACK ISR
        int                     send_rc;  // status code controller gives us for the ACK
};

static void mbox_test_receive_message(struct mbox_client *client, void *message)
{
	struct devf_data *devf_data = container_of(client, struct devf_data, client);
	struct mbox_test_device *tdev = devf_data->tdev;
        uint32_t *msg = message;
        unsigned i;

        // We don't need to lock here, because this is called from the receive ISR
        // and ISRs for the same IRQ are serialized.

        // TODO: so this can't race with itself, but can still race calls into
        // open/read/write/close from userspace

        if (devf_data->rx_msg) {
		dev_err(tdev->dev, "rx: dropped message: buffer full\n");
                return;
        }

        // TODO: memcpy? Need 4-byte word reads, tho.
        for (i = 0; i < HPSC_MBOX_DATA_REGS; ++i)
                devf_data->message[i] = msg[i];

        print_hex_dump_bytes("mailbox rcved", DUMP_PREFIX_ADDRESS,
                             devf_data->message, MBOX_MAX_MSG_LEN);

        devf_data->rx_msg = true;
}

static void mbox_test_message_sent(struct mbox_client *client,
				   void *message, int r)
{
	struct devf_data *devf_data = container_of(client, struct devf_data, client);

        // TODO: so this can't race with itself, but can still race calls into
        // open/read/write/close from userspace

	if (r)
		dev_warn(client->dev, "send: got NACK%d\n", r);
	else
		dev_info(client->dev, "sent: got ACK\n");

        devf_data->send_rc = r;
        devf_data->send_ack = true;
        // TODO: set an [N]ACK flag so that read() on an !incoming mbox return success
}

static struct mbox_chan *
mbox_test_request_channel(struct mbox_client *client, struct device *dev, unsigned index)
{
	struct mbox_chan *channel;

	client->dev		= dev;
        client->rx_callback	= mbox_test_receive_message;
        client->tx_done         = mbox_test_message_sent;
        client->tx_block	= false;
        client->knows_txdone    = false;

	channel = mbox_request_channel(client, index);
	if (IS_ERR(channel)) {
		dev_warn(dev, "Failed to request %u channel\n", index);
		return NULL;
	}

	return channel;
}

static int mbox_test_message_open(struct inode *inodep, struct file *filp)
{
	struct devf_data *devf_data;
	struct mbox_test_device *tdev;
	struct of_phandle_args spec;

        unsigned int major = imajor(inodep);
        unsigned int minor = iminor(inodep);

        if (major != major_num || minor < 0 || minor >= num_instances)
                return -ENODEV;

        devf_data = &devf_data_ar[minor];
        tdev = devf_data->tdev;

        filp->private_data = devf_data;

        spin_lock(&devf_data->lock); // protects devf_data->channel pointer

        // TODO: race with ISR?

        if (devf_data->channel) {
            spin_unlock(&devf_data->lock);
            dev_info(tdev->dev, "mailbox %u already claimed\n", devf_data->instance_idx);
            return -EBUSY;
        }

        // only one thread will make it here

        dev_info(tdev->dev, "devf_data: %p\n", devf_data);
	devf_data->channel = mbox_test_request_channel(&devf_data->client, tdev->dev, devf_data->instance_idx);
        dev_info(tdev->dev, "tx channel: %p\n", devf_data->channel);
        spin_unlock(&devf_data->lock);

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
				       devf_data->instance_idx, &spec)) {
		dev_warn(tdev->dev, "%s: can't parse \"mboxes\" property\n", __func__);
		return -EINVAL;
	}

        // NOTE: protocol also in of_xlate in mbox controller
        devf_data->incoming = spec.args[1];

        // We allow reading of outgoing mbox (to get the [N]ACK), but not
        // writing of incoming mbox.
        /* devf_data->incoming populated by the request call above */
        if (( devf_data->incoming && filp->f_mode & FMODE_WRITE)) {
                dev_err(tdev->dev, "mailbox test: file access mode disagrees with spec in DT node\n");
                return -EINVAL;
        }

        return 0;
}

static int mbox_test_message_release(struct inode *inodep, struct file *filp)
{
	struct devf_data *devf_data = filp->private_data;

        // TODO: race with ISR

        spin_lock(&devf_data->lock); // bad user might share FD among threads
        if (devf_data->channel) {
            mbox_free_channel(devf_data->channel);
            devf_data->channel = NULL;
        }
        spin_unlock(&devf_data->lock);
        return 0; // TODO: is there a default implementation that we need to call?
}

static ssize_t mbox_test_message_write(struct file *filp,
				       const char __user *userbuf,
				       size_t count, loff_t *ppos)
{
	struct devf_data *devf_data = filp->private_data;
	struct mbox_test_device *tdev = devf_data->tdev;
	int ret;

        spin_lock(&devf_data->lock); // bad user might share FD among threads

        // TODO: race with ISR

        BUG_ON(!devf_data->channel);
        BUG_ON(devf_data->incoming); // file mode should not call this func
                                     // TODO: unless we add getting ACK through read of !incoming mbox

	if (count > MBOX_MAX_MSG_LEN) {
		dev_err(tdev->dev, "message too long: %zd > %d\n",
			count, MBOX_MAX_MSG_LEN);
		ret = -EINVAL;
                goto out;
	}

	ret = copy_from_user(devf_data->message, userbuf, count);
	if (ret) {
		dev_err(tdev->dev, "failed to copy msg data from userspace\n");
		ret = -EFAULT;
		goto out;
	}

	print_hex_dump_bytes("mailbox send: ", DUMP_PREFIX_ADDRESS,
			     devf_data->message, MBOX_MAX_MSG_LEN);

        devf_data->send_ack = false;
        devf_data->send_rc = 0;

	ret = mbox_send_message(devf_data->channel, devf_data->message);
	if (ret < 0) {
		dev_err(tdev->dev, "Failed to send message via mailbox\n");
		ret = -EIO;
		goto out;
        }

        // TODO: return from here does not indicate successful receipt of
        //       sent message by the other end
out:
        spin_unlock(&devf_data->lock);
	return ret < 0 ? ret : count;
}

static ssize_t mbox_test_message_read(struct file *filp, char __user *userbuf,
				      size_t count, loff_t *ppos)
{
	struct devf_data *devf_data = filp->private_data;
	int ret;

        // TODO: read of !incoming mailbox should return acknowledgement

        spin_lock(&devf_data->lock); // bad user might share FD among threads

        // TODO: race with ISR

        if (devf_data->incoming) {

            if (!devf_data->rx_msg) {
                    ret = -EAGAIN;
                    goto out;
            }

            ret = simple_read_from_buffer(userbuf, MBOX_MAX_MSG_LEN, ppos,
                                           devf_data->message, MBOX_MAX_MSG_LEN);
            devf_data->rx_msg = false;

            // TODO: tell the controller to issue the ACK, since userspace has
            // taken the message from the kernel, so the remote sender may send
            // the next message, with the guarantee that we have an empty buffer
            // to accept it (since we have a buffer of size 1 message only).

        } else { // outgoing, return the ACK

            if (!devf_data->send_ack) {
                    ret = -EAGAIN;
                    goto out;
            }

            ret = simple_read_from_buffer(userbuf, MBOX_MAX_MSG_LEN, ppos,
                                          &devf_data->send_rc,
                                          sizeof(devf_data->send_rc));

            // If we clear here, then userspace can only fetch the [N]ACK once
            devf_data->send_ack = false;
            devf_data->send_rc = 0;
        }
out:
        spin_unlock(&devf_data->lock);
	return ret;
}

static const struct file_operations mbox_fops = {
	.write	= mbox_test_message_write,
	.read	= mbox_test_message_read,
	.open	= mbox_test_message_open,
	.release = mbox_test_message_release,
};

static int mbox_device_create(struct devf_data *devf_data,
                            int major, int minor,
                            struct class *class,
                            const char *name)
{
        int rc = 0;
	struct mbox_test_device *tdev = devf_data->tdev;
        dev_t devno = MKDEV(major, minor);
        struct device *device;

        cdev_init(&devf_data->cdev, &mbox_fops);
        devf_data->cdev.owner = THIS_MODULE;

        rc = cdev_add(&devf_data->cdev, devno, 1);
        if (rc) {
                dev_err(tdev->dev, "%s: failed to add cdev\n", __func__);
                return rc;
        }

        // TODO: double check that device_destroy doesn't need this pointer
        device = device_create(class, /* parent */ NULL, devno, NULL, name);

        if (IS_ERR(device)) {
                rc = PTR_ERR(device);
                dev_err(tdev->dev, "%s: failed to create device\n", __func__);
                cdev_del(&devf_data->cdev);
                return rc;
        }
        return rc;
}

static void mbox_device_destroy(struct devf_data *devf_data,
                            int major, int minor,
                            struct class *class)
{
        device_destroy(class, MKDEV(major, minor));
        cdev_del(&devf_data->cdev);
}

static int mbox_create_dev_files(struct platform_device *pdev,
				 struct mbox_test_device *tdev)
{
        struct devf_data *devf_data;
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

	devf_data_ar = devm_kzalloc(&pdev->dev, num_instances * sizeof(struct devf_data), GFP_KERNEL);
	if (devf_data_ar == NULL) {
		dev_err(&pdev->dev, "failed to alloc mailbox instance state\n");
		rc = -ENOMEM;
                goto fail_devf;
        }

        rc = alloc_chrdev_region(&dev, /* baseminor */ 0, num_instances, MBOX_DEVICE_NAME);
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

        for (i = 0; i < num_instances; ++i) {

            devf_data = &devf_data_ar[i];

            devf_data->dev = &pdev->dev;
            devf_data->tdev = tdev;
            devf_data->instance_idx = i;

	    spin_lock_init(&devf_data->lock);

            if (names_prop && mbox_name) { // name from DT node
                fname = mbox_name;

                // Advance the iterator over the names
                mbox_name = of_prop_next_string(names_prop, mbox_name);
                if (!mbox_name && i < num_instances - 1) {
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

            rc = mbox_device_create(devf_data, major_num, i, class, fname);
            if (rc) {
		dev_err(&pdev->dev, "failed to construct mailbox device\n");
                goto fail_dev;
            }
        }

        return 0;
fail_dev:
        for (--i; i <= 0; --i) // i-th has failed, so no cleanup for i-th
                mbox_device_destroy(&devf_data_ar[i], major_num, i, class);
        class_destroy(class);
fail_class:
        unregister_chrdev_region(MKDEV(major_num, 0), num_instances);
fail_region:
        kfree(devf_data_ar);
fail_devf:
	return rc;
}

static int mbox_test_probe(struct platform_device *pdev)
{
	struct mbox_test_device *tdev;
	int ret;

	dev_info(&pdev->dev, "mailbox test: probe\n");

	tdev = devm_kzalloc(&pdev->dev, sizeof(*tdev), GFP_KERNEL);
	if (!tdev) {
		dev_err(&pdev->dev, "mailbox test: no mem\n");
		return -ENOMEM;
	}

	tdev->dev = &pdev->dev;
	platform_set_drvdata(pdev, tdev);

	spin_lock_init(&tdev->lock);

        num_instances = of_count_phandle_with_args(tdev->dev->of_node,
                                DT_MBOXES_PROP, DT_MBOXES_CELLS);
        dev_warn(tdev->dev, "%s: num instances in '%s' property: %d\n",
                 __func__, DT_MBOXES_PROP, num_instances);
        if (num_instances < 0) {
		return -EINVAL;
        }

	ret = mbox_create_dev_files(pdev, tdev);
	if (ret)
		return ret;

	dev_info(&pdev->dev, "Successfully registered\n");
	return 0;
}

static int mbox_test_remove(struct platform_device *pdev)
{
        int i;

        for (i = num_instances - 1; i <= 0; --i)
                mbox_device_destroy(&devf_data_ar[i], major_num, i, class);

        class_destroy(class);
        unregister_chrdev_region(MKDEV(major_num, 0), num_instances);

        // devf_data deallocated by device manager

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
