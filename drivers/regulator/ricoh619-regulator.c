/*
 * drivers/regulator/ricoh619-regulator.c
 *
 * Regulator driver for RICOH R5T619 power management chip.
 *
 * Copyright (C) 2012-2014 RICOH COMPANY,LTD
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
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/err.h>
#include <linux/platform_device.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>
#include <linux/mfd/ricoh619.h>
#include <linux/regulator/ricoh619-regulator.h>

#include <linux/of.h>
#include <linux/regulator/of_regulator.h>
#include <linux/power/axp_depend.h>
#include "../power/supply/axp/axp-core.h"
#include "../power/supply/axp/axp-regulator.h"

#include "../../arch/arm/mach-imx/hardware.h"

#include "../../arch/arm/mach-sunxi/ntx_hwconfig.h"

struct ricoh61x_regulator {
	int		id;
	int		sleep_id;
	/* Regulator register address.*/
	u8		reg_en_reg;
	u8		en_bit;
	u8		reg_disc_reg;
	u8		disc_bit;
	u8		vout_reg;
	u8		vout_mask;
	u8		vout_reg_cache;
	u8		sleep_reg;
	u8		eco_reg;
	u8		eco_bit;
	u8		eco_slp_reg;
	u8		eco_slp_bit;

	/* chip constraints on regulator behavior */
	int			min_uV;
	int			max_uV;
	int			step_uV;
	int			nsteps;

	/* regulator specific turn-on delay */
	u16			delay;

	/* used by regulator core */
	struct regulator_desc	desc;

	/* Device */
	struct device		*dev;
};

extern volatile NTX_HWCONFIG *gptHWCFG;
extern int gSleep_Mode_Suspend;


static unsigned int ricoh61x_suspend_status;

static inline struct device *to_ricoh61x_dev(struct regulator_dev *rdev)
{
	return rdev_get_dev(rdev)->parent->parent;
}

static int ricoh61x_regulator_enable_time(struct regulator_dev *rdev)
{
	struct ricoh61x_regulator *ri = rdev_get_drvdata(rdev);

	return ri->delay;
}

static int ricoh61x_reg_is_enabled(struct regulator_dev *rdev)
{
	struct ricoh61x_regulator *ri = rdev_get_drvdata(rdev);
	struct device *parent = to_ricoh61x_dev(rdev);
	uint8_t control;
	int ret;

	ret = ricoh61x_read(parent, ri->reg_en_reg, &control);
	if (ret < 0) {
		dev_err(&rdev->dev, "Error in reading the control register\n");
		return ret;
	}
	return (((control >> ri->en_bit) & 1) == 1);
}

static int ricoh61x_reg_enable(struct regulator_dev *rdev)
{
	struct ricoh61x_regulator *ri = rdev_get_drvdata(rdev);
	struct device *parent = to_ricoh61x_dev(rdev);
	int ret;

	ret = ricoh61x_set_bits(parent, ri->reg_en_reg, (1 << ri->en_bit));
	if (ret < 0) {
		dev_err(&rdev->dev, "Error in updating the STATE register\n");
		return ret;
	}
	udelay(ri->delay);
	return ret;
}

static int ricoh61x_reg_disable(struct regulator_dev *rdev)
{
	struct ricoh61x_regulator *ri = rdev_get_drvdata(rdev);
	struct device *parent = to_ricoh61x_dev(rdev);
	int ret;

	ret = ricoh61x_clr_bits(parent, ri->reg_en_reg, (1 << ri->en_bit));
	if (ret < 0)
		dev_err(&rdev->dev, "Error in updating the STATE register\n");

	return ret;
}

static int ricoh61x_list_voltage(struct regulator_dev *rdev, unsigned index)
{
	struct ricoh61x_regulator *ri = rdev_get_drvdata(rdev);

	return ri->min_uV + (ri->step_uV * index);
}

static int __ricoh61x_set_s_voltage(struct device *parent,
		struct ricoh61x_regulator *ri, int min_uV, int max_uV)
{
	int vsel;
	int ret;

	if ((min_uV < ri->min_uV) || (max_uV > ri->max_uV))
		return -EDOM;

	vsel = (min_uV - ri->min_uV + ri->step_uV - 1)/ri->step_uV;
	if (vsel > ri->nsteps)
		return -EDOM;

	printk ("%s at %duv suspend\n", ri->desc.name, min_uV);
	ret = ricoh61x_update(parent, ri->sleep_reg, vsel, ri->vout_mask);
	if (ret < 0)
		dev_err(ri->dev, "Error in writing the sleep register\n");
	return ret;
}

static int __ricoh61x_set_voltage(struct device *parent,
		struct ricoh61x_regulator *ri, int min_uV, int max_uV,
		unsigned *selector)
{
	int vsel;
	int ret;
	uint8_t vout_val;

	if ((min_uV < ri->min_uV) || (max_uV > ri->max_uV))
		return -EDOM;

	printk ("%s() %s set to %duv \n", __func__, ri->desc.name, min_uV);
	vsel = (min_uV - ri->min_uV + ri->step_uV - 1)/ri->step_uV;
	if (vsel > ri->nsteps)
		return -EDOM;

	if (selector)
		*selector = vsel;

	vout_val = (ri->vout_reg_cache & ~ri->vout_mask) |
				(vsel & ri->vout_mask);
	ret = ricoh61x_write(parent, ri->vout_reg, vout_val);
	if (ret < 0)
		dev_err(ri->dev, "Error in writing the Voltage register\n");
	else
		ri->vout_reg_cache = vout_val;

	return ret;
}

static int ricoh61x_set_voltage(struct regulator_dev *rdev,
		int min_uV, int max_uV, unsigned *selector)
{
	struct ricoh61x_regulator *ri = rdev_get_drvdata(rdev);
	struct device *parent = to_ricoh61x_dev(rdev);

	if (ricoh61x_suspend_status)
		return -EBUSY;

	return __ricoh61x_set_voltage(parent, ri, min_uV, max_uV, selector);
}

static int ricoh619_set_suspend_voltage(struct regulator_dev *rdev,
		int uV)
{
	struct ricoh61x_regulator *ri = rdev_get_drvdata(rdev);
	struct device *parent = to_ricoh61x_dev(rdev);

	return __ricoh61x_set_s_voltage(parent, ri, uV, uV);
}

static int ricoh61x_get_voltage(struct regulator_dev *rdev)
{
	struct ricoh61x_regulator *ri = rdev_get_drvdata(rdev);
	uint8_t vsel;

	if (!ri->vout_reg_cache) {
		struct device *parent = to_ricoh61x_dev(rdev);
		ricoh61x_read(parent, ri->vout_reg, &ri->vout_reg_cache);
	}
	vsel = ri->vout_reg_cache & ri->vout_mask;
	return ri->min_uV + vsel * ri->step_uV;
}

int ricoh61x_regulator_enable_eco_mode(struct regulator_dev *rdev)
{
	struct ricoh61x_regulator *ri = rdev_get_drvdata(rdev);
	struct device *parent = to_ricoh61x_dev(rdev);
	int ret;

	ret = ricoh61x_set_bits(parent, ri->eco_reg, (1 << ri->eco_bit));
	if (ret < 0)
		dev_err(&rdev->dev, "Error Enable LDO eco mode\n");

	return ret;
}
EXPORT_SYMBOL_GPL(ricoh61x_regulator_enable_eco_mode);

