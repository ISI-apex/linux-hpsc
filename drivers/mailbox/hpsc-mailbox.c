#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/kernel.h>
#include <linux/mailbox_controller.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>

#define REG_OWNER             0x00
#define REG_INT_ENABLE        0x04
#define REG_INT_CAUSE         0x08
#define REG_INT_STATUS        0x0C
#define REG_INT_CLEAR         0x08 /* TODO: is this overlap by design */
#define REG_INT_SET           0x0C
#define REG_DESTINATION       0x1C
#define REG_DATA              0x20

#define HPSC_MBOX_INT_A 0x1 // receive interrupt
#define HPSC_MBOX_INT_B 0x2 // ack interrupt

#define HPSC_MBOX_DATA_REGS 16
#define HPSC_MBOX_INTS 2
#define HPSC_MBOX_INSTANCES 32
#define HPSC_MBOX_INSTANCE_REGION (REG_DATA + HPSC_MBOX_DATA_REGS * 4)

struct hpsc_mbox {
	void __iomem *regs;
	//spinlock_t lock; // TODO: should not be necessary, because can't access same device file from two cores
	struct mbox_controller controller;
};

struct hpsc_mbox_chan {
        struct hpsc_mbox *mbox;
	void __iomem *regs;

        // Config from DT, stays constant
        unsigned instance;
        bool incoming;
        unsigned owner;
        unsigned dest;

        unsigned rcv_irqnum;
        unsigned ack_irqnum;
        unsigned int_enabled;
};

static struct hpsc_mbox *hpsc_mbox_link_mbox(struct mbox_chan *link)
{
	return container_of(link->mbox, struct hpsc_mbox, controller);
}

static irqreturn_t hpsc_mbox_rcv_irq(int irq, void *dev_id)
{
	struct mbox_chan *link = dev_id;
	struct hpsc_mbox *mbox = hpsc_mbox_link_mbox(link);
	struct hpsc_mbox_chan *chan = link->con_priv;

        dev_dbg(mbox->controller.dev, "rcv ISR instance %u\n", chan->instance);

        mbox_chan_received_data(link, chan->regs + REG_DATA); // TODO: can client memcpy_fromio?

        dev_dbg(mbox->controller.dev, "clear int A\n");
        writel(HPSC_MBOX_INT_A, chan->regs + REG_INT_CLEAR);

	return IRQ_HANDLED;
}

static irqreturn_t hpsc_mbox_ack_irq(int irq, void *dev_id)
{
	struct mbox_chan *link = dev_id;
	struct hpsc_mbox *mbox = hpsc_mbox_link_mbox(link);
	struct hpsc_mbox_chan *chan = link->con_priv;

        dev_dbg(mbox->controller.dev, "ack ISR instance %u\n", chan->instance);

        dev_dbg(mbox->controller.dev, "clear int B\n");
        writel(HPSC_MBOX_INT_B, chan->regs + REG_INT_CLEAR);

        mbox_chan_txdone(link, /* status = OK */ 0);

	return IRQ_HANDLED;
}

static int hpsc_mbox_send_data(struct mbox_chan *link, void *data)
{
	struct hpsc_mbox *mbox = hpsc_mbox_link_mbox(link);
	struct hpsc_mbox_chan *chan = link->con_priv;
        unsigned i;
	u32 *msg = (u32 *)data;

	//spin_lock(&mbox->lock);
	dev_dbg(mbox->controller.dev, "send: ");
        for (i = 0; i < HPSC_MBOX_DATA_REGS; ++i) {
	        writel(msg[i], chan->regs + REG_DATA + i * 4);
	        dev_dbg(mbox->controller.dev, "%08X ", msg[i]);
        }
	dev_dbg(mbox->controller.dev, "\n");

	dev_dbg(mbox->controller.dev, "set int A\n");
	writel(HPSC_MBOX_INT_A, chan->regs + REG_INT_SET);

	//spin_unlock(&mbox->lock);
	return 0;
}

// This function is overloaded/abused, its purpose is changed to:
// the client notifies the controller that the client's receive
// buffer in the kernel is free (i.e. userspace has read the message) so that
// client can receive the next message from the remote sender. Here, the
// controller uses this notification event to send the ack to the remote
// sender. The remote sender is expected to not send another message
// until getting an ACK.
static bool hpsc_mbox_peek_data(struct mbox_chan *link)
{
	struct hpsc_mbox *mbox = hpsc_mbox_link_mbox(link);
	struct hpsc_mbox_chan *chan = link->con_priv;

        dev_dbg(mbox->controller.dev, "client notified of receipt: set int B\n");
        writel(HPSC_MBOX_INT_B, chan->regs + REG_INT_SET);

        return false;
}

