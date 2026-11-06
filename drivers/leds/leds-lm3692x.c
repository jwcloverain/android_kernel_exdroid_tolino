// SPDX-License-Identifier: GPL-2.0
// TI LM3692x LED chip family driver
// Copyright (C) 2017-18 Texas Instruments Incorporated - http://www.ti.com/

#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <uapi/linux/uleds.h>
#include <linux/sunxi-gpio.h>

#include "../../arch/arm/mach-sunxi/ntx_firmware.h"
#include "../../arch/arm/mach-sunxi/ntx_hwconfig.h"

#define LM36922_MODEL	0
#define LM36923_MODEL	1

#define LM3692X_REV		0x0
#define LM3692X_RESET		0x1
#define LM3692X_EN		0x10
#define LM3692X_BRT_CTRL	0x11
#define LM3692X_PWM_CTRL	0x12
#define LM3692X_BOOST_CTRL	0x13
#define LM3692X_AUTO_FREQ_HI	0x15
#define LM3692X_AUTO_FREQ_LO	0x16
#define LM3692X_BL_ADJ_THRESH	0x17
#define LM3692X_BRT_LSB		0x18
#define LM3692X_BRT_MSB		0x19
#define LM3692X_FAULT_CTRL	0x1e
#define LM3692X_FAULT_FLAGS	0x1f

#define LM3692X_SW_RESET	BIT(0)
#define LM3692X_DEVICE_EN	BIT(0)
#define LM3692X_LED1_EN		BIT(1)
#define LM3692X_LED2_EN		BIT(2)
#define LM36923_LED3_EN		BIT(3)
#define LM3692X_ENABLE_MASK	(LM3692X_DEVICE_EN | LM3692X_LED1_EN | \
				 LM3692X_LED2_EN | LM36923_LED3_EN)

/* Brightness Control Bits */
#define LM3692X_BL_ADJ_POL	BIT(0)
#define LM3692X_RAMP_RATE_125us	0x00
#define LM3692X_RAMP_RATE_250us	BIT(1)
#define LM3692X_RAMP_RATE_500us BIT(2)
#define LM3692X_RAMP_RATE_1ms	(BIT(1) | BIT(2))
#define LM3692X_RAMP_RATE_2ms	BIT(3)
#define LM3692X_RAMP_RATE_4ms	(BIT(3) | BIT(1))
#define LM3692X_RAMP_RATE_8ms	(BIT(2) | BIT(3))
#define LM3692X_RAMP_RATE_16ms	(BIT(1) | BIT(2) | BIT(3))
#define LM3692X_RAMP_EN		BIT(4)
#define LM3692X_BRHT_MODE_REG	0x00
#define LM3692X_BRHT_MODE_PWM	BIT(5)
#define LM3692X_BRHT_MODE_MULTI_RAMP BIT(6)
#define LM3692X_BRHT_MODE_RAMP_MULTI (BIT(5) | BIT(6))
#define LM3692X_MAP_MODE_EXP	BIT(7)

/* PWM Register Bits */
#define LM3692X_PWM_FILTER_100	BIT(0)
#define LM3692X_PWM_FILTER_150	BIT(1)
#define LM3692X_PWM_FILTER_200	(BIT(0) | BIT(1))
#define LM3692X_PWM_HYSTER_1LSB BIT(2)
#define LM3692X_PWM_HYSTER_2LSB	BIT(3)
#define LM3692X_PWM_HYSTER_3LSB (BIT(3) | BIT(2))
#define LM3692X_PWM_HYSTER_4LSB BIT(4)
#define LM3692X_PWM_HYSTER_5LSB (BIT(4) | BIT(2))
#define LM3692X_PWM_HYSTER_6LSB (BIT(4) | BIT(3))
#define LM3692X_PWM_POLARITY	BIT(5)
#define LM3692X_PWM_SAMP_4MHZ	BIT(6)
#define LM3692X_PWM_SAMP_24MHZ	BIT(7)

/* Boost Control Bits */
#define LM3692X_OCP_PROT_1A	BIT(0)
#define LM3692X_OCP_PROT_1_25A	BIT(1)
#define LM3692X_OCP_PROT_1_5A	(BIT(0) | BIT(1))
#define LM3692X_OVP_21V		BIT(2)
#define LM3692X_OVP_25V		BIT(3)
#define LM3692X_OVP_29V		(BIT(2) | BIT(3))
#define LM3692X_MIN_IND_22UH	BIT(4)
#define LM3692X_BOOST_SW_1MHZ	BIT(5)
#define LM3692X_BOOST_SW_NO_SHIFT	BIT(6)

/* Fault Control Bits */
#define LM3692X_FAULT_CTRL_OVP BIT(0)
#define LM3692X_FAULT_CTRL_OCP BIT(1)
#define LM3692X_FAULT_CTRL_TSD BIT(2)
#define LM3692X_FAULT_CTRL_OPEN BIT(3)

/* Fault Flag Bits */
#define LM3692X_FAULT_FLAG_OVP BIT(0)
#define LM3692X_FAULT_FLAG_OCP BIT(1)
#define LM3692X_FAULT_FLAG_TSD BIT(2)
#define LM3692X_FAULT_FLAG_SHRT BIT(3)
#define LM3692X_FAULT_FLAG_OPEN BIT(4)

/**
 * struct lm3692x_led -
 * @lock - Lock for reading/writing the device
 * @client - Pointer to the I2C client
 * @led_dev - LED class device pointer
 * @regmap - Devices register map
 * @enable_gpio - VDDIO/EN gpio to enable communication interface
 * @regulator - LED supply regulator pointer
 * @label - LED label
 * @led_enable - LED sync to be enabled
 * @model_id - Current device model ID enumerated
 */
struct lm3692x_led {
	struct mutex lock;
	struct i2c_client *client;
	struct led_classdev led_dev;
	struct regmap *regmap;
	int enable_gpio;
	struct regulator *regulator;
	char label[LED_MAX_NAME_SIZE];
	int led_enable;
	int model_id;
	unsigned int brightness_lsb;
	unsigned int  brightness_msb;

	u8 boost_ctrl;

	int percentage;			// 0~100 %
	int frontlight_table; 	// 2 color mix table index .
};

static const struct reg_default lm3692x_reg_defs[] = {
	{LM3692X_EN, 0xf},
	{LM3692X_BRT_CTRL, 0x61},
	{LM3692X_PWM_CTRL, 0x73},
	{LM3692X_BOOST_CTRL, 0x6f},
	{LM3692X_AUTO_FREQ_HI, 0x0},
	{LM3692X_AUTO_FREQ_LO, 0x0},
	{LM3692X_BL_ADJ_THRESH, 0x0},
	{LM3692X_BRT_LSB, 0x7},
	{LM3692X_BRT_MSB, 0xff},
	{LM3692X_FAULT_CTRL, 0x7},
};

