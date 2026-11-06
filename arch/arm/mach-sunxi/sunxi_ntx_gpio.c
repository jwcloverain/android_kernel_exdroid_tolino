
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/init.h>
#include <linux/err.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/delay.h>
 #include <linux/slab.h>


#include "ntx_hwconfig.h"
extern volatile NTX_HWCONFIG *gptHWCFG;



struct ntx_io_data {
	//struct device *dev;
	struct backlight_device *ptNTX_bl;
	int iInitPercent;
};
static const struct of_device_id ntx_io_dt_ids[] = {
	{ .compatible = "ntx,ntx_keypad", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ntx_io_dt_ids);



static int ntx_io_update_status(struct backlight_device *bl)
{
	struct ntx_bl_data *ptNTX_BL_Data=0;

        return 0 ;
}

/*
static const struct backlight_ops ntx_io_ops = {
//	.options = BL_CORE_SUSPENDRESUME,
	.update_status = ntx_io_update_status,
	//.get_brightness = ntx_io_get_brightness,
};
*/


static int set_keypad(void)
{
        return 0;
        switch(gptHWCFG->m_val.bKeyPad) //key pad define through bKeyPad in hwconfig
	{       
#if 0                
                case 12: // NO_Key
		        ntx_gpio_key_data.buttons = gpio_key_None;
		        ntx_gpio_key_data.nbuttons = ARRAY_SIZE(gpio_key_None);
			break;
#endif                        
                case 11: // FL_Key
                        if (78 == gptHWCFG->m_val.bPCB) {
                                //ntx_gpio_key_data.buttons = gpio_key_FL_1;
                                //ntx_gpio_key_data.nbuttons = ARRAY_SIZE(gpio_key_FL_1);
		        }
#if 0                        
			else if (82 == gptHWCFG->m_val.bPCB) {
				// M35QEX .
				mxc_iomux_v3_setup_pad (MX6SL_PAD_KEY_ROW5__GPIO_4_3);
                                ntx_gpio_key_data.buttons = gpio_key_FL_Signage;
                                ntx_gpio_key_data.nbuttons = ARRAY_SIZE(gpio_key_FL_Signage);
                        } 
			else {
                                ntx_gpio_key_data.buttons = gpio_key_FL;
                                ntx_gpio_key_data.nbuttons = ARRAY_SIZE(gpio_key_FL);
                        }
#endif                        
                        break;
 #if 0                       
		case 16: // FL+HOME PAD
	        case 13: // FL+HOME KEY
			if(50==gptHWCFG->m_val.bPCB) {
                		// E60QFX
		          	ntx_gpio_key_data.buttons = gpio_key_HOME_FL2;
		          	ntx_gpio_key_data.nbuttons = ARRAY_SIZE(gpio_key_HOME_FL2);
			}
			else {
		          	ntx_gpio_key_data.buttons = gpio_key_HOME_FL;
		          	ntx_gpio_key_data.nbuttons = ARRAY_SIZE(gpio_key_HOME_FL);
			}
		        break;
		case 14: // HOME
		case 18: // HOMEPAD
			if(58==gptHWCFG->m_val.bPCB||65==gptHWCFG->m_val.bPCB) {
				//E60QJX/E60QNX .
		          	ntx_gpio_key_data.buttons = gpio_key_HOME_ROW0;
		          	ntx_gpio_key_data.nbuttons = ARRAY_SIZE(gpio_key_HOME_ROW0);
			}
			else {
		          	ntx_gpio_key_data.buttons = gpio_key_HOME;
		          	ntx_gpio_key_data.nbuttons = ARRAY_SIZE(gpio_key_HOME);
			}
	                break;
		case 17: // RETURN+HOME+MENU
		        ntx_gpio_key_data.buttons = gpio_key_RETURN_HOME_MENU;
		        ntx_gpio_key_data.nbuttons = ARRAY_SIZE(gpio_key_RETURN_HOME_MENU);
		        break;
		case 22: // LEFT+RIGHT+HOME+MENU
		        ntx_gpio_key_data.buttons = gpio_key_LEFT_RIGHT_HOME_MENU;
		        ntx_gpio_key_data.nbuttons = ARRAY_SIZE(gpio_key_LEFT_RIGHT_HOME_MENU);
		        break;
		case 23: // L1+L2+R1+R2+TP
			ntx_gpio_key_data.buttons = gpio_key_L1_L2_R1_R2_TP;
			ntx_gpio_key_data.nbuttons = ARRAY_SIZE(gpio_key_L1_L2_R1_R2_TP);	
			break;
		case 24: // L1+L2+R1+R2+HOME
			if(84==gptHWCFG->m_val.bPCB) // E70Q54
			{
				ntx_gpio_key_data.buttons = gpio_key_L1_L2_R1_R2_HOME_1;
				ntx_gpio_key_data.nbuttons = ARRAY_SIZE(gpio_key_L1_L2_R1_R2_HOME_1);
			}
			else{
				ntx_gpio_key_data.buttons = gpio_key_L1_L2_R1_R2_HOME;
				ntx_gpio_key_data.nbuttons = ARRAY_SIZE(gpio_key_L1_L2_R1_R2_HOME);
			}
                        break;
		case 25:// L1+L2+R1+R2+FL
			ntx_gpio_key_data.buttons = gpio_key_L1_L2_R1_R2_FL;
			ntx_gpio_key_data.nbuttons = ARRAY_SIZE(gpio_key_L1_L2_R1_R2_FL);
			break;
		case 26:// PGUP+PGDN
			ntx_gpio_key_data.buttons = gpio_key_PGUP_PGDN;
			ntx_gpio_key_data.nbuttons = ARRAY_SIZE(gpio_key_PGUP_PGDN);
			break;					
		case 27: // PGUP+PGDN+TP
			ntx_gpio_key_data.buttons = gpio_key_PGUP_PGDN_TP;
			ntx_gpio_key_data.nbuttons = ARRAY_SIZE(gpio_key_PGUP_PGDN_TP);	
			break;
		case 28: // PGUP+PGDN+HOME
			ntx_gpio_key_data.buttons = gpio_key_PGUP_PGDN_HOME;
			ntx_gpio_key_data.nbuttons = ARRAY_SIZE(gpio_key_PGUP_PGDN_HOME);	
			break;
		case 29: // TTS
			ntx_gpio_key_data.buttons = gpio_key_TTS_Signage;
			ntx_gpio_key_data.nbuttons = ARRAY_SIZE(gpio_key_TTS_Signage);
			break;

		default: // FL+HOME
		        ntx_gpio_key_data.buttons = gpio_key_HOME_FL;
	                ntx_gpio_key_data.nbuttons = ARRAY_SIZE(gpio_key_HOME_FL);
		        break;
#else
		default: 

		        break;
#endif                        
	}

}

static int ntx_io_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	//struct backlight_properties props;

	struct ntx_io_data *ptNTX_IO_Data=0;

        printk(KERN_ERR"============%s_%d==============\n",__FUNCTION__,__LINE__);

	ptNTX_IO_Data = kmalloc(sizeof(struct ntx_io_data),GFP_KERNEL);
	if(!ptNTX_IO_Data) {
		dev_err(&pdev->dev,"%s() : memory not enought !\n",__FUNCTION__);
		return -ENOMEM;
	}

	memset(ptNTX_IO_Data,0,sizeof(struct ntx_io_data));

	platform_set_drvdata(pdev,ptNTX_IO_Data);
	


	//////////////////////////////////////
	//props.type = BACKLIGHT_RAW;
	//props.brightness = ptNTX_BL_Data->iInitPercent;
	//props.max_brightness = 100;


	printk(KERN_ERR"[%s] Success\n",__func__);
	return 0;

probe_failed:

	return -EIO;
}