int ricoh61x_regulator_disable_eco_mode(struct regulator_dev *rdev)
{
	struct ricoh61x_regulator *ri = rdev_get_drvdata(rdev);
	struct device *parent = to_ricoh61x_dev(rdev);
	int ret;

	ret = ricoh61x_clr_bits(parent, ri->eco_reg, (1 << ri->eco_bit));
	if (ret < 0)
		dev_err(&rdev->dev, "Error Disable LDO eco mode\n");

	return ret;
}
EXPORT_SYMBOL_GPL(ricoh61x_regulator_disable_eco_mode);

int ricoh61x_regulator_enable_eco_slp_mode(struct regulator_dev *rdev)
{
	struct ricoh61x_regulator *ri = rdev_get_drvdata(rdev);
	struct device *parent = to_ricoh61x_dev(rdev);
	int ret;

	ret = ricoh61x_set_bits(parent, ri->eco_slp_reg,
						(1 << ri->eco_slp_bit));
	if (ret < 0)
		dev_err(&rdev->dev, "Error Enable LDO eco mode in d during sleep\n");

	return ret;
}
EXPORT_SYMBOL_GPL(ricoh61x_regulator_enable_eco_slp_mode);

int ricoh61x_regulator_disable_eco_slp_mode(struct regulator_dev *rdev)
{
	struct ricoh61x_regulator *ri = rdev_get_drvdata(rdev);
	struct device *parent = to_ricoh61x_dev(rdev);
	int ret;

	ret = ricoh61x_clr_bits(parent, ri->eco_slp_reg,
						(1 << ri->eco_slp_bit));
	if (ret < 0)
		dev_err(&rdev->dev, "Error Enable LDO eco mode in d during sleep\n");

	return ret;
}
EXPORT_SYMBOL_GPL(ricoh61x_regulator_disable_eco_slp_mode);

static unsigned int ricoh619_dcdc_get_mode(struct regulator_dev *rdev)
{
	struct ricoh61x_regulator *ri = rdev_get_drvdata(rdev);
	struct device *parent = to_ricoh61x_dev(rdev);
	int ret;
	uint8_t control;
	u8 mask = 0x30;
	
	ret = ricoh61x_read(parent, ri->reg_en_reg,&control);
        if (ret < 0) {
                return ret;
        }
	control=(control & mask) >> 4;
	switch (control) {
	case 1:
		return REGULATOR_MODE_FAST;
	case 0:
		return REGULATOR_MODE_NORMAL;
	case 2:
		return REGULATOR_MODE_STANDBY;
	case 3:
		return REGULATOR_MODE_NORMAL;
	default:
		return -1;
	}

}
static int ricoh619_dcdc_set_mode(struct regulator_dev *rdev, unsigned int mode)
{
	struct ricoh61x_regulator *ri = rdev_get_drvdata(rdev);
	struct device *parent = to_ricoh61x_dev(rdev);
	int ret;
	uint8_t control;
	
	ret = ricoh61x_read(parent, ri->reg_en_reg,&control);
	switch(mode)
	{
	case REGULATOR_MODE_FAST:
		return ricoh61x_write(parent, ri->reg_en_reg, ((control & 0xcf) | 0x10));
	case REGULATOR_MODE_NORMAL:
		return ricoh61x_write(parent, ri->reg_en_reg, (control & 0xcf));
	case REGULATOR_MODE_STANDBY:
		return ricoh61x_write(parent, ri->reg_en_reg, ((control & 0xcf) | 0x20));	
	default:
		printk("error:pmu_619 only powersave pwm psm mode\n");
		return -EINVAL;
	}
	

}

static int ricoh619_dcdc_set_voltage_time_sel(struct regulator_dev *rdev,   unsigned int old_selector,
				     unsigned int new_selector)
{
	int old_volt, new_volt;
	
	old_volt = ricoh61x_list_voltage(rdev, old_selector);
	if (old_volt < 0)
		return old_volt;
	
	new_volt = ricoh61x_list_voltage(rdev, new_selector);
	if (new_volt < 0)
		return new_volt;

	return DIV_ROUND_UP(abs(old_volt - new_volt)*2, 14000);
}
static int ricoh619_dcdc_set_suspend_mode(struct regulator_dev *rdev, unsigned int mode)
{
	struct ricoh61x_regulator *ri = rdev_get_drvdata(rdev);
	struct device *parent = to_ricoh61x_dev(rdev);
	int ret;
	uint8_t control;
	
	ret = ricoh61x_read(parent, ri->reg_en_reg,&control);
	switch(mode)
	{
	case REGULATOR_MODE_FAST:
		return ricoh61x_write(parent, ri->reg_en_reg, ((control & 0x3f) | 0x40));
	case REGULATOR_MODE_NORMAL:
		return ricoh61x_write(parent, ri->reg_en_reg, (control & 0x3f));
	case REGULATOR_MODE_STANDBY:
		return ricoh61x_write(parent, ri->reg_en_reg, ((control & 0x3f) | 0x80));	
	default:
		printk("error:pmu_619 only powersave pwm psm mode\n");
		return -EINVAL;
	}
	

}
static int ricoh619_reg_suspend_enable(struct regulator_dev *rdev)
{
	struct ricoh61x_regulator *ri = rdev_get_drvdata(rdev);
	struct device *parent = to_ricoh61x_dev(rdev);
	int ret;

	ret = ricoh61x_set_bits(parent, (0x16 + ri->id), (0xf << 0));
	if (ret < 0) {
		dev_err(&rdev->dev, "Error in updating the STATE register\n");
		return ret;
	}
	udelay(ri->delay);
	return ret;
}

static int ricoh619_reg_suspend_disable(struct regulator_dev *rdev)
{
	struct ricoh61x_regulator *ri = rdev_get_drvdata(rdev);
	struct device *parent = to_ricoh61x_dev(rdev);
	int ret;

	ret = ricoh61x_clr_bits(parent, (0x16 + ri->id), (0xf <<0));
	if (ret < 0)
		dev_err(&rdev->dev, "Error in updating the STATE register\n");

	return ret;
}

static struct regulator_ops ricoh61x_ops = {
	.list_voltage	= ricoh61x_list_voltage,
	.set_voltage	= ricoh61x_set_voltage,
	.get_voltage	= ricoh61x_get_voltage,
	.set_suspend_voltage = ricoh619_set_suspend_voltage,
	.set_voltage_time_sel = ricoh619_dcdc_set_voltage_time_sel,
	.get_mode = ricoh619_dcdc_get_mode,
	.set_mode = ricoh619_dcdc_set_mode,
	.enable		= ricoh61x_reg_enable,
	.disable	= ricoh61x_reg_disable,
	.set_suspend_mode = ricoh619_dcdc_set_suspend_mode,
	.set_suspend_enable				= ricoh619_reg_suspend_enable,
	.set_suspend_disable				= ricoh619_reg_suspend_disable,
	.is_enabled	= ricoh61x_reg_is_enabled,
	.enable_time	= ricoh61x_regulator_enable_time,
};

