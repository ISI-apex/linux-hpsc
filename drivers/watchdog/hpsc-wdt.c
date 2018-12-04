/*
 * HPSC Chiplet watchdog driver.
 */
#include <linux/clocksource.h>
#include <linux/cpu.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/smp.h>
#include <linux/types.h>
#include <linux/watchdog.h>
#include "watchdog_pretimeout.h"

#define REG__ST1_TERMINAL    0x00
#define REG__ST1_COUNT       0x08
#define REG__ST2_TERMINAL    0x10
#define REG__ST2_COUNT       0x18
#define REG__CMD_ARM         0x28
#define REG__CMD_FIRE        0x2c
#define REG__CONFIG          0x20
#define REG__STATUS          0x24

#define REG__CONFIG__EN      0x1
#define REG__CONFIG__TICKDIV_SHIFT 2
#define REG__CONFIG__TICKDIV_MASK  0xFF
#define REG__STATUS__ST1_TIMEOUT 0x1

// Clearing first stage clears all stages, hence only one clear cmd
#define CMD_CLEAR_ARM		0xcd05
#define CMD_CLEAR_FIRE		0x05cd
#define CMD_CAPTURE_ST1_ARM	0xcd01
#define CMD_CAPTURE_ST1_FIRE	0x01cd
#define CMD_CAPTURE_ST2_ARM	0xcd02
#define CMD_CAPTURE_ST2_FIRE	0x02cd

#define HPSC_WDT_SIZE 0x10000
#define HPSC_WDT_CLK_FREQ_HZ 3906250
#define HPSC_WDT_TICKDIV_MAX 8

struct hpsc_wdt { // per cpu
	struct watchdog_device	wdd;
	void __iomem		*regs;
	int cpu;
};

// To dynamically allocate this per-cpu var and store a pointer to it in struct
// hpsc_wdt_global, introduces the problem of getting to this variable (or,
// equivalently, to the hpsc_wdt_global object) from cpu hotplug callbacks --
// the basic hotplug callback API does not take cookies, so to make this work,
// we would need to use the multi-instance API, which does take cookies, or
// maintain a global list of instances ourselves. It's doable, but doesn't
// seem justified since there can only be one instance of hpsc-wdt in the
// system anyway.
//
// For the same reason (inability to get to the hpsc_wdt_global object from cpu
// hotplug callbacks), we also don't use platform driver model, which implies
// all state has to be in a struct hpsc_wdt_global.  Instead, we use the
// TIMER_OF_DECLARE model with only an init function and no cleanup function.
// An additional advantage of TIMER_OF_DECLARE is that the WDT is initialized
// much earlier in the init sequence than a platform device would be, so if the
// kernel would ever wish to kick the WDT to monitor the boot process, it will
// be able to do so early.  Consequently, this driver has to be compiled in and
// cannot be a module, which is similar to drivers for other per-cpu timers.
static DEFINE_PER_CPU(struct hpsc_wdt, per_cpu_wdt);

// Given the above, there's no point in dynamically allocating a struct
// hpsc_wdt_global with just this one field; and storing a pointer
// to this global object in the per cpu state. More efficient and simpler
// to maintain the global state of the single instance as static variables.
static unsigned hpsc_wdt_irq;


static unsigned int cycles_to_sec(struct hpsc_wdt *wdt, unsigned int cycles)
{
	u64 config = readl(wdt->regs + REG__CONFIG);
	unsigned int tickdiv = ((config >> REG__CONFIG__TICKDIV_SHIFT) &
					REG__CONFIG__TICKDIV_MASK) + 1;
	return cycles / (HPSC_WDT_CLK_FREQ_HZ / tickdiv);
}
static unsigned int get_count(struct hpsc_wdt *wdt)
{
	u64 count;

	// There is one logical 64-bit timer presented by HW. This implies that
	// the sum of all stages has to be within 64-bits, enforced by HW.

	writel(CMD_CAPTURE_ST1_ARM, wdt->regs + REG__CMD_ARM);
	writel(CMD_CAPTURE_ST1_FIRE, wdt->regs + REG__CMD_FIRE);
	count = readq(wdt->regs + REG__ST1_COUNT);

	writel(CMD_CAPTURE_ST2_ARM, wdt->regs + REG__CMD_ARM);
	writel(CMD_CAPTURE_ST2_FIRE, wdt->regs + REG__CMD_FIRE);
	count += readq(wdt->regs + REG__ST2_COUNT);

	return count;
}
static unsigned int get_terminal(struct hpsc_wdt *wdt)
{
	// HW guarantees no overflow (see comment above)
	return readq(wdt->regs + REG__ST1_TERMINAL) +
	       readq(wdt->regs + REG__ST2_TERMINAL);
}

