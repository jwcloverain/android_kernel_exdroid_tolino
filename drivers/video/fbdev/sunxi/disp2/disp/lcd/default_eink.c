/*
 * Allwinner SoCs display driver.
 *
 * Copyright (C) 2016 Allwinner.
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2.  This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#include "default_eink.h"



#if defined(CONFIG_TPS65185_VCOM) || defined(CONFIG_TPS65185_VCOM_MODULE) || defined(CONFIG_SY7636_VCOM) || \
    defined(CONFIG_JD9930_VCOM)
#include <linux/ebc.h>
struct ebc_pwr_ops g_tps65185_ops;
extern int register_ebc_pwr_ops(struct ebc_pwr_ops *ops);

#if defined(CONFIG_JD9930_VCOM)
extern int register_ebc_jd9930_pwr_ops(struct ebc_pwr_ops *ops);
#endif

static int pmic_register_ops = 0;

#define POWER_OFF_DEFAULT_MS	500
#define VCOM_STABLE_DELAY       462
#define VEE_STABLE_DEFALUT_MS   0

struct workqueue_struct *pmic_ctrl_workqueue;
struct delayed_work pmic_off_work;
struct delayed_work pmic_on_work;
static int power_down_delay = POWER_OFF_DEFAULT_MS;
static int vcom_wait_stable_delay = VCOM_STABLE_DELAY;
static int vee_wait_off_stable_delay = VEE_STABLE_DEFALUT_MS;
static bool eclipse_mode_poweron = false;

#endif

static void Eink_int_panel(u32 sel);
static void EINK_power_on(u32 sel);
static void EINK_power_off(u32 sel);
static void EINK_vcom_enable(u32 sel);
static void EINK_vcom_disable(u32 sel);

struct timeval power_off_stimer, power_off_etimer;

static s32 get_delt_ms_timer(struct timeval start_timer, struct timeval end_timer)
{
	return ((end_timer.tv_sec - start_timer.tv_sec) * 1000 + (end_timer.tv_usec - start_timer.tv_usec)/1000);
}

ssize_t EINK_pmdelay_get(struct device *dev, struct device_attribute *attr, char *buf)
{
#if defined(CONFIG_TPS65185_VCOM) || defined(CONFIG_TPS65185_VCOM_MODULE) || defined(CONFIG_SY7636_VCOM) || \
    defined(CONFIG_JD9930_VCOM)
	return sprintf(buf, "%dus\n", power_down_delay);
#else
	return sprintf(buf, "disalbe\n");
#endif
}

ssize_t EINK_pmdelay_set(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
#if defined(CONFIG_TPS65185_VCOM) || defined(CONFIG_TPS65185_VCOM_MODULE) || defined(CONFIG_SY7636_VCOM) || \
    defined(CONFIG_JD9930_VCOM)
	int err;
	int delay;

	err = kstrtoint(buf, 10, &delay);
	if (err) {
		printk("Invalid size\n");
		return err;
	}

	if (delay >= 0) {
		power_down_delay = delay;
	}
#endif
	return count;
}

ssize_t EINK_vcomdelay_get(struct device *dev, struct device_attribute *attr, char *buf)
{
#if defined(CONFIG_TPS65185_VCOM) || defined(CONFIG_TPS65185_VCOM_MODULE) || defined(CONFIG_SY7636_VCOM) || \
    defined(CONFIG_JD9930_VCOM)
	return sprintf(buf, "%dus\n", vcom_wait_stable_delay);
#else
	return sprintf(buf, "disalbe\n");
#endif
}

ssize_t EINK_vcomdelay_set(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
#if defined(CONFIG_TPS65185_VCOM) || defined(CONFIG_TPS65185_VCOM_MODULE) || defined(CONFIG_SY7636_VCOM) || \
    defined(CONFIG_JD9930_VCOM)
	int err;
	int delay;

	err = kstrtoint(buf, 10, &delay);
	if (err) {
		printk("Invalid size\n");
		return err;
	}

	if (delay >= 0) {
		vcom_wait_stable_delay = delay;
	}
#endif
	return count;
}

ssize_t EINK_veedelay_get(struct device *dev, struct device_attribute *attr, char *buf)
{
#if defined(CONFIG_TPS65185_VCOM) || defined(CONFIG_TPS65185_VCOM_MODULE) || defined(CONFIG_SY7636_VCOM) || \
    defined(CONFIG_JD9930_VCOM)
	return sprintf(buf, "%d ms\n", vee_wait_off_stable_delay);
#else
	return sprintf(buf, "disalbe\n");
#endif
}

ssize_t EINK_veedelay_set(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
#if defined(CONFIG_TPS65185_VCOM) || defined(CONFIG_TPS65185_VCOM_MODULE) || defined(CONFIG_SY7636_VCOM) || \
    defined(CONFIG_JD9930_VCOM)
	int err;
	int delay;

	err = kstrtoint(buf, 10, &delay);
	if (err) {
		printk("Invalid size\n");
		return err;
	}

	if (delay >= 0) {
		vee_wait_off_stable_delay = delay;
	}
#endif
	return count;
}

int EINK_get_powergood_status(void)
{
	int status = -1;

	if (g_tps65185_ops.power_status) {
		status = g_tps65185_ops.power_status();
	}

	return status;
}

int EINK_power_off_time_chk(int status)
{
#if defined(CONFIG_TPS65185_VCOM) || defined(CONFIG_TPS65185_VCOM_MODULE) || defined(CONFIG_SY7636_VCOM) || \
    defined(CONFIG_JD9930_VCOM)
	
	if (status == EPD_SHUTDOWN || status == EPD_HIBERNATION) {
		do_gettimeofday(&power_off_etimer);
		if (vee_wait_off_stable_delay == 0) {
			return 0;        
		} else if (get_delt_ms_timer(power_off_stimer, power_off_etimer) < vee_wait_off_stable_delay) {
			printk("power off timing < %d ms, need wait VEE stable\n", vee_wait_off_stable_delay);
			return -1;
		}
	} else if (status == EPD_SUSPEND) {
		return EINK_get_powergood_status();
	} 
	
	return 0;
#endif
}

void EINK_pmic_force_onoff(bool enable)
{
#if defined(CONFIG_TPS65185_VCOM) || defined(CONFIG_TPS65185_VCOM_MODULE) || defined(CONFIG_SY7636_VCOM) || \
    defined(CONFIG_JD9930_VCOM)
	if (enable) {
		//LCD_OPEN_FUNC(0, Eink_int_panel, 0);
		cancel_delayed_work_sync(&(pmic_off_work));
		queue_delayed_work(pmic_ctrl_workqueue, &(pmic_on_work), 0);
		queue_delayed_work(pmic_ctrl_workqueue, &(pmic_off_work), msecs_to_jiffies(power_down_delay));
	} else {
		cancel_delayed_work_sync(&(pmic_off_work));
        
		if (g_tps65185_ops.vcom_disable) {
			g_tps65185_ops.vcom_disable();
		} else {
			printk(KERN_WARNING "%s: PMIC vcom disable func has not been registed yet\n", __FUNCTION__);
		}
		if (g_tps65185_ops.power_down) {
			g_tps65185_ops.power_down();
		} else {
			printk(KERN_WARNING "%s: PMIC power down func has not been registed yet\n", __FUNCTION__);
		}
		
		do_gettimeofday(&power_off_stimer);
	}
#endif
	return;
}

void EINK_pmic_force_onoff_ex(int enable)
{
#if defined(CONFIG_TPS65185_VCOM) || defined(CONFIG_TPS65185_VCOM_MODULE) || defined(CONFIG_SY7636_VCOM) || \
    defined(CONFIG_JD9930_VCOM)
	if (enable) {
		//LCD_OPEN_FUNC(0, Eink_int_panel, 0);
		cancel_delayed_work_sync(&(pmic_off_work));
		
		if (g_tps65185_ops.power_on_ex) {
			g_tps65185_ops.power_on_ex(EBC_PWR_FLAG_XON);
			eclipse_mode_poweron = true;
		} else {
			printk(KERN_WARNING "%s: PMIC power on func has not been registed yet\n", __FUNCTION__);
		}
	} else {
		cancel_delayed_work_sync(&(pmic_off_work));
        
		if (g_tps65185_ops.power_down_ex) {
			g_tps65185_ops.power_down_ex(EBC_PWR_FLAG_XON);
			eclipse_mode_poweron = false;
		} else {
			printk(KERN_WARNING "%s: PMIC power down ex func has not been registed yet\n", __FUNCTION__);
		}
		
		do_gettimeofday(&power_off_stimer);
	}
#endif
	return;
}

void EINK_vcom_onoff_ex(int enable)
{
#if defined(CONFIG_TPS65185_VCOM) || defined(CONFIG_TPS65185_VCOM_MODULE) || defined(CONFIG_SY7636_VCOM) || \
    defined(CONFIG_JD9930_VCOM)
	if (enable) {
        
	} else {
		if (g_tps65185_ops.vcom_disable_ex) {
			g_tps65185_ops.vcom_disable_ex(EBC_PWR_FLAG_XON);
		} else {
			printk(KERN_WARNING "%s: PMIC vcom disable func has not been registed yet\n", __FUNCTION__);
		}
	}
#endif
}
EXPORT_SYMBOL(EINK_vcom_onoff_ex);

static void EINK_cfg_panel_info(struct panel_extend_para *info)
{
	u32 i = 0, j = 0;
	u32 items;
	u8 lcd_gamma_tbl[][2] = {
		/* {input value, corrected value} */
		{0, 0},
		{15, 15},
		{30, 30},
		{45, 45},
		{60, 60},
		{75, 75},
		{90, 90},
		{105, 105},
		{120, 120},
		{135, 135},
		{150, 150},
		{165, 165},
		{180, 180},
		{195, 195},
		{210, 210},
		{225, 225},
		{240, 240},
		{255, 255},
	};

	u32 lcd_cmap_tbl[2][3][4] = {
		{
		 {LCD_CMAP_G0, LCD_CMAP_B1, LCD_CMAP_G2, LCD_CMAP_B3},
		 {LCD_CMAP_B0, LCD_CMAP_R1, LCD_CMAP_B2, LCD_CMAP_R3},
		 {LCD_CMAP_R0, LCD_CMAP_G1, LCD_CMAP_R2, LCD_CMAP_G3},
		 },
		{
		 {LCD_CMAP_B3, LCD_CMAP_G2, LCD_CMAP_B1, LCD_CMAP_G0},
		 {LCD_CMAP_R3, LCD_CMAP_B2, LCD_CMAP_R1, LCD_CMAP_B0},
		 {LCD_CMAP_G3, LCD_CMAP_R2, LCD_CMAP_G1, LCD_CMAP_R0},
		 },
	};

	items = sizeof(lcd_gamma_tbl) / 2;
	for (i = 0; i < items - 1; i++) {
		u32 num = lcd_gamma_tbl[i + 1][0] - lcd_gamma_tbl[i][0];

		for (j = 0; j < num; j++) {
			u32 value = 0;

			value =
			    lcd_gamma_tbl[i][1] +
			    ((lcd_gamma_tbl[i + 1][1] -
			      lcd_gamma_tbl[i][1]) * j) / num;
			info->lcd_gamma_tbl[lcd_gamma_tbl[i][0] + j] =
			    (value << 16) + (value << 8) + value;
		}
	}
	info->lcd_gamma_tbl[255] =
	    (lcd_gamma_tbl[items - 1][1] << 16) +
	    (lcd_gamma_tbl[items - 1][1] << 8) + lcd_gamma_tbl[items - 1][1];

	memcpy(info->lcd_cmap_tbl, lcd_cmap_tbl, sizeof(lcd_cmap_tbl));

}