#define RICOH61x_REG(_id, _en_reg, _en_bit, _disc_reg, _disc_bit, _vout_reg, \
		_vout_mask, _ds_reg, _min_mv, _max_mv, _step_uV, _nsteps,    \
		_ops, _delay, _eco_reg, _eco_bit, _eco_slp_reg, _eco_slp_bit) \
{								\
	.reg_en_reg	= _en_reg,				\
	.en_bit		= _en_bit,				\
	.reg_disc_reg	= _disc_reg,				\
	.disc_bit	= _disc_bit,				\
	.vout_reg	= _vout_reg,				\
	.vout_mask	= _vout_mask,				\
	.sleep_reg	= _ds_reg,				\
	.min_uV		= _min_mv * 1000,			\
	.max_uV		= _max_mv * 1000,			\
	.step_uV	= _step_uV,				\
	.nsteps		= _nsteps,				\
	.delay		= _delay,				\
	.id		= RICOH619_ID_##_id,			\
	.sleep_id	= RICOH619_DS_##_id,			\
	.eco_reg	=  _eco_reg,				\
	.eco_bit	=  _eco_bit,				\
	.eco_slp_reg	=  _eco_slp_reg,			\
	.eco_slp_bit	=  _eco_slp_bit,			\
	.desc = {						\
		.name = ricoh619_rails(_id),			\
		.id = RICOH619_ID_##_id,			\
		.n_voltages = _nsteps,				\
		.ops = &_ops,					\
		.type = REGULATOR_VOLTAGE,			\
		.owner = THIS_MODULE,				\
	},							\
}

static struct ricoh61x_regulator ricoh61x_regulator[] = {
	RICOH61x_REG(DC1, 0x2C, 0, 0x2C, 1, 0x36, 0xFF, 0x3B,
			600, 3500, 12500, 0xE8, ricoh61x_ops, 500,
			0x00, 0, 0x00, 0),

	RICOH61x_REG(DC2, 0x2E, 0, 0x2E, 1, 0x37, 0xFF, 0x3C,
			600, 3500, 12500, 0xE8, ricoh61x_ops, 500,
			0x00, 0, 0x00, 0),

	RICOH61x_REG(DC3, 0x30, 0, 0x30, 1, 0x38, 0xFF, 0x3D,
			600, 3500, 12500, 0xE8, ricoh61x_ops, 500,
			0x00, 0, 0x00, 0),

	RICOH61x_REG(DC4, 0x32, 0, 0x32, 1, 0x39, 0xFF, 0x3E,
			600, 3500, 12500, 0xE8, ricoh61x_ops, 500,
			0x00, 0, 0x00, 0),

	RICOH61x_REG(DC5, 0x34, 0, 0x34, 1, 0x3A, 0xFF, 0x3F,
			600, 3500, 12500, 0xE8, ricoh61x_ops, 500,
			0x00, 0, 0x00, 0),

	RICOH61x_REG(LDO1, 0x44, 0, 0x46, 0, 0x4C, 0x7F, 0x58,
			900, 3500, 25000, 0x68, ricoh61x_ops, 500,
			0x48, 0, 0x4A, 0),

	RICOH61x_REG(LDO2, 0x44, 1, 0x46, 1, 0x4D, 0x7F, 0x59,
			900, 3500, 25000, 0x68, ricoh61x_ops, 500,
			0x48, 1, 0x4A, 1),

	RICOH61x_REG(LDO3, 0x44, 2, 0x46, 2, 0x4E, 0x7F, 0x5A,
			900, 3500, 25000, 0x68, ricoh61x_ops, 500,
			0x48, 2, 0x4A, 2),

	RICOH61x_REG(LDO4, 0x44, 3, 0x46, 3, 0x4F, 0x7F, 0x5B,
			900, 3500, 25000, 0x68, ricoh61x_ops, 500,
			0x48, 3, 0x4A, 3),

	RICOH61x_REG(LDO5, 0x44, 4, 0x46, 4, 0x50, 0x7F, 0x5C,
			600, 3500, 25000, 0x74, ricoh61x_ops, 500,
			0x48, 4, 0x4A, 4),

	RICOH61x_REG(LDO6, 0x44, 5, 0x46, 5, 0x51, 0x7F, 0x5D,
			600, 3500, 25000, 0x74, ricoh61x_ops, 500,
			0x48, 5, 0x4A, 5),

	RICOH61x_REG(LDO7, 0x44, 6, 0x46, 6, 0x52, 0x7F, 0x5E,
			900, 3500, 25000, 0x68, ricoh61x_ops, 500,
			0x00, 0, 0x00, 0),

	RICOH61x_REG(LDO8, 0x44, 7, 0x46, 7, 0x53, 0x7F, 0x5F,
			900, 3500, 25000, 0x68, ricoh61x_ops, 500,
			0x00, 0, 0x00, 0),

	RICOH61x_REG(LDO9, 0x45, 0, 0x47, 0, 0x54, 0x7F, 0x60,
			900, 3500, 25000, 0x68, ricoh61x_ops, 500,
			0x00, 0, 0x00, 0),

	RICOH61x_REG(LDO10, 0x45, 1, 0x47, 1, 0x55, 0x7F, 0x61,
			900, 3500, 25000, 0x68, ricoh61x_ops, 500,
			0x00, 0, 0x00, 0),

	RICOH61x_REG(LDORTC1, 0x45, 4, 0x00, 0, 0x56, 0x7F, 0x00,
			1700, 3500, 25000, 0x48, ricoh61x_ops, 500,
			0x00, 0, 0x00, 0),

	RICOH61x_REG(LDORTC2, 0x45, 5, 0x00, 0, 0x57, 0x7F, 0x00,
			900, 3500, 25000, 0x68, ricoh61x_ops, 500,
			0x00, 0, 0x00, 0),
};
static inline struct ricoh61x_regulator *find_regulator_info(int id)
{
	struct ricoh61x_regulator *ri;
	int i;

	for (i = 0; i < ARRAY_SIZE(ricoh61x_regulator); i++) {
		ri = &ricoh61x_regulator[i];
		if (ri->desc.id == id)
			return ri;
	}
	return NULL;
}

#if 0
static int ricoh61x_regulator_preinit(struct device *parent,
		struct ricoh61x_regulator *ri,
		struct ricoh619_regulator_platform_data *ricoh61x_pdata)
{
	int ret = 0;

	if (!ricoh61x_pdata->init_apply)
		return 0;

	if (ricoh61x_pdata->init_uV >= 0) {
		ret = __ricoh61x_set_voltage(parent, ri,
				ricoh61x_pdata->init_uV,
				ricoh61x_pdata->init_uV, 0);
		if (ret < 0) {
			dev_err(ri->dev, "Not able to initialize voltage %d "
				"for rail %d err %d\n", ricoh61x_pdata->init_uV,
				ri->desc.id, ret);
			return ret;
		}
		if (ricoh61x_pdata->sleep_uV <= 0) 
			ricoh61x_pdata->sleep_uV = ricoh61x_pdata->init_uV;
	}

	if (ricoh61x_pdata->sleep_uV > 0) {
		int vReg = ri->min_uV + (ri->vout_reg_cache & ri->vout_mask) * ri->step_uV;

		if (3300*1000 == vReg)
			ret = __ricoh61x_set_s_voltage(parent, ri, vReg,vReg);
		else
			ret = __ricoh61x_set_s_voltage(parent, ri,
				ricoh61x_pdata->sleep_uV,
				ricoh61x_pdata->sleep_uV);
		if (ret < 0) {
			dev_err(ri->dev, "Not able to initialize sleep voltage %d "
				"for rail %d err %d\n", ricoh61x_pdata->sleep_uV,
				ri->desc.id, ret);
			return ret;
		}
	}