static int ntx_io_remove(struct platform_device *pdev)
{
	struct ntx_io_data *ptNTX_IO_Data;

	ptNTX_IO_Data = platform_get_drvdata(pdev);

	kfree(ptNTX_IO_Data);

	return 0;
}

void ntx_io_shutdown(struct platform_device *pdev)
{

}

#ifdef CONFIG_PM_SLEEP
static int ntx_io_suspend(struct device *dev)
{
	return 0;
}

static int ntx_io_resume(struct device *dev)
{
	return 0;
}
#else
#define ntx_io_suspend	NULL
#define ntx_io_resume	NULL
#endif

#ifdef CONFIG_PM
static int ntx_io_runtime_suspend(struct device *dev)
{
	return 0;
}

static int ntx_io_runtime_resume(struct device *dev)
{
	return 0;
}
#else
#define ntx_io_runtime_suspend	NULL
#define ntx_io_runtime_resume	NULL
#endif


static const struct dev_pm_ops ntx_io_pm_ops = {
	SET_RUNTIME_PM_OPS(ntx_io_runtime_suspend,ntx_io_runtime_resume, NULL)
	SET_SYSTEM_SLEEP_PM_OPS(ntx_io_suspend, ntx_io_resume)
};

static struct platform_driver ntx_io_driver = {
	.probe = ntx_io_probe,
	.remove = ntx_io_remove,
	.shutdown = ntx_io_shutdown,
	.driver = {
		   .name = "ntx_keypad",
		   .owner = THIS_MODULE,
		   .of_match_table = of_match_ptr(ntx_io_dt_ids),
		   //.pm = &ntx_bl_pm_ops,
		   },
};

static int __init ntx_io_init(void)
{
	return platform_driver_register(&ntx_io_driver);
}

static void __exit ntx_io_exit(void)
{
	return platform_driver_unregister(&ntx_io_driver);
}


/*
 * Module entry points
 */
subsys_initcall(ntx_io_init);
module_exit(ntx_io_exit);

