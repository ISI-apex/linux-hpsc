#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/kernel.h>
#include <linux/mailbox_client.h>
#include <linux/mailbox_controller.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>

#define REG_CONFIG              0x00
#define REG_EVENT_CAUSE         0x04
#define REG_EVENT_CLEAR         0x04
#define REG_EVENT_STATUS        0x08
#define REG_EVENT_SET           0x08
#define REG_INT_ENABLE          0x0C
#define REG_DATA                0x10

#define REG_CONFIG__UNSECURE      0x1
#define REG_CONFIG__OWNER__SHIFT  8
#define REG_CONFIG__OWNER__MASK   0x0000ff00
#define REG_CONFIG__SRC__SHIFT    16
#define REG_CONFIG__SRC__MASK     0x00ff0000
#define REG_CONFIG__DEST__SHIFT   24
#define REG_CONFIG__DEST__MASK    0xff000000

#define HPSC_MBOX_EVENT_A 0x1
#define HPSC_MBOX_EVENT_B 0x2

// rcv (map event A to int 'idx')
#define HPSC_MBOX_INT_A(idx) (1 << (2 * (idx)))
// ack (map event B to int 'idx')
#define HPSC_MBOX_INT_B(idx) (1 << (2 * (idx) + 1))

#define HPSC_MBOX_DATA_REGS 16
#define HPSC_MBOX_INTS 2
#define HPSC_MBOX_INSTANCES 32
#define HPSC_MBOX_INSTANCE_REGION (REG_DATA + HPSC_MBOX_DATA_REGS * 4)

#define DT_PROP_INTERRUPT_IDX_RCV "interrupt-idx-rcv"
#define DT_PROP_INTERRUPT_IDX_ACK "interrupt-idx-ack"

struct hpsc_mbox {
	void __iomem *regs;
	struct mbox_controller controller;
	unsigned rcv_int_idx;
	unsigned ack_int_idx;
	unsigned rcv_irqnum;
	unsigned ack_irqnum;
};

struct hpsc_mbox_chan {
	struct hpsc_mbox *mbox;
	void __iomem *regs;

	// Config from DT, stays constant
	unsigned instance;
	unsigned owner;
	unsigned src;
	unsigned dest;
};

static struct hpsc_mbox *hpsc_mbox_link_mbox(struct mbox_chan *link)
{
	return container_of(link->mbox, struct hpsc_mbox, controller);
}

static void hpsc_mbox_memcpy_toio(void __iomem *dest, void *src)
{
	int i;
	for (i = 0; i < HPSC_MBOX_DATA_REGS; i++)
		writel(((u32 *)src)[i], dest + i * 4);
}

static void hpsc_mbox_memcpy_fromio(void *dest, void __iomem *src)
{
	int i;
	for (i = 0; i < HPSC_MBOX_DATA_REGS; i++)
		((u32 *)dest)[i] = readl(src + i * 4);
}

static void hpsc_mbox_send_ack(struct hpsc_mbox_chan *chan, int r)
{
	if (unlikely(r)) {
		// TODO: use a different event than ACK
		dev_dbg(chan->mbox->controller.dev, "NACK: set int C: %d\n", r);
		writel(HPSC_MBOX_EVENT_B, chan->regs + REG_EVENT_SET);
	} else {
		dev_dbg(chan->mbox->controller.dev, "ACK: set int B\n");
		writel(HPSC_MBOX_EVENT_B, chan->regs + REG_EVENT_SET);
	}
}

static bool hpsc_mbox_is_subscribed(struct hpsc_mbox_chan *chan, unsigned event,
				    unsigned interrupt)
{
	// Are we 'signed up' for this event (A) from this channel?
	// Two criteria: (1) Cause (or Status) is set, and (2) Mapped to our IRQ
	u32 val = readl(chan->regs + REG_EVENT_CAUSE);
	if (!(val & event)) {
		val = readl(chan->regs + REG_EVENT_STATUS);
		if (!(val & event))
			return false;
	}
	val = readl(chan->regs + REG_INT_ENABLE);
	if (!(val & interrupt))
		return false;
	return true;
}

