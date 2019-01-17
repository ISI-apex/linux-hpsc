/*
 * HPSC Chiplet RTI Timer driver.
 */
#include <linux/clocksource.h>
#include <linux/cpu.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/smp.h>
#include <linux/types.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include <linux/poll.h>
#include <linux/fs.h>
#include <linux/uaccess.h>

#include "interval_timer.h"

#define LOG_CAT "HPSC RTI Timer"

#define REG__INTERVAL		0x00
#define REG__COUNT		0x08

#define REG__CMD_ARM		0x10
#define REG__CMD_FIRE		0x14

// Clearing first stage clears all stages, hence only one clear cmd
#define CMD_CAPTURE_ARM		0xcd01
#define CMD_CAPTURE_FIRE	0x01cd
#define CMD_LOAD_ARM		0xcd02
#define CMD_LOAD_FIRE		0x02cd

#define HPSC_RTI_TMR_SIZE 	0x10000

struct hpsc_rti_tmr { // per cpu
	struct interval_timer itmr;
	void __iomem *regs;
	int cpu;
};

struct hpsc_rti_tmr_block {
	struct interval_timer_block itmr_block;
	int irq;
	struct hpsc_rti_tmr (*of_xlate)(struct hpsc_rti_tmr_block *b,
					struct of_phandle_args *spec);
};

// We could dynamically allocate the block struct and store a pointer to this
// global object in each per cpu object.
//
// But, if we do, there's a problem of getting to the block object (to get to
// the per-cpu objects) from cpu hotplug callbacks. The basic hotplug callback
// API does not take cookies, so to make this work, we would need to use the
// multi-instance API, which does take cookies, or maintain a global list of
// instances ourselves. It's doable, but doesn't seem justified since there can
// only be one instance of hpsc-rti-timer block in the system anyway.
//
// For the same reason (inability to get to the hpsc_rti_tmr_block object from cpu
// hotplug callbacks), we also don't use platform driver model, which implies
// all state has to be in a struct hpsc_rti_tmr_block.  Instead, we use the
// TIMER_OF_DECLARE model with only an init function and no cleanup function.
// A difference of TIMER_OF_DECLARE is that the timer is initialized much
// earlier in the init sequence than a platform device would be.  Consequently,
// this driver has to be compiled in and cannot be a module, which is similar
// to drivers for other per-cpu timers.
//
static struct hpsc_rti_tmr_block tmr_block;

// The per-cpu state objects could be defined within the block struct, but to
// do so would require dynamically allocating these per-cpu objects and storing
// pointeres to them in the block struct. Could be done either way -- this
// design choice is orthogonal to the above major choice due to constraints.
static DEFINE_PER_CPU(struct hpsc_rti_tmr, per_cpu_rti_tmr);

static void set_interval(struct hpsc_rti_tmr *tmr, uint64_t interval)
{
	writeq(interval, tmr->regs + REG__INTERVAL);

	writel(CMD_LOAD_ARM, tmr->regs + REG__CMD_ARM);
	writel(CMD_LOAD_FIRE, tmr->regs + REG__CMD_FIRE);
}

static uint64_t capture(struct hpsc_rti_tmr *tmr)
{
	writel(CMD_CAPTURE_ARM, tmr->regs + REG__CMD_ARM);
	writel(CMD_CAPTURE_FIRE, tmr->regs + REG__CMD_FIRE);

	return readq(tmr->regs + REG__COUNT);
}

static irqreturn_t hpsc_rti_tmr_event(int irq, void *priv)
{
	struct hpsc_rti_tmr *tmr = priv;
	pr_info("%s: event interrupt for cpu %u on cpu %u\n", LOG_CAT,
		smp_processor_id(), tmr->cpu);
	BUG_ON(smp_processor_id() != tmr->cpu); // ensured by IRQ framework
	interval_timer_notify(&tmr->itmr);
	return IRQ_HANDLED;
}

static int check_cpu(struct hpsc_rti_tmr *tmr, int cpu, const char *op)
{
	if (likely(tmr->cpu == cpu)) {
		pr_debug("%s: cpu %u: operation '%s'\n", LOG_CAT, cpu, op);
		return 0;
	}
	pr_err("%s: attempted '%s' operation for core %d from core %d\n",
		LOG_CAT, op, tmr->cpu, cpu);
	return -EINVAL;
}

static int hpsc_rti_tmr_set_interval(struct interval_timer *itmr, uint64_t interval)
{
	struct hpsc_rti_tmr *tmr =
		container_of(itmr, struct hpsc_rti_tmr, itmr);

	int cpu = get_cpu();
	int ret = check_cpu(tmr, cpu, "set_interval");
	if (ret)
		goto out;

	set_interval(tmr, interval);
out:
	put_cpu();
	return ret;
}

static int hpsc_rti_tmr_capture(struct interval_timer *itmr, uint64_t *count)
{
	struct hpsc_rti_tmr *tmr =
		container_of(itmr, struct hpsc_rti_tmr, itmr);

	int cpu = get_cpu();
	int ret = check_cpu(tmr, cpu, "capture");
	if (ret)
		goto out;

	*count = capture(tmr);
out:
	put_cpu();
	return ret;
}