static const struct regmap_config lm3692x_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,

	.max_register = LM3692X_FAULT_FLAGS,
	.reg_defaults = lm3692x_reg_defs,
	.num_reg_defaults = ARRAY_SIZE(lm3692x_reg_defs),
	.cache_type = REGCACHE_RBTREE,
};

extern NTX_FW_LM36922FL_DUALFL_percent_tab *gptLm36922fl_dualcolor_percent_tab; 
extern NTX_FW_LM36922FL_dualcolor_hdr *gptLm36922fl_dualcolor_tab_hdr; 
extern volatile NTX_HWCONFIG *gptHWCFG;

extern int gSleep_Mode_Suspend;
static volatile int giFL_PWR_state;
static volatile int giFL_PWR_state_max;
static struct lm3692x_led *gpled[2];
static int gilm36922_leds;
static int fl_en_status = 0;
static int giSuspend = 0;
int fl_lm36922_x2_percentage (int iFL_Percentage);


static ssize_t led_color_get(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	int val = 0 , i ;
	struct led_classdev *pLedclass = dev_get_drvdata(dev);

	for(i=0 ; i< gilm36922_leds ; i++)
	{
		if(strcmp(gpled[i]->label,pLedclass->name)==0){
			sprintf (buf, "%d", gpled[i]->frontlight_table);
		}
	}

	return strlen(buf);
}

static ssize_t led_color_set(struct device *dev, struct device_attribute *attr,
		       const char *buf, size_t count)
{
	int val = simple_strtoul (buf, NULL, 10);
	int i=0,select_i;
	struct led_classdev *pLedclass = dev_get_drvdata(dev);

	for(i=0 ; i< gilm36922_leds ; i++)
	{
		if(strcmp(gpled[i]->label,pLedclass->name)==0){
			select_i = i;
		}
		gpled[i]->frontlight_table = val ;
	}
	fl_lm36922_x2_percentage ( gpled[select_i]->percentage);

	return count;
}

static ssize_t led_max_color_get(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	int iColors;
	if (gptLm36922fl_dualcolor_percent_tab) {
		iColors = (int)gptLm36922fl_dualcolor_tab_hdr->dwTotalColors ;
	}
	else {
		iColors = 1 ;
	}
	sprintf (buf, "%d", (iColors-1));
	return strlen(buf);
}

static ssize_t led_max_color_set(struct device *dev, struct device_attribute *attr,
		       const char *buf, size_t count)
{
	return count;
}


#if 1
static ssize_t bl_adj_thr_get(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	int ret;
	unsigned int read_buf;

	ret = regmap_read(gpled[0]->regmap, LM3692X_BL_ADJ_THRESH, &read_buf);
	if (ret)
		printk(KERN_ERR"[%s_%d] regmap_read error :%d \n",__FUNCTION__,__LINE__,ret);	

	printk(KERN_INFO"[%s_%d] value: 0x%x \n",__FUNCTION__,__LINE__,read_buf);
	return strlen(buf);
};


static ssize_t brightness_lsb_get(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	int ret , i;
	unsigned int read_buf;
	struct led_classdev *pLedclass = dev_get_drvdata(dev);

	for(i=0 ; i< gilm36922_leds ; i++)
	{
		if(strcmp(gpled[i]->label,pLedclass->name)==0){
			ret = regmap_read(gpled[i]->regmap, LM3692X_BRT_LSB, &read_buf);
			if (ret)
				printk(KERN_ERR"[%s_%d] regmap_read error :%d \n",__FUNCTION__,__LINE__,ret);	
		}
	}

	printk(KERN_INFO"[%s_%d] value: 0x%x \n",__FUNCTION__,__LINE__,read_buf);
	return strlen(buf);
};

static ssize_t brightness_msb_get(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	int ret,i;
	unsigned int read_buf = 0;
	struct led_classdev *pLedclass = dev_get_drvdata(dev);

	for(i=0 ; i< gilm36922_leds ; i++)
	{
		if(strcmp(gpled[i]->label,pLedclass->name)==0){
			ret = regmap_read(gpled[i]->regmap, LM3692X_BRT_MSB, &read_buf);
			if (ret)
			printk(KERN_ERR"[%s_%d] regmap_read error :%d \n",__FUNCTION__,__LINE__,ret);
		}
	}

	printk(KERN_INFO"[%s_%d] value: 0x%x \n",__FUNCTION__,__LINE__,read_buf);
	return strlen(buf);
};

static ssize_t en_ctl_get(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	int ret,i;
	unsigned int read_buf=0;
	struct led_classdev *pLedclass = dev_get_drvdata(dev);

	for(i=0 ; i< gilm36922_leds ; i++)
	{
		if(strcmp(gpled[i]->label,pLedclass->name)==0){
			ret = regmap_read(gpled[i]->regmap, LM3692X_EN, &read_buf);
			if (ret)
			printk(KERN_ERR"[%s_%d] regmap_read error :%d \n",__FUNCTION__,__LINE__,ret);
		}
	}

	printk(KERN_INFO"[%s_%d] value: 0x:%x \n",__FUNCTION__,__LINE__,read_buf);
	return strlen(buf);
};

static ssize_t fault_flag_get(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	int ret,i;
	unsigned int read_buf=0;
	struct led_classdev *pLedclass = dev_get_drvdata(dev);

	for(i=0 ; i< gilm36922_leds ; i++)
	{
		if(strcmp(gpled[i]->label,pLedclass->name)==0){
			ret = regmap_read(gpled[i]->regmap, LM3692X_FAULT_FLAGS, &read_buf);
			if (ret)
				printk(KERN_ERR"[%s_%d] regmap_read error :%d \n",__FUNCTION__,__LINE__,ret);
		}
	}

	printk(KERN_INFO"[%s_%d] value: 0x:%x \n",__FUNCTION__,__LINE__,read_buf);
	return strlen(buf);
};

static ssize_t freq_hi_get(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	int ret,i;
	unsigned int read_buf=0;
	struct led_classdev *pLedclass = dev_get_drvdata(dev);

	for(i=0 ; i< gilm36922_leds ; i++)
	{
		if(strcmp(gpled[i]->label,pLedclass->name)==0){
			ret = regmap_read(gpled[i]->regmap, LM3692X_AUTO_FREQ_HI, &read_buf);
			if (ret)
				printk(KERN_ERR"[%s_%d] regmap_read error :%d \n",__FUNCTION__,__LINE__,ret);
		}
	}

	printk(KERN_INFO"[%s_%d] value: 0x:%x \n",__FUNCTION__,__LINE__,read_buf);
	return strlen(buf);
};

