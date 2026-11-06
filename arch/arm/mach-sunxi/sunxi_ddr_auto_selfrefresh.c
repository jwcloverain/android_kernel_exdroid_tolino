
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/slab.h>

#include <linux/io.h>


static void __iomem	*gpDDR_auto_selfrefresh_membase;

static int giDDR_auto_selfrefresh_enabled = 1; // 1 : enable , 0 : disable , -1 : don't control .
static int _ddr_auto_selfrefresh_get_state(void)
{
	u32 uiChk;

	uiChk = readl(gpDDR_auto_selfrefresh_membase);
	printk(KERN_DEBUG"%s(%d) %p read = 0x%x\n",__func__,__LINE__,
			gpDDR_auto_selfrefresh_membase,uiChk);

	return (uiChk)?1:0;
}

static int _ddr_auto_selfrefresh_enable(int iEnable)
{
	int iRet = _ddr_auto_selfrefresh_get_state();
	if(1==iEnable) {
		printk(KERN_DEBUG"enabling ddr auto selfrefresh ... \n");
		writel(0x9,gpDDR_auto_selfrefresh_membase);
	}
	else if(0==iEnable){
		printk(KERN_DEBUG"disabling ddr auto selfrefresh ... \n");
		writel(0,gpDDR_auto_selfrefresh_membase);
	}
	else {
	}
	return iRet;
}


static ssize_t enable_sysfs_show(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	return sprintf(buf, "%d(%d)\n",
			giDDR_auto_selfrefresh_enabled,
			_ddr_auto_selfrefresh_get_state());
}
static ssize_t enable_sysfs_set(struct device *dev,
					struct device_attribute *attr,
					const char *buf,
					size_t count)
{
	unsigned long val;
	
	if (kstrtol(buf, 10, &val) < 0) {
		printk(KERN_ERR"parameters error !\n");
		return -EINVAL;
	}

	if(1==val) {
		_ddr_auto_selfrefresh_enable(1);
		giDDR_auto_selfrefresh_enabled = 1;
	}
	else if(0==val) {
		_ddr_auto_selfrefresh_enable(0);
		giDDR_auto_selfrefresh_enabled = 0;
	}
	else {
		giDDR_auto_selfrefresh_enabled = -1;
	}



	return count;
}



static DEVICE_ATTR(enable, S_IWUSR | S_IRUGO,
		enable_sysfs_show, enable_sysfs_set);

static struct attribute *_sysfs_attributes[] = {
	&dev_attr_enable.attr,
	NULL,
};

static const struct attribute_group _sysfs_attr_group = {
	.attrs = _sysfs_attributes,
};

static int _sysfs_init(struct kobject *kobj)
{
	int ret=0;
	ret += sysfs_create_group(kobj,&_sysfs_attr_group);
	return ret;
}

int sunxi_ddr_auto_selfrefresh_suspend(struct platform_device *pdev, pm_message_t state)
{
	int iRet=0;
	return iRet;
}

int sunxi_ddr_auto_selfrefresh_resume(struct platform_device *pdev)
{
	int iRet=0;
	if(1==giDDR_auto_selfrefresh_enabled) {
		_ddr_auto_selfrefresh_enable(1);
	}
	else if(0==giDDR_auto_selfrefresh_enabled) {
		_ddr_auto_selfrefresh_enable(0);
	}
	return iRet;
}

static int sunxi_ddr_auto_selfrefresh_probe(struct platform_device *pdev)
{
	int iRet = 0;
	struct resource *res;

	//printk("%s(%d)\n",__func__,__LINE__);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	gpDDR_auto_selfrefresh_membase = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(gpDDR_auto_selfrefresh_membase)) {
		printk(KERN_ERR"%s(%d) get resource failed !\n",__func__,__LINE__);
		return PTR_ERR(gpDDR_auto_selfrefresh_membase);
	}

	printk("%s(%d):membase=%p\n",__func__,__LINE__,gpDDR_auto_selfrefresh_membase);

	_sysfs_init(&pdev->dev.kobj);

	if(1==giDDR_auto_selfrefresh_enabled) {
		_ddr_auto_selfrefresh_enable(1);
	}
	else if(0==giDDR_auto_selfrefresh_enabled) {
		_ddr_auto_selfrefresh_enable(0);
	}

	return iRet;
}
static int sunxi_ddr_auto_selfrefresh_remove(struct platform_device *pdev)
{
	int iRet = 0;

	printk(KERN_DEBUG"%s(%d)\n",__func__,__LINE__);
	
	return iRet;
}

static const struct of_device_id sunxi_ddr_auto_selfrefresh_match[] = {
	{ .compatible = "allwinner,sunxi-ddr-auto-selfrefresh", },
	{}
};
MODULE_DEVICE_TABLE(of, sunxi_ddr_auto_selfrefresh_match);

static struct platform_driver sunxi_ddr_auto_selfrefresh_driver = {
	.probe	= sunxi_ddr_auto_selfrefresh_probe,
	.remove	= sunxi_ddr_auto_selfrefresh_remove,
	.suspend = sunxi_ddr_auto_selfrefresh_suspend,
	.resume = sunxi_ddr_auto_selfrefresh_resume,
	.driver	= {
		.name		= "sunxi-ddr-auto-selfrefresh",
		.owner		= THIS_MODULE,
		.of_match_table	= sunxi_ddr_auto_selfrefresh_match,
	},
};

static int __init sunxi_ddr_auto_selfrefresh_init(void)
{
	int ret;

	ret = platform_driver_register(&sunxi_ddr_auto_selfrefresh_driver);
	if (IS_ERR_VALUE(ret)) {
		pr_err("register ddr audo selfrefresh driver failed\n");
		return -EINVAL;
	}
	return 0;
}
postcore_initcall(sunxi_ddr_auto_selfrefresh_init);

MODULE_AUTHOR("Gallen<gallen.lin@netronixinc.com>");
MODULE_DESCRIPTION("Allwinner ddr auto self refresh driver");
MODULE_LICENSE("GPL");