static void LCD_power_on(u32 sel)
{
	sunxi_lcd_power_enable(sel, 0);//config lcd_power pin to open lcd power0
	sunxi_lcd_pin_cfg(sel, 1);
}

static void LCD_power_off(u32 sel)
{
	sunxi_lcd_pin_cfg(sel, 0);
	sunxi_lcd_power_disable(sel, 0);//config lcd_power pin to close lcd power0
}

static void EINK_pmic_off(struct work_struct *work)
{
#if defined(CONFIG_TPS65185_VCOM) || defined(CONFIG_TPS65185_VCOM_MODULE) || defined(CONFIG_SY7636_VCOM) || \
    defined(CONFIG_JD9930_VCOM)
	if (g_tps65185_ops.vcom_disable) {
		g_tps65185_ops.vcom_disable();
	} else {
#ifndef CONFIG_JD9930_VCOM
		printk(KERN_WARNING "%s: PMIC vcom disable func has not been registed yet\n", __FUNCTION__);
#endif
	}
	
	if (g_tps65185_ops.power_down) {
		g_tps65185_ops.power_down();
		eclipse_mode_poweron = false;
	} else {
		printk(KERN_WARNING "%s: PMIC power down func has not been registed yet\n", __FUNCTION__);
	}
	
	LCD_power_off(0);
	do_gettimeofday(&power_off_stimer);
#endif
}