static ssize_t freq_low_get(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	int ret,i;
	unsigned int read_buf=0;
	struct led_classdev *pLedclass = dev_get_drvdata(dev);

	for(i=0 ; i< gilm36922_leds ; i++)
	{
		if(strcmp(gpled[i]->label,pLedclass->name)==0){
			ret = regmap_read(gpled[i]->regmap, LM3692X_AUTO_FREQ_LO, &read_buf);
			if (ret)
				printk(KERN_ERR"[%s_%d] regmap_read error :%d \n",__FUNCTION__,__LINE__,ret);
		}
	}

	printk(KERN_INFO"[%s_%d] value: 0x:%x \n",__FUNCTION__,__LINE__,read_buf);
	return strlen(buf);
};

static ssize_t pwm_ctl_get(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	int ret,i;
	unsigned int read_buf=0;
	struct led_classdev *pLedclass = dev_get_drvdata(dev);

	for(i=0 ; i< gilm36922_leds ; i++)
	{
		if(strcmp(gpled[i]->label,pLedclass->name)==0){
			ret = regmap_read(gpled[i]->regmap, LM3692X_PWM_CTRL, &read_buf);
			if (ret)
				printk(KERN_ERR"[%s_%d] regmap_read error :%d \n",__FUNCTION__,__LINE__,ret);
		}
	}

	printk(KERN_INFO"[%s_%d] value: 0x:%x \n",__FUNCTION__,__LINE__,read_buf);
	return strlen(buf);
};

static ssize_t reset_get(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	int ret;

	return strlen(buf);
};


static ssize_t boost_ctl_get(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	int ret,i;
	unsigned int read_buf=0;
	struct led_classdev *pLedclass = dev_get_drvdata(dev);

	for(i=0 ; i< gilm36922_leds ; i++)
	{
		if(strcmp(gpled[i]->label,pLedclass->name)==0){
			ret = regmap_read(gpled[i]->regmap, LM3692X_BOOST_CTRL, &read_buf);
			if (ret)
				printk(KERN_ERR"[%s_%d] regmap_read error :%d \n",__FUNCTION__,__LINE__,ret);
		}
	}

	printk(KERN_INFO"[%s_%d] value: 0x:%x \n",__FUNCTION__,__LINE__,read_buf);
	return strlen(buf);
};

static ssize_t brt_ctl_get(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	int ret,i;
	unsigned int read_buf=0;
	struct led_classdev *pLedclass = dev_get_drvdata(dev);

	for(i=0 ; i< gilm36922_leds ; i++)
	{
		if(strcmp(gpled[i]->label,pLedclass->name)==0){
			ret = regmap_read(gpled[i]->regmap, LM3692X_BRT_CTRL, &read_buf);
			if (ret)
				printk(KERN_ERR"[%s_%d] regmap_read error :%d \n",__FUNCTION__,__LINE__,ret);
		}
	}

	printk(KERN_INFO"[%s_%d] value: 0x:%x \n",__FUNCTION__,__LINE__,read_buf);
	return strlen(buf);
};



static ssize_t bl_adj_thr_set(struct device *dev, struct device_attribute *attr,
		       const char *buf, size_t count)
{
	int ret , i;
	unsigned long value;
	char *pHead = NULL;
	struct led_classdev *pLedclass = dev_get_drvdata(dev);

	if(pHead = strstr(buf,"0x"))
		value = simple_strtol(buf, NULL, 16);
	else
		value = simple_strtoul(buf, NULL, 10);

	for(i=0 ; i< gilm36922_leds ; i++)
	{
		if(strcmp(gpled[i]->label,pLedclass->name)==0){
			ret = regmap_write(gpled[i]->regmap, LM3692X_BL_ADJ_THRESH, value);
			if (ret)
				printk(KERN_ERR"[%s_%d] regmap_write error :%d \n",__FUNCTION__,__LINE__,ret);
		}
	}

	return count;
};


static ssize_t brightness_lsb_set(struct device *dev, struct device_attribute *attr,
		       const char *buf, size_t count)
{
	int ret , i;
	unsigned long value;
	char *pHead = NULL;
	struct led_classdev *pLedclass = dev_get_drvdata(dev);

	if(pHead = strstr(buf,"0x"))
		value = simple_strtol(buf, NULL, 16);
	else
		value = simple_strtoul(buf, NULL, 10);

	for(i=0 ; i< gilm36922_leds ; i++)
	{
		if(strcmp(gpled[i]->label,pLedclass->name)==0){
			ret = regmap_write(gpled[i]->regmap, LM3692X_BRT_LSB, value);
			if (ret)
				printk(KERN_ERR"[%s_%d] regmap_write error :%d \n",__FUNCTION__,__LINE__,ret);
		}
	}

	return count;
};

static ssize_t brightness_msb_set(struct device *dev, struct device_attribute *attr,
		       const char *buf, size_t count)
{
	int ret ,i;
	unsigned long value;
	char *pHead = NULL;
	struct led_classdev *pLedclass = dev_get_drvdata(dev);

	if(pHead = strstr(buf,"0x"))
		value = simple_strtol(buf, NULL, 16);
	else
		value = simple_strtoul(buf, NULL, 10);

	for(i=0 ; i< gilm36922_leds ; i++)
	{
		if(strcmp(gpled[i]->label,pLedclass->name)==0){
			ret = regmap_write(gpled[i]->regmap, LM3692X_BRT_MSB, value);
			if (ret)
				printk(KERN_ERR"[%s_%d] regmap_write error :%d \n",__FUNCTION__,__LINE__,ret);	
		}
	}

	return count;
};

static ssize_t en_ctl_set(struct device *dev, struct device_attribute *attr,
		       const char *buf, size_t count)
{
	int ret,i;
	unsigned long value;
	char *pHead = NULL;
	struct led_classdev *pLedclass = dev_get_drvdata(dev);

	if(pHead = strstr(buf,"0x"))
		value = simple_strtol(buf, NULL, 16);
	else
		value = simple_strtoul(buf, NULL, 10);

	for(i=0 ; i< gilm36922_leds ; i++)
	{
		if(strcmp(gpled[i]->label,pLedclass->name)==0){
			ret = regmap_write(gpled[i]->regmap, LM3692X_EN, value);
			if (ret)
				printk(KERN_ERR"[%s_%d] regmap_write error :%d \n",__FUNCTION__,__LINE__,ret);
		}
	}

	return count;
};

static ssize_t fault_flag_set(struct device *dev, struct device_attribute *attr,
		       const char *buf, size_t count)
{
	int ret ,i ;
	unsigned long value;
	char *pHead = NULL;
	struct led_classdev *pLedclass = dev_get_drvdata(dev);

	if(pHead = strstr(buf,"0x"))
		value = simple_strtol(buf, NULL, 16);
	else
		value = simple_strtoul(buf, NULL, 10);

	for(i=0 ; i< gilm36922_leds ; i++)
	{
		if(strcmp(gpled[i]->label,pLedclass->name)==0){
			ret = regmap_write(gpled[i]->regmap, LM3692X_FAULT_FLAGS, value);
			if (ret)
				printk(KERN_ERR"[%s_%d] regmap_write error :%d \n",__FUNCTION__,__LINE__,ret);
		}
	}

	return count;
};

