/*
 * cyttsp5_i2c.c
 * Parade TrueTouch(TM) Standard Product V5 I2C Module.
 * For use with Parade touchscreen controllers.
 * Supported parts include:
 * CYTMA5XX
 * CYTMA448
 * CYTMA445A
 * CYTT21XXX
 * CYTT31XXX
 *
 * Copyright (C) 2015 Parade Technologies
 * Copyright (C) 2012-2015 Cypress Semiconductor
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2, and only version 2, as published by the
 * Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * Contact Parade Technologies at www.paradetech.com <ttdrivers@paradetech.com>
 *
 */

#include "cyttsp5_regs.h"
#include "cyttsp5_core.h"


#include <linux/i2c.h>
#include <linux/version.h>
#include "../../init-input.h"

#define CY_I2C_DATA_SIZE  (2 * 256)

#include "../../../../arch/arm/mach-sunxi/ntx_hwconfig.h"
extern volatile NTX_HWCONFIG *gptHWCFG;

static struct ctp_config_info config_info = {
	.input_type = CTP_TYPE,
	.name = NULL,
	.int_number = 0,
};

static u32 screen_max_x = 0;
static u32 screen_max_y = 0;
static u32 revert_x_flag = 0;
static u32 revert_y_flag = 0;
static u32 exchange_x_y_flag = 0;

static __u32 twi_id;

extern void cy_tp_device_exit(void);
extern int cy_tp_device_init(void);



static int cyttsp5_i2c_read_default(struct device *dev, void *buf, int size)
{
	struct i2c_client *client = to_i2c_client(dev);
	int rc;

	if (!buf || !size || size > CY_I2C_DATA_SIZE)
		return -EINVAL;

	rc = i2c_master_recv(client, buf, size);

	return (rc < 0) ? rc : rc != size ? -EIO : 0;
}

static int cyttsp5_i2c_read_default_nosize(struct device *dev, u8 *buf, u32 max)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct i2c_msg msgs[2];
	u8 msg_count = 1;
	int rc;
	u32 size;

	if (!buf)
		return -EINVAL;

	msgs[0].addr = client->addr;
	msgs[0].flags = (client->flags & I2C_M_TEN) | I2C_M_RD;
	msgs[0].len = 2;
	msgs[0].buf = buf;
	rc = i2c_transfer(client->adapter, msgs, msg_count);
	if (rc < 0 || rc != msg_count)
		return (rc < 0) ? rc : -EIO;

	size = get_unaligned_le16(&buf[0]);
	if (!size || size == 2 || size >= CY_PIP_1P7_EMPTY_BUF)
		/*
		 * Before PIP 1.7, empty buffer is 0x0002;
		 * From PIP 1.7, empty buffer is 0xFFXX
		 */
		return 0;

	if (size > max)
		return -EINVAL;

	rc = i2c_master_recv(client, buf, size);

	return (rc < 0) ? rc : rc != (int)size ? -EIO : 0;
}

static int cyttsp5_i2c_write_read_specific(struct device *dev, u8 write_len,
		u8 *write_buf, u8 *read_buf)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct i2c_msg msgs[2];
	u8 msg_count = 1;
	int rc;

	if (!write_buf || !write_len)
		return -EINVAL;

	msgs[0].addr = client->addr;
	msgs[0].flags = client->flags & I2C_M_TEN;
	msgs[0].len = write_len;
	msgs[0].buf = write_buf;
	rc = i2c_transfer(client->adapter, msgs, msg_count);

	if (rc < 0 || rc != msg_count)
		return (rc < 0) ? rc : -EIO;

	rc = 0;

	if (read_buf)
		rc = cyttsp5_i2c_read_default_nosize(dev, read_buf,
				CY_I2C_DATA_SIZE);

	return rc;
}