static struct interval_timer *
hpsc_rti_tmr_of_xlate(struct interval_timer_block *itmr_block,
		      const struct of_phandle_args *spec)
{
	int cpu = spec->args[0];
	struct hpsc_rti_tmr *tmr;
	if (cpu >= num_possible_cpus())
	{
		pr_err("%s: xlate: invalid cpu index: %u\n", LOG_CAT, cpu);
		return ERR_PTR(-EINVAL);
	}
	tmr = &per_cpu(per_cpu_rti_tmr, cpu);
	return &tmr->itmr;
}

struct interval_timer_block_ops hpsc_rti_tmr_itmr_block_ops = {
	.of_xlate = hpsc_rti_tmr_of_xlate,
};
struct interval_timer_ops hpsc_rti_tmr_itmr_ops = {
	.set_interval = hpsc_rti_tmr_set_interval,
	.capture = hpsc_rti_tmr_capture,
};

static int hpsc_rti_tmr_cpu_up(unsigned int cpu)
{
	int irq = tmr_block.irq;
	u32 flags = irq_get_trigger_type(irq);
	BUG_ON(cpu != smp_processor_id()); // a check on CPU Hotplug API
	pr_info("%s: cpu %d up: enable PPI IRQ%d\n", LOG_CAT, cpu, irq);
	enable_percpu_irq(irq, flags);
	return 0;
}

static int hpsc_rti_tmr_cpu_down(unsigned int cpu)
{
	int irq = tmr_block.irq;
	BUG_ON(cpu != smp_processor_id()); // a check on CPU Hotplug API
	pr_info("%s: cpu %d down: disable PPI IRQ%d\n", LOG_CAT, cpu, irq);
	disable_percpu_irq(irq);
	return 0;
}

static int __init hpsc_rti_tmr_init(struct device_node *np)
{
	struct resource res;
	void __iomem *base;
	struct hpsc_rti_tmr *tmr;
	int cpu;
	int ret = 0;

	pr_info("%s: probe\n", LOG_CAT);

	if (of_address_to_resource(np, 0, &res)) {
		pr_err("%s: failed to get resource from DT node", LOG_CAT);
		return -ENODEV;
	}
	base = ioremap(res.start, resource_size(&res));
	if (!base) {
		pr_err("%s: failed to remap regs", LOG_CAT);
		return -ENODEV;
	}
	pr_debug("%s: res addr %p size %llx base %p\n", LOG_CAT,
		 (void *)res.start, resource_size(&res), base);

	interval_timer_block_init(&tmr_block.itmr_block,
				  &hpsc_rti_tmr_itmr_block_ops);
	interval_timer_block_register(&tmr_block.itmr_block, np);

	for_each_possible_cpu(cpu) {
		tmr = &per_cpu(per_cpu_rti_tmr, cpu);
		tmr->cpu = cpu;
		tmr->regs = base + cpu * HPSC_RTI_TMR_SIZE;
		interval_timer_init(&tmr->itmr, &hpsc_rti_tmr_itmr_ops);
	}

	tmr_block.irq = irq_of_parse_and_map(np, /* idx */ 0);
	if (!tmr_block.irq) {
		pr_err("%s: failed to parse/map irq", LOG_CAT);
		ret = -ENODEV;
		goto irq_fail;
	}

	ret = request_percpu_irq(tmr_block.irq, hpsc_rti_tmr_event,
				 "hpsc-rti-timer", &per_cpu_rti_tmr);
	if (ret) {
		pr_err("%s: failed to register IRQ handler: %d\n",
			LOG_CAT, ret);
		goto irq_fail;
	}

	// We have to hook into cpu hotplug events because to enable the
	// private per-cpu (PPI) IRQ, the enable_per_cpu call must be executed
	// by each CPU in order to enable the irq for that CPU.
	ret = cpuhp_setup_state(CPUHP_AP_ONLINE_DYN, "hpsc/rti-timer",
				hpsc_rti_tmr_cpu_up, hpsc_rti_tmr_cpu_down);
	if (ret < 0) {
		pr_err("%s: failed to register with CPU Hotplug: %d\n",
			LOG_CAT, ret);
		goto hp_fail;
	}
	return 0;

hp_fail:
	disable_percpu_irq(tmr_block.irq); // for CPU 0 (i.e. ourselves)
	free_percpu_irq(tmr_block.irq, &per_cpu_rti_tmr);
irq_fail:
	interval_timer_block_unregister(&tmr_block.itmr_block);
	iounmap(base);
	return ret;
}

TIMER_OF_DECLARE(hpsc_rti_tmr, "hpsc,hpsc-rti-timer", hpsc_rti_tmr_init);

MODULE_DESCRIPTION("HPSC Chiplet RTI Timer driver");
MODULE_AUTHOR("Alexei Colin <acolin@isi.edu>");
MODULE_LICENSE("GPL v2");