static ssize_t freq_hi_set(struct device *dev, struct device_attribute *attr,
		       const char *buf, size_t count)
{
	int ret ,i;
	unsigned long value;
	char *pHead = NULL;
	struct led_classdev *pLedclass = dev_get_drvdata(dev);

	if(pHead = strstr(buf,"0x"))
		value = simple_strtol(buf, NULL, 16);
	else
		value = simple_strtoul(buf, NULL, 10);

	for(i=0 ; i< gilm36922_leds ; i++)
	{
		if(strcmp(gpled[i]->label,pLedclass->name)==0){
			ret = regmap_write(gpled[i]->regmap, LM3692X_AUTO_FREQ_HI, value);
			if (ret)
				printk(KERN_ERR"[%s_%d] regmap_write error :%d \n",__FUNCTION__,__LINE__,ret);
		}
	}

	return count;
};

static ssize_t freq_low_set(struct device *dev, struct device_attribute *attr,
		       const char *buf, size_t count)
{
	int ret ,i;
	unsigned long value;
	char *pHead = NULL;
	struct led_classdev *pLedclass = dev_get_drvdata(dev);

	if(pHead = strstr(buf,"0x"))
		value = simple_strtol(buf, NULL, 16);
	else
		value = simple_strtoul(buf, NULL, 10);

	for(i=0 ; i< gilm36922_leds ; i++)
	{
		if(strcmp(gpled[i]->label,pLedclass->name)==0){
			ret = regmap_write(gpled[i]->regmap, LM3692X_AUTO_FREQ_LO, value);
			if (ret)
				printk(KERN_ERR"[%s_%d] regmap_write error :%d \n",__FUNCTION__,__LINE__,ret);
		}
	}

	return count;
};

static ssize_t reset_set(struct device *dev, struct device_attribute *attr,
		       const char *buf, size_t count)
{
	int ret , i ;
	unsigned long value;
	char *pHead = NULL;
	struct led_classdev *pLedclass = dev_get_drvdata(dev);

	if(pHead = strstr(buf,"0x"))
		value = simple_strtol(buf, NULL, 16);
	else
		value = simple_strtoul(buf, NULL, 10);

	for(i=0 ; i< gilm36922_leds ; i++)
	{
		if(strcmp(gpled[i]->label,pLedclass->name)==0){
			ret = regmap_write(gpled[i]->regmap, LM3692X_RESET, value);
			if (ret)
				printk(KERN_ERR"[%s_%d] regmap_write error :%d \n",__FUNCTION__,__LINE__,ret);
		}
	}

	return count;
};

static ssize_t pwm_ctl_set(struct device *dev, struct device_attribute *attr,
		       const char *buf, size_t count)
{
	int ret , i ;
	unsigned long value;
	char *pHead = NULL;
	struct led_classdev *pLedclass = dev_get_drvdata(dev);

	if(pHead = strstr(buf,"0x"))
		value = simple_strtol(buf, NULL, 16);
	else
		value = simple_strtoul(buf, NULL, 10);

	for(i=0 ; i< gilm36922_leds ; i++)
	{
		if(strcmp(gpled[i]->label,pLedclass->name)==0){
			ret = regmap_write(gpled[0]->regmap, LM3692X_PWM_CTRL, value);
			if (ret)
				printk(KERN_ERR"[%s_%d] regmap_write error :%d \n",__FUNCTION__,__LINE__,ret);	
		}
	}

	return count;
};

static ssize_t boost_ctl_set(struct device *dev, struct device_attribute *attr,
		       const char *buf, size_t count)
{
	int ret ;
	unsigned long value;
	char *pHead = NULL;

	if(pHead = strstr(buf,"0x"))
		value = simple_strtol(buf, NULL, 16);
	else
		value = simple_strtoul(buf, NULL, 10);

	ret = regmap_write(gpled[0]->regmap, LM3692X_BOOST_CTRL, value);
	if (ret)
		printk(KERN_ERR"[%s_%d] regmap_write error :%d \n",__FUNCTION__,__LINE__,ret);

	return count;
};

static ssize_t brt_ctl_set(struct device *dev, struct device_attribute *attr,
		       const char *buf, size_t count)
{
	int ret ;
	unsigned long value;
	char *pHead = NULL;

	if(pHead = strstr(buf,"0x"))
		value = simple_strtol(buf, NULL, 16);
	else
		value = simple_strtoul(buf, NULL, 10);

	ret = regmap_write(gpled[0]->regmap, LM3692X_BRT_CTRL, value);
	if (ret)
		printk(KERN_ERR"[%s_%d] regmap_write error :%d \n",__FUNCTION__,__LINE__,ret);

	return count;
};

static DEVICE_ATTR (brt_ctl, 0644, brt_ctl_get, brt_ctl_set);
static DEVICE_ATTR (brightness_lsb, 0644, brightness_lsb_get, brightness_lsb_set);
static DEVICE_ATTR (brightness_msb, 0644, brightness_msb_get, brightness_msb_set);
static DEVICE_ATTR (en_ctl, 0644, en_ctl_get, en_ctl_set);
static DEVICE_ATTR (fault_flag, 0644, fault_flag_get, fault_flag_set);
static DEVICE_ATTR (freq_hi, 0644, freq_hi_get, freq_hi_set);
static DEVICE_ATTR (freq_lo, 0644, freq_low_get, freq_low_set);
static DEVICE_ATTR (pwm_ctl, 0644, pwm_ctl_get, pwm_ctl_set);
static DEVICE_ATTR (reset_ctl, 0644, reset_get, reset_set);
static DEVICE_ATTR (boost_ctl, 0644, boost_ctl_get, boost_ctl_set);
static DEVICE_ATTR (bl_adj_thr, 0644, bl_adj_thr_get, bl_adj_thr_set);
#endif
static DEVICE_ATTR (max_color, 0644, led_max_color_get, led_max_color_set);
static DEVICE_ATTR (color, 0644, led_color_get, led_color_set);


static int _fl_lm36922_set_color (struct lm3692x_led *led,int iFL_color)
{
	if (gptLm36922fl_dualcolor_percent_tab) {
		if ((int)gptLm36922fl_dualcolor_tab_hdr->dwTotalColors <= iFL_color) {
			printk ("[%s-%d] Front light color index %d >= %d\n", 
					__func__,__LINE__, iFL_color,(int)gptLm36922fl_dualcolor_tab_hdr->dwTotalColors);
			return -1;
		}
	}
	else {
		printk (KERN_ERR"[%s-%d] Front light table not exist !!\n", __func__,__LINE__);
		return -1;
	}
	led->frontlight_table = iFL_color;
	fl_lm36922_x2_percentage (led->percentage);
	return 0;
}