static void EINK_pmic_on(struct work_struct *work)
{
#if defined(CONFIG_TPS65185_VCOM) || defined(CONFIG_TPS65185_VCOM_MODULE) || defined(CONFIG_SY7636_VCOM) || \
    defined(CONFIG_JD9930_VCOM)
	if (g_tps65185_ops.power_on) {
		g_tps65185_ops.power_on();
	} else {
		printk(KERN_WARNING "%s: PMIC power on func has not been registed yet\n", __FUNCTION__);
	}
#endif
}

static void Eink_int_panel(u32 sel)
{
#if defined(CONFIG_TPS65185_VCOM) || defined(CONFIG_TPS65185_VCOM_MODULE) || defined(CONFIG_SY7636_VCOM) || \
	defined(CONFIG_JD9930_VCOM)
	//printk("%s: register tps65185 ops\n", __func__);
	s32 ret = 0;
	char primary_key[25];
	s32 value = 0;
	
	if (!pmic_register_ops) {
		ret = register_ebc_pwr_ops(&g_tps65185_ops);
#if defined(CONFIG_JD9930_VCOM)
		if (ret)
			ret = register_ebc_jd9930_pwr_ops(&g_tps65185_ops);
#endif
		pmic_register_ops = 1;
        sprintf(primary_key, "lcd0");
		ret = disp_sys_script_get_item(primary_key, "vcom_delay", &value, 1);
		if (ret == 1) {
			vcom_wait_stable_delay = value;
		}
		
		ret = disp_sys_script_get_item(primary_key, "vee_off_delay", &value, 1);
		if (ret == 1) {
			vee_wait_off_stable_delay = value;
		}
	}
	
	if (!pmic_ctrl_workqueue) {
		pmic_ctrl_workqueue = create_workqueue("PMIC on/off ctrl");
		INIT_DELAYED_WORK(&(pmic_off_work), EINK_pmic_off);
		INIT_DELAYED_WORK(&(pmic_on_work), EINK_pmic_on);
	}
#endif
}