static void hpsc_mbox_clear_event(struct hpsc_mbox_chan *chan, unsigned event)
{
	dev_dbg(chan->mbox->controller.dev, "clear event: %u\n", event);
	writel(event, chan->regs + REG_EVENT_CLEAR);
}

static irqreturn_t hpsc_mbox_isr(struct hpsc_mbox *mbox, unsigned event,
				 unsigned interrupt)
{
	u32 data[HPSC_MBOX_DATA_REGS];
	struct mbox_chan *link;
	struct hpsc_mbox_chan *chan;
	unsigned i;

	// Check all mailbox instances; could do better if we maintain another
	// list of actually enabled mailboxes; could do even better if HW
	// provides disambiguation information about (instance index).
	for (i = 0; i < mbox->controller.num_chans; ++i) {
		link = &mbox->controller.chans[i];
		chan = link->con_priv;

		if (!hpsc_mbox_is_subscribed(chan, event, interrupt))
			continue;

		dev_dbg(mbox->controller.dev, "ISR %u instance %u\n", event,
			chan->instance);

		// This could be resolved statically, at the cost of duplicating
		// the disambiguation code in both ISRs or using callbacks
		switch (event) {
		case HPSC_MBOX_EVENT_A:
			// Note: Race condition on link->cl between if statement
			// and mbox_chan_received_data, but using link->lock as
			// a guard can deadlock. Since this is only an
			// optimization to send NACKs, worst case scenario is
			// that we don't NACK if channel is closed.
			// Events should be cleared before sending new messages
			// or [N]ACKs, otherwise IRQ may be raised again
			if (likely(link->cl)) {
				hpsc_mbox_memcpy_fromio(data,
							chan->regs + REG_DATA);
				hpsc_mbox_clear_event(chan, event);
				mbox_chan_received_data(link, data);
			} else {
				dev_warn(mbox->controller.dev,
					 "chan closed before IRQ handled: %u\n",
					 chan->instance);
				hpsc_mbox_clear_event(chan, event);
				hpsc_mbox_send_ack(chan, -ENOLINK);
			}
			break;
		case HPSC_MBOX_EVENT_B:
			// can't use link lock here, but we don't actually care
			hpsc_mbox_clear_event(chan, event);
			mbox_chan_txdone(link, /* status = OK */ 0);
			break;
		}
	}
	return IRQ_HANDLED;
}

// In the following, we introduce ambiguity (which event) that then has to be
// resolved dynamically in the common function, which is wasteful, and the only
// reason for it is to avoid duplicating the common code; perhaps a callback
// would be a middle ground.
static irqreturn_t hpsc_mbox_rcv_irq(int irq, void *priv)
{
	struct hpsc_mbox *mbox = priv;
	return hpsc_mbox_isr(mbox, HPSC_MBOX_EVENT_A,
			     HPSC_MBOX_INT_A(mbox->rcv_int_idx));
}
static irqreturn_t hpsc_mbox_ack_irq(int irq, void *priv)
{
	struct hpsc_mbox *mbox = priv;
	return hpsc_mbox_isr(mbox, HPSC_MBOX_EVENT_B,
			     HPSC_MBOX_INT_B(mbox->ack_int_idx));
}

static int hpsc_mbox_send_data(struct mbox_chan *link, void *data)
{
	struct hpsc_mbox *mbox = hpsc_mbox_link_mbox(link);
	struct hpsc_mbox_chan *chan = link->con_priv;

	if (IS_ERR_OR_NULL(data)) {
		hpsc_mbox_send_ack(chan, PTR_ERR_OR_ZERO(data));
	} else {
		hpsc_mbox_memcpy_toio(chan->regs + REG_DATA, data);
		dev_dbg(mbox->controller.dev, "set int A\n");
		writel(HPSC_MBOX_EVENT_A, chan->regs + REG_EVENT_SET);
	}

	return 0;
}