int fl_lm36922_set_color (int iFL_color)
{
	_fl_lm36922_set_color(gpled[0],iFL_color);
	return 0;
}

static int lm3692x_fault_check(struct lm3692x_led *led)
{
	int ret;
	unsigned int read_buf;

	ret = regmap_read(led->regmap, LM3692X_FAULT_FLAGS, &read_buf);
	if (ret)
		return ret;

	if (read_buf)
		dev_err(&led->client->dev, "Detected a fault 0x%X\n", read_buf);

	/* The first read may clear the fault.  Check again to see if the fault
	 * still exits and return that value.
	 */
	regmap_read(led->regmap, LM3692X_FAULT_FLAGS, &read_buf);
	if (read_buf)
		dev_err(&led->client->dev, "Second read of fault flags 0x%X\n",
			read_buf);

	return read_buf;
}


static int lm3692x_init(struct lm3692x_led *led)
{
	int enable_state;
	int ret;

	printk(KERN_INFO"[%s_%d] %s \n",__FUNCTION__,__LINE__,led->label);
	if (led->regulator) {
		ret = regulator_enable(led->regulator);
		if (ret) {
			dev_err(&led->client->dev,
				"Failed to enable regulator\n");
			return ret;
		}
	}

	if(gilm36922_leds ==0 ) {
		if (led->enable_gpio > 0 ){
			if(gpio_is_valid(led->enable_gpio)) {
				if (gpio_request(led->enable_gpio, "lm36922_bl_hwen") < 0) {
					dev_err(&led->client->dev,"Failed to request gpio %d .\n", led->enable_gpio);
				}
				else {
					gpio_direction_output(led->enable_gpio,1);
					fl_en_status = 1;
				}
			}
			else {
				dev_err(&led->client->dev,"hwen_gpio=%d is invalid !\n",led->enable_gpio);
			}
		}
	}
	else
		led->enable_gpio = gpled[0]->enable_gpio;

	ret = lm3692x_fault_check(led);
	if (ret) {
		dev_err(&led->client->dev, "Cannot read/clear faults\n");
		goto not_exist;
	}

	ret = regmap_write(led->regmap, LM3692X_EN, 0);
	if (ret)
		goto out;


	ret = regmap_write(led->regmap, LM3692X_BRT_CTRL, 0x00);
	if (ret)
		goto out;

	/*
	 * For glitch free operation, the following data should
	 * only be written while LEDx enable bits are 0 and the device enable
	 * bit is set to 1.
	 * per Section 7.5.14 of the data sheet
	 */
	ret = regmap_write(led->regmap, LM3692X_EN, LM3692X_DEVICE_EN);
	if (ret)
		goto out;

	/* Set the brightness to 0 so when enabled the LEDs do not come
	 * on with full brightness.
	 */
	ret = regmap_write(led->regmap, LM3692X_BRT_MSB, 0);
	if (ret)
		goto out;

	ret = regmap_write(led->regmap, LM3692X_BRT_LSB, 0);
	if (ret)
		goto out;

	ret = regmap_write(led->regmap, LM3692X_PWM_CTRL,
		LM3692X_PWM_FILTER_100 | LM3692X_PWM_SAMP_24MHZ);
	if (ret)
		goto out;

	ret = regmap_write(led->regmap, LM3692X_BOOST_CTRL,
			LM3692X_BRHT_MODE_RAMP_MULTI |
			LM3692X_BL_ADJ_POL |
			LM3692X_RAMP_RATE_250us | led->boost_ctrl);
	if (ret)
		goto out;

	ret = regmap_write(led->regmap, LM3692X_AUTO_FREQ_HI, 0x00);
	if (ret)
		goto out;

	ret = regmap_write(led->regmap, LM3692X_AUTO_FREQ_LO, 0x00);
	if (ret)
		goto out;

	ret = regmap_write(led->regmap, LM3692X_BL_ADJ_THRESH, 0x00);
	if (ret)
		goto out;

	
	ret = regmap_write(led->regmap, LM3692X_BRT_CTRL,
			LM3692X_BL_ADJ_POL /*| LM3692X_PWM_HYSTER_4LSB*/); // Disable Ramp
			
	if (ret)
		goto out;

	switch (led->led_enable) {
		case 0:
		default:
			if (led->model_id == LM36923_MODEL)
				enable_state = LM3692X_LED1_EN | LM3692X_LED2_EN |
					LM36923_LED3_EN;
			else
				enable_state = LM3692X_LED1_EN | LM3692X_LED2_EN;

			break;
		case 1:
			enable_state = LM3692X_LED1_EN;
			break;
		case 2:
			enable_state = LM3692X_LED2_EN;
			break;

		case 3:
			if (led->model_id == LM36923_MODEL) {
				enable_state = LM36923_LED3_EN;
				break;
			}

		ret = -EINVAL;
		dev_err(&led->client->dev,
			"LED3 sync not available on this device\n");
		goto out;
	}

	ret = regmap_update_bits(led->regmap, LM3692X_EN, LM3692X_ENABLE_MASK,
				 enable_state | LM3692X_DEVICE_EN);

	return ret;
out:

	dev_err(&led->client->dev, "Fail writing initialization values\n");
	if(gilm36922_leds ==0 ) {
		if (led->enable_gpio)
			gpio_direction_output(led->enable_gpio, 0);
	}

	if (led->regulator) {
		ret = regulator_disable(led->regulator);
		if (ret)
			dev_err(&led->client->dev,
				"Failed to disable regulator\n");
	}
	return ret;
not_exist:
	dev_err(&led->client->dev, "lm3692x_fault_check failed\n");

	return ret;
}

static int lm3692x_chips_init(void)
{
	int i;

	for(i=0;i<gilm36922_leds;i++) {
		lm3692x_init(gpled[i]);
	}
}

static int lm3692x_save_brightness(void)
{
	int i ,ret;	
	unsigned int read_buf;

	for(i=0;i<gilm36922_leds;i++) {
		ret = regmap_read(gpled[i]->regmap, LM3692X_BRT_LSB, &read_buf);
		if (ret)
			printk(KERN_ERR"[%s_%d] regmap_read LM3692X_BRT_LSB error :%d \n",__FUNCTION__,__LINE__,ret);	
		else
			gpled[i]->brightness_lsb = read_buf;
		ret = regmap_read(gpled[i]->regmap, LM3692X_BRT_MSB, &read_buf);
		if (ret)
			printk(KERN_ERR"[%s_%d] regmap_read LM3692X_BRT_MSB error :%d \n",__FUNCTION__,__LINE__,ret);	
		else
			gpled[i]->brightness_msb = read_buf;
	}
}