static struct cyttsp5_bus_ops cyttsp5_i2c_bus_ops = {
	.bustype = BUS_I2C,
	.read_default = cyttsp5_i2c_read_default,
	.read_default_nosize = cyttsp5_i2c_read_default_nosize,
	.write_read_specific = cyttsp5_i2c_write_read_specific,
};

#ifdef CONFIG_TOUCHSCREEN_CYPRESS_CYTTSP5_DEVICETREE_SUPPORT
static const struct of_device_id cyttsp5_i2c_of_match[] = {
	{ .compatible = "cy,cyttsp5_i2c_adapter", },
	{ }
};
MODULE_DEVICE_TABLE(of, cyttsp5_i2c_of_match);
#endif

static int cyttsp5_i2c_probe(struct i2c_client *client,
	const struct i2c_device_id *i2c_id)
{
	struct device *dev = &client->dev;
#ifdef CONFIG_TOUCHSCREEN_CYPRESS_CYTTSP5_DEVICETREE_SUPPORT
	const struct of_device_id *match;
#endif
	int rc;
	u8 buf[8]={0};

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		dev_err(dev, "I2C functionality not Supported\n");
		return -EIO;
	}
	//printk("%s 0\n", __func__);

	// Check device is exist
	rc = i2c_master_recv(client, buf, 8);
	if(rc<0){
		return rc ;
	}

#ifdef CONFIG_TOUCHSCREEN_CYPRESS_CYTTSP5_DEVICETREE_SUPPORT
	match = of_match_device(of_match_ptr(cyttsp5_i2c_of_match), dev);
	if (match) {
		rc = cyttsp5_devtree_create_and_get_pdata(dev);
		if (rc < 0)
			return rc;
	//printk("%s 1\n", __func__);
	}
#elif defined(CONFIG_TOUCHSCREEN_CYPRESS_CYTTSP5_SYSCONFIG_SUPPORT)
	rc = cyttsp5_sysconfig_create_and_get_pdata(dev);
	if (rc < 0)
		return rc;
	//printk("%s 1\n", __func__);
#endif

	//printk("%s 2\n", __func__);
	((struct cyttsp5_platform_data*)(dev->platform_data))->core_pdata->irq_gpio = config_info.irq_gpio.gpio;
	((struct cyttsp5_platform_data*)(dev->platform_data))->core_pdata->rst_gpio = config_info.wakeup_gpio.gpio;
	((struct cyttsp5_platform_data*)(dev->platform_data))->core_pdata->ctp_power_ldo = config_info.ctp_power_ldo;
	rc = cyttsp5_probe(&cyttsp5_i2c_bus_ops, &client->dev, client->irq,
			  CY_I2C_DATA_SIZE);

#ifdef CONFIG_TOUCHSCREEN_CYPRESS_CYTTSP5_DEVICETREE_SUPPORT
	if (rc && match)
		cyttsp5_devtree_clean_pdata(dev);
#endif
	//printk("%s 3\n", __func__);

	return rc;
}

static int cyttsp5_i2c_remove(struct i2c_client *client)
{
#ifdef CONFIG_TOUCHSCREEN_CYPRESS_CYTTSP5_DEVICETREE_SUPPORT
	struct device *dev = &client->dev;
	const struct of_device_id *match;
#endif
	struct cyttsp5_core_data *cd = i2c_get_clientdata(client);

	cyttsp5_release(cd);

#ifdef CONFIG_TOUCHSCREEN_CYPRESS_CYTTSP5_DEVICETREE_SUPPORT
	match = of_match_device(of_match_ptr(cyttsp5_i2c_of_match), dev);
	if (match)
		cyttsp5_devtree_clean_pdata(dev);
#endif

	return 0;
}

/**
 * ctp_print_info - sysconfig print function
 * return value:
 *
 */