static int hpsc_mbox_maybe_claim_owner(struct hpsc_mbox_chan *chan)
{
	u32 config;
	u32 config_claimed;
	if (chan->owner) {
		config = ((chan->owner << REG_CONFIG__OWNER__SHIFT)
				& REG_CONFIG__OWNER__MASK) |
			 ((chan->src << REG_CONFIG__SRC__SHIFT)
				& REG_CONFIG__SRC__MASK) |
			 ((chan->dest << REG_CONFIG__DEST__SHIFT)
				& REG_CONFIG__DEST__MASK) |
			 REG_CONFIG__UNSECURE;

		dev_dbg(chan->mbox->controller.dev, "set config: %p <- %x\n",
			chan->regs + REG_CONFIG, config);
		writel(config, chan->regs + REG_CONFIG);

		config_claimed = readl(chan->regs + REG_CONFIG);
		dev_dbg(chan->mbox->controller.dev, "read config: %p <- %x\n",
			chan->regs + REG_CONFIG, config_claimed);
		if (config_claimed != config) {
			dev_err(chan->mbox->controller.dev,
				"failed to claim mbox: config %x != %x\n",
				config, config_claimed);
			return -EBUSY;
		}
	}
	return 0;
}

static void hpsc_mbox_maybe_release_owner(struct hpsc_mbox_chan *chan)
{
	if (chan->owner) {
		// clearing owner also clears dest (resets the instance)
		dev_dbg(chan->mbox->controller.dev, "clear config: %p <- 0\n",
			chan->regs + REG_CONFIG);
		writel(0, chan->regs + REG_CONFIG);
	}
}

static int hpsc_mbox_verify_config(struct hpsc_mbox_chan *chan, bool is_recv,
				   bool is_send)
{
	u32 config;
	u32 owner;
	u32 src;
	u32 dest;
	if (chan->src || chan->dest) {
		config = readl(chan->regs + REG_CONFIG);
		dev_dbg(chan->mbox->controller.dev, "read config: %p <- %x\n",
			chan->regs + REG_CONFIG, config);

		owner = (config & REG_CONFIG__OWNER__MASK) >> REG_CONFIG__OWNER__SHIFT;
		src   = (config & REG_CONFIG__SRC__MASK)   >> REG_CONFIG__SRC__SHIFT;
		dest  = (config & REG_CONFIG__DEST__MASK)  >> REG_CONFIG__DEST__SHIFT;

		if ((is_recv && chan->dest && dest != chan->dest) ||
		    (is_send && chan->src && src != chan->src)) {
			dev_err(chan->mbox->controller.dev,
				"src/dest mismatch: %x/%x (expected %x/%x)\n",
				src, dest, chan->src, chan->dest);
			return -EBUSY;
		}
	}
	return 0;
}

static int hpsc_mbox_startup(struct mbox_chan *link)
{
	struct hpsc_mbox *mbox = hpsc_mbox_link_mbox(link);
	struct hpsc_mbox_chan *chan = link->con_priv;
	u32 ie;
	int ret;
	// conceivably, send and recv not mutually exclusive
	bool is_recv = link->cl->rx_callback;
	bool is_send = link->cl->tx_done;

	// Note: owner+dest is entirely orthogonal to direction.
	// Note; owner+src+dest are entirely optional, may set both to zero in DT
	// Note: owner/src/dest access is not enforced by HW, it can only
	//       serve as a mild sanity check.
	ret = hpsc_mbox_maybe_claim_owner(chan);
	if (ret)
		return ret;

	// regardless of whether we're owner or not, check config
	ret = hpsc_mbox_verify_config(chan, is_recv, is_send);
	if (ret) {
		hpsc_mbox_maybe_release_owner(chan);
		return ret;
	}

	// only enable interrupts if our client can handle them
	// otherwise, another entity is expected to process the interrupts
	ie = readl(chan->regs + REG_INT_ENABLE);
	if (is_recv)
		ie |= HPSC_MBOX_INT_A(mbox->rcv_int_idx);
	if (is_send)
		ie |= HPSC_MBOX_INT_B(mbox->ack_int_idx);
	dev_dbg(mbox->controller.dev, "instance %u int_enable <- %08x (rcv)\n",
		chan->instance, ie);
	writel(ie, chan->regs + REG_INT_ENABLE);

	return 0;
}