	if (ricoh61x_pdata->init_enable) {
		if (RICOH619_ID_DC1 <= ri->id && RICOH619_ID_DC5 >= ri->id) {
			ret = ricoh61x_set_bits(parent, ri->reg_en_reg,
										((1 << ri->en_bit)|0x80));
		}
		else
			ret = ricoh61x_set_bits(parent, ri->reg_en_reg,
								(1 << ri->en_bit));
	}
	else
		ret = ricoh61x_clr_bits(parent, ri->reg_en_reg,
							(1 << ri->en_bit));
	if (ret < 0)
		dev_err(ri->dev, "Not able to %s rail %d err %d\n",
			(ricoh61x_pdata->init_enable) ? "enable" : "disable",
			ri->desc.id, ret);

	return ret;
}
#endif

static inline int ricoh61x_cache_regulator_register(struct device *parent,
	struct ricoh61x_regulator *ri)
{
	ri->vout_reg_cache = 0;
	return ricoh61x_read(parent, ri->vout_reg, &ri->vout_reg_cache);
}

#ifdef CONFIG_OF
static struct of_regulator_match ricoh619_regulator_matches[] = {
	{ .name	= "ricoh619_dc1",},
	{ .name = "ricoh619_dc2",},
	{ .name = "ricoh619_dc3",},
	{ .name = "ricoh619_dc4",},
	{ .name = "ricoh619_dc5",},
	{ .name = "ricoh619_ldo1",},
	{ .name = "ricoh619_ldo2",},
	{ .name = "ricoh619_ldo3",},
	{ .name = "ricoh619_ldo4",},
	{ .name = "ricoh619_ldo5",},
	{ .name = "ricoh619_ldo6",},
	{ .name = "ricoh619_ldo7",},
	{ .name = "ricoh619_ldo8",},
	{ .name = "ricoh619_ldo9",},
	{ .name = "ricoh619_ldo10",},
	{ .name = "ricoh619_ldortc1",},
	{ .name = "ricoh619_ldortc2",},
};
#endif

enum AXP_REGLS {
	VCC_DCDC1,
	VCC_DCDC2,
	VCC_DCDC3,
	VCC_DCDC4,
	VCC_DCDC5,
	VCC_LDO1,
	VCC_LDO2,
	VCC_LDO3,
	VCC_LDO4,
	VCC_LDO5,
	VCC_LDO6,
	VCC_LDO7,
	VCC_LDO8,
	VCC_LDO9,
	VCC_LDO10,
	VCC_LDORTC1,
	VCC_LDORTC2,
	VCC_RICOH619_MAX,
};

static struct regulator_consumer_supply ricoh61x_dc1_consumers[] = {
	/* ricoh619_dc1 none vdd-cpua vdd-cpub */
	REGULATOR_SUPPLY( "vcc-cpua", NULL),
	REGULATOR_SUPPLY( "vcc-cpub", NULL),
	REGULATOR_SUPPLY( "vcc-pl", NULL),
};

static struct regulator_consumer_supply ricoh61x_dc2_consumers[] = {
	/* ricoh619_dc2 none vdd-cpus */
	REGULATOR_SUPPLY( "vdd-cpus", NULL),
};

static struct regulator_consumer_supply ricoh61x_dc3_consumers[] = {
	/* ricoh619_dc3 none vcc-io vcc-pe vcc-3v3 vcc-emmc vcc-sensor vcc-rtc vcc-ctp */
	REGULATOR_SUPPLY( "vcc-io", NULL),
	REGULATOR_SUPPLY( "vcc-pe", NULL),
	REGULATOR_SUPPLY( "vcc-3v3", NULL),
	REGULATOR_SUPPLY( "vcc-emmc", NULL),
	REGULATOR_SUPPLY( "vcc-sensor", NULL),
	REGULATOR_SUPPLY( "vcc-rtc", NULL),
	REGULATOR_SUPPLY( "vcc-ctp", NULL),
};
	
static struct regulator_consumer_supply ricoh61x_dc4_consumers[] = {
	/* ricoh619_dc4 none vdd-sys vcc-usb vdd-gpu vdd-usb */
	REGULATOR_SUPPLY( "vdd-sys", NULL),
	REGULATOR_SUPPLY( "vcc-usb", NULL),
	REGULATOR_SUPPLY( "vdd-gpu", NULL),
	REGULATOR_SUPPLY( "vdd-usb", NULL),
};
	
static struct regulator_consumer_supply ricoh61x_dc5_consumers[] = {
	/* ricoh619_dc5 none vcc-dram vcc-1v1 vcc-1v2 vcc-1v5 vcc-1v35 */
	REGULATOR_SUPPLY( "vcc-dram", NULL),
	REGULATOR_SUPPLY( "vcc-1v1", NULL),
	REGULATOR_SUPPLY( "vcc-1v2", NULL),
	REGULATOR_SUPPLY( "vcc-1v5", NULL),
	REGULATOR_SUPPLY( "vcc-1v35", NULL),
};
	
static struct regulator_consumer_supply ricoh61x_ldo1_consumers[] = {
	/* ricoh619_ldo1 none vcc-wifi vcc-pg vcc-wifi-io */
	REGULATOR_SUPPLY("vcc-wifi", NULL),
	REGULATOR_SUPPLY("vcc-pg", NULL),
	REGULATOR_SUPPLY("vcc-wifi-io", NULL),
};

static struct regulator_consumer_supply ricoh61x_ldo2_consumers[] = {
	/* ricoh619_ldo2 none vdd18-dram vdd18-lpddr */
	REGULATOR_SUPPLY( "vdd18-dram", NULL),
	REGULATOR_SUPPLY( "vdd18-lpddr", NULL),
};
	
static struct regulator_consumer_supply ricoh61x_ldo4_consumers[] = {
	/* ricoh619_ldo4 none vcc-efuse vcc-lvds vdd-dsi vcc-cpvin vcc-mcsi */
	REGULATOR_SUPPLY( "vcc-efuse", NULL),
	REGULATOR_SUPPLY( "vcc-lvds", NULL),
	REGULATOR_SUPPLY( "vdd-dsi", NULL),
	REGULATOR_SUPPLY( "vcc-cpvin", NULL),
	REGULATOR_SUPPLY( "vcc-mcsi", NULL),
};

static struct regulator_consumer_supply ricoh61x_ldo5_consumers[] = {
	/* ricoh619_ldo5 none vcc-pc vcc-pd vcc-pf vcc-sdc vcc-pmic0 vcc-lcd vccio-dcxo vccio-wifi */
	REGULATOR_SUPPLY("vcc-pc", NULL),
	REGULATOR_SUPPLY("vcc-pd", NULL),
	REGULATOR_SUPPLY("vcc-pf", NULL),
	REGULATOR_SUPPLY("vcc-sdc", NULL),
	REGULATOR_SUPPLY("vcc-pmic0", NULL),
	REGULATOR_SUPPLY("vcc-lcd", NULL),
	REGULATOR_SUPPLY("vcc-dcxo", NULL),
};

static struct regulator_consumer_supply ricoh61x_ldo9_consumers[] = {
	/* ricoh619_ldo9 none avcc vcc-pll */
	REGULATOR_SUPPLY( "avcc", NULL),
	REGULATOR_SUPPLY( "vcc-pll", NULL),
};

static struct regulator_consumer_supply ricoh61x_ldo10_consumers[] = {
	/* ricoh619_ldo10 none vcc-pl */
	/* REGULATOR_SUPPLY( "vcc-pl", NULL),*/
};