static irqreturn_t hpsc_wdt_timeout(int irq, void *priv)
{
	struct hpsc_wdt *wdt = priv;
	u32 status;
	pr_info("HPSC WDT: stage 1 interrupt received for cpu %u on cpu %u\n",
		smp_processor_id(), wdt->cpu);
	BUG_ON(smp_processor_id() != wdt->cpu); // ensured by IRQ framework

	// TODO: unclear if this int flag will be clearable from here or from
	// EL3 or via CLEAR cmd.
	status = readl(wdt->regs + REG__STATUS);
	writel(status & ~REG__STATUS__ST1_TIMEOUT, wdt->regs + REG__STATUS);

	watchdog_notify_pretimeout(&wdt->wdd);
	return IRQ_HANDLED;
}

static int hpsc_wdt_check_cpu(struct hpsc_wdt *wdt, int cpu, const char *op)
{
	if (likely(wdt->cpu == cpu))
		return 0;
	pr_err("HPSC WDT: attempted '%s' operation for core %d from core %d\n",
	       op, wdt->cpu, cpu);
	return -EINVAL;
}

static int hpsc_wdt_start(struct watchdog_device *wdog)
{
	struct hpsc_wdt *wdt = watchdog_get_drvdata(wdog);
	u32 config;
	int cpu = get_cpu();
	int ret = hpsc_wdt_check_cpu(wdt, cpu, "start");
	if (ret)
		goto out;
	pr_info("HPSC WDT: cpu %u: start\n", wdt->cpu);
	// TODO: unclear if this will be allowed and if allowed from EL3 only.
	config = readl(wdt->regs + REG__CONFIG);
	writel(config | REG__CONFIG__EN, wdt->regs + REG__CONFIG);
out:
	put_cpu();
	return ret;
}

static int hpsc_wdt_stop(struct watchdog_device *wdog)
{
	// In HPSC WDT HW the monitored target does not have access to disable
	struct hpsc_wdt *wdt = watchdog_get_drvdata(wdog);
	int cpu = get_cpu();
	int ret = hpsc_wdt_check_cpu(wdt, cpu, "stop");
	if (ret)
		goto out;
	pr_warn("HPSC WDT: cpu %u: stop not allowed after start\n", wdt->cpu);
	ret = -EINVAL;
out:
	put_cpu();
	return ret;
}

static int hpsc_wdt_ping(struct watchdog_device *wdog)
{
	struct hpsc_wdt *wdt = watchdog_get_drvdata(wdog);

	// The kernel watchdog framework can call this method from any core, so
	// we need to either (A) reject the calls from cores that are not the
	// core associated with this watchdog instance (associated with the
	// /dev/watchdogN), or (B) allow other any core to kick any other
	// core's watchdog (which doesn't sound like good semantics) and
	// serialize with a lock here.
	//
	// A better design would be (C) if there was only one /dev/watchdog and the
	// opening core determined the device it refers to. Then, we would have
	// the invariant that this method only ever gets called for the correct
	// CPU and it would not be necessary to disable preemption here. But,
	// we can't implement such semantics for /dev/watchdogs if we rely on
	// the kernel framework, since it implements the device.
	//
	// Another option (D) is to register only one watchdog_device with the
	// framework and transparently "fan it out" into N devices at our layer
	// based on the caller core. The framework maintains important state
	// like "is open", which we would have to take as referring to the
	// watchdogs for all cores at once. This might be a good design, though.
	//
	// In either design, the userspace ought to access the device only from
	// a pinned process, otherwise the accesses will succeed, but they
	// won't make much sense, because which timer would be accessed would
	// be essentially randomly chosen, as the process migrates (even after
	// it's already in the kernel space).
	int cpu = get_cpu();
	int ret = hpsc_wdt_check_cpu(wdt, cpu, "ping");
	if (ret)
		goto out;
	pr_debug("HPSC WDT: cpu %u: ping\n", cpu);
	writel(CMD_CLEAR_ARM, wdt->regs + REG__CMD_ARM);
	writel(CMD_CLEAR_FIRE, wdt->regs + REG__CMD_FIRE);
out:
	put_cpu();
	return ret;
}

static unsigned int hpsc_wdt_get_timeleft(struct watchdog_device *wdog)
{
	struct hpsc_wdt *wdt = watchdog_get_drvdata(wdog);
	unsigned int timeout = get_terminal(wdt);
	unsigned int count = get_count(wdt);
	return timeout > count ? cycles_to_sec(wdt, timeout - count) : 0;
}

static int hpsc_wdt_cpu_up(unsigned int cpu)
{
	u32 flags = irq_get_trigger_type(hpsc_wdt_irq);
	BUG_ON(cpu != smp_processor_id()); // a check on CPU Hotplug API
	pr_info("HPSC WDT: cpu %d up: enable PPI IRQ%d\n", cpu, hpsc_wdt_irq);
	enable_percpu_irq(hpsc_wdt_irq, flags);
	return 0;
}

static int hpsc_wdt_cpu_down(unsigned int cpu)
{
	BUG_ON(cpu != smp_processor_id()); // a check on CPU Hotplug API
	pr_info("HPSC WDT: cpu %d down: disable PPI IRQ%d\n", cpu, hpsc_wdt_irq);
	disable_percpu_irq(hpsc_wdt_irq);
	return 0;
}