static void hpsc_mbox_shutdown(struct mbox_chan *link)
{
	struct hpsc_mbox *mbox = hpsc_mbox_link_mbox(link);
	struct hpsc_mbox_chan *chan = link->con_priv;

	// Could just rely on HW reset-on-release behavior, but for symmetry...
	u32 ie = readl(chan->regs + REG_INT_ENABLE);
	ie &= ~HPSC_MBOX_INT_A(mbox->rcv_int_idx);
	ie &= ~HPSC_MBOX_INT_B(mbox->ack_int_idx);
	dev_dbg(mbox->controller.dev, "instance %u int_enable <- %08x (rcv)\n",
		chan->instance, ie);
	writel(ie, chan->regs + REG_INT_ENABLE);

	hpsc_mbox_maybe_release_owner(chan);
}

static bool hpsc_mbox_peek_data(struct mbox_chan *link)
{
	u32 data[HPSC_MBOX_DATA_REGS];
	struct hpsc_mbox *mbox = hpsc_mbox_link_mbox(link);
	struct hpsc_mbox_chan *chan = link->con_priv;
	bool ret = hpsc_mbox_is_subscribed(chan, HPSC_MBOX_EVENT_A,
					   HPSC_MBOX_INT_A(mbox->rcv_int_idx));
	dev_dbg(mbox->controller.dev, "peek: %s\n", ret ? "true" : "false");
	if (ret) {
		hpsc_mbox_memcpy_fromio(data, chan->regs + REG_DATA);
		hpsc_mbox_clear_event(chan, HPSC_MBOX_EVENT_A);
		mbox_chan_received_data(link, data);
	}
	return ret;
}

static const struct mbox_chan_ops hpsc_mbox_chan_ops = {
	.startup	= hpsc_mbox_startup,
	.shutdown	= hpsc_mbox_shutdown,
	.send_data	= hpsc_mbox_send_data,
	.peek_data	= hpsc_mbox_peek_data,
};

/* Parse the channel identifiers from client's device tree node */
static struct mbox_chan *hpsc_mbox_of_xlate(struct mbox_controller *mbox,
					    const struct of_phandle_args *sp)
{
	struct mbox_chan *link;
	struct hpsc_mbox_chan *chan;

	if (sp->args[0] >= HPSC_MBOX_INSTANCES) {
		dev_err(mbox->dev,
			"mailbox index in DT node is %u, but must be < %u\n",
			sp->args[0], HPSC_MBOX_INSTANCES);
		return ERR_PTR(-EINVAL);
	}

	link = &mbox->chans[sp->args[0]];

	// Slightly not nice, since adding side-effects to an otherwise pure function
	chan = (struct hpsc_mbox_chan *)link->con_priv;
	chan->owner = sp->args[1];
	chan->src = sp->args[2];
	chan->dest = sp->args[3];

	return link;
}

static void hpsc_mbox_chans_init(struct hpsc_mbox_chan *hpsc_chans,
				 unsigned num_chans, struct hpsc_mbox *mbox,
				 struct mbox_chan *mbox_chans)
{
	struct hpsc_mbox_chan *chan;
	unsigned i;
	for (i = 0; i < num_chans; i++) {
		chan = &hpsc_chans[i];
		chan->mbox = mbox;
		chan->regs = mbox->regs + i * HPSC_MBOX_INSTANCE_REGION;
		chan->instance = i;
		mbox_chans[i].con_priv = chan;
	}
}

static void hpsc_mbox_controller_init(struct mbox_controller *ctlr,
				      struct device *dev,
				      struct mbox_chan *chans, int num_chans)
{
	ctlr->dev = dev;
	ctlr->ops = &hpsc_mbox_chan_ops;
	ctlr->chans = chans;
	ctlr->num_chans = num_chans;
	ctlr->txdone_irq = true;
	ctlr->of_xlate = &hpsc_mbox_of_xlate;
}