static int hpsc_mbox_startup(struct mbox_chan *link)
{
	struct hpsc_mbox *mbox = hpsc_mbox_link_mbox(link);
	struct hpsc_mbox_chan *chan = link->con_priv;
        u32 val, dest;

        // TODO: can this also race with ISR? race through client API?


        // Note: owner+dest is entirely orthogonal to direction.
        // Note; owner+dest are entirely optional, may set both to zero in DT
        // Note: owner/dest access is not enforced by HW, it can only
        //       serve as a mild sanity check.
        if (chan->owner) {
            dev_dbg(mbox->controller.dev, "set owner: %p <- %x\n",
                    chan->regs + REG_OWNER, chan->owner);
            writel(chan->owner, chan->regs + REG_OWNER);
        }

        if (chan->dest) {
                if (chan->owner) { // we are onwer, so set destination
                    dev_dbg(mbox->controller.dev, "write dest: %p <- %x\n",
                            chan->regs + REG_DESTINATION, chan->dest);
                    writel(chan->dest, chan->regs + REG_DESTINATION);
                } else { // we are not the owner, only check
                    dest = readl(chan->regs + REG_DESTINATION);
                    dev_dbg(mbox->controller.dev, "read dest: %p -> %x\n",
                            chan->regs + REG_DESTINATION, dest);
                    if (chan->dest != dest) {
                        dev_err(mbox->controller.dev,
                                "destination mismatch: %x (expected %x)\n",
                                dest, chan->dest);
                        return -EBUSY;
                    }
                }
        }

        if (chan->incoming) {
            enable_irq(chan->rcv_irqnum);

	    dev_dbg(mbox->controller.dev, "enable int A (rcv)\n");
            val = readl(chan->regs + REG_INT_ENABLE);
            writel(val | HPSC_MBOX_INT_A, chan->regs + REG_INT_ENABLE);

        } else { // TX
            enable_irq(chan->ack_irqnum);

	    dev_dbg(mbox->controller.dev, "enable int B (ack)\n");
            val = readl(chan->regs + REG_INT_ENABLE);
            writel(val | HPSC_MBOX_INT_B, chan->regs + REG_INT_ENABLE);
        }

	return 0;
}

static void hpsc_mbox_shutdown(struct mbox_chan *link)
{
	struct hpsc_mbox *mbox = hpsc_mbox_link_mbox(link);
	struct hpsc_mbox_chan *chan = link->con_priv;

        // TODO: race with ISR. Also, cace through client API?

        u32 ie = readl(chan->regs + REG_INT_ENABLE);

        if (chan->incoming) {
	    dev_dbg(mbox->controller.dev, "disable int A (rcv)\n");
            writel(ie & ~HPSC_MBOX_INT_A, chan->regs + REG_INT_ENABLE);

            disable_irq(chan->rcv_irqnum);

        } else { // TX
	    dev_dbg(mbox->controller.dev, "disable int B (ack)\n");
            writel(ie & ~HPSC_MBOX_INT_B, chan->regs + REG_INT_ENABLE);

            disable_irq(chan->ack_irqnum);
        }

        if (chan->owner) {
                dev_dbg(mbox->controller.dev, "clear owner: %p <- 0\n",
                        chan->regs + REG_OWNER);
                writel(0, chan->regs + REG_OWNER);

                // clearing owner also clears dest (resets the instance)
        }
}

static const struct mbox_chan_ops hpsc_mbox_chan_ops = {
        .startup	= hpsc_mbox_startup,
        .shutdown	= hpsc_mbox_shutdown,
        .send_data	= hpsc_mbox_send_data,
        .peek_data      = hpsc_mbox_peek_data,
};

/* Parse the channel identifiers from client's device tree node */
static struct mbox_chan *hpsc_mbox_of_xlate(struct mbox_controller *mbox,
                                            const struct of_phandle_args *sp)
{
       struct mbox_chan *link;
       struct hpsc_mbox_chan *chan;

       if (sp->args_count != 4) {
	       dev_err(mbox->dev, "invalid mailbox instance reference in DT node\n");
               return NULL;
        }

       link = &mbox->chans[sp->args[0]];

       // Slightly not nice, since adding side-effects to an otherwise pure function
       chan = (struct hpsc_mbox_chan *)link->con_priv;
       chan->incoming = sp->args[1];
       chan->owner = sp->args[2];
       chan->dest = sp->args[3];

       return link;
}