static struct watchdog_ops hpsc_wdt_ops = {
	.owner =	THIS_MODULE,
	.start =	hpsc_wdt_start, // method required
	.stop =		hpsc_wdt_stop,  // method required
	.ping =		hpsc_wdt_ping,
	.get_timeleft = hpsc_wdt_get_timeleft,
};

static struct watchdog_info hpsc_wdt_info = {
	.options =	WDIOF_KEEPALIVEPING | WDIOC_GETTIMEOUT |
			WDIOF_PRETIMEOUT | WDIOC_GETTIMELEFT,
	.identity =	"HPSC Chiplet watchdog timer",
};

static void hpsc_wdt_wdd_init(struct watchdog_device *wdd)
{
	wdd->parent =		NULL;
	wdd->info =		&hpsc_wdt_info;
	wdd->ops =		&hpsc_wdt_ops;
	wdd->min_timeout =	0; // sec
	wdd->max_timeout =	~0 / (HPSC_WDT_CLK_FREQ_HZ / HPSC_WDT_TICKDIV_MAX); // sec
}

static int hpsc_wdt_percpu_init(struct hpsc_wdt *wdt, unsigned cpu)
{
	unsigned int timeout_sec;
	int err;

	hpsc_wdt_wdd_init(&wdt->wdd);
	watchdog_set_drvdata(&wdt->wdd, wdt);

	timeout_sec = cycles_to_sec(wdt, get_terminal(wdt));
	watchdog_init_timeout(&wdt->wdd, timeout_sec, NULL);

	err = watchdog_register_device(&wdt->wdd);
	if (err) {
		pr_err("HPSC WDT: Failed to register watchdog device");
		return err;
	}
	pr_info("HPSC WDT: registered WDD id %d for cpu %u: timeout %u sec\n",
		 wdt->wdd.id, cpu, timeout_sec);
	return 0;
}

static int __init hpsc_wdt_init(struct device_node *np)
{
	struct resource res;
	void __iomem *base;
	struct hpsc_wdt *wdt;
	int cpu;
	int ret;

	pr_info("HPSC WDT: probe\n");

	if (of_address_to_resource(np, 0, &res)) {
		pr_err("HPSC WDT: Failed to get resource from DT node");
		return -ENODEV;
	}
	pr_debug("HPSC WDT: res %p %llx\n", (void *)res.start,
		 resource_size(&res));
	base = ioremap(res.start, resource_size(&res));
	if (!base) {
		pr_err("HPSC WDT: Failed to remap watchdog regs");
		return -ENODEV;
	}
	pr_debug("HPSC WDT: base %p\n", base);

	for_each_possible_cpu(cpu) {
		wdt = &per_cpu(per_cpu_wdt, cpu);
		wdt->cpu = cpu;
		wdt->regs = base + cpu * HPSC_WDT_SIZE;
		pr_debug("HPSC WDT: cpu %u: regs %p\n", cpu, wdt->regs);
		ret = hpsc_wdt_percpu_init(wdt, cpu);
		if (ret)
			goto init_fail;
	}

	hpsc_wdt_irq = irq_of_parse_and_map(np, /* idx */ 0);
	if (!hpsc_wdt_irq) {
		pr_err("HPSC WDT: Failed to parse/map irq");
		ret = -ENODEV;
		goto init_fail;
	}

	ret = request_percpu_irq(hpsc_wdt_irq, hpsc_wdt_timeout, "hpsc-wdt",
				 &per_cpu_wdt);
	if (ret) {
		pr_err("HPSC WDT: Failed to register IRQ handler: %d\n", ret);
		goto init_fail;
	}

	// We have to hook into cpu hotplug events because to enable the
	// private per-cpu (PPI) IRQ, the enable_per_cpu call must be executed
	// by each CPU in order to enable the irq for that CPU.
	ret = cpuhp_setup_state(CPUHP_AP_ONLINE_DYN, "hpsc/wdt",
				hpsc_wdt_cpu_up, hpsc_wdt_cpu_down);
	if (ret < 0) {
		pr_err("HPSC WDT: Failed to register with CPU Hotplug: %d\n", ret);
		disable_percpu_irq(hpsc_wdt_irq); // for CPU 0 (i.e. ourselves)
		free_percpu_irq(hpsc_wdt_irq, &per_cpu_wdt);
		goto init_fail;
	}
	return 0;

init_fail:
	iounmap(base);
	return ret;
}

TIMER_OF_DECLARE(hpsc_wdt, "hpsc,hpsc-wdt", hpsc_wdt_init);

MODULE_DESCRIPTION("HPSC Chiplet watchdog driver");
MODULE_AUTHOR("Connor Imes <cimes@isi.edu>");
MODULE_AUTHOR("Alexei Colin <acolin@isi.edu>");
MODULE_LICENSE("GPL v2");