static int lm3692x_restore_brightness(void)
{
	int i ,ret;	
	unsigned int read_buf;

	for(i=0;i<gilm36922_leds;i++) {
		ret = regmap_write(gpled[i]->regmap, LM3692X_BRT_LSB, gpled[i]->brightness_lsb);
		if (ret)
			printk(KERN_ERR"[ERROR] regmap_write LM3692X_BRT_LSB error :%d \n",ret);

		ret = regmap_write(gpled[i]->regmap, LM3692X_BRT_MSB, &gpled[i]->brightness_msb);
		if (ret)
			printk(KERN_ERR"[ERROR] regmap_write LM3692X_BRT_MSB error :%d \n",ret);
	}
}



static int lm3692x_reload_gpio(struct lm3692x_led *led){
	struct device_node *np = NULL;
	struct gpio_config config;
	int ret = 0;

	np = of_find_node_by_name(NULL, "led-controller");
	if (!np) {
		pr_err("ERROR! get led-controller failed, func:%s, line:%d\n",	__func__, __LINE__);
		return -1 ;
	}

	led->enable_gpio = 	of_get_named_gpio_flags(np, "enable-gpios",
					0, (enum of_gpio_flags *)&config);
	if(led->enable_gpio  < 0){
		printk(KERN_ERR"[%s_%d] enable-gpios read failed \n",__FUNCTION__,__LINE__);
		return -1;
	}

	return ret;
}


static int lm3692x_brightness_set(struct led_classdev *led_cdev,
				enum led_brightness brt_val)
{
	struct lm3692x_led *led =
			container_of(led_cdev, struct lm3692x_led, led_dev);
	int ret;
	int lsb_value = (brt_val & 0x07);
	int msb_value =  (brt_val >>3);

	if( brt_val > LED_FULL)
	{
		dev_err(&led->client->dev, "Cannot set over LED_FULL\n");
		goto out;
	}

	printk(KERN_INFO"[%s_%d] name:%s flag:%d \n",__FUNCTION__,__LINE__,led->label,led_cdev->flags);

	mutex_lock(&led->lock);

	ret = lm3692x_fault_check(led);
	if (ret) {
		dev_err(&led->client->dev, "Cannot read/clear faults\n");
		goto out;
	}

	ret = regmap_write(led->regmap, LM3692X_BRT_MSB, msb_value);
	if (ret) {
		dev_err(&led->client->dev, "Cannot write MSB\n");
		goto out;
	}

	ret = regmap_write(led->regmap, LM3692X_BRT_LSB, lsb_value);
	if (ret) {
		dev_err(&led->client->dev, "Cannot write LSB\n");
		goto out;
	}

	if(led_cdev->flags & LED_SUSPENDED)
	{
		giSuspend = 1 ;//suspend
		if (gSleep_Mode_Suspend) {
			if(--giFL_PWR_state<=0) {
				if(giFL_PWR_state<0) {
					printk(KERN_WARNING"%s():[WARNING] FL PWR state <0 !!\n",__FUNCTION__);
					giFL_PWR_state=0;
				}
					
				if (0!=fl_en_status) {
					if(gpio_is_valid(led->enable_gpio)) {

						// Disable LED DEVICE (If not disable device , only disable FL_EN can't trigger the reset of LED)
						ret = regmap_write(led->regmap, LM3692X_EN, 0);
						if (ret)
							goto out;

						lm3692x_save_brightness();
						printk("%s():FL PWR[OFF] \n",__FUNCTION__);
						gpio_direction_output(led->enable_gpio, 0);
					}
					fl_en_status = 0;
				}
			}
		}
		else {
			/*
			//fl_current_now = lm3630a_get_FL_current();
			if (fl_current_now == -1 && (0!=fl_en_status)) {
				if(gpio_is_valid(led->enable_gpio)) {
					printk("%s():FL PWR[OFF] \n",__FUNCTION__);
					gpio_direction_output(led->enable_gpio, 0);
					fl_en_status = 0;
				}
			}
			*/
		}
	}
	else if(giSuspend==1)
	{
		giSuspend = 0 ;	// resume
		if (gSleep_Mode_Suspend) {
			if(++giFL_PWR_state>giFL_PWR_state_max) {
				printk(KERN_WARNING"%s():[WARNING] FL PWR state > max !!\n",__FUNCTION__);
				giFL_PWR_state=giFL_PWR_state_max;
			}

			if (giFL_PWR_state==1) {
				if(gpio_is_valid(led->enable_gpio)) {
					printk("%s():FL PWR[ON] \n",__FUNCTION__);
					lm3692x_reload_gpio(led);
					printk(KERN_INFO"=== enable_gpio:%d boost_ctrl:%d \n",led->enable_gpio,led->boost_ctrl);
					gpio_direction_output(led->enable_gpio, 1);

				}
				fl_en_status = 1;
				msleep (50);
				lm3692x_chips_init();
				//lm3692x_restore_brightness();
			}
		}
		else {
			/*
			fl_en_status = 1;
			if(gpio_is_valid(pchip->hwen_gpio)) {
				gpio_direction_output(pchip->hwen_gpio, 1);
				msleep (50);
				lm3692x_chips_init();
			}
			*/
		}
	}

	//printk(KERN_INFO"====%s_%d====\n",__FUNCTION__,__LINE__);
out:
	mutex_unlock(&led->lock);
	return ret;
}


int lm36922_set_FL(int index,int brightness)
{
	int ret = 0;

	if(!gpled[index]) {
		printk(KERN_ERR"%s() : led device%d not exist !\n",__func__,index);
		return -1;
	}

	led_set_brightness(&gpled[index]->led_dev,brightness);

	return 0 ;
}

int fl_lm36922_x2_percentage (int iFL_Percentage)
{
	if( iFL_Percentage > 100 || iFL_Percentage < 0)
	{
		printk(KERN_ERR"[%s_%d] iFL_Percentage is out of range !!");
		return -1 ;
	}

	if(0x0a==gptHWCFG->m_val.bFL_PWM)
	{
		int iColor = gpled[0]->frontlight_table;
		if(gptLm36922fl_dualcolor_percent_tab){
			if( 0 == iFL_Percentage){
				lm36922_set_FL(0,0);
				lm36922_set_FL(1,0);
			}
			else{
				lm36922_set_FL(0,gptLm36922fl_dualcolor_percent_tab[iColor].bBrightnessA[iFL_Percentage-1]);
				lm36922_set_FL(1,gptLm36922fl_dualcolor_percent_tab[iColor].bBrightnessB[iFL_Percentage-1]);
				printk(KERN_INFO"[LM36922] iColor:%d Percentage:%d , A:%d , B:%d \n",iColor,iFL_Percentage,
					gptLm36922fl_dualcolor_percent_tab[iColor].bBrightnessA[iFL_Percentage-1],gptLm36922fl_dualcolor_percent_tab[iColor].bBrightnessB[iFL_Percentage-1]);
			}
			//printk(KERN_ERR"=== LM36922 set percentage %d ,%d  \n",iFL_Percentage,gptLm36922fl_dualcolor_percent_tab[iColor].bBrightnessA[iFL_Percentage]);
			gpled[0]->percentage = iFL_Percentage;
			gpled[1]->percentage = iFL_Percentage;
		}
	}

	return 0;
}