static void ctp_print_info(struct ctp_config_info info, int debug_level)
{
#define pr_debug printk
#define DEBUG_INIT 1
	if (debug_level == DEBUG_INIT) {
		pr_debug("info.ctp_used:%d\n", info.ctp_used);
		pr_debug("info.twi_id:%d\n", info.twi_id);
		pr_debug("info.screen_max_x:%d\n", info.screen_max_x);
		pr_debug("info.screen_max_y:%d\n", info.screen_max_y);
		pr_debug("info.revert_x_flag:%d\n", info.revert_x_flag);
		pr_debug("info.revert_y_flag:%d\n", info.revert_y_flag);
		pr_debug("info.exchange_x_y_flag:%d\n", info.exchange_x_y_flag);
		pr_debug("info.irq_gpio_number:%d\n", info.irq_gpio.gpio);
		pr_debug("info.wakeup_gpio_number:%d\n", info.wakeup_gpio.gpio);
	}
}

int cyttsp5_get_revert_status(struct revert_status *revert)
{
	revert->revert_x_flag = revert_x_flag;
	revert->revert_y_flag = revert_y_flag;
	revert->exchange_x_y_flag = exchange_x_y_flag;
	revert->screen_max_x = screen_max_x;
	revert->screen_max_y = screen_max_y;

	return 0;
}

/**
 * ctp_wakeup - function
 *
 */
static int ctp_wakeup(int status, int ms)
{
	pr_debug("***CTP*** %s:status:%d, ms = %d\n", __func__, status, ms);
	if (status == 0) {
		if (ms == 0)
			__gpio_set_value(config_info.wakeup_gpio.gpio, 0);
		else {
			__gpio_set_value(config_info.wakeup_gpio.gpio, 0);
			msleep(ms);
			__gpio_set_value(config_info.wakeup_gpio.gpio, 1);
		}
	}
	if (status == 1) {
		if (ms == 0)
			__gpio_set_value(config_info.wakeup_gpio.gpio, 1);
		else {
			__gpio_set_value(config_info.wakeup_gpio.gpio, 1);
			msleep(ms);
			__gpio_set_value(config_info.wakeup_gpio.gpio, 0);
		}
	}
	usleep_range(5000, 6000);

	return 0;
}

/**
 * ctp_detect - Device detection callback for automatic device creation
 * return value:
 *                    = 0; success;
 *                    < 0; err
 */
static int ctp_detect(struct i2c_client *client, struct i2c_board_info *info)
{
	struct i2c_adapter *adapter = client->adapter;
	int  i = 0;

	//printk("%s\n", __func__);
	if (!i2c_check_functionality(adapter, I2C_FUNC_SMBUS_BYTE_DATA)) {
		pr_info("======return=====\n");
		return -ENODEV;
	}
	//printk("%s 1\n", __func__);
	if (twi_id == adapter->nr) {
		/*
		i2c_read_bytes(client, read_chip_value, 3);
		pr_debug("addr:0x%x,chip_id_value:0x%x\n",
					client->addr, read_chip_value[2]);
		*/
		//printk("%s 2\n", __func__);
		strlcpy(info->type, CYTTSP5_I2C_NAME, I2C_NAME_SIZE);
		//printk("%s: %s\n", __func__, info->type);
		return 0;
	} else
		return -ENODEV;
}


static const struct i2c_device_id cyttsp5_i2c_id[] = {
	{ CYTTSP5_I2C_NAME, 0, },
	{ }
};
MODULE_DEVICE_TABLE(i2c, cyttsp5_i2c_id);

static const unsigned short normal_i2c[2] = {0x24, I2C_CLIENT_END};

static struct i2c_driver cyttsp5_i2c_driver = {
	.class = I2C_CLASS_HWMON,
	.driver = {
		.name = CYTTSP5_I2C_NAME,
		.owner = THIS_MODULE,
		.pm = &cyttsp5_pm_ops,
#ifdef CONFIG_TOUCHSCREEN_CYPRESS_CYTTSP5_DEVICETREE_SUPPORT
		.of_match_table = cyttsp5_i2c_of_match,
#elif defined(CONFIG_TOUCHSCREEN_CYPRESS_CYTTSP5_SYSCONFIG_SUPPORT)
		//.of_match_table = cyttsp5_i2c_of_match,
#endif
	},
	.detect         = ctp_detect,
	.probe = cyttsp5_i2c_probe,
	.remove = cyttsp5_i2c_remove,
	.id_table = cyttsp5_i2c_id,
	.address_list	= normal_i2c,
};