static struct regulator_init_data axp_regl_init_data[] = {
	[VCC_DCDC1] = {
		.constraints = {
			.name = "ricoh61x_dc1",
			.min_uV = 600000,
			.max_uV = 1200000,
			.boot_on = 1,
			.always_on = 1,
			.valid_ops_mask = REGULATOR_CHANGE_VOLTAGE
				| REGULATOR_CHANGE_STATUS,
		},
		.num_consumer_supplies = ARRAY_SIZE(ricoh61x_dc1_consumers),
		.consumer_supplies = ricoh61x_dc1_consumers,
	},
	[VCC_DCDC2] = {
		.constraints = {
			.name = "ricoh61x_dc2",
			.min_uV = 600000,
			.max_uV = 1100000,
			.boot_on = 1,
			.always_on = 1,
			.valid_ops_mask = REGULATOR_CHANGE_VOLTAGE
				| REGULATOR_CHANGE_STATUS,
		},
		.num_consumer_supplies = ARRAY_SIZE(ricoh61x_dc2_consumers),
		.consumer_supplies = ricoh61x_dc2_consumers,
	},
	[VCC_DCDC3] = {
		.constraints = {
			.name = "ricoh61x_dc3",
			.min_uV =  600000,
			.max_uV = 3300000,
			.boot_on = 1,
			.always_on = 1,
			.valid_ops_mask = REGULATOR_CHANGE_VOLTAGE
				| REGULATOR_CHANGE_STATUS,
		},
		.num_consumer_supplies = ARRAY_SIZE(ricoh61x_dc3_consumers),
		.consumer_supplies = ricoh61x_dc3_consumers,
	},
	[VCC_DCDC4] = {
		.constraints = {
			.name = "ricoh61x_dc4",
			.min_uV = 600000,
			.max_uV = 1200000,
			.valid_ops_mask = REGULATOR_CHANGE_VOLTAGE
				| REGULATOR_CHANGE_STATUS,
		},
		.num_consumer_supplies = ARRAY_SIZE(ricoh61x_dc4_consumers),
		.consumer_supplies = ricoh61x_dc4_consumers,
	},
	[VCC_DCDC5] = {
		.constraints = {
			.name = "ricoh61x_dc5",
			.min_uV = 1100000,
			.max_uV = 1300000,
			.boot_on = 1,
			.always_on = 1,
			.valid_ops_mask = REGULATOR_CHANGE_VOLTAGE
				| REGULATOR_CHANGE_STATUS,
		},
		.num_consumer_supplies = ARRAY_SIZE(ricoh61x_dc5_consumers),
		.consumer_supplies = ricoh61x_dc5_consumers,
	},
	[VCC_LDO1] = {
		.constraints = {
			.name = "ricoh61x_ldo1",
			.min_uV = 2500000,
			.max_uV = 3300000,
			.boot_on = 1,
			.always_on = 1,
			.valid_ops_mask = REGULATOR_CHANGE_VOLTAGE
				| REGULATOR_CHANGE_STATUS,
		},
		.num_consumer_supplies = ARRAY_SIZE(ricoh61x_ldo1_consumers),
		.consumer_supplies = ricoh61x_ldo1_consumers,
	},
	[VCC_LDO2] = {
		.constraints = {
			.name = "ricoh61x_ldo2",
			.min_uV =  900000,
			.max_uV = 1875000,
			.boot_on = 1,
			.always_on = 1,
			.valid_ops_mask = REGULATOR_CHANGE_VOLTAGE
				| REGULATOR_CHANGE_STATUS,
		},
		.num_consumer_supplies = ARRAY_SIZE(ricoh61x_ldo2_consumers),
		.consumer_supplies = ricoh61x_ldo2_consumers,
	},
	[VCC_LDO3] = {
		.constraints = {
			.name = "ricoh61x_ldo3",
			.min_uV = 900000,
			.max_uV = 900000,
			.boot_on = 1,
			.always_on = 1,
			.valid_ops_mask = REGULATOR_CHANGE_VOLTAGE
				| REGULATOR_CHANGE_STATUS,
		},
	},
	[VCC_LDO4] = {
		.constraints = {
			.name = "ricoh61x_ldo4",
			.min_uV =  900000,
			.max_uV = 1800000,
			.boot_on = 1,
			.always_on = 1,
			.valid_ops_mask = REGULATOR_CHANGE_VOLTAGE
				| REGULATOR_CHANGE_STATUS,
		},
		.num_consumer_supplies = ARRAY_SIZE(ricoh61x_ldo4_consumers),
		.consumer_supplies = ricoh61x_ldo4_consumers,
	},
	[VCC_LDO5] = {
		.constraints = {
			.name = "ricoh61x_ldo5",
			.min_uV =  900000,
			.max_uV = 3300000,
			.boot_on = 1,
			.always_on = 1,
			.valid_ops_mask = REGULATOR_CHANGE_VOLTAGE
				| REGULATOR_CHANGE_STATUS,
		},
		.num_consumer_supplies = ARRAY_SIZE(ricoh61x_ldo5_consumers),
		.consumer_supplies = ricoh61x_ldo5_consumers,
	},
	[VCC_LDO6] = {
		.constraints = {
			.name = "ricoh61x_ldo6",
			.min_uV = 600000,
			.max_uV = 600000,
			.boot_on = 1,
			.always_on = 1,
			.valid_ops_mask = REGULATOR_CHANGE_VOLTAGE
				| REGULATOR_CHANGE_STATUS,
		},
	},
	[VCC_LDO7] = {
		.constraints = {
			.name = "ricoh61x_ldo7",
			.min_uV =  900000,
			.max_uV = 3300000,
			.boot_on = 1,
			.always_on = 1,
			.valid_ops_mask = REGULATOR_CHANGE_VOLTAGE
				| REGULATOR_CHANGE_STATUS,
		},
	},
	[VCC_LDO8] = {
		.constraints = {
			.name = "ricoh61x_ldo8",
			.min_uV =  900000,
			.max_uV = 1800000,
			.boot_on = 1,
			.always_on = 1,
			.valid_ops_mask = REGULATOR_CHANGE_VOLTAGE
				| REGULATOR_CHANGE_STATUS,
		},
	},
	[VCC_LDO9] = {
		.constraints = {
			.name = "ricoh61x_ldo9",
			.min_uV =  900000,
			.max_uV = 1800000,
			.boot_on = 1,
			.always_on = 1,
			.valid_ops_mask = REGULATOR_CHANGE_VOLTAGE
				| REGULATOR_CHANGE_STATUS,
		},
		.num_consumer_supplies = ARRAY_SIZE(ricoh61x_ldo9_consumers),
		.consumer_supplies = ricoh61x_ldo9_consumers,
	},
	[VCC_LDO10] = {
		.constraints = {
			.name = "ricoh61x_ldo10",
			.min_uV =  900000,
			.max_uV = 3300000,
			.boot_on = 1,
			.always_on = 1,
			.valid_ops_mask = REGULATOR_CHANGE_VOLTAGE
				| REGULATOR_CHANGE_STATUS,
		},
		.num_consumer_supplies = ARRAY_SIZE(ricoh61x_ldo10_consumers),
		.consumer_supplies = ricoh61x_ldo10_consumers,
	},
	[VCC_LDORTC1] = {
		.constraints = {
			.name = "ricoh61x_ldortc1",
			.min_uV = 1700000,
			.max_uV = 3300000,
			.valid_ops_mask = REGULATOR_CHANGE_VOLTAGE
				| REGULATOR_CHANGE_STATUS,
		},
	},
	[VCC_LDORTC2] = {
		.constraints = {
			.name = "ricoh61x_ldortc2",
			.min_uV =  900000,
			.max_uV = 3300000,
			.valid_ops_mask = REGULATOR_CHANGE_VOLTAGE
				| REGULATOR_CHANGE_STATUS,
		},
	},
};

