/*
 * Allwinner SoCs display driver.
 *
 * Copyright (C) 2016 Allwinner.
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2.  This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#ifndef __DEFAULT_PANEL_H__
#define  __DEFAULT_PANEL_H__

#include "panels.h"

typedef enum {
    EPD_SHUTDOWN = 0,
    EPD_HIBERNATION,
    EPD_SUSPEND,
} EPD_STATUS;

extern struct __lcd_panel default_eink;

extern ssize_t EINK_pmdelay_get(struct device *dev, struct device_attribute *attr, char *buf);
extern ssize_t EINK_pmdelay_set(struct device *dev, struct device_attribute *attr, const char *buf, size_t count);
extern ssize_t EINK_vcomdelay_get(struct device *dev, struct device_attribute *attr, char *buf);
extern ssize_t EINK_vcomdelay_set(struct device *dev, struct device_attribute *attr, const char *buf, size_t count);
extern ssize_t EINK_veedelay_get(struct device *dev, struct device_attribute *attr, char *buf);
extern ssize_t EINK_veedelay_set(struct device *dev, struct device_attribute *attr, const char *buf, size_t count);
extern void EINK_vcom_onoff_ex(int enable);

extern void EINK_pmic_force_onoff(bool enable);
extern int EINK_power_off_time_chk(int status);

#endif
