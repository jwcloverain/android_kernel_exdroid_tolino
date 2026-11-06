#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/gpio.h>
#include <linux/delay.h>

#include <linux/io.h>

#define GDEBUG 0
#include <linux/gallen_dbg.h>

#include "../../../arch/arm/mach-sunxi/ntx_hwconfig.h"
extern volatile NTX_HWCONFIG *gptHWCFG;

#define SINGLE_SHOT_TIME (6*1000) //in ms

#ifdef CONFIG_NTX_CILIX
#include <linux/miscdevice.h>
#include <linux/power/cilix-ricoh619.h>
#include "../../misc/ntx_cilix.h"
extern struct cilix* gcx;
extern bool cilix_otg_charger(void);
#endif

#ifdef CONFIG_MFD_RICOH619 //[
#include <linux/power/ricoh619_battery.h>
extern int ricoh619_charger_detect(void);
#endif
#if defined(CONFIG_MFD_ROHM_BD71828)
#include <linux/mfd/rohm-generic.h>
extern int bd71828_charger_detect(void);
#endif
#if !defined(CONFIG_MFD_RICOH619) && !defined(CONFIG_MFD_ROHM_BD71828)
#define NO_CHARGER_PLUGGED	0
#define SDP_CHARGER					1
#endif //]CONFIG_MFD_RICOH619

#define USB_PLUG_STATE_INIT     0x9000


struct usb_plug_priv {
	struct device    *ddev;
	int (*get_status) (void);
};

static struct usb_plug_priv *mxc_usbplug;
extern void set_pmic_dc_charger_state(int dccharger);

void usb_plug_handler(int prescan_type)
{
	int plugged;
	static int last_status=-1;

	GALLEN_DBGLOCAL_BEGIN();

	printk("%s(%d)\n",__func__,prescan_type);

	if(0==mxc_usbplug) {
		printk(KERN_ERR "%s(%d):skip %s() when mxc_usbplug not exist !\n",
				__FILE__,__LINE__,__FUNCTION__);
		return ;
	}
	if(0==mxc_usbplug->ddev) {
		printk(KERN_ERR "%s(%d):skip %s() when mxc_usbplug->ddev not exist !\n",
				__FILE__,__LINE__,__FUNCTION__);
		return ;
	}

//#ifdef CONFIG_MFD_RICOH619 //[

	/* charger type is not yet available when booting
		 execute a charger detect to get it. Otherwise
		 use type passed through function paramter*/
#ifdef CONFIG_NTX_CILIX
	if(gcx)
	{
			if(gcx->status->active && -1 == last_status)
			{
					printk(KERN_DEBUG "skip %s() when booting with cilix active !\n", __FUNCTION__);
				last_status = -2;
				return;
			}
	}
	else
	{
			/* no cilix model or cilix not ready */
			//printk(KERN_ERR "cilix driver is not ready. skip status check!\n", __FUNCTION__);
	}
#endif

	if ( (1==gptHWCFG->m_val.bPMIC) || (6==gptHWCFG->m_val.bPMIC) ) {
		if(prescan_type == USB_PLUG_STATE_INIT)
			plugged = ricoh619_charger_detect ();
		else
			plugged = prescan_type;
	}
//#else //][
	else {
		printk("%s() : charger detecting function TBD !\n",__FUNCTION__);
		plugged = SDP_CHARGER ;
	}


//#endif //]CONFIG_MFD_RICOH619



	if(plugged==last_status) {
		return ;
	}
	if (NO_CHARGER_PLUGGED != plugged) {
		if ( (SDP_CHARGER == plugged) || (SDP_OVRLIM_CHARGER == plugged) || (CDP_CHARGER == plugged)) {
			kobject_rename(&mxc_usbplug->ddev->kobj, "usb_host");
		}
		else {
			kobject_rename(&mxc_usbplug->ddev->kobj, "usb_plug");
		}

		pr_info("usb plugged 0x%x\n", plugged);
		kobject_uevent(&mxc_usbplug->ddev->kobj, KOBJ_ADD);
	} else {
		kobject_uevent(&mxc_usbplug->ddev->kobj, KOBJ_REMOVE);
		kobject_set_name(&mxc_usbplug->ddev->kobj, "%s", "usb_state");
		pr_info("usb unplugged\n");
	}
	last_status = plugged;
	GALLEN_DBGLOCAL_END();
}

static void single_shot_worker(struct work_struct *work)
{
	GALLEN_DBGLOCAL_BEGIN();
#ifdef CONFIG_NTX_CILIX
    if(cilix_otg_charger())
    {
        usb_plug_handler(CILIX_CHARGER);
    }
    else
#endif
    {
	    usb_plug_handler(USB_PLUG_STATE_INIT);
    }
	GALLEN_DBGLOCAL_END();

}
static DECLARE_DELAYED_WORK(single_shot_work, single_shot_worker);

static int usb_plug_probe(struct platform_device *pdev)
{
	int ret;
	struct usbplug_event_platform_data *pdata = pdev->dev.platform_data;

	GALLEN_DBGLOCAL_BEGIN();
	mxc_usbplug = kzalloc(sizeof(struct usb_plug_priv), GFP_KERNEL);
	if (!mxc_usbplug) {
		GALLEN_DBGLOCAL_RUNLOG(0);
		dev_err(&pdev->dev, "Error: kzalloc\n");
		ret = -ENOMEM;
		goto err_allocate;
	}

	mxc_usbplug->ddev = &pdev->dev;
	kobject_set_name(&mxc_usbplug->ddev->kobj, "%s", "usb_state");
	schedule_delayed_work(&single_shot_work, msecs_to_jiffies(SINGLE_SHOT_TIME));

	GALLEN_DBGLOCAL_END();
	return 0;

err_allocate:

	return ret;
}

static int usb_plug_remove(struct platform_device *pdev)
{
	GALLEN_DBGLOCAL_BEGIN();
	cancel_delayed_work_sync(&single_shot_work);
	kfree(mxc_usbplug);
	GALLEN_DBGLOCAL_END();
	return 0;
}

static struct platform_driver usb_plug_driver = {
	.probe		= usb_plug_probe,
	.remove		= usb_plug_remove,
	.driver		= {
		.name	= "usb_plug",
		.owner	= THIS_MODULE,
	},
};
#ifndef CONFIG_FSL_UTP
static int usb_plug_init(void)
{
	return platform_driver_register(&usb_plug_driver);
}
module_init(usb_plug_init);

static void usb_plug_exit(void)
{
	platform_driver_unregister(&usb_plug_driver);
}
module_exit(usb_plug_exit);
#endif