#if 0
static s32 ricoh61x_regu_dependence(const char *ldo_name)
{
	s32 ricoh61x_dependence = 0;

	if (strstr(ldo_name, "dc1") != NULL)
		ricoh61x_dependence |= AXP858_1506_DCDC1;
	else if (strstr(ldo_name, "dc2") != NULL)
		ricoh61x_dependence |= AXP858_1506_CPUS;
	else if (strstr(ldo_name, "dc3") != NULL)
		ricoh61x_dependence |= AXP858_1506_DCDC3;
	else if (strstr(ldo_name, "dc4") != NULL)
		ricoh61x_dependence |= AXP858_1506_DCDC4;
	else if (strstr(ldo_name, "dc5") != NULL)
		ricoh61x_dependence |= AXP858_1506_DCDC5;
	else if (strstr(ldo_name, "ldo1") != NULL)
		ricoh61x_dependence |= AXP858_1506_ALDO1;
	else if (strstr(ldo_name, "ldo2") != NULL)
		ricoh61x_dependence |= AXP858_1506_ALDO2;
	else if (strstr(ldo_name, "ldo3") != NULL)
		ricoh61x_dependence |= AXP858_1506_ALDO3;
	else if (strstr(ldo_name, "ldo4") != NULL)
		ricoh61x_dependence |= AXP858_1506_ALDO4;
	else if (strstr(ldo_name, "ldo5") != NULL)
		ricoh61x_dependence |= AXP858_1506_ALDO5;
	else if (strstr(ldo_name, "ldo6") != NULL)
		ricoh61x_dependence |= AXP858_1506_BLDO1;
	else if (strstr(ldo_name, "ldo7") != NULL)
		ricoh61x_dependence |= AXP858_1506_BLDO2;
	else if (strstr(ldo_name, "ldo8") != NULL)
		ricoh61x_dependence |= AXP858_1506_BLDO3;
	else if (strstr(ldo_name, "ldo9") != NULL)
		ricoh61x_dependence |= AXP858_1506_BLDO4;
	else if (strstr(ldo_name, "ldo10") != NULL)
		ricoh61x_dependence |= AXP858_1506_BLDO5;
	else if (strstr(ldo_name, "ldortc1") != NULL)
		ricoh61x_dependence |= AXP858_1506_CLDO1;
	else if (strstr(ldo_name, "ldortc2") != NULL)
		ricoh61x_dependence |= AXP858_1506_CLDO2;
	else
		return -1;

	return ricoh61x_dependence;
}
#endif

#ifdef CONFIG_OF
static int ricoh619_regulator_dt_init(struct platform_device *pdev,
				    struct regulator_config *config,
				    int regidx)
{
	struct device_node *np;
	int rcount;

	np = of_find_node_by_name(NULL, "regulators");
	if (!np) {
		dev_err(&pdev->dev, "failed to find regulators node\n");
		return -ENODEV;
	}

	rcount = of_regulator_match(&pdev->dev, np,
				&ricoh619_regulator_matches[regidx], 1);
	of_node_put(np);
	if (rcount < 0)
		return -ENODEV;
	config->init_data = ricoh619_regulator_matches[regidx].init_data;
	config->of_node = ricoh619_regulator_matches[regidx].of_node;

	return 0;
}
#else
#define ricoh619_regulator_dt_init(x, y, z)	(-1)
#endif

static int ricoh61x_regulator_probe(struct platform_device *pdev)
{
#ifdef CONFIG_OF
	struct ricoh61x_regulator *ri = NULL;
	struct regulator_dev *rdev;
	struct regulator_config config = { };
	int err,id=0;
	struct device_node *ricoh61x_reg_np;
	
	printk(KERN_INFO "PMU: %s\n", __func__);
	if (pdev->dev.of_node) 
		ricoh61x_reg_np = pdev->dev.of_node;
	else {
		ricoh61x_reg_np = of_find_compatible_node(NULL, NULL,"ricoh61x-regulator");
	}

#if 0
	if (ricoh61x_reg_np) {
		int ret;
		ret = axp_regulator_dt_parse(ricoh61x_reg_np,
					axp_regl_init_data,
					ricoh61x_regu_dependence);
		if (ret) {
			pr_err("%s parse device tree err\n", __func__);
			return -EINVAL;
		}
	} else {
		pr_err("ricoh61x regulator device tree err!\n");
		return -EBUSY;
	}
#endif

	rdev = devm_kzalloc(&pdev->dev, RICOH619_NUM_REGULATOR *
				sizeof(*rdev), GFP_KERNEL);
	if (!rdev) {
		dev_err(&pdev->dev, "Mmemory alloc failed\n");
		return -ENOMEM;
	}

	for (id = 0; id < RICOH619_NUM_REGULATOR; ++id) {
		ri = find_regulator_info(id);
		if (!ri) {
			dev_err(&pdev->dev, "invalid regulator ID %d specified\n", id);
			err = -EINVAL;
		}
		else
			ri->dev = &pdev->dev;
		config.dev = &pdev->dev;
		config.driver_data = ri;

		config.of_node = ricoh619_regulator_matches[id].of_node;

		err = ricoh619_regulator_dt_init(pdev, &config, id);
		if (err < 0) {
			config.init_data = &axp_regl_init_data[id];
			dev_err(&pdev->dev, "failed to regulator dt init\n");
		}
		else {
			ricoh619_regulator_matches[id].init_data->num_consumer_supplies = axp_regl_init_data[id].num_consumer_supplies;
			ricoh619_regulator_matches[id].init_data->consumer_supplies = axp_regl_init_data[id].consumer_supplies;
		}

#if 1
		//if(NTXHWCFG_TST_FLAG(gptHWCFG->m_val.bPCB_Flags2,5)) // VDDR_INC 
		if(gptHWCFG->m_val.bPCB>=100) // PCBA newer than E60P5X .
		{
			if(4==id)
			{
				// dcdc5 dram 1v1 .
				printk("[%d]regulator name=\"%s\"\n",id,ricoh619_regulator_matches[id].name);
				config.init_data->constraints.min_uV = 1162500;
				config.init_data->constraints.max_uV = 1162500;
				printk("changed %d uV ~ %d uV\n",
					config.init_data->constraints.min_uV,config.init_data->constraints.max_uV);
			}
		}
#endif 

#if 1
		if(100==gptHWCFG->m_val.bPCB) {
			// E60P5X 
			if(1==gptHWCFG->m_val.bDisplayResolution) {
				// 1024x758 .
				if(12==id) {
					// ldo8 .
					printk("[%d]regulator name=\"%s\"\n",id,ricoh619_regulator_matches[id].name);
					config.init_data->constraints.min_uV = 3300000;
					config.init_data->constraints.max_uV = 3300000;
					printk("changed %d uV ~ %d uV\n",
					config.init_data->constraints.min_uV,config.init_data->constraints.max_uV);
				}
			}
		}
#endif

		rdev = regulator_register(&ri->desc, &config);
		if (IS_ERR_OR_NULL(rdev)) {
			dev_err(&pdev->dev, "failed to register regulator %s\n",
					ri->desc.name);
			return PTR_ERR(rdev);
		}
		{	// reset sleep slot to avoid system cannot enter suspend if reset in hibernation.
			int offset = 0x16 + id;
			unsigned char temp;
			ricoh61x_read(to_ricoh61x_dev(rdev), offset, &temp);
			temp |= 0x0F;
			ricoh61x_write(to_ricoh61x_dev(rdev), offset, temp);
		}
	}

	platform_set_drvdata(pdev, rdev);
#else
	struct ricoh61x_regulator *ri = NULL;
	struct regulator_dev *rdev;
	struct ricoh619_regulator_platform_data *tps_pdata;
	int id = pdev->id;
	int err;

//	printk(KERN_INFO "PMU: %s\n", __func__);
	ri = find_regulator_info(id);
	if (ri == NULL) {
		dev_err(&pdev->dev, "invalid regulator ID specified\n");
		return -EINVAL;
	}
	tps_pdata = pdev->dev.platform_data;
	ri->dev = &pdev->dev;
	ricoh61x_suspend_status = 0;

	err = ricoh61x_cache_regulator_register(pdev->dev.parent, ri);
	if (err) {
		dev_err(&pdev->dev, "Fail in caching register\n");
		return err;
	}

	err = ricoh61x_regulator_preinit(pdev->dev.parent, ri, tps_pdata);
	if (err) {
		dev_err(&pdev->dev, "Fail in pre-initialisation\n");
		return err;
	}
	rdev = regulator_register(&ri->desc, &pdev->dev,
				&tps_pdata->regulator, ri);
	if (IS_ERR_OR_NULL(rdev)) {
		dev_err(&pdev->dev, "failed to register regulator %s\n",
				ri->desc.name);
		return PTR_ERR(rdev);
	}

	platform_set_drvdata(pdev, rdev);
#endif

#if 0
	if (3 == gptHWCFG->m_val.bTouch2Ctrl) { //Digitizer
		if (RICOH619_ID_LDO1 == id) {
			printk("[%s-%d] ID=%d: eco_slp_reg=%2X, setting to 0\n", __func__, __LINE__, id, ri-> eco_slp_reg);
			ri->eco_slp_reg=0;
		}
	}
#endif
	
	if (ri->eco_slp_reg) 
		ricoh61x_regulator_enable_eco_slp_mode (rdev);

	return 0;
}

