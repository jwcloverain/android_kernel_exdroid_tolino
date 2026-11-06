/*
 * driver/mfd/ricoh619.c
 *
 * Core driver implementation to access RICOH R5T619 power management chip.
 *
 * Copyright (C) 2012-2013 RICOH COMPANY,LTD
 *
 * Based on code
 *	Copyright (C) 2011 NVIDIA Corporation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */
/*#define DEBUG			1*/
/*#define VERBOSE_DEBUG		1*/
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/gpio.h>
#include <linux/i2c.h>
#include <linux/mfd/core.h>
#include "../power/supply/axp/axp-core.h"
#include <linux/sunxi-gpio.h>
#include <linux/mfd/ricoh619.h>
#include <linux/rtc/rtc-ricoh619.h>
#include <linux/power/ricoh619_battery.h>
#include <linux/of_irq.h>
#include <linux/of_gpio.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/regulator/of_regulator.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>
#include <linux/regmap.h>
#include <linux/delay.h>
#include <asm/system_misc.h>

#if defined(CONFIG_SENSORS_P15USB30216C)
#include  "../usb/sunxi_usb/manager/usb_msg_center.h"
#include  "../usb/sunxi_usb/manager/usbc_platform.h"
#include  "../usb/sunxi_usb/include/sunxi_usb_board.h"
extern int P15USB30216C_charger_type_get(void);
#endif

#include "../../arch/arm/mach-sunxi/ntx_hwconfig.h"
extern volatile NTX_HWCONFIG *gptHWCFG;


#ifdef CONFIG_NTX_CILIX
extern struct cilix* gcx;
extern int m_msp430_int_pin_reset(void);
#endif

#define SUNXI_BOOTFLAG_BY_RICOH		1

struct ricoh61x *g_ricoh61x;
struct sleep_control_data {
	u8 reg_add;
};

#if defined(CONFIG_SENSORS_P15USB30216C)
int usb_device_chose(void)
{
	hw_rmmod_usb_host();
	hw_rmmod_usb_device();
	usb_msg_center(&g_usb_cfg);

	hw_insmod_usb_device();
	usb_msg_center(&g_usb_cfg);
	return 0;
}
#endif

static struct mfd_cell ricoh61xs[] = {
	{
		.name = "ricoh61x-regulator",
	},
	{
		.name = "ricoh619-battery",
	},
	{
		.name = "rtc_ricoh619",
	},
	{
		.name = "ricoh619-pwrkey",
	},
};

