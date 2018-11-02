/*
 * HPSC Chiplet watchdog driver (currently a stub).
 * This is a two-stage watchdog.
 */
#include <linux/io.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/reboot.h>
#include <linux/types.h>
#include <linux/of_address.h>
#include <linux/watchdog.h>

#define HPSC_WDT_TIMEOUT_MIN 1
#define HPSC_WDT_TIMEOUT_MAX 60
#define HPSC_WDT_TIMEOUT_DEFAULT 10

// TODO: SW timer to be replaced by HW watchdog stage 1 timer interrupt
#define HPSC_WDT_USE_SW_TIMER

#ifdef HPSC_WDT_USE_SW_TIMER
#include <linux/jiffies.h>
#include <linux/timer.h>
#include <linux/hpsc_msg.h>
static unsigned int hpsc_wdt_dummy_cpu_ctr = 0;
static void hpsc_wdt_timeout(unsigned long data) {
	module_put(THIS_MODULE);
	pr_info("HPSC Chiplet watchdog expired for cpu %lu\n", data);
	hpsc_msg_wdt_timeout((unsigned int) data);
	pr_crit("Initiating system reboot\n");
	emergency_restart();
	pr_crit("Reboot didn't ?????\n");
}
#endif

struct hpsc_wdt {
	struct watchdog_device	wdd;
	spinlock_t		lock;
	struct device		*dev;
	void __iomem		*base;
	unsigned int		cpu;
#ifdef HPSC_WDT_USE_SW_TIMER
	struct timer_list	timer;
#endif
};

static int hpsc_wdt_start(struct watchdog_device *wdog)
{
	struct hpsc_wdt *wdt = watchdog_get_drvdata(wdog);
	unsigned long flags;
	dev_info(wdt->dev, "start\n");
	spin_lock_irqsave(&wdt->lock, flags);
	// TODO
#ifdef HPSC_WDT_USE_SW_TIMER
	mod_timer(&wdt->timer, jiffies+(wdog->timeout*HZ));
#endif
	spin_unlock_irqrestore(&wdt->lock, flags);
	return 0;
}


static int hpsc_wdt_stop(struct watchdog_device *wdog)
{
	struct hpsc_wdt *wdt = watchdog_get_drvdata(wdog);
	unsigned long flags;
	dev_info(wdt->dev, "stop\n");
	spin_lock_irqsave(&wdt->lock, flags);
	// TODO
#ifdef HPSC_WDT_USE_SW_TIMER
	del_timer(&wdt->timer);
#endif
	spin_unlock_irqrestore(&wdt->lock, flags);
	return 0;
}

static int hpsc_wdt_ping(struct watchdog_device *wdog)
{
	struct hpsc_wdt *wdt = watchdog_get_drvdata(wdog);
	unsigned long flags;
	dev_info(wdt->dev, "ping\n");
	spin_lock_irqsave(&wdt->lock, flags);
	// TODO
#ifdef HPSC_WDT_USE_SW_TIMER
	mod_timer(&wdt->timer, jiffies+(wdog->timeout*HZ));
#endif
	spin_unlock_irqrestore(&wdt->lock, flags);
	return 0;
}

static int hpsc_wdt_set_timeout(struct watchdog_device *wdog, unsigned int t)
{
	struct hpsc_wdt *wdt = watchdog_get_drvdata(wdog);
	dev_info(wdt->dev, "set_timeout=%u\n", t);
	wdog->timeout = t;
	// TODO
	// Per the API, return -EINVAL if timeout is out of range,
	// -EIO if we fail to write to hardware
	return 0;
}

static struct watchdog_ops hpsc_wdt_ops = {
	.owner =	THIS_MODULE,
	.start =	hpsc_wdt_start,
	.stop =		hpsc_wdt_stop,
	.ping =		hpsc_wdt_ping,
	.set_timeout =	hpsc_wdt_set_timeout,
};

static struct watchdog_info hpsc_wdt_info = {
	.options =	WDIOF_SETTIMEOUT | WDIOF_MAGICCLOSE |
			WDIOF_KEEPALIVEPING,
	.identity =	"HPSC Chiplet watchdog timer",
};

static void hpsc_wdt_wdd_init(struct watchdog_device *wdd, struct device *dev)
{
	wdd->parent =		dev;
	wdd->info =		&hpsc_wdt_info;
	wdd->ops =		&hpsc_wdt_ops;
	wdd->timeout =		HPSC_WDT_TIMEOUT_DEFAULT;
	wdd->min_timeout =	HPSC_WDT_TIMEOUT_MIN;
	wdd->max_timeout =	HPSC_WDT_TIMEOUT_MAX;
}

static void hpsc_wdt_init(struct hpsc_wdt *wdt, struct device *dev,
			  void __iomem *base)
{
	hpsc_wdt_wdd_init(&wdt->wdd, dev);
	spin_lock_init(&wdt->lock);
	wdt->dev = dev;
	wdt->base = base;
}

static int hpsc_wdt_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct hpsc_wdt *wdt;
	void __iomem *base;
	int err;

	dev_info(dev, "probe\n");
	wdt = devm_kzalloc(dev, sizeof(*wdt), GFP_KERNEL);
	if (!wdt)
		return -ENOMEM;
	platform_set_drvdata(pdev, wdt);

	// TODO: determine which CPU this watchdog is for
	wdt->cpu = 0;

#ifdef HPSC_WDT_USE_SW_TIMER
	setup_timer(&wdt->timer, hpsc_wdt_timeout, hpsc_wdt_dummy_cpu_ctr);
	hpsc_wdt_dummy_cpu_ctr++;
#endif

	base = of_iomap(dev->of_node, 0);
	if (!base) {
		dev_err(dev, "Failed to remap watchdog regs");
		return -ENODEV;
	}

	hpsc_wdt_init(wdt, dev, base);
	watchdog_set_drvdata(&wdt->wdd, wdt);
	watchdog_init_timeout(&wdt->wdd, HPSC_WDT_TIMEOUT_DEFAULT, dev);
	err = watchdog_register_device(&wdt->wdd);
	if (err) {
		dev_err(dev, "Failed to register watchdog device");
		iounmap(base);
		return err;
	}

	dev_info(dev, "registered\n");
	return 0;
}

static int hpsc_wdt_remove(struct platform_device *pdev)
{
	struct hpsc_wdt *wdt = platform_get_drvdata(pdev);
	dev_info(&pdev->dev, "remove\n");

	watchdog_unregister_device(&wdt->wdd);
	iounmap(wdt->base);

	// hpsc_wdt instance managed for us
	dev_info(&pdev->dev, "unregistered\n");
	return 0;
}

static void hpsc_wdt_shutdown(struct platform_device *pdev)
{
	struct hpsc_wdt *wdt = platform_get_drvdata(pdev);
	hpsc_wdt_stop(&wdt->wdd);
}

static const struct of_device_id hpsc_wdt_of_match[] = {
	{ .compatible = "hpsc,hpsc-wdt", },
	{},
};
MODULE_DEVICE_TABLE(of, hpsc_wdt_of_match);

static struct platform_driver hpsc_wdt_driver = {
	.probe		= hpsc_wdt_probe,
	.remove		= hpsc_wdt_remove,
	.shutdown	= hpsc_wdt_shutdown,
	.driver = {
		.name = "hpsc_wdt",
		.of_match_table = hpsc_wdt_of_match,
	},
};
module_platform_driver(hpsc_wdt_driver);

MODULE_DESCRIPTION("HPSC Chiplet watchdog driver");
MODULE_AUTHOR("Connor Imes <cimes@isi.edu>");
MODULE_LICENSE("GPL v2");