static int ricoh61x_regulator_remove(struct platform_device *pdev)
{
	struct regulator_dev *rdev = platform_get_drvdata(pdev);

	regulator_unregister(rdev);
	return 0;
}

#ifdef CONFIG_PM

static uint8_t regulator_slot[RICOH619_ID_LDORTC2+1]; 
static int ricoh61x_regulator_suspend(struct device *dev)
{
	struct regulator_dev *rdev = platform_get_drvdata(to_platform_device(dev));
//	struct ricoh61x_regulator *ri = rdev_get_drvdata(rdev);
//	struct ricoh619_regulator_platform_data *ricoh61x_pdata = ((struct device*)(ri->dev))->platform_data;
	uint8_t temp;
	int reg_id;

//	printk ("[%s-%d] ...\n", __func__, __LINE__);
	for (reg_id=RICOH619_ID_DC1; reg_id<=RICOH619_ID_LDORTC2; reg_id++) {
		int offset = 0x16 + (reg_id - RICOH619_ID_DC1);
		ricoh61x_read(to_ricoh61x_dev(rdev), offset, &regulator_slot[reg_id]);
		temp = (regulator_slot[reg_id] & 0xF0) | 0x0E;
		if (gSleep_Mode_Suspend ) 
		{
			switch (reg_id) {
			case RICOH619_ID_DC1:	// core2_0v9_CPU
			// ricoh619_dc1 none vdd-cpua vdd-cpub
				ricoh61x_write(to_ricoh61x_dev(rdev), offset, temp);
				break;
			case RICOH619_ID_DC2:	// Core1_0V9_CPUSLDO
			// ricoh619_dc2 none vdd-cpus
				ricoh61x_write(to_ricoh61x_dev(rdev), 0x2E, 0x83); // set psm mode in sleep mode
				ricoh61x_write(to_ricoh61x_dev(rdev), 0x2F, 0x03);
				break;
			case RICOH619_ID_DC3:	// Core2_3V3
#if 0
			// ricoh619_dc3 none vcc-io vcc-pe vcc-3v3 vcc-emmc vcc-sensor vcc-rtc vcc-ctp
				ricoh61x_write(to_ricoh61x_dev(rdev), 0x30, 0x83); // set psm mode in sleep mode
				ricoh61x_write(to_ricoh61x_dev(rdev), 0x31, 0x03);
#else
				ricoh61x_write(to_ricoh61x_dev(rdev), offset, temp);
#endif
			break;

			case RICOH619_ID_DC4:	
				switch(gptHWCFG->m_val.bPCB)
				{
					case 89 :// TA0P04
						// core1_0V9_SYS old design . 
						// ricoh619_dc4 none vdd-sys vcc-usb vdd-gpu vdd-usb
						ricoh61x_write(to_ricoh61x_dev(rdev), offset, temp);
						break;
					default:
						// core2_1v8_dram for new design . 
						ricoh61x_write(to_ricoh61x_dev(rdev), 0x32, 0x83); // set psm mode in sleep mode
						break;
				}
				
				break;
			case RICOH619_ID_DC5:	// core1_1V2
			// ricoh619_dc5 none vcc-dram vcc-1v1 vcc-1v2 vcc-1v5 vcc-1v35
				ricoh61x_write(to_ricoh61x_dev(rdev), 0x34, 0x83); // set psm mode in sleep mode
				ricoh61x_write(to_ricoh61x_dev(rdev), 0x35, 0x03);
				break;

			// ricoh619_ldo1 none vcc-wifi vcc-pg vcc-wifi-io 
			// ricoh619_ldo2 none vdd18-dram vdd18-lpddr
			// ricoh619_ldo3 none
			// ricoh619_ldo4 none vcc-efuse vcc-lvds vdd-dsi vcc-cpvin vcc-mcsi
			// ricoh619_ldo5 none vcc-pc vcc-pd vcc-pf vcc-sdc vcc-pmic0 vcc-lcd vccio-dcxo vccio-wifi
			// ricoh619_ldo6 none
			// ricoh619_ldo7 none
			// ricoh619_ldo8 none
			// ricoh619_ldo9 none avcc vcc-pll
			// ricoh619_ldo10 none vcc-pl

			case RICOH619_ID_LDO2:	
				switch(gptHWCFG->m_val.bPCB)
				{
					case 89 :// TA0P04
						// core2_1V8_dram for old design . 
						break;
					default:
						// core1_0V9_SYS
						ricoh61x_write(to_ricoh61x_dev(rdev), offset, temp);
						break;
				}
				break;
			case RICOH619_ID_LDO10:	// 
				switch(gptHWCFG->m_val.bPCB)
				{
					case 89 :// TA0P04
						//Core2_3V3_VCC_PL for old design . 
						break;
					default:
						ricoh61x_write(to_ricoh61x_dev(rdev), offset, temp);
						break;
				}
				break;
			case RICOH619_ID_LDO1:	// Core2_3V3_VCC_PL
			// keep output.
				break;
			case RICOH619_ID_LDO3:	// CORE2_0V9
			case RICOH619_ID_LDO4:	// Core2_1V8_LVDS
			case RICOH619_ID_LDO5:	// SPD_3V3
			case RICOH619_ID_LDO6:	// DDR_0V6
			case RICOH619_ID_LDO7:	// FL_3V3
			case RICOH619_ID_LDO8:	// EP_1V8
			case RICOH619_ID_LDO9:	// Core2_1V8_AVCC
				ricoh61x_write(to_ricoh61x_dev(rdev), offset, temp);
				break;
			}
		}
		else {
			switch (reg_id) {
			case RICOH619_ID_DC1:	// core2_0v9_CPU
			// ricoh619_dc1 none vdd-cpua vdd-cpub
				ricoh61x_write(to_ricoh61x_dev(rdev), offset, temp);
				break;
			case RICOH619_ID_DC2:	// Core1_0V9_CPUSLDO
			// ricoh619_dc2 none vdd-cpus
				ricoh61x_write(to_ricoh61x_dev(rdev), 0x2E, 0x83); // set psm mode in sleep mode
				ricoh61x_write(to_ricoh61x_dev(rdev), 0x2F, 0x03);
				break;
			case RICOH619_ID_DC3:	// Core2_3V3
			break;
			case RICOH619_ID_DC4:	
				switch(gptHWCFG->m_val.bPCB)
				{
					case 89: // TA0P04
						// core1_0V9_SYS
						// ricoh619_dc4 none vdd-sys vcc-usb vdd-gpu vdd-usb
						ricoh61x_write(to_ricoh61x_dev(rdev), offset, temp);
						break;
					default: // core1_0V9_SYS 
						// core2_1v8_dram for new design
						break;
				}
				break;
			case RICOH619_ID_DC5:	// core1_1V2
			// ricoh619_dc5 none vcc-dram vcc-1v1 vcc-1v2 vcc-1v5 vcc-1v35
				if(96==gptHWCFG->m_val.bPCB) {
					// E70P1X
					ricoh61x_write(to_ricoh61x_dev(rdev), 0x34, 0x03); // set auto mode in sleep mode
				}
				else {
					ricoh61x_write(to_ricoh61x_dev(rdev), 0x34, 0x83); // set psm mode in sleep mode
				}
				ricoh61x_write(to_ricoh61x_dev(rdev), 0x35, 0x03);
				break;

			// ricoh619_ldo1 none vcc-wifi vcc-pg vcc-wifi-io 
			// ricoh619_ldo2 none vdd18-dram vdd18-lpddr
			// ricoh619_ldo3 none
			// ricoh619_ldo4 none vcc-efuse vcc-lvds vdd-dsi vcc-cpvin vcc-mcsi
			// ricoh619_ldo5 none vcc-pc vcc-pd vcc-pf vcc-sdc vcc-pmic0 vcc-lcd vccio-dcxo vccio-wifi
			// ricoh619_ldo6 none
			// ricoh619_ldo7 none
			// ricoh619_ldo8 none
			// ricoh619_ldo9 none avcc vcc-pll
			// ricoh619_ldo10 none vcc-pl

			case RICOH619_ID_LDO2:	
				switch(gptHWCFG->m_val.bPCB)
				{
					case 89 :// TA0P04
						break;
					default:
						// core1_0V9_SYS for new design
						ricoh61x_write(to_ricoh61x_dev(rdev), offset, temp);
						break;
				}
				break;
			case RICOH619_ID_LDO10:	
				switch(gptHWCFG->m_val.bPCB)
				{
					case 89: // TA0P04
						// Core2_3V3_VCC_PL
						break;
					default: 
						ricoh61x_write(to_ricoh61x_dev(rdev), offset, temp);
						break;
				}
				
				break;
			case RICOH619_ID_LDO1:	// Core2_3V3_VCC_PL
			case RICOH619_ID_LDO5:	// SPD_3V3
			case RICOH619_ID_LDO7:	// FL_3V3
			// keep output.
				break;
			case RICOH619_ID_LDO3:	// CORE2_0V9
			case RICOH619_ID_LDO4:	// Core2_1V8_LVDS
			case RICOH619_ID_LDO6:	// DDR_0V6
			case RICOH619_ID_LDO9:	// Core2_1V8_AVCC
			case RICOH619_ID_LDO8:	// EP_1V8
				ricoh61x_write(to_ricoh61x_dev(rdev), offset, temp);
				break;
			}
		}
	}

	ricoh61x_write(to_ricoh61x_dev(rdev), 0x4A, 0x03);	// set DDR_1V8 eco mode
	ricoh61x_write(to_ricoh61x_dev(rdev), 0x2A, 0x0E);

	ricoh61x_suspend_status = 1;

	return 0;
}