#define SLEEP_INIT(_id, _reg)		\
	[RICOH619_DS_##_id] = {.reg_add = _reg}
/*
static struct sleep_control_data sleep_data[] = {
	SLEEP_INIT(DC1, 0x16),
	SLEEP_INIT(DC2, 0x17),
	SLEEP_INIT(DC3, 0x18),
	SLEEP_INIT(DC4, 0x19),
	SLEEP_INIT(DC5, 0x1A),
	SLEEP_INIT(LDO1, 0x1B),
	SLEEP_INIT(LDO2, 0x1C),
	SLEEP_INIT(LDO3, 0x1D),
	SLEEP_INIT(LDO4, 0x1E),
	SLEEP_INIT(LDO5, 0x1F),
	SLEEP_INIT(LDO6, 0x20),
	SLEEP_INIT(LDO7, 0x21),
	SLEEP_INIT(LDO8, 0x22),
	SLEEP_INIT(LDO9, 0x23),
	SLEEP_INIT(LDO10, 0x24),
	SLEEP_INIT(PSO0, 0x25),
	SLEEP_INIT(PSO1, 0x26),
	SLEEP_INIT(PSO2, 0x27),
	SLEEP_INIT(PSO3, 0x28),
	SLEEP_INIT(PSO4, 0x29),
	SLEEP_INIT(LDORTC1, 0x2A),
};
*/

#ifdef CONFIG_NTX_CILIX
/*count +1 if val=1 and reset count if val=0*/
static void ricoh61x_i2c_err_cnt(int val)
{
    static unsigned int count = 0;
//    printk("%s() entered(%d), count=%d\n", __func__, val, count);

    if(val)
    {
        count = count + 1;
        if(count >= 10)
        {
            if(gcx)
            {
                /*if 10 i2c errors without success, reset msp430*/
                m_msp430_int_pin_reset();
                count = 0;
            }
        }
    }
    else
    {
        count = 0;
    }

}
#endif

static inline int __ricoh61x_read(struct i2c_client *client,
				  u8 reg, uint8_t *val)
{
	int ret;

	ret = i2c_smbus_read_byte_data(client, reg);
	if (ret < 0) {
		dev_err(&client->dev, "failed reading at 0x%02x\n", reg);
#ifdef CONFIG_NTX_CILIX
        ricoh61x_i2c_err_cnt(1);
#endif
		return ret;
	}
#ifdef CONFIG_NTX_CILIX
    else
    {
        // once communication is okay, reset error count
        ricoh61x_i2c_err_cnt(0);
    }
#endif

	*val = (uint8_t)ret;
	dev_dbg(&client->dev, "ricoh61x: reg read  reg=%x, val=%x\n",
				reg, *val);
	return 0;
}

static inline int __ricoh61x_bulk_reads(struct i2c_client *client, u8 reg,
				int len, uint8_t *val)
{
	int ret;
	int i;

	ret = i2c_smbus_read_i2c_block_data(client, reg, len, val);
	if (ret < 0) {
		dev_err(&client->dev, "failed reading from 0x%02x\n", reg);
		return ret;
	}
	for (i = 0; i < len; ++i) {
		dev_dbg(&client->dev, "ricoh61x: reg read  reg=%x, val=%x\n",
				reg + i, *(val + i));
	}
	return 0;
}

static inline int __ricoh61x_write(struct i2c_client *client,
				 u8 reg, uint8_t val)
{
	int ret;
#if 0// specific register debug .
	if( (0==g_ricoh61x->bank_num) && ( (0x34==reg)||(0x35==reg) )) {
		dev_info(&client->dev, "ricoh61x: reg write  reg=%x, val=%x\n",
				reg, val);
		dump_stack();
	}
	else 
#endif
	{
		dev_dbg(&client->dev, "ricoh61x: reg write  reg=%x, val=%x\n",
				reg, val);
	}
	ret = i2c_smbus_write_byte_data(client, reg, val);
	if (ret < 0) {
#ifdef CONFIG_NTX_CILIX
        ricoh61x_i2c_err_cnt(1);
#endif
		dev_err(&client->dev, "failed writing 0x%02x to 0x%02x\n",
				val, reg);
		return ret;
    }
#ifdef CONFIG_NTX_CILIX
    else
    {
        // once communication is okay, reset error count
        ricoh61x_i2c_err_cnt(0);
    }
#endif
	return 0;
}

static inline int __ricoh61x_bulk_writes(struct i2c_client *client, u8 reg,
				  int len, uint8_t *val)
{
	int ret;
	int i;

	for (i = 0; i < len; ++i) {
		dev_dbg(&client->dev, "ricoh61x: reg write  reg=%x, val=%x\n",
				reg + i, *(val + i));
	}

	ret = i2c_smbus_write_i2c_block_data(client, reg, len, val);
	if (ret < 0) {
		dev_err(&client->dev, "failed writings to 0x%02x\n", reg);
		return ret;
	}

	return 0;
}

static inline int set_bank_ricoh61x(struct device *dev, int bank)
{
	struct ricoh61x *ricoh61x = dev_get_drvdata(dev);
	int ret;

	if (bank != (bank & 1))
		return -EINVAL;
	if (bank == ricoh61x->bank_num)
		return 0;
	ret = __ricoh61x_write(to_i2c_client(dev), RICOH61x_REG_BANKSEL, bank);
	if (!ret)
		ricoh61x->bank_num = bank;

	return ret;
}

int ricoh61x_write_ex(struct device *dev, u8 reg, uint8_t val,int is_safe_op)
{
	struct ricoh61x *ricoh61x = dev_get_drvdata(dev);
	int ret = 0;

	if(!is_safe_op) {
		mutex_lock(&ricoh61x->io_lock);
	}
	ret = set_bank_ricoh61x(dev, 0);
	if (!ret)
		ret = __ricoh61x_write(to_i2c_client(dev), reg, val);
	if(!is_safe_op) {
		mutex_unlock(&ricoh61x->io_lock);
	}

	return ret;
}
EXPORT_SYMBOL_GPL(ricoh61x_write_ex);
int ricoh61x_write(struct device *dev, u8 reg, uint8_t val)
{
	return ricoh61x_write_ex(dev, reg, val,0);
}
EXPORT_SYMBOL_GPL(ricoh61x_write);

int ricoh61x_write_bank1_ex(struct device *dev, u8 reg, uint8_t val,int is_safe_op)
{
	struct ricoh61x *ricoh61x = dev_get_drvdata(dev);
	int ret = 0;
	if(!is_safe_op) {
		mutex_lock(&ricoh61x->io_lock);
	}
	ret = set_bank_ricoh61x(dev, 1);
	if (!ret)
		ret = __ricoh61x_write(to_i2c_client(dev), reg, val);
	if(!is_safe_op) {
		mutex_unlock(&ricoh61x->io_lock);
	}
	return ret;
}
EXPORT_SYMBOL_GPL(ricoh61x_write_bank1_ex);
int ricoh61x_write_bank1(struct device *dev, u8 reg, uint8_t val)
{
	return ricoh61x_write_bank1_ex(dev, reg, val, 0);
}
EXPORT_SYMBOL_GPL(ricoh61x_write_bank1);

int ricoh61x_bulk_writes(struct device *dev, u8 reg, u8 len, uint8_t *val)
{
	struct ricoh61x *ricoh61x = dev_get_drvdata(dev);
	int ret = 0;

	mutex_lock(&ricoh61x->io_lock);
	ret = set_bank_ricoh61x(dev, 0);
	if (!ret)
		ret = __ricoh61x_bulk_writes(to_i2c_client(dev), reg, len, val);
	mutex_unlock(&ricoh61x->io_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(ricoh61x_bulk_writes);

int ricoh61x_bulk_writes_bank1(struct device *dev, u8 reg, u8 len, uint8_t *val)
{
	struct ricoh61x *ricoh61x = dev_get_drvdata(dev);
	int ret = 0;

	mutex_lock(&ricoh61x->io_lock);
	ret = set_bank_ricoh61x(dev, 1);
	if (!ret)
		ret = __ricoh61x_bulk_writes(to_i2c_client(dev), reg, len, val);
	mutex_unlock(&ricoh61x->io_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(ricoh61x_bulk_writes_bank1);

int ricoh61x_read_ex(struct device *dev, u8 reg, uint8_t *val,int is_safe_op)
{
	struct ricoh61x *ricoh61x = dev_get_drvdata(dev);
	int ret = 0;

	if(!is_safe_op) {
		mutex_lock(&ricoh61x->io_lock);
	}
	ret = set_bank_ricoh61x(dev, 0);
	if (!ret)
		ret = __ricoh61x_read(to_i2c_client(dev), reg, val);

	if(!is_safe_op) {
		mutex_unlock(&ricoh61x->io_lock);
	}

	return ret;
}
EXPORT_SYMBOL_GPL(ricoh61x_read_ex);
int ricoh61x_read(struct device *dev, u8 reg, uint8_t *val) {
	return ricoh61x_read_ex(dev, reg, val,0);
}
EXPORT_SYMBOL_GPL(ricoh61x_read);

int ricoh61x_read_bank1_ex(struct device *dev, u8 reg, uint8_t *val,int is_safe_op)
{
	struct ricoh61x *ricoh61x = dev_get_drvdata(dev);
	int ret = 0;

	if(!is_safe_op) {
		mutex_lock(&ricoh61x->io_lock);
	}

	ret = set_bank_ricoh61x(dev, 1);
	if (!ret)
		ret =  __ricoh61x_read(to_i2c_client(dev), reg, val);

	if(!is_safe_op) {
		mutex_unlock(&ricoh61x->io_lock);
	}

	return ret;
}
EXPORT_SYMBOL_GPL(ricoh61x_read_bank1_ex);
int ricoh61x_read_bank1(struct device *dev, u8 reg, uint8_t *val)
{
	return ricoh61x_read_bank1_ex(dev, reg, val,0);
}
EXPORT_SYMBOL_GPL(ricoh61x_read_bank1);

int ricoh61x_bulk_reads(struct device *dev, u8 reg, u8 len, uint8_t *val)
{
	struct ricoh61x *ricoh61x = dev_get_drvdata(dev);
	int ret = 0;

	mutex_lock(&ricoh61x->io_lock);
	ret = set_bank_ricoh61x(dev, 0);
	if (!ret)
		ret = __ricoh61x_bulk_reads(to_i2c_client(dev), reg, len, val);
	mutex_unlock(&ricoh61x->io_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(ricoh61x_bulk_reads);

int ricoh61x_bulk_reads_bank1(struct device *dev, u8 reg, u8 len, uint8_t *val)
{
	struct ricoh61x *ricoh61x = dev_get_drvdata(dev);
	int ret = 0;

	mutex_lock(&ricoh61x->io_lock);
	ret = set_bank_ricoh61x(dev, 1);
	if (!ret)
		ret = __ricoh61x_bulk_reads(to_i2c_client(dev), reg, len, val);
	mutex_unlock(&ricoh61x->io_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(ricoh61x_bulk_reads_bank1);

int ricoh61x_set_bits(struct device *dev, u8 reg, uint8_t bit_mask)
{
	struct ricoh61x *ricoh61x = dev_get_drvdata(dev);
	uint8_t reg_val;
	int ret = 0;

	mutex_lock(&ricoh61x->io_lock);
	ret = set_bank_ricoh61x(dev, 0);
	if (!ret) {
		ret = __ricoh61x_read(to_i2c_client(dev), reg, &reg_val);
		if (ret)
			goto out;

		if ((reg_val & bit_mask) != bit_mask) {
			reg_val |= bit_mask;
			ret = __ricoh61x_write(to_i2c_client(dev), reg,
								 reg_val);
		}
	}
out:
	mutex_unlock(&ricoh61x->io_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(ricoh61x_set_bits);

int ricoh61x_clr_bits(struct device *dev, u8 reg, uint8_t bit_mask)
{
	struct ricoh61x *ricoh61x = dev_get_drvdata(dev);
	uint8_t reg_val;
	int ret = 0;

	mutex_lock(&ricoh61x->io_lock);
	ret = set_bank_ricoh61x(dev, 0);
	if (!ret) {
		ret = __ricoh61x_read(to_i2c_client(dev), reg, &reg_val);
		if (ret)
			goto out;

		if (reg_val & bit_mask) {
			reg_val &= ~bit_mask;
			ret = __ricoh61x_write(to_i2c_client(dev), reg,
								 reg_val);
		}
	}
out:
	mutex_unlock(&ricoh61x->io_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(ricoh61x_clr_bits);

int ricoh61x_update(struct device *dev, u8 reg, uint8_t val, uint8_t mask)
{
	struct ricoh61x *ricoh61x = dev_get_drvdata(dev);
	uint8_t reg_val;
	int ret = 0;

	mutex_lock(&ricoh61x->io_lock);
	ret = set_bank_ricoh61x(dev, 0);
	if (!ret) {
		ret = __ricoh61x_read(ricoh61x->client, reg, &reg_val);
		if (ret)
			goto out;

		if ((reg_val & mask) != val) {
			reg_val = (reg_val & ~mask) | (val & mask);
			ret = __ricoh61x_write(ricoh61x->client, reg, reg_val);
		}
	}
out:
	mutex_unlock(&ricoh61x->io_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(ricoh61x_update);

int ricoh61x_update_bank1(struct device *dev, u8 reg, uint8_t val, uint8_t mask)
{
	struct ricoh61x *ricoh61x = dev_get_drvdata(dev);
	uint8_t reg_val;
	int ret = 0;

	mutex_lock(&ricoh61x->io_lock);
	ret = set_bank_ricoh61x(dev, 1);
	if (!ret) {
		ret = __ricoh61x_read(ricoh61x->client, reg, &reg_val);
		if (ret)
			goto out;

		if ((reg_val & mask) != val) {
			reg_val = (reg_val & ~mask) | (val & mask);
			ret = __ricoh61x_write(ricoh61x->client, reg, reg_val);
		}
	}
out:
	mutex_unlock(&ricoh61x->io_lock);
	return ret;
}

static struct i2c_client *ricoh61x_i2c_client;
int ricoh619_cc_sum_clear(void)
{
	int ret;
	uint8_t cc_clr[4] = {0, 0, 0, 0}; /* temporary box */

	if (!ricoh61x_i2c_client)
		return -EINVAL;

	/* CC_pause enter */
	ret = __ricoh61x_write(ricoh61x_i2c_client, RICOH61x_CC_CTRL, 0x01);
	if (ret < 0)
		dev_err(&ricoh61x_i2c_client->dev, 
					"Error in writing CC_CTRL_REG\n");

	/* Write CC_SUM */
	ret = __ricoh61x_bulk_writes(ricoh61x_i2c_client, RICOH61x_CC_SUMREG3, 4, cc_clr);
	if (ret < 0)
		dev_err(&ricoh61x_i2c_client->dev, 
					"Error in writing CC_SUM_REG\n");

	/* CC_pause exit */
	ret = __ricoh61x_write(ricoh61x_i2c_client, RICOH61x_CC_CTRL, 0);
	if (ret < 0)
		dev_err(&ricoh61x_i2c_client->dev, 
					"Error in writing CC_CTRL_REG\n");

	return 0;
}

void ricoh619_power_off(void)
{
	int ret;
	uint8_t reg_val;
	reg_val = g_soc;
	reg_val &= 0x7f;

	if (!ricoh61x_i2c_client)
		return ;


	mutex_lock(&g_ricoh61x->io_lock);

	ricoh619_cc_sum_clear();

	ret = __ricoh61x_write(ricoh61x_i2c_client, RICOH61x_PSWR, reg_val);
	if (ret < 0)
		dev_err(&ricoh61x_i2c_client->dev, 
					"Error in writing PSWR_REG\n");

	if (g_fg_on_mode == 0) {
		/* Clear RICOH61x_FG_CTRL 0x01 bit */
		ret = __ricoh61x_read(ricoh61x_i2c_client,
				      RICOH61x_FG_CTRL, &reg_val);
        if (ret < 0)
			dev_err(&ricoh61x_i2c_client->dev, 
					"Error in read RICOH61x_FG_CTRL\n");
		if (reg_val & 0x01) {
			reg_val &= ~0x01;
			ret = __ricoh61x_write(ricoh61x_i2c_client,
					       RICOH61x_FG_CTRL, reg_val);
            if (ret < 0)
			dev_err(&ricoh61x_i2c_client->dev, 
					"Error in writing FG_CTRL\n");
		}
	}

	/* set rapid timer 300 min */
	ret = __ricoh61x_read(ricoh61x_i2c_client,
				      TIMSET_REG, &reg_val);
    if (ret < 0)
		dev_err(&ricoh61x_i2c_client->dev, 
				"Error in read the TIMSET_Reg\n");
		
	reg_val |= 0x03;
		
	ret = __ricoh61x_write(ricoh61x_i2c_client,
					       TIMSET_REG, reg_val);
	if (ret < 0)
		dev_err(&ricoh61x_i2c_client->dev, 
				"Error in writing the TIMSET_Reg\n");
  
  	/* Disable all Interrupt */
    ret = __ricoh61x_write(ricoh61x_i2c_client, RICOH61x_INTC_INTEN, 0);
    if (ret < 0)
        dev_err(&ricoh61x_i2c_client->dev, 
				"Error in writing the RICOH61x_INTC_INTEN\n");

  	/* Disable rtc Interrupt */
	ret = __ricoh61x_read(ricoh61x_i2c_client,
				      rtc_ctrl1, &reg_val);
    if (ret < 0)
        dev_err(&ricoh61x_i2c_client->dev, 
				"Error in read the rtc_ctrl1\n");
        
	reg_val &= 0xBF;	// clear DALE flag
    ret = __ricoh61x_write(ricoh61x_i2c_client, rtc_ctrl1, reg_val);
	if (ret < 0)
		dev_err(&ricoh61x_i2c_client->dev, 
				"Error in writing the rtc_ctrl1\n");
        
    mutex_unlock(&g_ricoh61x->io_lock);

    while(1) {
        printk ("[%s-%d] starting power down ...\n", __func__, __LINE__);
        mutex_lock(&g_ricoh61x->io_lock);
        printk ("[%s-%d] ricoh power down ...\n", __func__, __LINE__);
        /* Not repeat power ON after power off(Power Off/N_OE) */
        __ricoh61x_write(ricoh61x_i2c_client, RICOH61x_PWR_REP_CNT, 0x0);

        /* Power OFF */
        __ricoh61x_write(ricoh61x_i2c_client, RICOH61x_PWR_SLP_CNT, 0x1);
        mutex_unlock(&g_ricoh61x->io_lock);
        mdelay(1000);
    }

	return;
}

#include <linux/power/ricoh619_battery.h>
#include <linux/delay.h>

#ifdef CONFIG_ARCH_SUN8IW15P1 //[
extern int cfg_udc_pullup_command(int);
#endif //] CONFIG_ARCH_SUN8IW15P1

#if defined(USB_GADGET_DETECT) //[
extern int check_usb_state_by_ep(unsigned long dwTimeout_ms);
#endif //] 

int ricoh619_charger_detect_ex(int iForceReDetectTry)
{
#ifdef CONFIG_FSL_UTP
	if (!ricoh61x_i2c_client)
		return SDP_CHARGER;
#else
	if (!ricoh61x_i2c_client)
		return 0;
#endif
	uint8_t reg_val=0;
	int result = NO_CHARGER_PLUGGED;
	struct ricoh61x *ricoh61x = i2c_get_clientdata(ricoh61x_i2c_client);
	int ret;
	int iInINT = in_interrupt();
	int iCC_ChargerType = 0; // 0: not detected ; >1 : high/medium current type . 
	
	//printk("%s(),suspend=%d,atomic=%d,interrupt=%d\n",__FUNCTION__,ricoh61x->iIsSuspending,in_atomic()?1:0,in_interrupt()?1:0);
	
	ricoh61x->iIsChargerDetecting = 1;
	
	if(iInINT) {
		ret = __ricoh61x_read (ricoh61x_i2c_client, CHGSTATE_REG, &reg_val);
	}
	else {
		ret = ricoh61x_read (ricoh61x->dev, CHGSTATE_REG, &reg_val);
	}

#if defined(CONFIG_SENSORS_P15USB30216C)//[
	if (0xC0 & reg_val) 
	{
		volatile unsigned long ulJiffies_timeout = jiffies + msecs_to_jiffies(500);
		while(0==P15USB30216C_charger_type_get()) {
			if(time_is_after_jiffies(ulJiffies_timeout)) {
				printk(KERN_DEBUG"%s : waiting for first cc detecting ... \n",__func__);
			}
			else {
				printk(KERN_ERR"%s : waiting for first cc detection timeout !!\n",__func__);
				break;
			}
			msleep(20);
		}
	}
	iCC_ChargerType = P15USB30216C_charger_type_get();
#endif //] CONFIG_SENSORS_P15USB30216C
	if (0xC0 & reg_val) {
		int retryMax = 50;
		int retryCnt = 0;
		result = UNKOWN_CHARGER;



#ifdef CONFIG_ARCH_SUN8IW15P1 //[
		//printk("%s(%d) sunxi udc disable \n",__func__,__LINE__);
		cfg_udc_pullup_command(0);
#endif //] CONFIG_ARCH_SUN8IW15P1
		
		do {

			do {
				if(retryCnt++>=retryMax) {
					printk(KERN_ERR"%s() : retry read EXTIF_GCHGDET_REG cnt >=%d \n",
							__FUNCTION__,retryMax);
					break;
				}

				if(iInINT) {
					ret = __ricoh61x_read (ricoh61x_i2c_client, EXTIF_GCHGDET_REG, &reg_val);
				}
				else {
					ret = ricoh61x_read (ricoh61x->dev, EXTIF_GCHGDET_REG, &reg_val);
				}

				if(0!=ret) {
					printk(KERN_ERR"%s() :read EXTIF_GCHGDET_REG failed!!,%s\n",
							__FUNCTION__,ricoh61x->iIsSuspending?"suspending":"normal");
				}
				else {
					//printk("%s():CHGDET=0x%x\n",__func__,reg_val);
					if(0x8 == (reg_val&0x0C)) {
						// charger Detect complete state ...
						 
						#if 0
						if(1==retryCnt) {
							// 第一次中斷進來就charger detect complete . 
							if(0==iForceReDetectTry) {
								printk(KERN_ERR"%s():force redetect charger...\n",__func__);
								iForceReDetectTry=1;// 強制redetcet . 
							}
						}
						#endif

                                               if (0x20 == (reg_val&0x30))     {// DCP detected.
							result = DCP_CHARGER;
							//if(iForceReDetectTry) 
							{
								printk("%s : DCP charger\n",__func__);
							}
						}
						else if (0x10 == (reg_val&0x30)) {// CDP detected. 
							result = CDP_CHARGER;
							//if(iForceReDetectTry) 
							{
								printk("%s : CDP charger\n",__func__);
							}
						}
						else if (0x00 == (reg_val&0x30)) {// SDP detected.
							result = SDP_CHARGER;
							//if(iForceReDetectTry) 
							{
								printk("%s : SDP charger\n",__func__);
							}

						}
						else {
							printk("Other charger (0x%02X)\n", reg_val);

						}

						break;
					}
					
					if( (0x8 != (reg_val&0x0C)) || (NO_CHARGER_PLUGGED == result) ) 
					{
						// detect not completely or charger detecting result is unkown .

						if(reg_val&0x2) {
							// primary detecting result is CDP/DCP . 
							result = CDP_CHARGER;
							printk("%s : primary detection CDP(DCP) charger\n",__func__);
						}

						if (reg_val&0x80) {
							printk("DCD timeout!! (0x%02X)\n",reg_val);
							//break;
							#if 0
							if(iInINT) {
								if(__ricoh61x_write(ricoh61x_i2c_client, EXTIF_GCHGDET_REG,1)<0) {
									printk(KERN_ERR"[INT]Charger redetecting failed !\n");
									break;
								}
							}
							else {
								if(ricoh61x_write(ricoh61x->dev, EXTIF_GCHGDET_REG,1)<0) {
									printk(KERN_ERR"Charger redetecting failed !\n");
									break;
								}
							}
							#endif
						}
						if (reg_val&0x40) {
							printk("Irregular port!! (0x%02X)\n", reg_val);
							break;
						}

						if(ricoh61x->iIsSuspending || iInINT ) {
							mdelay (100);
						}
						else {
							msleep (100);
						}
					}

				}
				if(ricoh61x->iIsSuspending || iInINT ) {
					mdelay (50);
				}
				else {
					msleep (50);
				}
			} while (1);

			if(iForceReDetectTry) {
				printk("%s : re-deteceting charger ...\n",__func__);
				if(iInINT) {
					if(__ricoh61x_write(ricoh61x_i2c_client, EXTIF_GCHGDET_REG,1)<0) {
						printk(KERN_ERR"%s(%d): [INT]Charger redetecting failed !\n",__func__,__LINE__);
					}
					mdelay(10);
				}
				else {
					if(ricoh61x_write(ricoh61x->dev, EXTIF_GCHGDET_REG,1)<0) {
						printk(KERN_ERR"%s(%d): Charger redetecting failed !\n",__func__,__LINE__);
					}
					msleep(10);
				}
			}

		}while(--iForceReDetectTry>=0);

		if( ((SDP_CHARGER==result)||(UNKOWN_CHARGER==result)||CDP_CHARGER==result)  ) {
			if(iCC_ChargerType>1) {
				result = SDP_OVRLIM_CHARGER;
				printk("%s : SDP_OVRLIM charger type detected by CC\n",__func__);
			}

		}


#ifdef CONFIG_ARCH_SUN8IW15P1 //[
		//printk("%s(%d) sunxi udc enable\n",__func__,__LINE__);
		cfg_udc_pullup_command(1);
#if defined(CONFIG_SENSORS_P15USB30216C)
		if(g_usb_cfg.port.port_type==2)
		{
			usb_device_chose();
		}
#endif
#endif //] CONFIG_ARCH_SUN8IW15P1


#if defined(USB_GADGET_DETECT) //[
		printk(KERN_ERR"====%s_%d, Flags:%d====\n",__FUNCTION__,__LINE__,gptHWCFG->m_val.bTestFlags);
		if( gptHWCFG->m_val.bTestFlags & 0x02)	// MFG MODE
		{
			// Skip to detect usb
			printk(KERN_ERR"[SKIP USB DETECT]\n");
		}
		else{
			if((UNKOWN_CHARGER==result)||(SDP_CHARGER==result)) 
			{ 
				int iChk = check_usb_state_by_ep(3000) ;

				if(1==iChk) {
					result = SDP_CHARGER;
				}
				else if(0==iChk){
					result = DCP_CHARGER;
				}
				printk(KERN_DEBUG"[ep0 detected] %s_%d , result:%d \n",__FUNCTION__,__LINE__,result);
			}
		}
#endif //] 



	}
	else {
#ifdef CONFIG_ARCH_SUN8IW15P1 //[
               //printk("%s(%d) sunxi udc triggered\n",__func__,__LINE__);
               cfg_udc_pullup_command(0);
               cfg_udc_pullup_command(1);
#endif //] CONFIG_ARCH_SUN8IW15P1
		printk("%s() : usb removed (0x%x)!\n",__func__,reg_val);
	}



	ricoh61x->iIsChargerDetecting = 0;
	return result;
}
int ricoh619_charger_detect(void)
{
	return ricoh619_charger_detect_ex(0);
}

static void (*b300_pm_restart)(enum reboot_mode reboot_mode, const char *cmd);
#define SUNXI_RTC_BASE                      (0x07000000L)
#define  RTC_DATA_HOLD_REG_BASE        (SUNXI_RTC_BASE + 0x100)
#define  SUNXI_RTC_GPREG_NUM 6

extern void ricoh619_save_to_bank1 (struct device *dev, unsigned long hour);
extern void ricoh619_save_to_bank1_ex (struct device *dev, unsigned long hour,int is_safe_op);

static void print_bank1_regs(const char *header, struct seq_file *s,
		struct i2c_client *client, int start_offset,int end_offset);


/* debug */
#define SUNXI_DEBUG_MODE_FLAG           (0x59)
/* efex */
#define SUNXI_EFEX_CMD_FLAG             (0x5A)
/* boot-resignature */
#define SUNXI_BOOT_RESIGNATURE_FLAG     (0x5B)
/* recovery or boot-recovery */
#define SUNXI_BOOT_RECOVERY_FLAG        (0x5C)
/* sysrecovery */
#define SUNXI_SYS_RECOVERY_FLAG         (0x5D)
/* usb-recovery*/
#define SUNXI_USB_RECOVERY_FLAG         (0x5E)
/* bootloader */
#define SUNXI_FASTBOOT_FLAG             (0x5F)


int _parse_sunxi_boot_cmd(const char *pszBootCmd,unsigned char *O_pbSunxiBootFlag)
{
	int iRet;
	unsigned char bSunxiBootFlag = 0;

	if(!pszBootCmd) {
		return -1;
	}

	if (!strncmp(pszBootCmd, "debug", sizeof("debug"))) {
		bSunxiBootFlag |= SUNXI_DEBUG_MODE_FLAG;
	} else if (!strncmp(pszBootCmd, "efex", sizeof("efex"))) {
		bSunxiBootFlag |= SUNXI_EFEX_CMD_FLAG;
	} else if (!strncmp(pszBootCmd, "boot-resignature",
						sizeof("boot-resignature"))) {
		bSunxiBootFlag |= SUNXI_BOOT_RESIGNATURE_FLAG;
	} else if (!strncmp(pszBootCmd, "recovery", sizeof("recovery"))
		|| !strncmp(pszBootCmd, "boot-recovery", sizeof("boot-recovery"))) {
		bSunxiBootFlag |= SUNXI_BOOT_RECOVERY_FLAG;
	} else if (!strncmp(pszBootCmd, "sysrecovery", sizeof("sysrecovery"))) {
		bSunxiBootFlag |= SUNXI_SYS_RECOVERY_FLAG;
	} else if (!strncmp(pszBootCmd, "bootloader", sizeof("bootloader"))) {
		bSunxiBootFlag |= SUNXI_FASTBOOT_FLAG;
	} else if (!strncmp(pszBootCmd, "usb-recovery", sizeof("usb-recovery"))) {
		bSunxiBootFlag |= SUNXI_USB_RECOVERY_FLAG;
	} else {
		return -1;
	}
	if(O_pbSunxiBootFlag) {
		*O_pbSunxiBootFlag = bSunxiBootFlag;
	}
	return 0;
}


void ricoh619_restart(enum reboot_mode reboot_mode, const char *cmd)
{
	int ret;
	uint8_t reg_val;
	unsigned long val=0xFFFFFF00; // for B300 . 
	

	printk ("[%s-%d] cmd=%s\n", __func__, __LINE__,cmd);

	mutex_lock(&g_ricoh61x->io_lock);

	/* Disable all Interrupt */
	__ricoh61x_write(ricoh61x_i2c_client, RICOH61x_INTC_INTEN, 0);

	/* Disable rtc Interrupt */
	ret = __ricoh61x_read(ricoh61x_i2c_client,rtc_ctrl1, &reg_val);
	reg_val &= 0xBF;	// clear DALE flag
	__ricoh61x_write(ricoh61x_i2c_client, rtc_ctrl1, reg_val);

	 
#ifdef SUNXI_BOOTFLAG_BY_RICOH //[
	{ // for B300 .
		uint8_t bSunxiBootFlag;

		if(_parse_sunxi_boot_cmd(cmd,&bSunxiBootFlag)<0) {
			//pr_warn("unkown reboot arg %s", (char *)cmd);
		}
		else {
			val |= bSunxiBootFlag;
		}
		ricoh619_save_to_bank1_ex (g_ricoh61x->dev, val,1);
		set_bank_ricoh61x(g_ricoh61x->dev, 0);
	}
#endif //] SUNXI_BOOTFLAG_BY_RICOH


	if(__ricoh61x_read(ricoh61x_i2c_client, RICOH61x_PSWR, &reg_val)>=0) {
			reg_val |= 0x80;// RRESET=1 (RSTB to ERSTB , RSTB registers will not reset)
			__ricoh61x_write(ricoh61x_i2c_client, RICOH61x_PSWR, reg_val);
	}
	else {
		pr_warn("%s(): PSWR read failed !!",__func__);
	}

	if(__ricoh61x_read(ricoh61x_i2c_client, RICOH61x_PWR_NOE_TIMSET, &reg_val)>=0) {
			pr_info("%s(): ONE_TIMSET=0x%02x\n",__func__,reg_val);
			reg_val &=~0x3; // clear OFF_PRESS_NOE fields value . 
			reg_val |= 0x3;// OFF_PRESS_NOE set to 3 secs .
			__ricoh61x_write(ricoh61x_i2c_client, RICOH61x_PWR_NOE_TIMSET, reg_val);
	}
	else {
		pr_warn("%s(): N_OE_TIMESET read failed !!",__func__);
	}

	mutex_unlock(&g_ricoh61x->io_lock);

#ifdef SUNXI_BOOTFLAG_BY_RICOH //[
	//if (!ricoh61x_i2c_client) 
	{
		void __iomem *base;


		//printk ("[%s-%d] \n", __func__, __LINE__);
		//print_bank1_regs("Bank1 FUEL Regs",		0, g_ricoh61x->client, 0xB2, 0xBF);
		
#if (SUNXI_BOOTFLAG_BY_RICOH==2) //[ b300 internal reset . 
		if(0==cmd || (0==*cmd) || 0==strcmp("efex",cmd) ) 
		{
			uint8_t reg,reg2;

			mutex_lock(&g_ricoh61x->io_lock);
			if(__ricoh61x_read(ricoh61x_i2c_client, RICOH61x_PWR_WD,&reg) >=0 ) {
				__ricoh61x_read(ricoh61x_i2c_client, RICOH61x_INT_EN_SYS,&reg2);
				printk ("[%s-%d] watchdog setting =0x%02x,int_en=0x%02x\n",
					 	__func__, __LINE__,reg,reg2);
				reg &= ~0x3;
				reg |= 0xd; // 8 second watchdog enable .
				if(__ricoh61x_write(ricoh61x_i2c_client, RICOH61x_PWR_WD, reg) <0) {
					printk(KERN_ERR"watchdog register 0x%x writting 8secs failed \n",RICOH61x_PWR_WD);
				}
			}
			else {
				printk(KERN_ERR"watchdog register 0x%x reading failed \n",RICOH61x_PWR_WD);
			}
			mutex_unlock(&g_ricoh61x->io_lock);

			
			base = devm_ioremap(g_ricoh61x->dev, RTC_DATA_HOLD_REG_BASE, 4096);
			/*write the data to reg*/
			writel((val&0xFF), base + SUNXI_RTC_GPREG_NUM*0x4);
			printk("write reboot flags 0x%x@%p\n",(val&0xFF), base + SUNXI_RTC_GPREG_NUM*0x4);
			b300_pm_restart (reboot_mode, cmd);
		}
#endif //](SUNXI_BOOTFLAG_BY_RICOH==2)


	}
#else //][!SUNXI_BOOTFLAG_BY_RICOH 
	if (!ricoh61x_i2c_client || (cmd && (0 != *cmd))) 
	{
		b300_pm_restart (reboot_mode, cmd);
	}
#endif //] SUNXI_BOOTFLAG_BY_RICOH

	
	while (1) {
		printk ("[%s-%d] restarting ...\n", __func__, __LINE__);

		mutex_lock(&g_ricoh61x->io_lock);
		printk ("[%s-%d] ricoh restart ...\n", __func__, __LINE__);
		/* Repeat power ON after power off(Power Off/N_OE) */
		__ricoh61x_write(ricoh61x_i2c_client, RICOH61x_PWR_REP_CNT, 0x37);//1000ms off time .
		/* Power OFF */
		__ricoh61x_write(ricoh61x_i2c_client, RICOH61x_PWR_SLP_CNT, 0x1);
		mutex_unlock(&g_ricoh61x->io_lock);

		mdelay(1000);
	}
}

static int ricoh619_reboot_callback(struct notifier_block *this,
		unsigned long code, void *data)
{
	int loop_counter = 0;
	bool canWriteFlag = true;
	unsigned long val=0xFFFFFF00;
	void __iomem *base;
	unsigned char bSunxiBootFlag;

	printk("ricoh619 reboot, arg %s\n", (char *)data);

	if (data == NULL)
		return NOTIFY_DONE;


	if(_parse_sunxi_boot_cmd((const char *)data,&bSunxiBootFlag)<0) {
		pr_warn("unkown reboot arg %s", (char *)data);
	}
	else {
		val |= bSunxiBootFlag;
	}

	base = devm_ioremap(g_ricoh61x->dev, RTC_DATA_HOLD_REG_BASE, 4096);
	/*write the data to reg*/
	writel((val&0xFF), base + SUNXI_RTC_GPREG_NUM*0x4);
	printk("write reboot flags 0x%x@%p\n",(val&0xFF), base + SUNXI_RTC_GPREG_NUM*0x4);

	ricoh619_save_to_bank1 (g_ricoh61x->dev, val);
	set_bank_ricoh61x(g_ricoh61x->dev, 0);

	//print_bank1_regs("FUEL  Regs",		0, g_ricoh61x->client, 0xB2, 0xBF);
	
	msleep(1000);
	return NOTIFY_DONE;
}

static struct notifier_block ricoh619_reboot_notifier = {
	.notifier_call = ricoh619_reboot_callback,
};

static int ricoh61x_gpio_get(struct gpio_chip *gc, unsigned offset)
{
	struct ricoh61x *ricoh61x = container_of(gc, struct ricoh61x,
								 gpio_chip);
	uint8_t val;
	int ret;

	ret = ricoh61x_read(ricoh61x->dev, RICOH61x_GPIO_MON_IOIN, &val);
	if (ret < 0)
		return ret;

	return ((val & (0x1 << offset)) != 0);
}

static void ricoh61x_gpio_set(struct gpio_chip *gc, unsigned offset,
			int value)
{
	struct ricoh61x *ricoh61x = container_of(gc, struct ricoh61x,
								 gpio_chip);
	if (value)
		ricoh61x_set_bits(ricoh61x->dev, RICOH61x_GPIO_IOOUT,
						1 << offset);
	else
		ricoh61x_clr_bits(ricoh61x->dev, RICOH61x_GPIO_IOOUT,
						1 << offset);
}

static int ricoh61x_gpio_input(struct gpio_chip *gc, unsigned offset)
{
	struct ricoh61x *ricoh61x = container_of(gc, struct ricoh61x,
								 gpio_chip);

	return ricoh61x_clr_bits(ricoh61x->dev, RICOH61x_GPIO_IOSEL,
						1 << offset);
}

static int ricoh61x_gpio_output(struct gpio_chip *gc, unsigned offset,
				int value)
{
	struct ricoh61x *ricoh61x = container_of(gc, struct ricoh61x,
								 gpio_chip);

	ricoh61x_gpio_set(gc, offset, value);
	return ricoh61x_set_bits(ricoh61x->dev, RICOH61x_GPIO_IOSEL,
						1 << offset);
}

static int ricoh61x_gpio_to_irq(struct gpio_chip *gc, unsigned off)
{
	struct ricoh61x *ricoh61x = container_of(gc, struct ricoh61x,
								 gpio_chip);

	if ((off >= 0) && (off < 8))
		return ricoh61x->irq_base + RICOH61x_IRQ_GPIO0 + off;

	return -EIO;
}

#if 0
static void ricoh61x_gpio_init(struct ricoh61x *ricoh61x,
	struct ricoh619_platform_data *pdata)
{
	int ret;
	int i;
	struct ricoh619_gpio_init_data *ginit;

	if (pdata->gpio_base  <= 0)
		return;

	for (i = 0; i < pdata->num_gpioinit_data; ++i) {
		ginit = &pdata->gpio_init_data[i];

		if (!ginit->init_apply)
			continue;

		if (ginit->output_mode_en) {
			/* GPIO output mode */
			if (ginit->output_val)
				/* output H */
				ret = ricoh61x_set_bits(ricoh61x->dev,
					RICOH61x_GPIO_IOOUT, 1 << i);
			else
				/* output L */
				ret = ricoh61x_clr_bits(ricoh61x->dev,
					RICOH61x_GPIO_IOOUT, 1 << i);
			if (!ret)
				ret = ricoh61x_set_bits(ricoh61x->dev,
					RICOH61x_GPIO_IOSEL, 1 << i);
		} else
			/* GPIO input mode */
			ret = ricoh61x_clr_bits(ricoh61x->dev,
					RICOH61x_GPIO_IOSEL, 1 << i);

		/* if LED function enabled in OTP */
		if (ginit->led_mode) {
			/* LED Mode 1 */
			if (i == 0)	/* GP0 */
				ret = ricoh61x_set_bits(ricoh61x->dev,
					 RICOH61x_GPIO_LED_FUNC,
					 0x04 | (ginit->led_func & 0x03));
			if (i == 1)	/* GP1 */
				ret = ricoh61x_set_bits(ricoh61x->dev,
					 RICOH61x_GPIO_LED_FUNC,
					 0x40 | (ginit->led_func & 0x03) << 4);

		}


		if (ret < 0)
			dev_err(ricoh61x->dev, "Gpio %d init "
				"dir configuration failed: %d\n", i, ret);

	}

	ricoh61x->gpio_chip.owner		= THIS_MODULE;
	ricoh61x->gpio_chip.label		= ricoh61x->client->name;
	ricoh61x->gpio_chip.dev			= ricoh61x->dev;
	ricoh61x->gpio_chip.base		= pdata->gpio_base;
	ricoh61x->gpio_chip.ngpio		= RICOH61x_NR_GPIO;
	ricoh61x->gpio_chip.can_sleep	= 1;

	ricoh61x->gpio_chip.direction_input	= ricoh61x_gpio_input;
	ricoh61x->gpio_chip.direction_output	= ricoh61x_gpio_output;
	ricoh61x->gpio_chip.set			= ricoh61x_gpio_set;
	ricoh61x->gpio_chip.get			= ricoh61x_gpio_get;
	ricoh61x->gpio_chip.to_irq		= ricoh61x_gpio_to_irq;

	ret = gpiochip_add(&ricoh61x->gpio_chip);
	if (ret)
		dev_warn(ricoh61x->dev, "GPIO registration failed: %d\n", ret);

#if 0 //[
	// initial ricoh watchdog
	ret = ricoh61x_write(ricoh61x->dev, RICOH61x_PWR_WD, 0x06);
	ret = ricoh61x_set_bits(ricoh61x->dev, RICOH61x_INT_EN_SYS, 0x40);
#endif //]

	// set vindac
	ret = ricoh61x_write(ricoh61x->dev, 0x03, 0x00);//set VINDAC 3.0V . 
}
#endif

static int ricoh61x_remove_subdev(struct device *dev, void *unused)
{
	platform_device_unregister(to_platform_device(dev));
	return 0;
}

static int ricoh61x_remove_subdevs(struct ricoh61x *ricoh61x)
{
	return device_for_each_child(ricoh61x->dev, NULL,
				     ricoh61x_remove_subdev);
}

static int ricoh61x_add_subdevs(struct ricoh61x *ricoh61x,
				struct ricoh619_platform_data *pdata)
{
	struct ricoh619_subdev_info *subdev;
	struct platform_device *pdev;
	int i, ret = 0;

	for (i = 0; i < pdata->num_subdevs; i++) {
		subdev = &pdata->subdevs[i];

		pdev = platform_device_alloc(subdev->name, subdev->id);

		pdev->dev.parent = ricoh61x->dev;
		pdev->dev.platform_data = subdev->platform_data;

		ret = platform_device_add(pdev);
		if (ret)
			goto failed;
	}
	return 0;

failed:
	ricoh61x_remove_subdevs(ricoh61x);
	return ret;
}

#ifdef CONFIG_DEBUG_FS
#include <linux/debugfs.h>
#include <linux/seq_file.h>
static void print_regs(const char *header, struct seq_file *s,
		struct i2c_client *client, int start_offset,
		int end_offset)
{
	uint8_t reg_val;
	int i;
	int ret;

	if(s) {
		seq_printf(s, "%s\n", header);
	}
	else {
		printk("%s\n", header);
	}
	for (i = start_offset; i <= end_offset; ++i) {
		ret = __ricoh61x_read(client, i, &reg_val);
		if (ret >= 0) {
			if(s) {
				seq_printf(s, "Reg 0x%02x Value 0x%02x\n", i, reg_val);
			}
			else {
				printk("Reg 0x%02x Value 0x%02x\n", i, reg_val);
			}
		}
	}
	if(s) {
		seq_printf(s, "------------------\n");
	}
	else {
		printk("------------------\n");
	}
}

static void print_bank1_regs(const char *header, struct seq_file *s,
		struct i2c_client *client, int start_offset,
		int end_offset)
{
	uint8_t reg_val;
	int i;
	int ret;
	if(s) {
		seq_printf(s, "%s\n",header);
	}
	else {
		printk("%s\n",header);
	}

	ret = set_bank_ricoh61x(&client->dev, 1);
	if(s) {
		seq_printf(s, "bank1\n");
	}
	else {
		printk("bank1\n");
	}
	for (i = start_offset; i <= end_offset; ++i) {
		ret = __ricoh61x_read(client, i, &reg_val);
		if (ret >= 0) {
			if(s) {
				seq_printf(s, "Reg 0x%02x Value 0x%02x\n", i, reg_val);
			}
			else {
				printk("Reg 0x%02x Value 0x%02x\n",i,reg_val);
			}
		}
	}
	if(s) {
		seq_printf(s, "------------------\n");
	}
	else {
		printk("------------------\n");
	}
	ret = set_bank_ricoh61x(&client->dev, 0);
}

int dbg_ricoh_show(struct seq_file *s, void *unused)
{
	struct ricoh61x *ricoh = 0;
	struct i2c_client *client = 0;

	if(s) {
		ricoh = s->private;
		client = ricoh->client;
		seq_printf(s, "RICOH61x Registers\n");
		seq_printf(s, "------------------\n");
	}
	else {
		ricoh = g_ricoh61x;
		client = ricoh->client;
		printk("RICOH61x Registers\n");
		printk("------------------\n");
	}

	print_regs("System Regs",		s, client, 0x0, 0x05);
	print_regs("Power Control Regs",	s, client, 0x07, 0x2B);
	print_regs("DCDC  Regs",		s, client, 0x2C, 0x43);
	print_regs("LDO   Regs",		s, client, 0x44, 0x61);
	print_regs("ADC   Regs",		s, client, 0x64, 0x8F);
	print_regs("GPIO  Regs",		s, client, 0x90, 0x98);
	print_regs("INTC  Regs",		s, client, 0x9C, 0x9E);
	print_regs("RTC   Regs",		s, client, 0xA0, 0xAF);
	print_regs("OPT   Regs",		s, client, 0xB0, 0xB1);
	print_regs("CHG   Regs",		s, client, 0xB2, 0xDF);
	print_regs("FUEL  Regs",		s, client, 0xE0, 0xFC);
	print_bank1_regs("FUEL  Regs",		s, client, 0xB2, 0xFF);
	return 0;
}

static int dbg_ricoh_open(struct inode *inode, struct file *file)
{
	return single_open(file, dbg_ricoh_show, inode->i_private);
}

static const struct file_operations debug_fops = {
	.open		= dbg_ricoh_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};
static void __init ricoh61x_debuginit(struct ricoh61x *ricoh)
{
	(void)debugfs_create_file("ricoh61x", S_IRUGO, NULL,
			ricoh, &debug_fops);
}
#else
static void print_regs(const char *header, struct i2c_client *client,
		int start_offset, int end_offset)
{
	uint8_t reg_val;
	int i;
	int ret;

	printk(KERN_INFO "%s\n", header);
	for (i = start_offset; i <= end_offset; ++i) {
		ret = __ricoh61x_read(client, i, &reg_val);
		if (ret >= 0)
			printk(KERN_INFO "Reg 0x%02x Value 0x%02x\n",
							 i, reg_val);
	}
	printk(KERN_INFO "------------------\n");
}
static void __init ricoh61x_debuginit(struct ricoh61x *ricoh)
{
	struct i2c_client *client = ricoh->client;

	printk(KERN_INFO "RICOH61x Registers\n");
	printk(KERN_INFO "------------------\n");

	print_regs("System Regs",		client, 0x0, 0x05);
	print_regs("Power Control Regs",	client, 0x07, 0x2B);
	print_regs("DCDC  Regs",		client, 0x2C, 0x43);
	print_regs("LDO   Regs",		client, 0x44, 0x5C);
	print_regs("ADC   Regs",		client, 0x64, 0x8F);
	print_regs("GPIO  Regs",		client, 0x90, 0x9B);
	print_regs("INTC  Regs",		client, 0x9C, 0x9E);
	print_regs("OPT   Regs",		client, 0xB0, 0xB1);
	print_regs("CHG   Regs",		client, 0xB2, 0xDF);
	print_regs("FUEL  Regs",		client, 0xE0, 0xFC);

	return 0;
}
#endif

#ifdef CONFIG_OF
extern int enable_gpio_wakeup_src(int para);
extern int disable_gpio_wakeup_src(int para);
static struct ricoh619_platform_data *ricoh61x_parse_dt(struct ricoh61x *ricoh61x)
{
	struct ricoh619_platform_data *pdata;
	struct device_node *ricoh61x_pmic_np;
	int chg_gpio, bat_low_gpio, pwr_key_gpio;

	if (!ricoh61x->dev->of_node) 
		ricoh61x_pmic_np = of_find_node_by_name(NULL, "ricoh619-i2c");
	else 
		ricoh61x_pmic_np = of_node_get(ricoh61x->dev->of_node);

	if (!ricoh61x_pmic_np) {
		printk("could not find pmic sub-node\n");
		return NULL;
	}

	pdata = devm_kzalloc(ricoh61x->dev, sizeof(*pdata), GFP_KERNEL);
	if (!pdata)
		return NULL;

	pdata->irq_gpio = of_get_named_gpio_flags(ricoh61x_pmic_np, "ricoh619_pmu_int", 0, 0);
		if (!gpio_is_valid(pdata->irq_gpio)) {
			printk("invalid gpio: %d\n",  pdata->irq_gpio);
			return NULL;
		}
		else
			printk ("ricoh61x irq gpio %d\n",pdata->irq_gpio);

	pdata->pmic_sleep_gpio = of_get_named_gpio_flags(ricoh61x_pmic_np, "ricoh619_pmu_sleep", 0, 0);
	if (!gpio_is_valid(pdata->pmic_sleep_gpio)) {
		printk("invalid sleep gpio: %d\n",  pdata->pmic_sleep_gpio);
	}

	pdata->dc_det = of_get_named_gpio_flags(ricoh61x_pmic_np, "ricoh619_pmu_dc_det", 0, 0);
			if (!gpio_is_valid(pdata->dc_det)) {
					printk("invalid dc_det gpio: %d\n",  pdata->dc_det);
			}

	chg_gpio = of_get_named_gpio_flags(ricoh61x_pmic_np, "ricoh619_pmu_chg", 0, 0);
			if (gpio_is_valid(chg_gpio)) {
				printk ("ricoh61x chg gpio %d\n",chg_gpio);
				if (gpio_request(chg_gpio, "ricoh619_chg") < 0) {
					dev_err(ricoh61x->dev,"Failed to request gpio %d \n", chg_gpio);
					return IRQ_NONE;
				}
				gpio_direction_input(chg_gpio);
			}
	bat_low_gpio = of_get_named_gpio_flags(ricoh61x_pmic_np, "ricoh619_pmu_bat_low", 0, 0);
			if (gpio_is_valid(bat_low_gpio)) {
				printk ("ricoh61x bat_low gpio %d\n",bat_low_gpio);
				if (gpio_request(bat_low_gpio, "ricoh619_bat_low") < 0) {
					dev_err(ricoh61x->dev,"Failed to request gpio %d \n", bat_low_gpio);
					return IRQ_NONE;
				}
				gpio_direction_input(bat_low_gpio);
			}
	pwr_key_gpio = of_get_named_gpio_flags(ricoh61x_pmic_np, "ricoh619_pmu_pwr_key", 0, 0);
			if (gpio_is_valid(pwr_key_gpio)) {
				printk ("ricoh61x power key gpio %d (%d)\n",pwr_key_gpio, GPIOL(4));
				if (gpio_request(pwr_key_gpio, "ricoh619_pwr_key") < 0) {
					dev_err(ricoh61x->dev,"Failed to request gpio %d \n", pwr_key_gpio);
					return IRQ_NONE;
				}
				gpio_direction_input(pwr_key_gpio);
			}

	pdata->pmic_sleep = true;
	
	pdata->pm_off = of_property_read_bool(ricoh61x_pmic_np,"ricoh619,system-power-controller");
		
	return pdata;
}

#else
static struct ricoh619_platform_data *ricoh61x_parse_dt(struct ricoh61x *ricoh61x)
{
	return NULL;
}
#endif

static void ricoh61x_noe_init(struct ricoh61x *ricoh)
{
	struct i2c_client *client = ricoh->client;

//	__ricoh61x_write(client, RICOH61x_PWR_NOE_TIMSET, 0x8); // disable N_OE

#if 0 	// do not set repeat power on bit to enable force power off by long press power key function.
	/* Repeat power ON after reset (Power Off/N_OE) */
	__ricoh61x_write(client, RICOH61x_PWR_REP_CNT, 0x1);
#endif
}

static int ricoh61x_pre_init(struct ricoh61x *ricoh61x)
{
	int ret=0;
	 printk("%s,line=%d\n", __func__,__LINE__);
	 /*
	ret = ricoh61x_read(ricoh61x->dev,0x09,&val);
	printk("%s,line=%d ricoh61x power on his %08x\n", __func__,__LINE__,val);
	ret = ricoh61x_read(ricoh61x->dev,0x0a,&val);
	printk("%s,line=%d ricoh61x power off his %08x\n", __func__,__LINE__,val);
	*/
	ricoh61x_set_bits(ricoh61x->dev, 0xae, (0x1 <<6));//enable alam_d
	ricoh61x_write(ricoh61x->dev, 0x2f, 0x43);//slove ripple
//	ricoh61x_write(ricoh61x->dev, 0x05, 0x07);//enable clkout2
	
	ricoh61x_noe_init(ricoh61x);
	/***************set PKEY long press time 0sec*******/
	/* power on/off timer setting to on 1s, off 8S */
	ricoh61x_write(ricoh61x->dev, RICOH61x_PWR_ON_TIMSET, 0x5b);
	//ret = ricoh61x_set_bits(ricoh61x->dev,RICOH61x_PWR_ON_TIMSET,(0x1 <<7));
	//ret = ricoh61x_clr_bits(ricoh61x->dev,RICOH61x_PWR_ON_TIMSET,(0x1 <<3));
	//ret = ricoh61x_clr_bits(ricoh61x->dev,RICOH61x_PWR_ON_TIMSET,(0x1 <<7));
 	/**********************************************/
	ret = ricoh61x_set_bits(ricoh61x->dev,BATSET2_REG,(3 << 0)); 
	ret = ricoh61x_clr_bits(ricoh61x->dev,BATSET2_REG,(1 << 2)); //set vrchg 4v

	
	return ret;
}

static int ricoh61x_i2c_probe(struct i2c_client *client,
			      const struct i2c_device_id *id)
{
	struct device_node *node;
	struct device *device;
	struct ricoh61x *ricoh61x;
	struct ricoh619_platform_data *pdata;
	int ret;
	uint8_t control;
	int i=0;
	printk(KERN_INFO "PMU: %s:\n", __func__);

	node = client->dev.of_node;
	device = &client->dev;
	pdata = client->dev.platform_data;
	ricoh61x = kzalloc(sizeof(struct ricoh61x), GFP_KERNEL);
	if (ricoh61x == NULL)
		return -ENOMEM;

	g_ricoh61x = ricoh61x;
	ricoh61x->client = client;
	ricoh61x->dev = &client->dev;
	i2c_set_clientdata(client, ricoh61x);

	mutex_init(&ricoh61x->io_lock);

//	ricoh61x->regmap = axp_regmap_init_i2c(device);

	ret = ricoh61x_read(ricoh61x->dev, 0x36, &control);
	if ((ret <0) || (control < 0) || (control == 0xff) || (control == 0) ){
		if (ret <0){
			printk(KERN_INFO "The device is not ricoh619 %08x %d\n",control,ret);
			goto err_irq_init;
		}
		else{
			do{
				ret = ricoh61x_write(ricoh61x->dev, 0xff, 0x00);
				ret = ricoh61x_read(ricoh61x->dev, 0x36, &control);
				i += 1;
				printk(KERN_INFO "##################:read ricoh619 0x36 error retry %08x %d\n",control,ret);
			}while( ((control == 0xff) || (control == 0) ) && (i < 10));
			if ((control == 0xff) || (control == 0) ){
				ret = -ENXIO;	
				printk(KERN_INFO "##################The device is not ricoh619 %08x %d\n",control,ret);
				goto err_irq_init;
			}
		}
	}

	{
		uint8_t reg_val;
		if(ricoh61x_read(ricoh61x->dev, RICOH61x_PSWR, &reg_val)>=0) {
			reg_val &= ~0x80;// RRESET=0 (default)
			ricoh61x_write(ricoh61x->dev, RICOH61x_PSWR, reg_val);
		}
	}

	ret = ricoh61x_pre_init(ricoh61x);
	if (ret < 0){
		printk("The ricoh61x_pre_init failed %d\n",ret);
		goto err_irq_init;
	}

	ricoh61x->bank_num = 0;

	pdata = ricoh61x_parse_dt(ricoh61x);
	if (!pdata)
		printk(KERN_INFO "################## Error ricoh61x_parse_dt\n");
	
	client->dev.platform_data = pdata;
	if (pdata->dc_det) 
		ricoh61x->dc_det = pdata->dc_det;
	
	/******************************set sleep vol & dcdc mode******************/
	#ifdef CONFIG_OF
	if (gpio_is_valid(pdata->pmic_sleep_gpio)) {
			ret = gpio_request(pdata->pmic_sleep_gpio, "ricoh619_pmic_sleep");
			if (ret < 0) {
				dev_err(ricoh61x->dev,"Failed to request gpio %d with ret:""%d\n",	pdata->pmic_sleep_gpio, ret);
				return IRQ_NONE;
			}
			gpio_direction_output(pdata->pmic_sleep_gpio,0);
			ret = gpio_get_value(pdata->pmic_sleep_gpio);
//			gpio_free(pdata->pmic_sleep_gpio);
			pr_info("%s: ricoh619_pmic_sleep=%x\n", __func__, ret);
	}	
	#endif
	/**********************************************************/
	ret = ricoh61x_irq_init(ricoh61x, pdata->irq_gpio, pdata);
	if (ret < 0)
		goto err_irq_init;
	
	ret = mfd_add_devices(ricoh61x->dev, -1,
			     ricoh61xs, ARRAY_SIZE(ricoh61xs),
			      NULL, 0,NULL);

	if (pdata->init_port) {
		/* For init PMIC_IRQ port */
		ret = pdata->init_port(client->irq);
	}

	if (client->irq) {
		ret = ricoh61x_irq_init(ricoh61x, client->irq, pdata->irq_base);
		if (ret) {
			dev_err(&client->dev, "IRQ init failed: %d\n", ret);
			goto err_irq_init;
		}
	}

	ricoh61x_debuginit(ricoh61x);

	ricoh61x_i2c_client = client;

	printk ("[%s-%d] pm_power_off %lx, arm_pm_restart %lx\n", __func__, __LINE__, pm_power_off, arm_pm_restart);

	pm_power_off = ricoh619_power_off;
	b300_pm_restart = arm_pm_restart;
	arm_pm_restart = ricoh619_restart;
#ifdef SUNXI_BOOTFLAG_BY_RICOH //[
#else //][!SUNXI_BOOTFLAG_BY_RICOH 
	register_reboot_notifier(&ricoh619_reboot_notifier);
#endif //] SUNXI_BOOTFLAG_BY_RICOH

	return 0;

err_add_devs:
	if (client->irq)
		ricoh61x_irq_exit(ricoh61x);
err_irq_init:
	kfree(ricoh61x);
	return ret;
}

static int ricoh61x_i2c_remove(struct i2c_client *client)
{
	struct ricoh61x *ricoh61x = i2c_get_clientdata(client);

	if (client->irq)
		ricoh61x_irq_exit(ricoh61x);

	ricoh61x_remove_subdevs(ricoh61x);
	kfree(ricoh61x);
	return 0;
}

extern volatile int giRicoh61x_rtc_user_enabled ;
extern int gSleep_Mode_Suspend;
#ifdef CONFIG_PM
static int ricoh61x_i2c_suspend(struct i2c_client *client, pm_message_t state)
{
	struct ricoh61x *ricoh61x = i2c_get_clientdata(client);
	uint8_t reg_val;
	int ret;
	struct ricoh619_platform_data *pdata=client->dev.platform_data;

	//printk ("[%s-%d] ...\n", __func__, __LINE__);

	if(ricoh61x->iIsChargerDetecting) {
		printk ("[%s-%d] skipped for charger detecting\n", __func__, __LINE__);
		return -1;
	}

	enable_gpio_wakeup_src(GPIOL(7));		// enable PMU_INT wakeup

	enable_gpio_wakeup_src(GPIOL(4));		// enable PWR_key wakeup
//	enable_gpio_wakeup_src(GPIOL(5));		// enable BAT_LOW_INT wakeup
//	enable_gpio_wakeup_src(GPIOL(6));		// enable CHG_INT wakeup
//	enable_gpio_wakeup_src(GPIOL(9));		// enable HALL wakeup

	ricoh61x->iIsSuspending = 1;

  /* Disable rtc Interrupt */
	ret = __ricoh61x_read(ricoh61x_i2c_client,
				      rtc_ctrl1, &reg_val);

	if(giRicoh61x_rtc_user_enabled) {
		reg_val |= 0x40;	// set DALE flag
    	__ricoh61x_write(ricoh61x_i2c_client, rtc_ctrl1, reg_val);
	}

	ret = __ricoh61x_read(client, RICOH61x_INT_IR_SYS, &reg_val);
	if (reg_val & 0x01) { /* If PWR_KEY wakeup */
		/* Clear PWR_KEY IRQ */
		__ricoh61x_write(client, RICOH61x_INT_IR_SYS, reg_val & 0xFE);
	}

	if(ricoh61x->iIsWatchdogEnabled)
	{
		u8 int_en_sys,pwr_wdog;

		ricoh61x_read(ricoh61x->dev,RICOH61x_PWR_WD, &pwr_wdog);
		pwr_wdog &= ~0xc ;// WDOGSLPEN , WDOGEN set 0 . 
		if( ricoh61x_write(ricoh61x->dev,RICOH61x_PWR_WD,pwr_wdog ) <0) {
			printk(KERN_ERR"%s():WDOG disable failed !\n", __func__);
		}
		
		ricoh61x_read(ricoh61x->dev,RICOH61x_INT_EN_SYS, &int_en_sys);
		//printk(KERN_INFO"%s():IN_TEN_SYS=0x%x\n",__func__,int_en_sys);
		int_en_sys &= ~0x40;
		if( ricoh61x_write(ricoh61x->dev,RICOH61x_INT_EN_SYS,int_en_sys ) <0) {
			printk(KERN_ERR"%s():WDOG int disable failed !\n", __func__);
		}
		
	}


#if 0
	printk(KERN_INFO "PMU: %s:\n", __func__);
	if (client->irq)
		disable_irq(client->irq);
#endif
	return 0;
}

int pwrkey_wakeup;
static int ricoh61x_i2c_resume(struct i2c_client *client)
{
	struct ricoh61x *ricoh61x = i2c_get_clientdata(client);
	uint8_t reg_val,reg_val2;
	int ret;
	int i=1;


	if(ricoh61x->iIsWatchdogEnabled)
	{
		u8 int_en_sys,pwr_wdog;
		ricoh61x_read(ricoh61x->dev,RICOH61x_INT_EN_SYS, &int_en_sys);
		//printk(KERN_INFO"%s():IN_TEN_SYS=0x%x\n",__func__,int_en_sys);
		int_en_sys |= 0x40;
		if( ricoh61x_write(ricoh61x->dev,RICOH61x_INT_EN_SYS,int_en_sys ) <0) {
			printk(KERN_ERR"%s():WDOG int enable failed !\n", __func__);
		}

		
		ricoh61x_read(ricoh61x->dev,RICOH61x_PWR_WD, &pwr_wdog);
		pwr_wdog |= 0xc ;// WDOGSLPEN , WDOGEN set 0 . 
		if( ricoh61x_write(ricoh61x->dev,RICOH61x_PWR_WD,pwr_wdog ) <0) {
			printk(KERN_ERR"%s():WDOG enable failed !\n", __func__);
		}
		
	}


//	printk(KERN_INFO "PMU: %s:\n", __func__);
	
	ret = __ricoh61x_read(client, RICOH61x_INT_IR_SYS, &reg_val);
	if (reg_val & 0x01) { /* If PWR_KEY wakeup */
		printk(KERN_INFO "PMU: %s: PWR_KEY Wakeup\n", __func__);
		pwrkey_wakeup = 1;
		/* Clear PWR_KEY IRQ */
		__ricoh61x_write(client, RICOH61x_INT_IR_SYS, reg_val & 0xFE);
	}
//	enable_irq(client->irq);

  /* Disable rtc Interrupt */
	ret = __ricoh61x_read(ricoh61x_i2c_client,
				      rtc_ctrl1, &reg_val);
	reg_val &= 0xBF;	// clear DALE flag
    __ricoh61x_write(ricoh61x_i2c_client, rtc_ctrl1, reg_val);

	ricoh61x->iIsSuspending = 0;

	return 0;
}

static int  ricoh61x_i2c_late_suspend(struct device *dev)
{
	struct i2c_client *client = i2c_verify_client(dev);

  return ricoh61x_i2c_suspend(client,PMSG_SUSPEND);
}

static int ricoh61x_i2c_late_resume(struct device *dev)
{
	struct i2c_client *client = i2c_verify_client(dev);    
    
  return ricoh61x_i2c_resume(client);
}

static const struct dev_pm_ops ricoh61x_i2c_dev_pm= {
	.suspend_late = ricoh61x_i2c_late_suspend,
	.resume_early = ricoh61x_i2c_late_resume,
};

#endif

static int ricoh61x_twi_id, ricoh61x_twi_addr;
static int ricoh61x_i2c_detect(struct i2c_client *client, struct i2c_board_info *info)
{
	struct i2c_adapter *adapter = client->adapter;

	if(ricoh61x_twi_id == adapter->nr) {
		strlcpy(info->type, "ricoh619", I2C_NAME_SIZE);
		
		pr_info("%s: Detected chip %s at adapter %d, address 0x%02x\n",
			 __func__, info->type, i2c_adapter_id(adapter), client->addr);
		return 0;
	}
	
	return -ENODEV;	
}

static const struct i2c_device_id ricoh61x_i2c_id[] = {
	{"ricoh619", 0},
	{}
};

MODULE_DEVICE_TABLE(i2c, ricoh61x_i2c_id);

#ifdef CONFIG_OF
static const struct of_device_id ricoh61x_dt_match[] = {
	{ .compatible = "ricoh,ricoh619", },
	{},
};
MODULE_DEVICE_TABLE(of, ricoh61x_dt_match);
#endif
static const unsigned short normal_i2c[2] = {0x32,I2C_CLIENT_END};

static struct i2c_driver ricoh61x_i2c_driver = {
	.class = I2C_CLASS_HWMON,
	.driver = {
		   .name = "ricoh619",
		   .owner = THIS_MODULE,
#ifdef CONFIG_PM
 		   .pm	= (&ricoh61x_i2c_dev_pm),
#endif
		   .of_match_table = of_match_ptr(ricoh61x_dt_match),
		   },
	.probe = ricoh61x_i2c_probe,
	.remove = ricoh61x_i2c_remove,
	.id_table = ricoh61x_i2c_id,
	.detect   = ricoh61x_i2c_detect,
	.address_list   = normal_i2c,
};

volatile int gRicoh619_temp_detect = -1 ;
EXPORT_SYMBOL(gRicoh619_temp_detect);

static int __init ricoh61x_i2c_init(void)
{
	int ret = -ENODEV;
	struct device_node *np = NULL;

	np = of_find_node_by_name(NULL, "ricoh619-i2c");
	if (!np) {
		pr_err("ERROR! get ricoh619_para failed, func:%s, line:%d\n",
							__func__, __LINE__);
		goto _err;
	}
	ret = of_property_read_u32(np, "ricoh619_twi_id", &ricoh61x_twi_id);
	if (ret) {
		pr_err("get ricoh619_twi_id failed, %d\n", ret);
		goto _err;
	}

	ret = of_property_read_u32(np, "ricoh619_twi_addr", &ricoh61x_twi_addr);
	if (ret) {
		pr_err("get ricoh619_twi_addr failed, %d\n", ret);
		goto _err;
	}

	ret = of_property_read_u32(np, "ricoh619_temp_detect", &gRicoh619_temp_detect);
	if (ret) {
		pr_err("get ricoh619_temp_detect failed, %d\n", ret);
		gRicoh619_temp_detect = -1 ;
	}
	else
		printk("Read ricoh619_temp_detect %d  \n",gRicoh619_temp_detect);

	printk(KERN_INFO "PMU: %s:\n", __func__);
	printk ("[%s-%d] twi%d addr 0x%X\n", __func__, __LINE__,ricoh61x_twi_id,ricoh61x_twi_addr);

	ret = i2c_add_driver(&ricoh61x_i2c_driver);
	if (ret != 0)
		pr_err("Failed to register I2C driver: %d\n", ret);

_err:
	return ret;
}

subsys_initcall(ricoh61x_i2c_init);

static void __exit ricoh61x_i2c_exit(void)
{
	i2c_del_driver(&ricoh61x_i2c_driver);
}

module_exit(ricoh61x_i2c_exit);

MODULE_DESCRIPTION("RICOH R5T619 PMU multi-function core driver");
MODULE_LICENSE("GPL");
