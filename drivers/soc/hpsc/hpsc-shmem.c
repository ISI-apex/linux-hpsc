/*
 * HPSC shared memory module - provides device files to be mmap'd by userspace.
 * Memory regions should be reserved physical addresses with fixed size.
 */
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>

#define SHMEM_DEVICE_NAME "hpsc_shmem"

struct hpsc_shmem_dev {
	struct device *dev;
	resource_size_t paddr;
	resource_size_t size;
	int major_num;
	struct cdev cdev;
};

// To support multiple instances, manage class at module init/exit
static struct class *class;

static int shmem_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct hpsc_shmem_dev *tdev = filp->private_data;
	unsigned long len = vma->vm_end - vma->vm_start;
	unsigned long pfn = tdev->paddr >> PAGE_SHIFT;
	int ret;
	dev_info(tdev->dev, "mmap: pfn=0x%lx, paddr=0x%llx, len=0x%lx\n",
		 pfn, tdev->paddr, len);
	if (len > tdev->size) {
		dev_err(tdev->dev, "mmap: length (0x%lx) > size (0x%llx)\n",
			len, tdev->size);
		return -EINVAL;
	}
	ret = remap_pfn_range(vma, vma->vm_start, pfn, len, vma->vm_page_prot);
	if (ret) {
		dev_err(tdev->dev, "remap_pfn_range failed\n");
		return -EAGAIN;
	}
	// TODO: Should VM_IO always be set?
	if (filp->f_flags & O_SYNC)
		vma->vm_flags |= VM_IO;
	return 0;
}

static int shmem_open(struct inode *inode, struct file *filp)
{
	struct hpsc_shmem_dev *tdev = container_of(inode->i_cdev,
						   struct hpsc_shmem_dev, cdev);
	dev_dbg(tdev->dev, "open\n");
	filp->private_data = tdev;
	return 0;
}

static int shmem_release(struct inode *inode, struct file *filp)
{
	struct hpsc_shmem_dev *tdev = container_of(inode->i_cdev,
						   struct hpsc_shmem_dev, cdev);
	dev_dbg(tdev->dev, "release\n");
	filp->private_data = NULL;
	return 0;
}

static const struct file_operations shmem_fops = {
	.mmap = shmem_mmap,
	.open = shmem_open,
	.release = shmem_release,
};

static int hpsc_shmem_parse_dt(struct hpsc_shmem_dev *tdev, const char **name)
{
	struct device_node *np;
	struct resource res;
	int ret = of_property_read_string(tdev->dev->of_node, "region-name",
					  name);
	if (ret) {
		dev_err(tdev->dev, "no DT 'region-name' property\n");
		return ret;
	}
	np = of_parse_phandle(tdev->dev->of_node, "memory-region", 0);
	if (!np) {
		dev_err(tdev->dev, "no DT 'memory-region' property\n");
		return -EFAULT;
	}
	// get memory region from DT
	ret = of_address_to_resource(np, 0, &res);
	if (ret) {
		dev_err(tdev->dev, "no address for DT 'memory-region'\n");
		return ret;
	}
	tdev->paddr = res.start;
	tdev->size = resource_size(&res);
	return 0;
}

static int hpsc_shmem_probe(struct platform_device *pdev)
{
	struct hpsc_shmem_dev *tdev;
	const char *name;
	dev_t devno;
	struct device *device;
	int ret;

	dev_info(&pdev->dev, "probe\n");
	tdev = devm_kzalloc(&pdev->dev, sizeof(*tdev), GFP_KERNEL);
	if (!tdev)
		return -ENOMEM;
	tdev->dev = &pdev->dev;
	platform_set_drvdata(pdev, tdev);

	ret = hpsc_shmem_parse_dt(tdev, &name);
	if (ret)
		return ret;

	// create device file
	ret = alloc_chrdev_region(&devno, 0, 1, SHMEM_DEVICE_NAME);
	if (ret < 0) {
		dev_err(tdev->dev, "alloc_chrdev_region failed\n");
		return ret;
	}
	tdev->major_num = MAJOR(devno);
	cdev_init(&tdev->cdev, &shmem_fops);
	ret = cdev_add(&tdev->cdev, devno, 1);
	if (ret) {
		dev_err(tdev->dev, "cdev_add failed\n");
		goto fail_cdev;
	}
	device = device_create(class, NULL, devno, NULL, "%s!%s",
			       class->name, name);
	if (IS_ERR(device)) {
		dev_err(tdev->dev, "device_create failed\n");
		ret = PTR_ERR(device);
		goto fail_dev;
	}

	dev_info(tdev->dev, "registered paddr=0x%llx, size=0x%llx\n",
		 tdev->paddr, tdev->size);
	return 0;
fail_dev:
	cdev_del(&tdev->cdev);
fail_cdev:
	unregister_chrdev_region(MKDEV(tdev->major_num, 0), 1);
	return ret;
}

static int hpsc_shmem_remove(struct platform_device *pdev)
{
	struct hpsc_shmem_dev *tdev = platform_get_drvdata(pdev);
	dev_info(tdev->dev, "remove: paddr=0x%llx, size=0x%llx\n",
		 tdev->paddr, tdev->size);
	device_destroy(class, MKDEV(tdev->major_num, 0));
	cdev_del(&tdev->cdev);
	unregister_chrdev_region(MKDEV(tdev->major_num, 0), 1);
	return 0;
}

static const struct of_device_id hpsc_shmem_match[] = {
	{ .compatible = "hpsc-shmem" },
	{},
};

static struct platform_driver hpsc_shmem_driver = {
	.driver = {
		.name = "hpsc_shmem",
		.of_match_table = hpsc_shmem_match,
	},
	.probe  = hpsc_shmem_probe,
	.remove = hpsc_shmem_remove,
};

static int __init hpsc_shmem_init(void)
{
	int ret;
	pr_info("hpsc-shmem: init\n");
	class = class_create(THIS_MODULE, SHMEM_DEVICE_NAME);
	if (IS_ERR(class)) {
		pr_err("hpsc-shmem: failed to create %s class\n",
		       SHMEM_DEVICE_NAME);
		return PTR_ERR(class);
	}
	ret = platform_driver_register(&hpsc_shmem_driver);
	if (ret) {
		pr_err("hpsc-shmem: failed to register driver\n");
		class_destroy(class);
	}
	return ret;
}

static void __exit hpsc_shmem_exit(void)
{
	pr_info("hpsc-shmem: exit\n");
	platform_driver_unregister(&hpsc_shmem_driver);
	class_destroy(class);
}

MODULE_DESCRIPTION("HPSC shared memory interface");
MODULE_AUTHOR("Connor Imes <cimes@isi.edu>");
MODULE_LICENSE("GPL v2");

module_init(hpsc_shmem_init);
module_exit(hpsc_shmem_exit);