static void Eink_uninit_panel(u32 sel)
{
#if defined(CONFIG_TPS65185_VCOM) || defined(CONFIG_TPS65185_VCOM_MODULE) || defined(CONFIG_SY7636_VCOM) || \
    defined(CONFIG_JD9930_VCOM)
	//printk("%s: unregister tps65185 ops\n", __func__);
	g_tps65185_ops.power_down = NULL;
    g_tps65185_ops.power_down_ex = NULL;
	g_tps65185_ops.power_on = NULL;
	g_tps65185_ops.power_on_ex = NULL;
	g_tps65185_ops.vcom_enable = NULL;
	g_tps65185_ops.vcom_disable = NULL;
    g_tps65185_ops.vcom_disable_ex = NULL;
    g_tps65185_ops.power_status = NULL;
#endif
}


static s32 EINK_open_flow(u32 sel)
{
#if defined(CONFIG_TPS65185_VCOM) || defined(CONFIG_TPS65185_VCOM_MODULE) || defined(CONFIG_SY7636_VCOM) || \
    defined(CONFIG_JD9930_VCOM)
	LCD_OPEN_FUNC(sel, Eink_int_panel, 0);
	LCD_OPEN_FUNC(sel, EINK_power_on, 0);
	LCD_OPEN_FUNC(sel, EINK_vcom_enable, 0);
#endif
	LCD_OPEN_FUNC(sel, sunxi_lcd_tcon_enable, 0);
	return 0;
}

static s32 EINK_close_flow(u32 sel)
{
	LCD_CLOSE_FUNC(sel, sunxi_lcd_tcon_disable, 0);
#if defined(CONFIG_TPS65185_VCOM) || defined(CONFIG_TPS65185_VCOM_MODULE) || defined(CONFIG_SY7636_VCOM) || \
    defined(CONFIG_JD9930_VCOM)
	//LCD_CLOSE_FUNC(sel, EINK_vcom_disable, 0);
	LCD_CLOSE_FUNC(sel, EINK_power_off, 0);
	//LCD_OPEN_FUNC(sel, Eink_uninit_panel, 0);
#endif
	return 0;
}