int lm36922_get_FL_current(void)
{
	int iRet = -1;
	unsigned long dwCurrent=0;

	unsigned long *pdwCurrTab;
	unsigned long dwCurrTabSize;
	int iTabIdx;

	if(0x0a==gptHWCFG->m_val.bFL_PWM)
	{
		int iColor = gpled[0]->frontlight_table;
		int iPercentIdx=gpled[0]->percentage-1;

		if(gptLm36922fl_dualcolor_percent_tab){
			dwCurrent = gptLm36922fl_dualcolor_percent_tab[iColor].dwCurrentA[iPercentIdx];
			iRet = dwCurrent;
		}
	}

	if(iRet<0) {
		printk(KERN_ERR"%s():curr table not avalible(%d) !!\n",__FUNCTION__,iRet);
	}

	return iRet;
}


#ifdef CONFIG_PM
static int lm3692x_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct lm3692x_led *led = i2c_get_clientdata(client);

	printk(KERN_INFO"[%s_%d] gSleep_Mode_Suspend:%d \n",__FUNCTION__,__LINE__,gSleep_Mode_Suspend);
	//led_classdev_suspend(&led->led_dev);

	mutex_lock(&led->lock);
	if (gSleep_Mode_Suspend) {
		if(--giFL_PWR_state<=0) {
			if(giFL_PWR_state<0) {
				printk(KERN_WARNING"%s():[WARNING] FL PWR state <0 !!\n",__FUNCTION__);
				giFL_PWR_state=0;
			}
					
			if (0!=fl_en_status) {
				if(gpio_is_valid(led->enable_gpio)) {
					lm3692x_save_brightness();
					printk("%s():FL PWR[OFF] ,led->enable_gpio:%d \n",__FUNCTION__,led->enable_gpio);
					gpio_direction_output(led->enable_gpio, 0);
				}
				fl_en_status = 0;
			}
		}
	}
	else {
		/*
		//fl_current_now = lm3630a_get_FL_current();
		if (fl_current_now == -1 && (0!=fl_en_status)) {
			if(gpio_is_valid(led->enable_gpio)) {
				printk("%s():FL PWR[OFF] \n",__FUNCTION__);
				gpio_direction_output(led->enable_gpio, 0);
				fl_en_status = 0;
			}
		}
		*/
	}
	mutex_unlock(&led->lock);

	return 0;
}

static int lm3692x_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct lm3692x_led *led = i2c_get_clientdata(client);

	printk(KERN_INFO"[%s_%d] gSleep_Mode_Suspend:%d \n",__FUNCTION__,__LINE__,gSleep_Mode_Suspend);
	//led_classdev_resume(&led->led_dev);

	mutex_lock(&led->lock);
	if (gSleep_Mode_Suspend) {
		if(++giFL_PWR_state>giFL_PWR_state_max) {
			printk(KERN_WARNING"%s():[WARNING] FL PWR state > max !!\n",__FUNCTION__);
			giFL_PWR_state=giFL_PWR_state_max;
		}

		if (giFL_PWR_state==1) {
			if(gpio_is_valid(led->enable_gpio)) {
				printk("%s():FL PWR[ON] \n",__FUNCTION__);
				lm3692x_reload_gpio(led);
				printk(KERN_INFO"=== enable_gpio:%d boost_ctrl:%d \n",led->enable_gpio,led->boost_ctrl);
				gpio_direction_output(led->enable_gpio, 1);
			}
			fl_en_status = 1;
			msleep (50);
			lm3692x_chips_init();
			
		}
		//else
		//	lm3692x_restore_brightness();
	}
	else {
		/*
		fl_en_status = 1;
		if(gpio_is_valid(pchip->hwen_gpio)) {
			gpio_direction_output(pchip->hwen_gpio, 1);
			msleep (50);
			lm3692x_chips_init();
		}
		*/
	}
	mutex_unlock(&led->lock);

	return 0;
}

#endif

static int lm3692x_probe_dt(struct lm3692x_led *led)
{
	struct fwnode_handle *child = NULL;
	const char *name;
	u32 ovp ;
	int ret;

#if 1
	struct device_node *np = NULL;
	struct gpio_config config;
	np = of_find_node_by_name(NULL, "led-controller");
	if (!np) {
		pr_err("ERROR! get led-controller failed, func:%s, line:%d\n",	__func__, __LINE__);
		return -1 ;
	}

	led->enable_gpio = 	of_get_named_gpio_flags(np, "enable-gpios",
					0, (enum of_gpio_flags *)&config);
	printk(KERN_INFO"[LM3692x] enable_gpio:%d \n",led->enable_gpio);
#else
	led->enable_gpio = devm_gpiod_get_optional(&led->client->dev,
						   "enable", GPIOD_OUT_LOW);
#endif

	if (IS_ERR(led->enable_gpio)) {
		ret = PTR_ERR(led->enable_gpio);
		dev_err(&led->client->dev, "Failed to get enable gpio: %d\n",
			ret);
		return ret;
	}

	ret = device_property_read_u32(&led->client->dev,
		"ti,ovp-microvolt", &ovp);

	if (ret) {
		led->boost_ctrl |= LM3692X_OVP_29V;
	}
	else {
		switch (ovp) {
			case 17000000:
				printk(KERN_INFO"[OVP] set as LM3692X_OVP_17V \n");
				break;
			case 21000000:
				led->boost_ctrl |= LM3692X_OVP_21V;
				printk(KERN_INFO"[OVP] set as LM3692X_OVP_21V \n");
				break;
			case 25000000:
				led->boost_ctrl |= LM3692X_OVP_25V;
				printk(KERN_INFO"[OVP] set as LM3692X_OVP_25V \n");
				break;
			case 29000000:
				led->boost_ctrl |= LM3692X_OVP_29V;
				printk(KERN_INFO"[OVP] set as LM3692X_OVP_29V \n");
				break;
			default:
				dev_err(&led->client->dev, "Invalid OVP %d\n", ovp);
				return -EINVAL;
		}
	}

	led->regulator = devm_regulator_get(&led->client->dev, "vled");
	if (IS_ERR(led->regulator))
		led->regulator = NULL;

	child = device_get_next_child_node(&led->client->dev, child);
	if (!child) {
		dev_err(&led->client->dev, "No LED Child node\n");
		return -ENODEV;
	}

	fwnode_property_read_string(child, "linux,default-trigger",
				    &led->led_dev.default_trigger);

	ret = fwnode_property_read_string(child, "label", &name);
	if (ret)
		snprintf(led->label, sizeof(led->label),
			"%s::", led->client->name);
	else
		snprintf(led->label, sizeof(led->label),
			 "%s_%s", led->client->name, name);

	ret = fwnode_property_read_u32(child, "reg", &led->led_enable);
	if (ret) {
		dev_err(&led->client->dev, "reg DT property missing\n");
		return ret;
	}

	led->led_dev.name = led->label;

	ret = devm_led_classdev_register(&led->client->dev, &led->led_dev);
	if (ret) {
		dev_err(&led->client->dev, "led register err: %d\n", ret);
		return ret;
	}

	led->led_dev.dev->of_node = to_of_node(child);

	return 0;
}