static int hpsc_mbox_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	int ret = 0;
	struct resource *iomem;
	struct hpsc_mbox *mbox;
        struct hpsc_mbox_chan *con_priv, *chan;
        unsigned i;

	mbox = devm_kzalloc(dev, sizeof(*mbox), GFP_KERNEL);
	if (mbox == NULL)
		return -ENOMEM;
	//spin_lock_init(&mbox->lock);

	iomem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	mbox->regs = devm_ioremap_resource(&pdev->dev, iomem);
	if (IS_ERR(mbox->regs)) {
		ret = PTR_ERR(mbox->regs);
		dev_err(&pdev->dev, "Failed to remap mailbox regs: %d\n", ret);
		return ret;
	}

	mbox->controller.txdone_irq = true;
	mbox->controller.ops = &hpsc_mbox_chan_ops;
        mbox->controller.of_xlate = &hpsc_mbox_of_xlate;
	mbox->controller.dev = dev;
	mbox->controller.num_chans = HPSC_MBOX_INSTANCES;
	mbox->controller.chans = devm_kzalloc(dev,
		sizeof(*mbox->controller.chans) * HPSC_MBOX_INSTANCES, GFP_KERNEL);
	if (!mbox->controller.chans)
		return -ENOMEM;

        // Allocate space for private state as a contiguous array, to reduce overhead.
        // Note: could also allocate on demand, but let's trade-off some memory for efficiency.
        con_priv = devm_kzalloc(dev,
                    sizeof(struct hpsc_mbox_chan) * mbox->controller.num_chans, GFP_KERNEL);
        if (!con_priv)
                return -ENOMEM;
        for (i = 0; i < mbox->controller.num_chans; ++i) {
                chan = &con_priv[i];

                chan->rcv_irqnum = irq_of_parse_and_map(dev->of_node, 2 * i);
                dev_info(dev, "hpsc_mbox_probe: rcv irq %u\n", chan->rcv_irqnum);
                ret = devm_request_irq(dev, chan->rcv_irqnum,
                                       hpsc_mbox_rcv_irq, 0, dev_name(dev), &mbox->controller.chans[i]);
                disable_irq(chan->rcv_irqnum); // TODO: is there a way to request in disable state?
                if (ret) {
                        dev_err(dev, "Failed to register mailbox rcv IRQ handler: %d\n",
                                ret);
                        return -ENODEV;
                }

                chan->ack_irqnum = irq_of_parse_and_map(dev->of_node, 2 * i + 1);
                dev_info(dev, "hpsc_mbox_probe: ack irq %u\n", chan->ack_irqnum);
                ret = devm_request_irq(dev, chan->ack_irqnum,
                                       hpsc_mbox_ack_irq, 0, dev_name(dev), &mbox->controller.chans[i]);
                disable_irq(chan->ack_irqnum); // TODO: is there a way to request in disable state?
                if (ret) {
                        dev_err(dev, "Failed to register mailbox ack IRQ handler: %d\n",
                                ret);
                        return -ENODEV;
                }

                chan->mbox = mbox;
                chan->instance = i;
                chan->regs = mbox->regs + i * HPSC_MBOX_INSTANCE_REGION;
                mbox->controller.chans[i].con_priv = &con_priv[i];
        }

	ret = mbox_controller_register(&mbox->controller);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, mbox);
	dev_info(dev, "mailbox enabled\n");
	dev_dbg(dev, "mailbox dynamic debug enabled\n");

	return ret;
}

static int hpsc_mbox_remove(struct platform_device *pdev)
{
	struct hpsc_mbox *mbox = platform_get_drvdata(pdev);
	mbox_controller_unregister(&mbox->controller);
	return 0;
}

static const struct of_device_id hpsc_mbox_of_match[] = {
	{ .compatible = "hpsc,hpsc-mbox", },
	{},
};
MODULE_DEVICE_TABLE(of, hpsc_mbox_of_match);

static struct platform_driver hpsc_mbox_driver = {
	.driver = {
		.name = "hpsc_mbox",
		.of_match_table = hpsc_mbox_of_match,
	},
	.probe		= hpsc_mbox_probe,
	.remove		= hpsc_mbox_remove,
};
module_platform_driver(hpsc_mbox_driver);

MODULE_AUTHOR("HPSC");
MODULE_DESCRIPTION("Mailbox for HPSC chiplet");
MODULE_LICENSE("GPL v2");