static int ricoh61x_regulator_resume(struct device *dev)
{
//	printk(KERN_INFO "PMU: %s %s\n", __func__, ri->desc.name);
	struct regulator_dev *rdev = platform_get_drvdata(to_platform_device(dev));
//	struct ricoh61x_regulator *ri = rdev_get_drvdata(rdev);
//	struct ricoh619_regulator_platform_data *ricoh61x_pdata = ((struct device*)(ri->dev))->platform_data;
	int reg_id;
	
	ricoh61x_write(to_ricoh61x_dev(rdev), 0x4A, 0x0);

	for (reg_id=RICOH619_ID_DC1; reg_id<=RICOH619_ID_LDORTC2; reg_id++) {
		int offset = 0x16 + (reg_id - RICOH619_ID_DC1);
		if (gSleep_Mode_Suspend) {
			ricoh61x_write(to_ricoh61x_dev(rdev), offset, regulator_slot[reg_id]);
		}
		else 
		{
			switch (reg_id) {
			case RICOH619_ID_LDO8:	// VDD_EP_1V8
				//ricoh61x_write(to_ricoh61x_dev(rdev), offset, regulator_slot[reg_id]);
				break;
			}
		}
	}

	ricoh61x_suspend_status = 0;

	return 0;
}

static const struct dev_pm_ops ricoh61x_regulator_pm_ops = {
	.suspend	= ricoh61x_regulator_suspend,
	.resume		= ricoh61x_regulator_resume,
};
#endif

static const struct of_device_id ricoh61x_regu_dt_ids[] = {
	{ .compatible = "ricoh61x-regulator", },
	{},
};
MODULE_DEVICE_TABLE(of, ricoh61x_regu_dt_ids);

static struct platform_driver ricoh61x_regulator_driver = {
	.driver	= {
		.name	= "ricoh61x-regulator",
		.owner	= THIS_MODULE,
#ifdef CONFIG_PM
		.pm	= &ricoh61x_regulator_pm_ops,
#endif
		.of_match_table = ricoh61x_regu_dt_ids,
	},
	.probe		= ricoh61x_regulator_probe,
	.remove		= ricoh61x_regulator_remove,
};

static int __init ricoh61x_regulator_init(void)
{
	printk(KERN_INFO "PMU: %s\n", __func__);
	return platform_driver_register(&ricoh61x_regulator_driver);
}
subsys_initcall_sync(ricoh61x_regulator_init);

static void __exit ricoh61x_regulator_exit(void)
{
	platform_driver_unregister(&ricoh61x_regulator_driver);
}
module_exit(ricoh61x_regulator_exit);

MODULE_DESCRIPTION("RICOH R5T619 regulator driver");
MODULE_ALIAS("platform:ricoh619-regulator");
MODULE_LICENSE("GPL");