static void EINK_power_on(u32 sel)
{
	//printk(KERN_ERR"-EINK_power_on-start--\n");
#if defined(CONFIG_TPS65185_VCOM) || defined(CONFIG_TPS65185_VCOM_MODULE) || defined(CONFIG_SY7636_VCOM) || \
    defined(CONFIG_JD9930_VCOM)
	//tps65185_power_wakeup_with_lock(1);
	cancel_delayed_work_sync(&(pmic_off_work));
  
	if (!eclipse_mode_poweron) {
		if (g_tps65185_ops.power_on) {
			g_tps65185_ops.power_on();
		} else {
			printk(KERN_WARNING "%s: PMIC power on func has not been registed yet\n", __FUNCTION__);
		}
	} else {
		eclipse_mode_poweron = false;
	} 

	LCD_power_on(sel);
#else
	/* pwr3 pb2 */
	sunxi_lcd_gpio_set_value(sel, 0, 1);
	sunxi_lcd_delay_ms(1);
	/* pwr0 pb0 */
	sunxi_lcd_gpio_set_value(sel, 1, 1);
	sunxi_lcd_delay_ms(1);
	/* pb1 */
	sunxi_lcd_gpio_set_value(sel, 2, 1);
	sunxi_lcd_delay_ms(1);
	/* pwr2 pd6 */
	sunxi_lcd_gpio_set_value(sel, 3, 1);
	sunxi_lcd_delay_ms(1);
	/* pwr com pd7 */
	sunxi_lcd_gpio_set_value(sel, 4, 1);
	sunxi_lcd_delay_ms(1);

	sunxi_lcd_gpio_set_value(sel, 5, 1);
	sunxi_lcd_delay_ms(2);

	sunxi_lcd_pin_cfg(sel, 1);
#endif
}

static void EINK_power_off(u32 sel)
{
#if defined(CONFIG_TPS65185_VCOM) || defined(CONFIG_TPS65185_VCOM_MODULE) || defined(CONFIG_SY7636_VCOM) || \
    defined(CONFIG_JD9930_VCOM)
	//tps65185_power_wakeup_with_lock(0);	
	queue_delayed_work(pmic_ctrl_workqueue, &(pmic_off_work), msecs_to_jiffies(power_down_delay));
	
	//LCD_power_off(sel);
#else

	sunxi_lcd_pin_cfg(sel, 0);

	sunxi_lcd_gpio_set_value(sel, 5, 0);
	sunxi_lcd_delay_ms(1);
	sunxi_lcd_gpio_set_value(sel, 4, 0);
	sunxi_lcd_delay_ms(1);
	sunxi_lcd_gpio_set_value(sel, 3, 0);
	sunxi_lcd_delay_ms(1);
	sunxi_lcd_gpio_set_value(sel, 2, 0);
	sunxi_lcd_delay_ms(1);
	sunxi_lcd_gpio_set_value(sel, 1, 0);
	sunxi_lcd_delay_ms(1);
	sunxi_lcd_gpio_set_value(sel, 0, 0);
	sunxi_lcd_delay_ms(2);
#endif
	//printk(KERN_ERR"-EINK_power_off-end---\n");
}

static void EINK_vcom_enable(u32 sel)
{
#if defined(CONFIG_TPS65185_VCOM) || defined(CONFIG_TPS65185_VCOM_MODULE) || defined(CONFIG_SY7636_VCOM) || \
    defined(CONFIG_JD9930_VCOM)
	if (g_tps65185_ops.vcom_enable) {
		g_tps65185_ops.vcom_enable();
		//mdelay(vcom_wait_stable_delay);
        udelay(vcom_wait_stable_delay);
	} else {
#ifndef CONFIG_JD9930_VCOM
		printk(KERN_WARNING "%s: PMIC vcom enable func has not been registed yet\n", __FUNCTION__);
#endif
	}
#endif
}

static void EINK_vcom_disable(u32 sel)
{
#if defined(CONFIG_TPS65185_VCOM) || defined(CONFIG_TPS65185_VCOM_MODULE) || defined(CONFIG_SY7636_VCOM) || \
    defined(CONFIG_JD9930_VCOM)
	if (g_tps65185_ops.vcom_disable) {
		g_tps65185_ops.vcom_disable();
	} else {
#ifndef CONFIG_JD9930_VCOM
		printk(KERN_WARNING "%s: PMIC vcom disable func has not been registed yet\n", __FUNCTION__);
#endif
	}
#endif
}

/* sel: 0:lcd0; 1:lcd1 */
static s32 EINK_user_defined_func(u32 sel, u32 para1, u32 para2, u32 para3)
{
	return 0;
}

struct __lcd_panel default_eink = {
	/* panel driver name, must mach the lcd_drv_name in sys_config.fex */
	.name = "default_eink",
	.func = {
		 .cfg_panel_info = EINK_cfg_panel_info,
		 .cfg_open_flow = EINK_open_flow,
		 .cfg_close_flow = EINK_close_flow,
		 .lcd_user_defined_func = EINK_user_defined_func,
		 }
	,
};