static int lm3692x_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	struct lm3692x_led *led;
	int ret;
	int rval;

	led = devm_kzalloc(&client->dev, sizeof(*led), GFP_KERNEL);
	if (!led)
		return -ENOMEM;
	
	mutex_init(&led->lock);

	led->client = client;
	led->led_dev.brightness_set_blocking = lm3692x_brightness_set;
	led->model_id = id->driver_data;
	led->brightness_lsb = 0 ;
	led->brightness_msb = 0 ;
	i2c_set_clientdata(client, led);

	led->regmap = devm_regmap_init_i2c(client, &lm3692x_regmap_config);
	if (IS_ERR(led->regmap)) {
		ret = PTR_ERR(led->regmap);
		dev_err(&client->dev, "Failed to allocate register map: %d\n",
			ret);
		return ret;
	}

	ret = lm3692x_probe_dt(led);
	if (ret)
		return ret;

#if 0
	/* backlight register */
	rval = lm36922_backlight_register(led,gilm36922_chips);
	if (rval < 0) {
		dev_err(&client->dev, "fail : backlight register.\n");
		goto err_exit;
	}
#endif

	ret = lm3692x_init(led);
	if (ret)
		return ret;

	rval = device_create_file(led->led_dev.dev, &dev_attr_brt_ctl);
	if (rval < 0) {
		dev_err(led->led_dev.dev, "fail : brt_ctrl create.\n");
		return rval;
	}

	rval = device_create_file(led->led_dev.dev, &dev_attr_brightness_lsb);
	if (rval < 0) {
		dev_err(led->led_dev.dev, "fail : brightness_lsb create.\n");
		return rval;
	}

	rval = device_create_file(led->led_dev.dev, &dev_attr_brightness_msb);
	if (rval < 0) {
		dev_err(led->led_dev.dev, "fail : brightness_msb create.\n");
		return rval;
	}

	rval = device_create_file(led->led_dev.dev, &dev_attr_en_ctl);
	if (rval < 0) {
		dev_err(led->led_dev.dev, "fail : en_ctrl create.\n");
		return rval;
	}

	rval = device_create_file(led->led_dev.dev, &dev_attr_fault_flag);
	if (rval < 0) {
		dev_err(led->led_dev.dev, "fail : fault_flag create.\n");
		return rval;
	}

	rval = device_create_file(led->led_dev.dev, &dev_attr_freq_hi);
	if (rval < 0) {
		dev_err(led->led_dev.dev, "fail : freq_hi create.\n");
		return rval;
	}


	rval = device_create_file(led->led_dev.dev, &dev_attr_freq_lo);
	if (rval < 0) {
		dev_err(led->led_dev.dev, "fail : freq_lo create.\n");
		return rval;
	}

	rval = device_create_file(led->led_dev.dev, &dev_attr_pwm_ctl);
	if (rval < 0) {
		dev_err(led->led_dev.dev, "fail : pwm_ctrl create.\n");
		return rval;
	}

	rval = device_create_file(led->led_dev.dev, &dev_attr_reset_ctl);
	if (rval < 0) {
		dev_err(led->led_dev.dev, "fail : reset_ctrl create.\n");
		return rval;
	}

	rval = device_create_file(led->led_dev.dev, &dev_attr_boost_ctl);
	if (rval < 0) {
		dev_err(led->led_dev.dev, "fail : boost_ctrl create.\n");
		return rval;
	}

	rval = device_create_file(led->led_dev.dev, &dev_attr_bl_adj_thr);
	if (rval < 0) {
		dev_err(led->led_dev.dev, "fail : bl_adj_thr create.\n");
		return rval;
	}

	rval = device_create_file(led->led_dev.dev, &dev_attr_max_color);
	if (rval < 0) {
		dev_err(led->led_dev.dev, "fail : max_color create.\n");
		return rval;
	}

	rval = device_create_file(led->led_dev.dev, &dev_attr_color);
	if (rval < 0) {
		dev_err(led->led_dev.dev, "fail : color create.\n");
		return rval;
	}

	giFL_PWR_state_max+=1;
	giFL_PWR_state+=1;
	gpled[gilm36922_leds++] = led;

	printk(KERN_INFO"[%s_%d] Probe OK \n",__FUNCTION__,__LINE__);

	return 0;

	return rval;

}

static int lm3692x_remove(struct i2c_client *client)
{
	struct lm3692x_led *led = i2c_get_clientdata(client);
	int ret;

	ret = regmap_update_bits(led->regmap, LM3692X_EN, LM3692X_DEVICE_EN, 0);
	if (ret) {
		dev_err(&led->client->dev, "Failed to disable regulator\n");
		return ret;
	}

	if (led->enable_gpio)
		gpio_direction_output(led->enable_gpio, 0);

	if (led->regulator) {
		ret = regulator_disable(led->regulator);
		if (ret)
			dev_err(&led->client->dev,
				"Failed to disable regulator\n");
	}

	mutex_destroy(&led->lock);

	return 0;
}

static const struct i2c_device_id lm3692x_id[] = {
	{ "lm36922", LM36922_MODEL },
	{ "lm36923", LM36923_MODEL },
	{ }
};
MODULE_DEVICE_TABLE(i2c, lm3692x_id);

static const struct of_device_id of_lm3692x_leds_match[] = {
	{ .compatible = "ti,lm36922", },
	{ .compatible = "ti,lm36923", },
	{},
};
MODULE_DEVICE_TABLE(of, of_lm3692x_leds_match);

static const struct dev_pm_ops lm3692x_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(lm3692x_suspend, lm3692x_resume)
};

static struct i2c_driver lm3692x_driver = {
	.driver = {
		.name	= "lm3692x",
		.pm    = &lm3692x_pm_ops,
		.of_match_table = of_lm3692x_leds_match,
	},
	.probe		= lm3692x_probe,
	.remove		= lm3692x_remove,
	.id_table	= lm3692x_id,
};
module_i2c_driver(lm3692x_driver);

MODULE_DESCRIPTION("Texas Instruments LM3692X LED driver");
MODULE_AUTHOR("Dan Murphy <dmurphy@ti.com>");
MODULE_LICENSE("GPL v2");