#if 0 //(KERNEL_VERSION(3, 3, 0) <= LINUX_VERSION_CODE)
module_i2c_driver(cyttsp5_i2c_driver);
#else
static int ctp_get_system_config(void)
{
	ctp_print_info(config_info, DEBUG_INIT);
	twi_id = config_info.twi_id;
	screen_max_x = config_info.screen_max_x;
	screen_max_y = config_info.screen_max_y;
	revert_x_flag = config_info.revert_x_flag;
	revert_y_flag = config_info.revert_y_flag;
	exchange_x_y_flag = config_info.exchange_x_y_flag;
	if ((screen_max_x == 0) || (screen_max_y == 0)) {
		pr_err("%s:read config error!\n", __func__);
		return 0;
	}

	return 1;
}

static int __init cyttsp5_i2c_init(void)
{
	int ret = -1;

	//printk("%s\n", __func__);
	pr_debug("*********************************************\n");
	if (!input_sensor_startup(&(config_info.input_type))) {
		ret = input_sensor_init(&(config_info.input_type));
		if (ret != 0) {
			pr_err("%s:ctp_ops.input_sensor_init err.\n", __func__);
			goto init_err;
		}
		input_set_power_enable(&(config_info.input_type), 1);
	} else {
		pr_err("%s: input_ctp_startup err.\n", __func__);
		return 0;
	}
	if (config_info.ctp_used == 0) {
		pr_info("*** ctp_used set to 0 !\n");
		pr_info("*** if use ctp, please put the sys_config.fex ctp_used set to 1.\n");
		ret = 0;
		goto init_err;
	}

	if (strcmp(config_info.name, "BYPROBE")==0) {
		printk("[CYTTSP5] BYPROBE!!\n");
		if(gptHWCFG->m_val.bPCB == 0x64) // E60P54
		{
			printk("[CYTTSP5] enforce probe !!\n");
			config_info.revert_x_flag = 0 ;
			config_info.revert_y_flag = 0;
			config_info.exchange_x_y_flag = 0;
		}
	}
	else{
		if (strncmp(config_info.name, "cyttsp5_i2c_adapter", strlen("cyttsp5_i2c_adapter"))) {
			pr_info("*** ctp_name set to %s !\n", config_info.name);
			ret = 0;
			goto init_err;
		}
	}

	if (!ctp_get_system_config()) {
		pr_err("%s:read config fail!\n", __func__);
		goto init_err;
	}
	ctp_wakeup(0, 2);

	ret = i2c_add_driver(&cyttsp5_i2c_driver);
	if (ret)
		goto init_err;

	pr_info("%s: Parade TTSP I2C Driver (Built %s) ret=%d\n",
			__func__, CY_DRIVER_VERSION, ret);
	cy_tp_device_init();
	pr_debug("*********************************************\n");
	return ret;

init_err:
	input_set_power_enable(&(config_info.input_type), 0);

	return ret;
}
module_init(cyttsp5_i2c_init);

static void __exit cyttsp5_i2c_exit(void)
{
	i2c_del_driver(&cyttsp5_i2c_driver);
	input_sensor_free(&(config_info.input_type));
	input_set_power_enable(&(config_info.input_type), 0);
	cy_tp_device_exit();
}
module_exit(cyttsp5_i2c_exit);
#endif

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Parade TrueTouch(R) Standard Product I2C driver");
MODULE_AUTHOR("Parade Technologies <ttdrivers@paradetech.com>");