static int hpsc_mbox_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	int ret;
	struct hpsc_mbox *mbox;
	struct mbox_chan *chans;
	struct hpsc_mbox_chan *hpsc_chans;
	struct resource *iomem;

	mbox = devm_kzalloc(dev, sizeof(*mbox), GFP_KERNEL);
	chans = devm_kcalloc(dev, HPSC_MBOX_INSTANCES, sizeof(*chans),
			     GFP_KERNEL);
	hpsc_chans = devm_kcalloc(dev, HPSC_MBOX_INSTANCES, sizeof(*hpsc_chans),
				  GFP_KERNEL);
	if (!mbox || !chans || !hpsc_chans)
		return -ENOMEM;

	// map registers
	iomem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	mbox->regs = devm_ioremap_resource(&pdev->dev, iomem);
	if (IS_ERR(mbox->regs)) {
		ret = PTR_ERR(mbox->regs);
		dev_err(&pdev->dev, "Failed to remap mailbox regs: %d\n", ret);
		return ret;
	}

	// Map all instances onto one pair of IRQs
	//
	// NOTE: So, do not expose the irq mapping as configurable. That would
	// be advanced functionality, only necessary if user desires to have
	// multiple *groups* of mailboxes mapped to different IRQ pairs in
	// order to achieve more isolation and to set interrupt priorities.
	if (of_property_read_u32(dev->of_node, DT_PROP_INTERRUPT_IDX_RCV,
				 &mbox->rcv_int_idx)) {
		dev_err(dev, "Failed to read '%s' property\n",
			DT_PROP_INTERRUPT_IDX_RCV);
		return -EINVAL;
	}
	if (of_property_read_u32(dev->of_node, DT_PROP_INTERRUPT_IDX_ACK,
				 &mbox->ack_int_idx)) {
		dev_err(dev, "Failed to read '%s' property\n",
			DT_PROP_INTERRUPT_IDX_ACK);
		return -EINVAL;
	}

	mbox->rcv_irqnum = irq_of_parse_and_map(dev->of_node, mbox->rcv_int_idx);
	if (!mbox->rcv_irqnum) {
		dev_err(dev, "Failed to parse/map rcv irq");
		return -EINVAL;
	}
	mbox->ack_irqnum = irq_of_parse_and_map(dev->of_node, mbox->ack_int_idx);
	if (!mbox->ack_irqnum) {
		dev_err(dev, "Failed to parse/map ack irq\n");
		return -EINVAL;
	}
	dev_info(dev, "probe: rcv irq %u ack irq %u\n", mbox->rcv_irqnum,
		 mbox->ack_irqnum);

	ret = devm_request_irq(dev, mbox->rcv_irqnum, hpsc_mbox_rcv_irq,
			       /* flags */ 0, dev_name(dev), mbox);
	if (ret) {
		dev_err(dev, "Failed to register mailbox rcv IRQ handler: %d\n",
			ret);
		return ret;
	}

	ret = devm_request_irq(dev, mbox->ack_irqnum, hpsc_mbox_ack_irq,
			       /* flags */ 0, dev_name(dev), mbox);
	if (ret) {
		dev_err(dev, "Failed to register mailbox ack IRQ handler: %d\n",
			ret);
		return ret;
	}

	// finally, register our controller with mbox API
	hpsc_mbox_chans_init(hpsc_chans, HPSC_MBOX_INSTANCES, mbox, chans);
	hpsc_mbox_controller_init(&mbox->controller, dev, chans,
				  HPSC_MBOX_INSTANCES);
	ret = mbox_controller_register(&mbox->controller);
	if (ret) {
		dev_err(&pdev->dev, "Failed to register controller: %d\n", ret);
		return ret;
	}

	platform_set_drvdata(pdev, mbox);
	dev_info(dev, "registered\n");
	dev_dbg(dev, "mailbox dynamic debug enabled\n");
	return ret;
}

static int hpsc_mbox_remove(struct platform_device *pdev)
{
	struct hpsc_mbox *mbox = platform_get_drvdata(pdev);
	mbox_controller_unregister(&mbox->controller);
	dev_info(&pdev->dev, "unregistered\n");
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

static int __init hpsc_mbox_init(void)
{
	return platform_driver_register(&hpsc_mbox_driver);
}

static void __exit hpsc_mbox_exit(void)
{
	platform_driver_unregister(&hpsc_mbox_driver);
}
// Can't use module_platform_driver() - must init before other platform drivers
subsys_initcall(hpsc_mbox_init);
module_exit(hpsc_mbox_exit);

MODULE_DESCRIPTION("HPSC Chiplet mailbox driver");
MODULE_AUTHOR("Alexei Colin <acolin@isi.edu>");
MODULE_AUTHOR("Connor Imes <cimes@isi.edu>");
MODULE_LICENSE("GPL v2");
