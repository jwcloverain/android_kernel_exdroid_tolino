/*
 * cilix.c - driver for i2c through uart bridge
 *
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/pinctrl/consumer.h>
#include <linux/delay.h>
#include <linux/miscdevice.h>
#include <linux/power/sy6974b_charger.h>
#include <linux/power/eg2805.h>
#include <linux/power/cilix-ricoh619.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/completion.h>

#include "ntx_cilix.h"

#include "../../../arch/arm/mach-sunxi/ntx_hwconfig.h"
extern volatile NTX_HWCONFIG *gptHWCFG;

extern int gSleep_Mode_Suspend;

#ifdef CONFIG_MFD_RICOH619
extern int ricoh619_get_battery_capacity(void);
extern int _cilix_config_ricoh619_charger_params(int);
#endif
#ifdef CONFIG_MFD_ROHM_BD71828
extern int bd71828_get_battery_capacity(void);
extern int _cilix_config_bd71828_charger_params(int);
#endif
extern int enable_gpio_wakeup_src(int para);
extern int disable_gpio_wakeup_src(int para);
#ifdef CONFIG_MFD_RICOH619
extern int ricoh619_is_sdp(void);
extern bool ricoh619_is_ready(void);
#endif
#ifdef CONFIG_MFD_ROHM_BD71828
extern bool bd71828_is_ready(void);
extern int bd71828_is_sdp(void);
#endif
static void run_state(struct cilix *, enum cilix_events);
static int FN_CX_OTG_ON(struct cilix *);
static int FN_CX_CHARGE_ON(struct cilix *);
static int cilix_exec_command(struct cilix *cx, enum cilix_command command);
static int sy6974b_set_sys_min(struct cilix *cx);

static DECLARE_COMPLETION(msp430_reset_done);

#define CILIX_ITERM         60
#define CILIX_TIMEOUT_MS	450
/* time for cc and ricoh to fininsh charger type detect */
#define CILIX_WAIT_FOR_RICOH_CHARGE_TYPE_MS   1500
#define CILIX_WAIT_FOR_ROHM_CHARGE_TYPE_MS   4000
#define CILIX_HRTIMER_INIT_MS   5000

struct cilix* gcx;

/* for usb_plug.c to know if it is supplied by cilix otg*/
bool cilix_otg_charger(void)
{
	if(!gcx)
		return false;
	else if(gcx->eb_charge_type == CILIX_CHARGER ||
        gcx->eb_charge_type == CILIX_CHARGER_02C ||
        gcx->eb_charge_type == CILIX_CHARGER_05C )
        return true;
    else
        return false;
}

static void reset_work_func(struct work_struct *work)
{
	struct cilix *cx = container_of(work, struct cilix, reset_work.work);
    struct pinctrl_state *default_state;
    int ret;

    dev_dbg(cx->dev,"%s(): enter\n",__func__);

    default_state = pinctrl_lookup_state(gcx->pinctrl, "default");
    if(IS_ERR_OR_NULL(default_state))
    {
        dev_info(cx->dev, "[cilix] get defualt state fail\n");
        return -EINVAL;
    }

    gpio_direction_input(cx->int_gpio);
    gpio_free(cx->int_gpio);

    ret = pinctrl_select_state(cx->pinctrl, default_state);
    if(ret < 0)
    {
        dev_info(cx->dev, "[cilix] Set default_state failed\n");
    }

    complete(&msp430_reset_done);
    cx->status->in_reset_progress = false;

    enable_irq(cx->irq);

    dev_info(cx->dev, "[cilix] msp430 reset finished\n");
}

static void otg_on_func(struct work_struct *work)
{
	struct cilix *cx = container_of(work, struct cilix, otg_on_work.work);
    int ret;

    dev_dbg(cx->dev,"%s(): enter\n",__func__);
	ret = cilix_exec_command(cx, CILIX_OTG_ON);
    if(ret < 0)
    {
		dev_info(cx->dev, "[Warning]%s failed %d\n",__func__,ret);
    }

    dev_info(cx->dev, "[cilix] %s() finished\n", __func__);
}

int m_msp430_int_pin_reset(void)
{
    struct pinctrl_state *reset_state;
    int ret;
    unsigned int int_pin_pull_low_time_ms = 10000;

    printk("[cilix] m_msp430_int_pin_reset() entered\n");
    if(gcx->status->in_reset_progress)
    {
        printk("[cilix] reset process ongoing, skip m_msp430_int_pin_reset()!\n");
       return -EBUSY; 
    }

    gcx->status->in_reset_progress = true;

    disable_irq(gcx->irq);
    
    reset_state = pinctrl_lookup_state(gcx->pinctrl, "reset");
    if(IS_ERR_OR_NULL(reset_state))
    {
        printk("[cilix] get reset state fail\n");
        return -EINVAL;
    }

    ret = pinctrl_select_state(gcx->pinctrl, reset_state);
    if(ret < 0)
    {
        printk("[cilix] Set reset_state failed\n");
        return ret;
    }
    ret = gpio_request(gcx->int_gpio, "cx_int_gpio");
    if(ret < 0)
    {
        printk("[cilix] request gpio failed\n");
    }
    else
    {
        gpio_direction_input(gcx->int_gpio);
        if(gpio_get_value(gcx->int_gpio) == 0)
        {
            /*need 10s to reset msp430 in normal condition
              if the INT pin was low (intterupt triggered)
              need 20s to reset msp430 under this situation*/
            int_pin_pull_low_time_ms = 20000;
        }
        printk("[cilix] pull low int pin for %dms\n",int_pin_pull_low_time_ms);
        gpio_direction_output(gcx->int_gpio, 0/*LOW*/);
        /*msp430 int pin pulled low, wait for reset_work_func() to set it as interrupt(high)*/
        queue_delayed_work(system_wq, &gcx->reset_work, msecs_to_jiffies(int_pin_pull_low_time_ms));
    }
    
    return 0;
}

static struct cilix* dev_to_cx(struct device *dev) {
    struct miscdevice *misc;
    struct cilix *cx;

    misc = dev_get_drvdata(dev);
    cx = container_of(misc, struct cilix, misc_dev);

    return cx;
}

static void cilix_lock(struct cilix *cx)
{
    dev_dbg(cx->dev, "%s() entered\n",__func__);

	mutex_lock(&cx->i2c_mutex);
	cx->cur_intrs = cx->intrs;
	cx->waiting_i2c_response = true;
}

static void cilix_unlock(struct cilix *cx)
{
    dev_dbg(cx->dev, "%s() entered\n",__func__);

	cx->waiting_i2c_response = false;
	mutex_unlock(&cx->i2c_mutex);
}

/* disable irq for low level sysfs i2c manipulations 
 * use for periodic check may cause unexpected interrupt loss
 */
static void __cilix_disable_irq(struct cilix *cx)
{
	if(cx->irq){
		dev_info(cx->dev, "%s FOR DEBUG ONLY\n",__func__);
		disable_irq(cx->irq);
	} else {
		dev_info(cx->dev, "%s skipped\n",__func__);
	}
}

static void __cilix_enable_irq(struct cilix *cx)
{
	if(cx->irq) {
		dev_info(cx->dev, "%s FOR DEBUG ONLY\n",__func__);
		enable_irq(cx->irq);
	} else {
		dev_info(cx->dev, "%s skipped\n",__func__);
	}
}

static int cilix_wait_for_data_ready(struct cilix *cx,
				     u32 timeout)
{
	if (cx->irq){
		s32 rc;

		rc = wait_event_interruptible_timeout(cx->read_queue,
						      cx->cur_intrs != cx->intrs,
						      timeout);
		if (rc == 0){
			dev_info(cx->dev,"%s():timed out\n",__func__);
			return -ETIMEDOUT;
		} else {
			return 0;
		}
	} else {
		msleep(100);
		return 0;
	}
}

/* send i2c data to MSP430 uart bridge */
static int msp430_i2c_read(struct cilix *cx, u8* resp, size_t resp_size)
{
	struct i2c_msg msg[1];
	int ret;

	if (!cx->client->adapter)
		return -ENODEV;

	msg[0].addr = cx->client->addr;
	msg[0].flags = I2C_M_RD;
	msg[0].buf = resp;
	msg[0].len = resp_size;

	ret = i2c_transfer(cx->client->adapter, msg, ARRAY_SIZE(msg));
	
	return ret;
}

/* read i2c data from MSP430 uart bridge */
static int msp430_i2c_write(struct cilix *cx, const u8* cmd, size_t cmd_size)
{
	struct i2c_msg msg[1];
	int ret;
	
	if (!cx->client->adapter)
		return -ENODEV;
	
	msg[0].addr = cx->client->addr;
	msg[0].flags = 0;
	msg[0].buf = (u8 *)cmd;
	msg[0].len = cmd_size;

	ret = i2c_transfer(cx->client->adapter, msg, ARRAY_SIZE(msg));

	/* i2c_transfer returns number of messages transferred */
	if (ret < 0)
		return ret;
	else if (ret != 1)
		return -EIO;

	return 0;
}

static int cilix_create_r_msg(u8 *cmd, u8 target, u8 reg){
	int length;
	if(target == I2C_ADDRESS_EG2805){
		cmd[0] = UART_CMD_START; /*START*/
		cmd[1] = target<<1; /*WRITE*/
		cmd[2] = 0x02; /*LENGTH*/
		if(reg < 0x06)
            cmd[3] = 0xF5; /*EG2805 read EEPROM command*/
        else
            cmd[3] = 0x55; /*EG2805 read RAM command*/
		cmd[4] = reg; /*reg want to read*/
		cmd[5] = UART_CMD_START;
		cmd[6] = target<<1|I2C_M_RD; /*READ*/
		cmd[7] = 0x01; /*one byte*/
		cmd[8] = UART_CMD_END; /*END*/
		length = 9;
	} else {	
		cmd[0] = UART_CMD_START; /*START*/
		cmd[1] = target<<1; /*WRITE*/
		cmd[2] = 0x01; /*LENGTH*/
		cmd[3] = reg; /*reg want to read*/
		cmd[4] = UART_CMD_START;
		cmd[5] = target<<1|I2C_M_RD; /*READ*/
		cmd[6] = 0x01; /*one byte*/
		cmd[7] = UART_CMD_END; /*END*/
		length = 8;
	}
	return length;
}

static void check_comm_work_func(struct work_struct *work)
{
	struct cilix *cx = container_of(work, struct cilix, check_comm_work.work);
    int ret;

    dev_info(cx->dev, "%s entered\n",__func__);

    ret = cilix_exec_command(cx, CILIX_COMM_CHECK);
    if(ret == 0)
    {
        /*ACK returned*/
        cx->status->comm_good = 1;
    }
    else
    {
        cx->status->comm_good = 0;
    }
}

/* check if uart communication still works */
static void check_cilix_comm(struct cilix *cx, int error_type)
{
    int uart_comm;

	dev_info(cx->dev, "%s entered\n",__func__);

    /* uart comm test result at the moment cilix connects */
    uart_comm = cilix_exec_command(cx, CILIX_M_MSP430_STAT) & BIT(STAT_UART_COMM);
    dev_info(cx->dev, "uart comm %s when connect\n", uart_comm ? "error" : "good" );

    /* comm_good is 1 before uart comm test*/
    if(cx->status->comm_good)
    {
        /*wait 2 sec before uart comm test*/
        queue_delayed_work(system_wq, &cx->check_comm_work, msecs_to_jiffies(2000));
    }
    else
    {
        dev_info(cx->dev, "comm_good already 0\n");
    }
}

/* 
*  Use msp430 uart to i2c bridge to read register
*  of msp430 it self or device behind msp430.
*  Data transfered is packed with UART_CMD_START 0x53 
*  and UART_CMD_END 0x50
*/
static int cilix_read_reg(struct cilix *cx, u8 target, u8 reg)
{
	int ret;
	u8 cmd[9]; // current max length
	u8 resp[2];
	int length;

	length = cilix_create_r_msg(cmd, target, reg);

	cilix_lock(cx);
	ret = msp430_i2c_write(cx, cmd, length);
    if (ret < 0){
        dev_info(cx->dev,"%s():i2c error\n",__func__);
        cilix_unlock(cx);
        return ret;
    }
	/*wait for data ready*/
	ret = cilix_wait_for_data_ready(cx, CILIX_TIMEOUT_MS);
	if (ret != 0){
		dev_info(cx->dev,"%s():timed out\n",__func__);
		cilix_unlock(cx);
		return ret;
	}
	ret = msp430_i2c_read(cx, resp, 2); /*ack/nack+1byte result*/
	cilix_unlock(cx);
	if (ret < 0){
		dev_info(cx->dev,"%s():i2c error\n",__func__);
		return ret;
	}
	if(resp[0] == I2C_ACK){
		return resp[1];
	} else if(resp[0] == I2C_NACK) {
		dev_info(cx->dev,"%s():target=0x%02X, reg=0x%02X", __func__, target, reg);
		dev_info(cx->dev,"%s():NACK! [%02X %02X]\n",__func__,resp[0],resp[1]);
        if(resp[1] == I2C_NACK_CILIX_DISCONNECTED || resp[1] == I2C_NACK_OPERATION_TIMEOUT)
        {
            check_cilix_comm(cx, resp[1]);
        }
		return -resp[1];
	} else {
		dev_info(cx->dev,"%s():target=0x%02X, reg=0x%02X", __func__, target, reg);
		dev_info(cx->dev,"%s():UNEXPECTED RESPONSE! [%02X %02X]\n",
					__func__,resp[0],resp[1]);
		return -1;
	}
}

/* read value from register, apply mask and right shift it */
static int cilix_read_reg_mask(struct cilix *cx, u8 target, 
				u8 reg, u8 mask, u8 shift)
{
	int ret;

	if (shift > 8)
		return -EINVAL;

	ret = cilix_read_reg(cx, target, reg);
	if (ret < 0)
		return ret;
	return (ret & mask) >> shift;
}

/* read value from register and return one specified bit */
static int cilix_read_reg_bit(struct cilix *cx, u8 target, 
				u8 reg, u8 bit)
{
	if (bit > 8)
		return -EINVAL;
	return cilix_read_reg_mask(cx, target, reg, BIT(bit), bit);
}

static int cilix_create_w_msg(u8 *cmd, u8 target, u8 reg, u8 val){
	int length;
	if(target == I2C_ADDRESS_EG2805){
		cmd[0] = UART_CMD_START; /*START*/
		cmd[1] = target<<1; /*WRITE*/
		cmd[2] = 0x03; /*LENGTH*/
		cmd[3] = 0xfa; /*EG2805 write command*/
		cmd[4] = reg; /*reg want to write*/
		cmd[5] = val;
		cmd[6] = UART_CMD_END; /*END*/
		length = 7;
	} else {	
		cmd[0] = UART_CMD_START; /*START*/
		cmd[1] = target<<1; /*WRITE*/
		cmd[2] = 0x02; /*LENGTH*/
		cmd[3] = reg; /*reg want to write*/
		cmd[4] = val;
		cmd[5] = UART_CMD_END; /*END*/
		length = 6;
	}
	return length;
}
/* 
*  Use msp430 uart to i2c bridge to write register
*  of msp430 it self or device behind msp430.
*  Data transfered is packed with UART_CMD_START 0x53 
*  and UART_CMD_END 0x50
*/
static int cilix_write_reg(struct cilix *cx, u8 target, u8 reg, u8 val)
{
	int ret;
	u8 cmd[7];
	u8 resp[2];
	int length;

	length = cilix_create_w_msg(cmd, target, reg, val);

	cilix_lock(cx);
	ret = msp430_i2c_write(cx, cmd, length);
	if (ret < 0)
		return ret;
	/*wait for data ready*/
	ret = cilix_wait_for_data_ready(cx, CILIX_TIMEOUT_MS);
	if (ret != 0){
		dev_info(cx->dev,"%s():timed out\n",__func__);
		cilix_unlock(cx);
		return ret;
	}

	ret = msp430_i2c_read(cx, resp, 2); /*ack/nack+1byte error code*/
	cilix_unlock(cx);
	if (ret < 0){
		dev_info(cx->dev,"%s():i2c error\n",__func__);
		return ret;
	}
	if(resp[0]==I2C_ACK)
		return 0;
	else if(resp[0]==I2C_NACK) {
		dev_info(cx->dev,"%s():target=0x%02X, reg=0x%02X", __func__, target, reg);
		dev_info(cx->dev,"%s():NACK! [%02X %02X]\n",__func__,resp[0],resp[1]);
		return -resp[1];
	} else {
		dev_info(cx->dev,"%s():target=0x%02X, reg=0x%02X", __func__, target, reg);
		dev_info(cx->dev,"%s():UNEXPECTED RESPONSE! [%02X %02X]\n",
					__func__,resp[0],resp[1]);
		return -1;
	}
}

/* read value from register, change it with mask left shifted and write back */
static int cilix_write_reg_mask(struct cilix *cx, u8 target, u8 reg,
					u8 val, u8 mask, u8 shift)
{
	int ret;

	if (shift > 8)
		return -EINVAL;

	ret = cilix_read_reg(cx, target, reg);
	if (ret < 0)
		return ret;

	ret &= ~mask;
	ret |= val << shift;

	return cilix_write_reg(cx, target, reg, ret);
}

/* change only one bit in register */
static int cilix_write_reg_bit(struct cilix *cx, u8 target, u8 reg,
				 u8 bit, bool val)
{
	if (bit > 8)
		return -EINVAL;
	return cilix_write_reg_mask(cx, target, reg, val, BIT(bit), bit);
}

/* 
 * use for DEBUG ONLY
 * directly read i2c messages from chip 
 */
static ssize_t msp430_sysfs_i2c_read_bytes(struct device *dev,
					   struct device_attribute *attr,
					   const char *buf,
					   size_t count)
{
	struct cilix *cx = dev_to_cx(dev);
	ssize_t ret = 0;
	u8 resp[100];
	unsigned int bytes;
	int i;

	if (sscanf(buf, "%d", &bytes) != 1)
		return -EINVAL;

	if (bytes > 100)
		return -EINVAL;

	ret = msp430_i2c_read(cx, resp, bytes);
	__cilix_enable_irq(cx);
	dev_info(cx->dev,"i2c(read) %d bytes:\n",bytes);
	for(i=0;i<bytes;i++){
		printk("0x%02X ",resp[i]);
	}
	printk("\n");

	if (ret < 0)
		return ret;
	return count;
}

/*return 0 if success, return -1 otherwise*/
static int eg2805_EEPROM_write_byte(struct cilix *cx, u16 address, u8 data)
{
    u8 addr_lo, addr_hi;
	int ret, i;
    addr_hi = address >> 8;
    addr_lo = address & 0xff;

    cilix_write_reg(cx, I2C_ADDRESS_EG2805, 0x00, addr_lo);
    cilix_write_reg(cx, I2C_ADDRESS_EG2805, 0x01, addr_hi);
    cilix_write_reg(cx, I2C_ADDRESS_EG2805, 0x02, data);
    cilix_write_reg(cx, I2C_ADDRESS_EG2805 ,0x03, 0x05); /* 0x05 write command*/
    for(i=0; i<10; i++)
    {
       ret = cilix_read_reg(cx, I2C_ADDRESS_EG2805, 0x03);
       if(ret == 0x00) break;
    }

	return ret == 0 ? 0 : -1;
}

/* return reg value if success, return -1 otherwise */
static int eg2805_EEPROM_read_byte(struct cilix *cx, u16 address)
{
    u8 addr_lo, addr_hi;
	int ret, i;
    addr_hi = address >> 8;
    addr_lo = address & 0xff;

    cilix_write_reg(cx, I2C_ADDRESS_EG2805, 0x00, addr_lo);
    cilix_write_reg(cx, I2C_ADDRESS_EG2805, 0x01, addr_hi);
    cilix_write_reg(cx, I2C_ADDRESS_EG2805 ,0x03, 0x06); /*0x06 read command*/
    for(i=0; i<10; i++)
    {
       ret = cilix_read_reg(cx, I2C_ADDRESS_EG2805, 0x03);
       if(ret == 0x00) break;
    }
    if(ret != 0)
        return -1;

    ret = cilix_read_reg(cx, I2C_ADDRESS_EG2805, 0x02);
    return ret;
}

/*return 0 if success, return reg read back value or -1 otherwise*/
static int eg2805_EEPROM_write_and_verify_byte(struct cilix *cx, u16 address, u8 data)
{
    u8 addr_lo, addr_hi;
	int ret, i;
    addr_hi = address >> 8;
    addr_lo = address & 0xff;

    /*write eeprom*/
    cilix_write_reg(cx, I2C_ADDRESS_EG2805, 0x00, addr_lo);
    cilix_write_reg(cx, I2C_ADDRESS_EG2805, 0x01, addr_hi);
    cilix_write_reg(cx, I2C_ADDRESS_EG2805, 0x02, data);
    cilix_write_reg(cx, I2C_ADDRESS_EG2805 ,0x03, 0x05); /* 0x05 write command*/
    for(i=0; i<10; i++)
    {
       ret = cilix_read_reg(cx, I2C_ADDRESS_EG2805, 0x03);
       if(ret == 0x00) break;
    }
    if(ret != 0)
        return -1;
    /*read back*/
    cilix_write_reg(cx, I2C_ADDRESS_EG2805 ,0x03, 0x06); /*0x06 read command*/
    for(i=0; i<10; i++)
    {
       ret = cilix_read_reg(cx, I2C_ADDRESS_EG2805, 0x03);
       if(ret == 0x00) break;
    }
    if(ret != 0)
        return -1;
    ret = cilix_read_reg(cx, I2C_ADDRESS_EG2805, 0x02);

	return ret == data ? 0 : ret;
}

static int eg2805_verify_EEPROM(struct cilix *cx)
{
	int ret, i;
	unsigned int count = 0;

    /*enter PT mode*/
	cilix_exec_command(cx, CILIX_PT_MODE_ENTER);

	for (i = 0; i < sizeof(battery_parameters)/sizeof(battery_parameters[0]); i++){
		ret = eg2805_EEPROM_read_byte(cx, battery_parameters[i][0]);
		dev_info(cx->dev, "line %d: 0x%04X = 0x%02X <--> 0x%02X\n", i,\
                battery_parameters[i][0], battery_parameters[i][1], ret);
		if( ret != battery_parameters[i][1]) {
			dev_info(cx->dev, "mismatch![%d]\n", ret);
			count++;
		}
	}
    /*exit PT mode*/
	cilix_exec_command(cx, CILIX_PT_MODE_EXIT);

	return count;
}

static int eg2805_write_EEPROM(struct cilix *cx)
{
	int ret, i;
	unsigned int count = 0;

    /*enter PT mode*/
	cilix_exec_command(cx, CILIX_PT_MODE_ENTER);

	for (i = 0; i < sizeof(battery_parameters)/sizeof(battery_parameters[0]); i++){
		ret = eg2805_EEPROM_write_byte(cx, battery_parameters[i][0], battery_parameters[i][1]);
		dev_info(cx->dev, "line %d: 0x%04X = 0x%02X\n", i, battery_parameters[i][0],\
											battery_parameters[i][1]);
		if(ret < 0) {
			dev_info(cx->dev, "failed!\n");
			count++;
		}
	}
    /*exit PT mode*/
	cilix_exec_command(cx, CILIX_PT_MODE_EXIT);
	
    return count;
}

/*return 0 if success, return 1 otherwise*/
static int eg2805_write_and_verify_EEPROM(struct cilix *cx)
{
	int ret, i;

    /*enter PT mode*/
	cilix_exec_command(cx, CILIX_PT_MODE_ENTER);

	for (i = 0; i < sizeof(battery_parameters)/sizeof(battery_parameters[0]); i++){
        ret = eg2805_EEPROM_write_and_verify_byte(cx, battery_parameters[i][0], battery_parameters[i][1]);
		dev_dbg(cx->dev, "line %d: 0x%04X = 0x%02X\n", i, battery_parameters[i][0],\
											battery_parameters[i][1]);
		if(ret != 0) {
		    dev_info(cx->dev, "line %d: 0x%04X = 0x%02X\n", i, battery_parameters[i][0],\
											battery_parameters[i][1]);
			dev_info(cx->dev, "mismatch![%d]\n", ret);
			ret = 1;
            break;
		}
	}
    /*exit PT mode*/
	cilix_exec_command(cx, CILIX_PT_MODE_EXIT);
	
    return ret;
}

/*return 0 if success, return 1 otherwise*/
static int eg2805_calibration(struct cilix *cx)
{
    int val;
    int i, ret;

    /*enter PT mode*/
	cilix_exec_command(cx, CILIX_PT_MODE_ENTER);

    val = eg2805_EEPROM_read_byte(cx, 0x014B);
    dev_info(cx->dev, "init EEPROM 0x014B = 0x%02x, bit5 = %d\n", val, val&BIT(5));
    /*set 0 on bit 5*/
    val &= ~0x20; 
    dev_info(cx->dev, "write EEPROM 0x014B = 0x%02x", val);
    eg2805_EEPROM_write_byte(cx, 0x014B, val);
    for (i = 0; i < 20; i++)
    {
        msleep(500);
        val = eg2805_EEPROM_read_byte(cx, 0x014B);
        dev_info(cx->dev, "[%d] read back 0x014B = 0x%02x, bit5 = %d\n", i, val, val&BIT(5));
        /*wait until bit 5 changed to 1*/
        if (0x00 != (val&BIT(5)))
        {
            dev_info(cx->dev, "calibration OK!\n");
            
            /*exit PT mode*/
	        cilix_exec_command(cx, CILIX_PT_MODE_EXIT);
            return 0;
        }
    }
    /*timeout and fail*/
    /*exit PT mode*/
	cilix_exec_command(cx, CILIX_PT_MODE_EXIT);
    return 1;
}

/**** global functions ****/

/* cilix exec command function */
static int cilix_exec_command(struct cilix *cx,
				enum cilix_command command)
{
	int ret;

	switch (command) {
    case CILIX_COMM_CHECK:
        return cilix_write_reg(cx, I2C_ADDRESS_T80P0C_MSP430_GPIO,
                MSP430_REG_FUNC_1, FUNC_COMM_CHK);
    case CILIX_M_MSP430_VER:
		ret = cilix_read_reg(cx, I2C_ADDRESS_E80P0M_MSP430_GPIO,
				MSP430_REG_VER_HIGH_BYTE);
		ret = ret<<8 | cilix_read_reg(cx, I2C_ADDRESS_E80P0M_MSP430_GPIO,
				MSP430_REG_VER_LOW_BYTE);
		return ret;
    case CILIX_C_MSP430_VER:
		ret = cilix_read_reg(cx, I2C_ADDRESS_T80P0C_MSP430_GPIO,
				MSP430_REG_VER_HIGH_BYTE);
		ret = ret<<8 | cilix_read_reg(cx, I2C_ADDRESS_T80P0C_MSP430_GPIO,
				MSP430_REG_VER_LOW_BYTE);
		return ret;
    case CILIX_M_MSP430_STAT:
		ret = cilix_read_reg(cx, I2C_ADDRESS_E80P0M_MSP430_GPIO,
				MSP430_REG_STAT_1);
		return ret;
    case CILIX_M_ADC:
		ret = cilix_read_reg(cx, I2C_ADDRESS_E80P0M_MSP430_GPIO,
				MSP430_REG_DET_ADC_HIGH_BYTE);
		ret = ret<<8 | cilix_read_reg(cx, I2C_ADDRESS_E80P0M_MSP430_GPIO,
				MSP430_REG_DET_ADC_LOW_BYTE);
		return ret;
    case CILIX_C_ADC:
		ret = cilix_read_reg(cx, I2C_ADDRESS_T80P0C_MSP430_GPIO,
				MSP430_REG_DET_ADC_HIGH_BYTE);
		ret = ret<<8 | cilix_read_reg(cx, I2C_ADDRESS_T80P0C_MSP430_GPIO,
				MSP430_REG_DET_ADC_LOW_BYTE);
		return ret;
	case CILIX_M_MSP430_RESET:
		return cilix_write_reg(cx, I2C_ADDRESS_E80P0M_MSP430_GPIO,
				MSP430_REG_FUNC_1, FUNC_MSP430_RESET);
	case CILIX_C_MSP430_RESET:
		return cilix_write_reg(cx, I2C_ADDRESS_T80P0C_MSP430_GPIO,
				MSP430_REG_FUNC_1, FUNC_MSP430_RESET);
    case CILIX_I2C_MONITOR_ENABLE:
		return cilix_write_reg(cx, I2C_ADDRESS_E80P0M_MSP430_GPIO,
				MSP430_REG_FUNC_1, FUNC_I2C_CHK_ENABLE);
    case CILIX_I2C_MONITOR_DISABLE:
		return cilix_write_reg(cx, I2C_ADDRESS_E80P0M_MSP430_GPIO,
				MSP430_REG_FUNC_1, FUNC_I2C_CHK_DISABLE);
	case CILIX_OTG_ON:
		return cilix_write_reg(cx, I2C_ADDRESS_T80P0C_MSP430_GPIO,
				MSP430_REG_FUNC_1, FUNC_CILIX_OTG_ON);
	case CILIX_OTG_OFF:
		return cilix_write_reg(cx, I2C_ADDRESS_T80P0C_MSP430_GPIO,
				MSP430_REG_FUNC_1, FUNC_CILIX_OTG_OFF);
	case CILIX_CHARGE_ON:
		return cilix_write_reg(cx, I2C_ADDRESS_T80P0C_MSP430_GPIO,
				MSP430_REG_FUNC_1, FUNC_CILIX_CHARGE_ON);
	case CILIX_CHARGE_OFF:
		return cilix_write_reg(cx, I2C_ADDRESS_T80P0C_MSP430_GPIO,
				MSP430_REG_FUNC_1, FUNC_CILIX_CHARGE_OFF);
    case CILIX_SHIPPING_MODE:
		return cilix_write_reg(cx, I2C_ADDRESS_T80P0C_MSP430_GPIO,
				MSP430_REG_FUNC_1, FUNC_SHIPPING_MODE);
    case CILIX_SHIPPING_MODE_CANCEL:
		return cilix_write_reg(cx, I2C_ADDRESS_T80P0C_MSP430_GPIO,
				MSP430_REG_FUNC_1, FUNC_SHIPPING_MODE_CANCEL);
    case CILIX_PT_MODE_ENTER:
		return cilix_write_reg(cx, I2C_ADDRESS_T80P0C_MSP430_GPIO,
				MSP430_REG_FUNC_1, FUNC_PT_MODE_ENTER);
    case CILIX_PT_MODE_EXIT:
		return cilix_write_reg(cx, I2C_ADDRESS_T80P0C_MSP430_GPIO,
				MSP430_REG_FUNC_1, FUNC_PT_MODE_EXIT);

	case CILIX_GLF72120_EN_STATUS:
		return cilix_read_reg_bit(cx, I2C_ADDRESS_E80P0M_MSP430_GPIO,
				MSP430_REG_GPIO_P1, M_MSP430_BIT_GPIO_GLF72120_EN);
	case CILIX_GLF72120_EN_ENABLE:
		return cilix_write_reg_bit(cx, I2C_ADDRESS_E80P0M_MSP430_GPIO,
				MSP430_REG_GPIO_P1, M_MSP430_BIT_GPIO_GLF72120_EN, 1);
	case CILIX_GLF72120_EN_DISABLE:
		return cilix_write_reg_bit(cx, I2C_ADDRESS_E80P0M_MSP430_GPIO,
				MSP430_REG_GPIO_P1, M_MSP430_BIT_GPIO_GLF72120_EN, 0);

	case CILIX_PIN_CE_STATUS:
		return cilix_read_reg_bit(cx, I2C_ADDRESS_T80P0C_MSP430_GPIO,
				MSP430_REG_GPIO_P1, C_MSP430_BIT_GPIO_PIN_CE);
	case CILIX_PIN_CE_HIGH:
		return cilix_write_reg_bit(cx, I2C_ADDRESS_T80P0C_MSP430_GPIO,
				MSP430_REG_GPIO_P1, C_MSP430_BIT_GPIO_PIN_CE, 1);
	case CILIX_PIN_CE_LOW:
		return cilix_write_reg_bit(cx, I2C_ADDRESS_T80P0C_MSP430_GPIO,
				MSP430_REG_GPIO_P1, C_MSP430_BIT_GPIO_PIN_CE, 0);

	case CILIX_CONN_STATUS:
		return cilix_read_reg_bit(cx, I2C_ADDRESS_E80P0M_MSP430_GPIO,
				MSP430_REG_STAT_1, STAT_CILIX_CONN);
	case USB_CONN_STATUS:
		return cilix_read_reg_bit(cx, I2C_ADDRESS_E80P0M_MSP430_GPIO,
				MSP430_REG_GPIO_P2, M_MSP430_BIT_GPIO_VUSBVADP_IN);

	case SY6974B_WATCHDOG_RESET:
		return cilix_write_reg_bit(cx, I2C_ADDRESS_SY6974B,
				SY6974B_REG1, SY6974B_BIT_WD_RST, 1);
	case SY6974B_WATCHDOG_STATUS:
		ret = cilix_read_reg_mask(cx, I2C_ADDRESS_SY6974B,
				SY6974B_REG5, SY6974B_MASK_WATCHDOG, 
				SY6974B_SHIFT_WATCHDOG);
		return ret == 0 ? 0 : 1;
	case SY6974B_WATCHDOG_ENABLE: /*defualt:0x01->40s*/
		return cilix_write_reg_mask(cx, I2C_ADDRESS_SY6974B,
				SY6974B_REG5, 0x01, SY6974B_MASK_WATCHDOG, 
				SY6974B_SHIFT_WATCHDOG);
	case SY6974B_WATCHDOG_DISABLE:
		return cilix_write_reg_mask(cx, I2C_ADDRESS_SY6974B,
				SY6974B_REG5, 0x00, SY6974B_MASK_WATCHDOG, 
				SY6974B_SHIFT_WATCHDOG);

	case SY6974B_OTG_STATUS:
		return cilix_read_reg_bit(cx, I2C_ADDRESS_SY6974B,
				SY6974B_REG1, SY6974B_BIT_OTG_CONFIG);
	case SY6974B_OTG_ENABLE:
		return cilix_write_reg_bit(cx, I2C_ADDRESS_SY6974B,
				SY6974B_REG1, SY6974B_BIT_OTG_CONFIG, 1);
	case SY6974B_OTG_DISABLE:
		return cilix_write_reg_bit(cx, I2C_ADDRESS_SY6974B,
				SY6974B_REG1, SY6974B_BIT_OTG_CONFIG, 0);
	
	case SY6974B_CHARGE_STATUS:
		ret = cilix_read_reg_mask(cx, I2C_ADDRESS_SY6974B,
				SY6974B_REG1, SY6974B_MASK_CHG_CONFIG, 
				SY6974B_SHIFT_CHG_CONFIG);
		return ret == 1 ? 1 : 0;
	case SY6974B_CHARGE_ENABLE:
		return cilix_write_reg_mask(cx, I2C_ADDRESS_SY6974B,
				SY6974B_REG1, 1, SY6974B_MASK_CHG_CONFIG, 
				SY6974B_SHIFT_CHG_CONFIG);
	case SY6974B_CHARGE_DISABLE:
		return cilix_write_reg_bit(cx, I2C_ADDRESS_SY6974B,
				SY6974B_REG1, SY6974B_BIT_CHG_CONFIG, 0);
	
	case CILIX_CHARGE_STATUS:
		ret = cilix_read_reg_mask(cx, I2C_ADDRESS_SY6974B,
				SY6974B_REG8, SY6974B_MASK_CHRG_STAT,
				SY6974B_SHIFT_CHRG_STAT);
		return ret;

    case CILIX_BAT_CAPACITY:
		ret = cilix_read_reg(cx, I2C_ADDRESS_EG2805,
				EG2805_REG_STATE_OF_CHARGE_H);
		ret = ret<<8 | cilix_read_reg(cx, I2C_ADDRESS_EG2805,
				EG2805_REG_STATE_OF_CHARGE_L);
		return ret;
    case EG2805_AVG_CURRENT:
		ret = cilix_read_reg(cx, I2C_ADDRESS_EG2805,
				EG2805_REG_AVERAGE_CURRENT_H);
		ret = ret<<8 | cilix_read_reg(cx, I2C_ADDRESS_EG2805,
				EG2805_REG_AVERAGE_CURRENT_L);
		return ret > 32768 ? ret-65536 : ret;
    case EG2805_BAT_TEMPERATURE:
		ret = cilix_read_reg(cx, I2C_ADDRESS_EG2805,
				EG2805_REG_TEMPERATURE_H);
		ret = ret<<8 | cilix_read_reg(cx, I2C_ADDRESS_EG2805,
				EG2805_REG_TEMPERATURE_L);
		return (ret / 10 - 273);
    case EG2805_BAT_VOLTAGE:
		ret = cilix_read_reg(cx, I2C_ADDRESS_EG2805,
				EG2805_REG_VOLTAGE_H);
		ret = ret<<8 | cilix_read_reg(cx, I2C_ADDRESS_EG2805,
				EG2805_REG_VOLTAGE_L);
		return ret;
    case EG2805_BAT_TIME_TO_EMPTY:
		ret = cilix_read_reg(cx, I2C_ADDRESS_EG2805,
				EG2805_REG_TIME_TO_EMPTY_H);
		ret = ret<<8 | cilix_read_reg(cx, I2C_ADDRESS_EG2805,
				EG2805_REG_TIME_TO_EMPTY_L);
		return ret;
    case EG2805_BAT_TIME_TO_FULL:
		ret = cilix_read_reg(cx, I2C_ADDRESS_EG2805,
				EG2805_REG_TIME_TO_FULL_H);
		ret = ret<<8 | cilix_read_reg(cx, I2C_ADDRESS_EG2805,
				EG2805_REG_TIME_TO_FULL_L);
		return ret;
	}
	
	return -EINVAL;
}

/* returns a state in enum cilix_states */
static enum cilix_states select_cilix_init_state(struct cilix *cx){
	enum cilix_states state;
	/* reference ntx_cilix.h for printing enum names*/
	const char* state_names[] = {"ST_EBCX_APART", "ST_EBCX_CHARGING", "ST_EBCX_CONN",\
								 "ST_EB_CHARGING", "ST_EB_CHARGING_CX_OTG", "ST_EB_FULL",\
								 "ST_CX_CRITICAL", "ST_CX_BAT_HIGH"};

	if(cx->status->cilix_conn) {
        state = ST_EBCX_CONN;
	} else {
		state = ST_EBCX_APART;
	}	
	dev_info(cx->dev,"%s entered. init state[%s]\n",__func__,state_names[state]);
	
	return state;
}

static void cilix_hrtimer_ctrl(struct hrtimer *timer, int status){
	struct cilix *cx = container_of(timer, struct cilix, status_check_timer);
	dev_dbg(cx->dev,"%s entered.\n",__func__);

	if(status == 0 && cx->status->active == true) {
		/* disable polling timer */
		hrtimer_cancel(timer);
		cx->status->active = false;
		dev_info(cx->dev, "hrtimer canceled\n");
	} else if(status == 1 && cx->status->active == false){
		/* enable polling timer */
		hrtimer_start(timer, ms_to_ktime(CILIX_HRTIMER_INIT_MS),HRTIMER_MODE_REL);
		cx->status->active = true;
		dev_info(cx->dev, "start hrtimer with %ds interval\n",cx->detect_interval);
	} else {
		/* do nothing */
	}
}

bool in_range(int x, int min, int max) {
	return x >= min && x <= max;
}

static enum cilix_events check_ebook_battery_event(int ebook_bat_capacity)
{
	enum cilix_events event = 0;
	static int last_ebook_bat_capacity = -1;

	if(in_range(ebook_bat_capacity, 0, 49) && !in_range(last_ebook_bat_capacity, 0, 49))
		event = EV_EBOOK_BAT_0_49;
	else if(in_range(ebook_bat_capacity, 50, 69) && !in_range(last_ebook_bat_capacity, 50, 69))
		event = EV_EBOOK_BAT_50_69;
	else if(in_range(ebook_bat_capacity, 70, 79) && !in_range(last_ebook_bat_capacity, 70, 79))
		event = EV_EBOOK_BAT_70_79;
	else if(in_range(ebook_bat_capacity, 80, 99) && !in_range(last_ebook_bat_capacity, 80, 99))
		event = EV_EBOOK_BAT_80_99;
	else if(ebook_bat_capacity==100 && last_ebook_bat_capacity!=100)
		event = EV_EBOOK_BAT_FULL;

	last_ebook_bat_capacity = ebook_bat_capacity;		
	return event;
}

static void reset_ebook_battery_event(void)
{
		/* set last_ebook_battery_capacity=-1 to ensure EV_EBOOK_BAT_* events
		   be sent next time even when the capacity is in the same range */
		check_ebook_battery_event(-1);
}

static void cilix_check_status_work(struct work_struct *work)
{
	struct cilix *cx = container_of(work, struct cilix, status_check_work.work);
	enum cilix_events event=0;
	int ret;

    dev_dbg(cx->dev,"%s(): enter\n",__func__);

    /* update usb_conn 
	 * update usb_conn first to prevent false OTG operation
	 */
	ret = cilix_exec_command(cx, USB_CONN_STATUS);
	if (ret!=cx->status->usb_conn){
		if(ret)
			event = EV_USB_CONN;
		else
			event = EV_USB_OUT;
	}
	cx->status->usb_conn = ret;
	dev_dbg(cx->dev,"%s(): usb_conn=%d\n",__func__,ret);
	if(event){
		run_state(cx, event);
		event = 0;
	}

	/* update cilix_conn */
	ret = cilix_exec_command(cx, CILIX_CONN_STATUS);
	if (ret!=cx->status->cilix_conn){
		if(ret)
			event = EV_CILIX_CONN;
		else
			event = EV_CILIX_OUT;
	}
	cx->status->cilix_conn = ret;
	dev_dbg(cx->dev,"%s(): cilix_conn=%d\n",__func__,ret);
	if(event){
		run_state(cx, event);
		event = 0;
	}

	/* After usb_conn and cilix_conn updated,
	 * check if periodic check need to be initialized.
	 * if so, skip battery capacity check in this run
	 * to ensure power supply class is ready.
	 */
	if(cx->status->cilix_conn) {
		if(!cx->status->active){
            /* set sys_min */
            ret = sy6974b_set_sys_min(cx);
            if(ret < 0)
	            dev_info(cx->dev,"[WARNING] set sys_min failed\n");

			/* give an initial state for state machine*/
			cx->cur_state = select_cilix_init_state(cx);
            if(cx->cur_state == ST_EBCX_CONN)
            {
                /*turn off OTG if already enabled without our control*/
                if(cilix_exec_command(cx, SY6974B_OTG_STATUS))
                {
	                dev_info(cx->dev,"turn off otg\n");
                    cilix_exec_command(cx, SY6974B_OTG_DISABLE);
                }
            }
			/* enable periodic check */
			cilix_hrtimer_ctrl(&cx->status_check_timer, 1);
            if(cx->status->usb_conn)
                run_state(cx, EV_USB_CONN);
            return;
		}
	} else {
		cx->status->cilix_bat_capacity = -1;
		/* disable periodic check */
		cilix_hrtimer_ctrl(&cx->status_check_timer, 0);
		return;
	}

	/* update ebook_bat_capacity */
//#ifdef CONFIG_MFD_RICOH619
    if((1==gptHWCFG->m_val.bPMIC) && ricoh619_is_ready()) {
        ret = ricoh619_get_battery_capacity();
        event = check_ebook_battery_event(ret);
        cx->status->ebook_bat_capacity = ret;
        dev_dbg(cx->dev,"%s(): ebook_bat_capacity=%d\n",__func__,ret);
        if(event){
            run_state(cx, event);
            event = 0;
        }
    }
//#endif
//#ifdef CONFIG_MFD_ROHM_BD71828
	if((6==gptHWCFG->m_val.bPMIC) && bd71828_is_ready()) {
        ret = bd71828_get_battery_capacity();
        event = check_ebook_battery_event(ret);
        cx->status->ebook_bat_capacity = ret;
        dev_dbg(cx->dev,"%s(): ebook_bat_capacity=%d\n",__func__,ret);
        if(event){
            run_state(cx, event);
            event = 0;
        }
	}
//#endif

	/* update cilix_bat_capacity */
	if(cx->status->cilix_conn) {
		ret = cilix_exec_command(cx, CILIX_BAT_CAPACITY);
        if(ret < 0)
        {
            dev_info(cx->dev, "[Warning] read battery capacity failed (%d)\n", ret);
        }
        else
		{
            if(ret == 0)
            {
                int bat_voltage;
                bat_voltage = cilix_exec_command(cx, EG2805_BAT_VOLTAGE);
                if(bat_voltage > 3900)
                {
                    dev_info(cx->dev, "[Warning] cilix battery capacity 0%% but > 3.9V, skip!\n");
                    return;
                }
            }

            if(ret <= cx->bat_critical_pct)
                event = EV_CILIX_BAT_OUT;
            else if(ret >= 70)
                event = EV_CILIX_BAT_70_100;
            else if(ret >= 20 && ret < 50)
                event = EV_CILIX_BAT_20_49;
            else if(ret < 20)
				event = EV_CILIX_BAT_0_19;

            cx->status->cilix_bat_capacity = ret;

            dev_dbg(cx->dev,"%s(): cilix_bat_capacity=%d\n",__func__,ret);
	        if(event){
                run_state(cx, event);
                event = 0;
	        }
        }
	}
}

static enum hrtimer_restart cilix_hrtimer_function(struct hrtimer *timer)
{
	struct cilix *cx = container_of(timer, struct cilix, status_check_timer);

	dev_dbg(cx->dev,"%s(): enter\n",__func__);
	queue_delayed_work(system_wq, &cx->status_check_work,0);
	hrtimer_forward_now(timer, ms_to_ktime(cx->detect_interval*1000));

	return HRTIMER_RESTART;
}

/**** charger mode functions ****/

/**** properties functions ****/

/* set current limit in mA */
static int sy6974b_set_current_limit(struct cilix *cx, int mA)
{
	int val;

	if(mA > 3200 || mA < 100){
		dev_info(cx->dev,"Invalid current limit\n");
		return -EINVAL;
	} else {
		val = (mA - 100) / 100;
	}
	return cilix_write_reg_mask(cx, I2C_ADDRESS_SY6974B, SY6974B_REG0,
			 val,SY6974B_MASK_IINLIM, SY6974B_SHIFT_IINLIM);
}

/* get current limit in mA */
static int sy6974b_get_current_limit(struct cilix *cx)
{
	int ret;

	ret = cilix_read_reg_mask(cx, I2C_ADDRESS_SY6974B, SY6974B_REG0,
			SY6974B_MASK_IINLIM, SY6974B_SHIFT_IINLIM);
	return (ret * 100 + 100);
}

/* set charge current in mA */
static int sy6974b_set_charge_current(struct cilix *cx, int mA)
{
	int val;

	if(mA > 3000){
		dev_info(cx->dev,"Invalid charge current\n");
		return -EINVAL;
	} else {
		val = mA / 60;
	}
	return cilix_write_reg_mask(cx, I2C_ADDRESS_SY6974B, SY6974B_REG2,
		 	val, SY6974B_MASK_ICHG, SY6974B_SHIFT_ICHG);
}

/* get charge current in mA */
static int sy6974b_get_charge_current(struct cilix *cx)
{
	int ret;

	ret = cilix_read_reg_mask(cx, I2C_ADDRESS_SY6974B, SY6974B_REG2,
			SY6974B_MASK_ICHG, SY6974B_SHIFT_ICHG);
	return (ret * 60);
}

/* set termination current limit (ITERM) in mA */
static int sy6974b_set_termination_current(struct cilix *cx, int mA)
{
	int val;

	if(mA > 960){
		dev_info(cx->dev,"Invalid termination current limit\n");
		return -EINVAL;
	} else {
		val = (mA / 60) - 1;
	}
	return cilix_write_reg_mask(cx, I2C_ADDRESS_SY6974B, SY6974B_REG3,
		 	val, SY6974B_MASK_ITERM, SY6974B_SHIFT_ITERM);
}

/* get termination current limit (ITERM) in mA */
static int sy6974b_get_termination_current(struct cilix *cx)
{
	int ret;

	ret = cilix_read_reg_mask(cx, I2C_ADDRESS_SY6974B, SY6974B_REG3,
			SY6974B_MASK_ITERM, SY6974B_SHIFT_ITERM);
	return ((ret + 1) * 60);
}

/* set system min voltage limit to 3.2V*/
static int sy6974b_set_sys_min(struct cilix *cx)
{
    /*default value 0b011 = 3.2V*/
	int val = 0b011;

	return cilix_write_reg_mask(cx, I2C_ADDRESS_SY6974B, SY6974B_REG1,
		 	val, SY6974B_MASK_SYS_MIN, SY6974B_SHIFT_SYS_MIN);
}

/* 
 * use for DEBUG ONLY
 * directly send i2c messages to chip 
 * MUST followed by a msp430_sysfs_i2c_read_command
 * if not, may have unexpected interrupt loss
 */
static ssize_t msp430_sysfs_i2c_write_command(struct device *dev,
					   struct device_attribute *attr,
					   const char *buf,
					   size_t count)
{
	struct cilix *cx = dev_to_cx(dev);
	ssize_t sscount;
	ssize_t ret = 0;
	u8 cmd[12];
	int i;


	sscount = sscanf(buf, "%x %x %x %x %x %x %x %x %x %x %x %x", 
					&cmd[0], &cmd[1], &cmd[2], &cmd[3], &cmd[4], &cmd[5],
					&cmd[6], &cmd[7], &cmd[8], &cmd[9], &cmd[10], &cmd[11]);

	if (sscount > 12)
		return -EINVAL;

	__cilix_disable_irq(cx);
	ret = msp430_i2c_write(cx, cmd, sscount);

	if (ret < 0)
		return ret;

	dev_info(cx->dev,"i2c(write) %d bytes sent:\n",sscount);
	for(i=0;i<sscount;i++){
		printk("0x%02X ",cmd[i]);
	}
	printk("\n");
	dev_info(cx->dev,"[WARNING] an i2c_r command is needed\n");

	return count;
}

/* directly set raw value to chip register, format: 'register value' */
static ssize_t cilix_sysfs_register_rw(struct device *dev,
					   struct device_attribute *attr,
					   const char *buf,
					   size_t count)
{
	struct cilix *cx = dev_to_cx(dev);
	ssize_t ret = 0;
	ssize_t length;
	unsigned int addr;
	unsigned int reg;
	unsigned int val=0;

	length = sscanf(buf, "%x %x %x", &addr, &reg, &val);
	if (addr> 255 || val > 255)
		return -EINVAL;
	if(length == 3)	{
		ret = cilix_write_reg(cx, addr, reg, val);
	} else if(length == 2) {
		ret = cilix_read_reg(cx, addr, reg);
		dev_info(cx->dev,"reg=0x%02X\n",ret);
	} else {
		return -EINVAL;
	}
	
	if (ret < 0)
		return ret;
	return count;
}

/* directly set raw value to chip register, format: 'register value' */
static ssize_t cilix_sysfs_register_bit_rw(struct device *dev,
					   struct device_attribute *attr,
					   const char *buf,
					   size_t count)
{
	struct cilix *cx = dev_to_cx(dev);
	ssize_t ret = 0;
	ssize_t length;
	unsigned int addr;
	unsigned int reg;
	unsigned int bit;
	unsigned int val=0;

	length = sscanf(buf, "%x %x %d %x", &addr, &reg, &bit, &val);
	if (addr> 255 || bit > 7 || val > 255)
		return -EINVAL;
	if(length == 4)	{
		ret = cilix_write_reg_bit(cx, addr, reg, bit, val);
	} else if(length == 3) {
		ret = cilix_read_reg_bit(cx, addr, reg, bit);
		dev_info(cx->dev,"bit=%d\n",ret);
	} else {
		return -EINVAL;
	}
	
	if (ret < 0)
		return ret;
	return count;
}

/* interface to sysfs to execute functions provided by cilix msp430 */
static ssize_t cilix_sysfs_exec_c_msp430_func(struct device *dev,
					   struct device_attribute *attr,
					   const char *buf,
					   size_t count)
{
	struct cilix *cx = dev_to_cx(dev);
	ssize_t ret = 0;
	ssize_t length;
    int func_number;

	length = sscanf(buf, "%x", &func_number);

	if(length == 1)	{
        ret = cilix_write_reg(cx, I2C_ADDRESS_T80P0C_MSP430_GPIO,
                MSP430_REG_FUNC_1, func_number);
	} else {
		return -EINVAL;
	}
	
	if (ret < 0)
		return ret;
	return count;
}

/* interface to sysfs to execute functions provided by main msp430 */
static ssize_t cilix_sysfs_exec_m_msp430_func(struct device *dev,
					   struct device_attribute *attr,
					   const char *buf,
					   size_t count)
{
	struct cilix *cx = dev_to_cx(dev);
	ssize_t ret = 0;
	ssize_t length;
    int func_number;

	length = sscanf(buf, "%x", &func_number);

	if(length == 1)	{
        ret = cilix_write_reg(cx, I2C_ADDRESS_E80P0M_MSP430_GPIO,
                MSP430_REG_FUNC_1, func_number);
	} else {
		return -EINVAL;
	}
	
	if (ret < 0)
		return ret;
	return count;
}

static ssize_t cilix_sysfs_exec_m_msp430_reset(struct device *dev,
					   struct device_attribute *attr,
					   const char *buf,
					   size_t count)
{
	struct cilix *cx = dev_to_cx(dev);
	ssize_t ret = 0;

    ret = m_msp430_int_pin_reset();
	
    return count;
}

/* print value of chip register, format: 'register=value' */
static ssize_t cilix_sysfs_print_reg(struct cilix *cx,
				       u8 target, u8 reg,
				       char *buf)
{

	int ret = cilix_read_reg(cx, target, reg);

	if (ret < 0)
		return sprintf(buf, "%#.2x=error %d\n", reg, ret);
	return sprintf(buf, "%#.2x=%#.2x\n", reg, ret);
}

/* show all raw values of chip register, format per line: 'register=value' */
static ssize_t cilix_sysfs_show_registers(struct device *dev,
					    struct device_attribute *attr,
					    char *buf)
{
	struct cilix *cx = dev_to_cx(dev);
	ssize_t ret = 0;
	int i;

	for(i=0;i<12;i++){
		ret += cilix_sysfs_print_reg(cx, I2C_ADDRESS_SY6974B, i, buf+ret);
		msleep(10);
	}
	return ret;
}

/* show all raw values of chip register, format per line: 'register=value' */
static ssize_t eg2805_sysfs_show_registers(struct device *dev,
					    struct device_attribute *attr,
					    char *buf)
{
	struct cilix *cx = dev_to_cx(dev);
	ssize_t ret = 0;
	int i;

	for(i=6;i<48;i++){ // register 0x06-0x2f
		ret += cilix_sysfs_print_reg(cx, I2C_ADDRESS_EG2805, i, buf+ret);
		msleep(10);
	}
	return ret;
}

/* set *_enable entries */
static ssize_t cilix_sysfs_set_enable(struct device *dev,
					struct device_attribute *attr,
					const char *buf,
					size_t count)
{
	struct cilix *cx = dev_to_cx(dev);
	enum cilix_command command;
	long val;
	int ret;

	if (kstrtol(buf, 10, &val) < 0)
		return -EINVAL;

	if (strcmp(attr->attr.name, "glf72120_enable") == 0)
		command = val ? CILIX_GLF72120_EN_ENABLE :
			CILIX_GLF72120_EN_DISABLE;
	else if (strcmp(attr->attr.name, "charge_enable") == 0)
		command = val ? SY6974B_CHARGE_ENABLE :
			SY6974B_CHARGE_DISABLE;
	else if (strcmp(attr->attr.name, "otg_enable") == 0)
		command = val ? SY6974B_OTG_ENABLE :
			SY6974B_OTG_DISABLE;
	else if (strcmp(attr->attr.name, "watchdog_enable") == 0)
		command = val ? SY6974B_WATCHDOG_ENABLE :
			SY6974B_WATCHDOG_DISABLE;
	else if (strcmp(attr->attr.name, "pin_ce") == 0)
		command = val ? CILIX_PIN_CE_HIGH :
			CILIX_PIN_CE_LOW;
	else if (strcmp(attr->attr.name, "shipping_mode") == 0)
		command = val ? CILIX_SHIPPING_MODE :
            CILIX_SHIPPING_MODE_CANCEL;
	else
		return -EINVAL;

	ret = cilix_exec_command(cx, command);
	if (ret < 0)
		return ret;
	return count;
}

/* show *_enable entries */
static ssize_t cilix_sysfs_show_enable(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	struct cilix *cx = dev_to_cx(dev);
	enum cilix_command command;
	int ret;

	if (strcmp(attr->attr.name, "glf72120_enable") == 0)
		command = CILIX_GLF72120_EN_STATUS;
	else if (strcmp(attr->attr.name, "charge_enable") == 0)
		command = SY6974B_CHARGE_STATUS;
	else if (strcmp(attr->attr.name, "otg_enable") == 0)
		command = SY6974B_OTG_STATUS;
	else if (strcmp(attr->attr.name, "watchdog_enable") == 0)
		command = SY6974B_WATCHDOG_STATUS;
	else if (strcmp(attr->attr.name, "pin_ce") == 0)
		command = CILIX_PIN_CE_STATUS;
	else
		return -EINVAL;

	ret = cilix_exec_command(cx, command);
	if (ret < 0)
		return ret;
	return sprintf(buf, "%d\n", ret);
}

/* set *_enable entries */
static ssize_t cilix_sysfs_set_limit(struct device *dev,
					struct device_attribute *attr,
					const char *buf,
					size_t count)
{
	struct cilix *cx = dev_to_cx(dev);
	long val;
	int ret;

	if (kstrtol(buf, 10, &val) < 0)
		return -EINVAL;

	if (strcmp(attr->attr.name, "current_limit") == 0)
		ret = sy6974b_set_current_limit(cx, val);
	else if (strcmp(attr->attr.name, "charge_current") == 0)
		ret = sy6974b_set_charge_current(cx, val);
	else if (strcmp(attr->attr.name, "termination_current") == 0)
	    ret = sy6974b_set_termination_current(cx, val);
	else
		return -EINVAL;
	
	if (ret < 0)
		return ret;
	return count;
}

/* show *_enable entries */
static ssize_t cilix_sysfs_show_limit(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	struct cilix *cx = dev_to_cx(dev);
	int ret;

	if (strcmp(attr->attr.name, "current_limit") == 0)
	    ret = sy6974b_get_current_limit(cx);
	else if (strcmp(attr->attr.name, "charge_current") == 0)
	    ret = sy6974b_get_charge_current(cx);
	else if (strcmp(attr->attr.name, "termination_current") == 0)
	    ret = sy6974b_get_termination_current(cx);
	else
		return -EINVAL;

	if (ret < 0)
		return ret;
	return sprintf(buf, "%d\n", ret);
}

/* set state entries */
static ssize_t cilix_sysfs_set_state(struct device *dev,
					struct device_attribute *attr,
					const char *buf,
					size_t count)
{
	struct cilix *cx = dev_to_cx(dev);
	long val;

	if (kstrtol(buf, 10, &val) < 0)
		return -EINVAL;

	if (strcmp(attr->attr.name, "auto_mode") == 0) {
		cx->status->auto_mode = val;
		if(val) {
			queue_delayed_work(system_wq, &cx->status_check_work,0);
		} else {
			cilix_hrtimer_ctrl(&cx->status_check_timer, 0);
		}
	}
	else if (strcmp(attr->attr.name, "detect_interval") == 0) {
		if(val > 1) /* mininum interval 2 seconds*/
			cx->detect_interval = val;
		else
			return -EINVAL;
	}
	else
		return -EINVAL;

	return count;
}

static ssize_t cilix_sysfs_show_state(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	struct cilix *cx = dev_to_cx(dev);
	int ret;

	if (strcmp(attr->attr.name, "active") == 0)
	    ret = cx->status->active;
	else if (strcmp(attr->attr.name, "auto_mode") == 0)
	    ret = cx->status->auto_mode;
	else if (strcmp(attr->attr.name, "usb_conn") == 0)
    {
        if(cx->status->active)
            ret = cx->status->usb_conn;
        else
	        ret = cilix_exec_command(cx, USB_CONN_STATUS);
    }
	else if (strcmp(attr->attr.name, "cilix_conn") == 0)
    {
        if(cx->status->active)
        {
            /*adc shows connection but communication failed*/
            if(cx->status->cilix_conn && !cx->status->comm_good)
                return sprintf(buf, "-1\n");
            else
                ret = cx->status->cilix_conn;
        }
        else
        {
            ret = cilix_exec_command(cx, CILIX_CONN_STATUS);
        }
    }
	else if (strcmp(attr->attr.name, "ebook_bat_capacity") == 0)
    {
		if (1==gptHWCFG->m_val.bPMIC)
			ret = ricoh619_get_battery_capacity();
		if (6==gptHWCFG->m_val.bPMIC)
			ret = bd71828_get_battery_capacity();
    }
	else if (strcmp(attr->attr.name, "cilix_bat_capacity") == 0)
    {
        if(cx->status->cilix_conn) {
		    if(cx->status->active)
                ret = cx->status->cilix_bat_capacity;
            else
                ret = cilix_exec_command(cx, CILIX_BAT_CAPACITY);
        } else {
		    return -EINVAL;
        }
    }
	else if (strcmp(attr->attr.name, "charge_status") == 0)
    {
        if(cx->status->cilix_conn)
		    ret = cilix_exec_command(cx, CILIX_CHARGE_STATUS);
        else
		    return -EINVAL;
    }
	else if (strcmp(attr->attr.name, "bat_temperature") == 0)
		ret = cilix_exec_command(cx, EG2805_BAT_TEMPERATURE);
	else if (strcmp(attr->attr.name, "bat_voltage") == 0)
		ret = cilix_exec_command(cx, EG2805_BAT_VOLTAGE);
	else if (strcmp(attr->attr.name, "avg_current") == 0)
	{
		ret = cilix_exec_command(cx, EG2805_AVG_CURRENT);
		if(ret < 0)
			return sprintf(buf, "-%d\n", abs(ret));
	}
	else if (strcmp(attr->attr.name, "detect_interval") == 0)
		ret = cx->detect_interval;
	else if (strcmp(attr->attr.name, "verify_bat_parameters") == 0)
		ret = eg2805_verify_EEPROM(cx);
    else if (strcmp(attr->attr.name, "write_bat_parameters") == 0)
        ret = eg2805_write_EEPROM(cx);
    else if (strcmp(attr->attr.name, "write_and_verify_bat_parameters") == 0)
        ret = eg2805_write_and_verify_EEPROM(cx);
    else if (strcmp(attr->attr.name, "calibration") == 0)
        ret = eg2805_calibration(cx);
	else if (strcmp(attr->attr.name, "bat_time_to_empty") == 0)
		ret = cilix_exec_command(cx, EG2805_BAT_TIME_TO_EMPTY);
	else if (strcmp(attr->attr.name, "bat_time_to_full") == 0)
		ret = cilix_exec_command(cx, EG2805_BAT_TIME_TO_FULL);
	else
		return -EINVAL;

	if (ret < 0)
		return ret;
	return sprintf(buf, "%d\n", ret);
}

static ssize_t cilix_sysfs_read_msp430_ver(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	struct cilix *cx = dev_to_cx(dev);
    int high_byte, major, minor, build;
    int target;

	if (strcmp(attr->attr.name, "m_msp430_ver") == 0)
        target = I2C_ADDRESS_E80P0M_MSP430_GPIO;
	else if (strcmp(attr->attr.name, "c_msp430_ver") == 0)
		target = I2C_ADDRESS_T80P0C_MSP430_GPIO;
	else
		return -EINVAL;

	high_byte = cilix_read_reg(cx, target, MSP430_REG_VER_HIGH_BYTE);
	build = cilix_read_reg(cx, target, MSP430_REG_VER_LOW_BYTE);

    if (high_byte < 0 || build < 0)
		return -EINVAL;

    minor = high_byte & 0xf;
    major = high_byte > 4;

	return sprintf(buf, "%d.%d.%d\n", major, minor, build);
}

static ssize_t cilix_sysfs_exec_command(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	struct cilix *cx = dev_to_cx(dev);
	enum cilix_command command;
	int ret;

	if (strcmp(attr->attr.name, "m_msp430_stat") == 0)
		command = CILIX_M_MSP430_STAT;
	else if (strcmp(attr->attr.name, "m_adc") == 0)
		command = CILIX_M_ADC;
	else if (strcmp(attr->attr.name, "c_adc") == 0)
		command = CILIX_C_ADC;
	else
		return -EINVAL;

	ret = cilix_exec_command(cx, command);
	if (ret < 0)
		return ret;
	return sprintf(buf, "%d\n", ret);
}

/*cilix global attributes*/
static DEVICE_ATTR(m_msp430_ver, S_IRUGO,
		cilix_sysfs_read_msp430_ver, NULL);
static DEVICE_ATTR(c_msp430_ver, S_IRUGO,
		cilix_sysfs_read_msp430_ver, NULL);
static DEVICE_ATTR(m_msp430_stat, S_IRUGO,
		cilix_sysfs_exec_command, NULL);
static DEVICE_ATTR(m_adc, S_IRUGO,
		cilix_sysfs_exec_command, NULL);
static DEVICE_ATTR(c_adc, S_IRUGO,
		cilix_sysfs_exec_command, NULL);
static DEVICE_ATTR(detect_interval, S_IWUSR | S_IRUGO,
		cilix_sysfs_show_state, cilix_sysfs_set_state);

static DEVICE_ATTR(i2c_r, S_IWUSR | S_IRUGO,
		NULL, msp430_sysfs_i2c_read_bytes);
static DEVICE_ATTR(i2c_w, S_IWUSR | S_IRUGO,
		NULL, msp430_sysfs_i2c_write_command);

static DEVICE_ATTR(register_rw, S_IWUSR | S_IRUGO,
		NULL, cilix_sysfs_register_rw);
static DEVICE_ATTR(bit_rw, S_IWUSR | S_IRUGO,
		NULL, cilix_sysfs_register_bit_rw);
static DEVICE_ATTR(c_msp430_func, S_IWUSR,
		NULL, cilix_sysfs_exec_c_msp430_func);
static DEVICE_ATTR(m_msp430_func, S_IWUSR,
		NULL, cilix_sysfs_exec_m_msp430_func);
static DEVICE_ATTR(m_msp430_reset, S_IWUSR,
		NULL, cilix_sysfs_exec_m_msp430_reset);

static DEVICE_ATTR(glf72120_enable, S_IWUSR | S_IRUGO,
		cilix_sysfs_show_enable, cilix_sysfs_set_enable);
static DEVICE_ATTR(auto_mode, S_IWUSR | S_IRUGO,
		cilix_sysfs_show_state, cilix_sysfs_set_state);
static DEVICE_ATTR(active, S_IRUGO,
		cilix_sysfs_show_state, NULL);
static DEVICE_ATTR(pin_ce, S_IWUSR | S_IRUGO,
		cilix_sysfs_show_enable, cilix_sysfs_set_enable);
static DEVICE_ATTR(usb_conn, S_IRUGO,
		cilix_sysfs_show_state, NULL);
static DEVICE_ATTR(cilix_conn, S_IRUGO,
		cilix_sysfs_show_state, NULL);
static DEVICE_ATTR(ebook_bat_capacity, S_IRUGO,
		cilix_sysfs_show_state, NULL);
static DEVICE_ATTR(cilix_bat_capacity, S_IRUGO,
		cilix_sysfs_show_state, NULL);
static DEVICE_ATTR(charge_status, S_IRUSR | S_IRUGO,
		cilix_sysfs_show_state, NULL);

/*sy6974b attributes*/
static DEVICE_ATTR(sy6974b_registers, S_IRUGO,
		cilix_sysfs_show_registers, NULL);

static DEVICE_ATTR(charge_enable, S_IWUSR | S_IRUGO,
		cilix_sysfs_show_enable, cilix_sysfs_set_enable);
static DEVICE_ATTR(otg_enable, S_IWUSR | S_IRUGO,
		cilix_sysfs_show_enable, cilix_sysfs_set_enable);
static DEVICE_ATTR(watchdog_enable, S_IWUSR | S_IRUGO,
		cilix_sysfs_show_enable, cilix_sysfs_set_enable);
static DEVICE_ATTR(current_limit, S_IWUSR | S_IRUGO,
		cilix_sysfs_show_limit, cilix_sysfs_set_limit);
static DEVICE_ATTR(charge_current, S_IWUSR | S_IRUGO,
		cilix_sysfs_show_limit, cilix_sysfs_set_limit);
static DEVICE_ATTR(termination_current, S_IWUSR | S_IRUGO,
		cilix_sysfs_show_limit, cilix_sysfs_set_limit);

static DEVICE_ATTR(shipping_mode, S_IWUSR,
		NULL, cilix_sysfs_set_enable);
/*eg2805 attributes*/
static DEVICE_ATTR(eg2805_registers, S_IRUGO,
		eg2805_sysfs_show_registers, NULL);
static DEVICE_ATTR(avg_current, S_IRUGO,
		cilix_sysfs_show_state, NULL);
static DEVICE_ATTR(bat_temperature, S_IRUGO,
		cilix_sysfs_show_state, NULL);
static DEVICE_ATTR(bat_voltage, S_IRUGO,
		cilix_sysfs_show_state, NULL);
static DEVICE_ATTR(verify_bat_parameters, S_IRUGO,
		cilix_sysfs_show_state, NULL);
static DEVICE_ATTR(write_bat_parameters, S_IRUGO,
		cilix_sysfs_show_state, NULL);
static DEVICE_ATTR(write_and_verify_bat_parameters, S_IRUGO,
		cilix_sysfs_show_state, NULL);
static DEVICE_ATTR(calibration, S_IRUGO,
		cilix_sysfs_show_state, NULL);
static DEVICE_ATTR(bat_time_to_empty, S_IRUGO,
		cilix_sysfs_show_state, NULL);
static DEVICE_ATTR(bat_time_to_full, S_IRUGO,
		cilix_sysfs_show_state, NULL);

static struct attribute *cilix_sysfs_attributes[] = {
	&dev_attr_m_msp430_ver.attr,
	&dev_attr_c_msp430_ver.attr,
	&dev_attr_m_msp430_stat.attr,
	&dev_attr_m_adc.attr,
	&dev_attr_c_adc.attr,
	&dev_attr_detect_interval.attr,

	&dev_attr_i2c_r.attr,
	&dev_attr_i2c_w.attr,
	
	&dev_attr_register_rw.attr,
	&dev_attr_bit_rw.attr,
    &dev_attr_c_msp430_func.attr,
    &dev_attr_m_msp430_func.attr,
    &dev_attr_m_msp430_reset.attr,

	&dev_attr_glf72120_enable.attr,
	&dev_attr_pin_ce.attr,
	&dev_attr_auto_mode.attr,
	&dev_attr_active.attr,
	&dev_attr_usb_conn.attr,
	&dev_attr_cilix_conn.attr,
	&dev_attr_ebook_bat_capacity.attr,
	&dev_attr_cilix_bat_capacity.attr,
	
	&dev_attr_charge_status.attr,
	NULL,
};

static struct attribute *sy6974b_sysfs_attributes[] = {
	&dev_attr_sy6974b_registers.attr,
	
	&dev_attr_charge_enable.attr,
	&dev_attr_otg_enable.attr,
	&dev_attr_watchdog_enable.attr,
	&dev_attr_current_limit.attr,
	&dev_attr_charge_current.attr,
	&dev_attr_termination_current.attr,
    
    &dev_attr_shipping_mode.attr,
	NULL,
};

static struct attribute *eg2805_sysfs_attributes[] = {
	&dev_attr_eg2805_registers.attr,
	&dev_attr_avg_current.attr,
	&dev_attr_bat_temperature.attr,
	&dev_attr_bat_voltage.attr,
    &dev_attr_verify_bat_parameters.attr,
    &dev_attr_write_bat_parameters.attr,
    &dev_attr_write_and_verify_bat_parameters.attr,
    &dev_attr_calibration.attr,
    &dev_attr_bat_time_to_empty.attr,
    &dev_attr_bat_time_to_full.attr,
	NULL,
};

static const struct attribute_group cilix_sysfs_attr_group = {
	.attrs = cilix_sysfs_attributes,
};

static const struct attribute_group sy6974b_sysfs_attr_group = {
	.name = "sy6974b",
	.attrs = sy6974b_sysfs_attributes,
};

static const struct attribute_group eg2805_sysfs_attr_group = {
	.name = "eg2805",
	.attrs = eg2805_sysfs_attributes,
};

/* set ebook charge current and input current limit*/
static int _cilix_config_ebook_parameters(int type)
{
//#ifdef CONFIG_MFD_RICOH619
	if((1==gptHWCFG->m_val.bPMIC) && ricoh619_is_ready())
		return _cilix_config_ricoh619_charger_params(type);
//#endif
//#ifdef CONFIG_MFD_ROHM_BD71828
	else if((6==gptHWCFG->m_val.bPMIC) && bd71828_is_ready())
		return _cilix_config_bd71828_charger_params(type);
//#endif
	else
		printk("%s() skipped, power class may not ready\n",__func__);
		return -ENODEV;
};

/* set cilix charge current and input current limit*/
static void _cilix_config_sy6974b_paramters(struct cilix *cx, int ilim, int chg){
	int ret;

	dev_info(cx->dev, "set cilix current limit %d mA and charge current %d mA\n", ilim, chg);

	ret = sy6974b_set_current_limit(cx, ilim);
	if(ret)
		dev_info(cx->dev, "[Warning]%s set ilim failed %d\n",__func__,ret);

	ret = sy6974b_set_charge_current(cx, chg);
	if(ret)
		dev_info(cx->dev, "[Warning]%s set ichg failed %d\n",__func__,ret);

	ret = sy6974b_set_termination_current(cx, CILIX_ITERM);
	if(ret)
		dev_info(cx->dev, "[Warning]%s set iterm failed %d\n",__func__,ret);
}

/* set both ebook and cilix charging parameters after a proper delay */
static void cilix_set_charge_parameters_work(struct work_struct *work)
{
    struct cilix *cx = container_of(work, struct cilix, set_charge_para_work.work);
    int cx_ilim = 0;
    int cx_ichg = 0;

    dev_info(cx->dev,"%s(): enter\n",__func__);
	cx->status->in_setting_progress = true;

   /*before charger type is specified, set cilix to no charge*/ 
    _cilix_config_sy6974b_paramters(cx, 100, 0);

    /*before set ebook charaging paramter, check if state changed*/
    if(cx->eb_charge_type == CILIX_CHARGER_DISCONN){
        dev_info(cx->dev,"%s()-%d: type= %d!\n",__func__, __LINE__, cx->eb_charge_type);
        _cilix_config_ebook_parameters(cx->eb_charge_type);
        return;
    }
    
    /*before set ebook charaging paramter, check if state changed*/
    if(cx->eb_charge_type == CILIX_CHARGER ||
       cx->eb_charge_type == CILIX_CHARGER_02C ||
       cx->eb_charge_type == CILIX_CHARGER_05C)
    {
        dev_info(cx->dev,"%s()-%d: type= %d!\n",__func__, __LINE__, cx->eb_charge_type);
        if(cx->cur_state != ST_EB_CHARGING_CX_OTG) {
            dev_info(cx->dev,"%s(): NOT in ST_EB_CHARGING_CX_OTG, skip!\n",__func__);
        } else {
	        /* set ebook */
            _cilix_config_ebook_parameters(cx->eb_charge_type);
        }
    }
    /* CILIX_CONN_EB_BAT_LOW / CILIX_CONN_EB_BAT_MED / CILIX_CONN_EB_BAT_HIGH */
    else
    {
        dev_dbg(cx->dev,"%s()-%d: type= %d!\n",__func__, __LINE__, cx->eb_charge_type);
        if(cx->cur_state != ST_EBCX_CHARGING) {
            dev_info(cx->dev,"%s(): NOT in ST_EBCX_CHARGING, skip!\n",__func__);
            return;
        } else {
	        /* set ebook */
            /* wait ricoh to detect charger type*/
            if(cx->eb_charge_type != CILIX_CHARGER)
            {
                if (1==gptHWCFG->m_val.bPMIC) 
                {
                    msleep(CILIX_WAIT_FOR_RICOH_CHARGE_TYPE_MS);
                }
                else if(6==gptHWCFG->m_val.bPMIC) 
                {
                    msleep(CILIX_WAIT_FOR_ROHM_CHARGE_TYPE_MS);
                }
                _cilix_config_ebook_parameters(cx->eb_charge_type);
            }
        }
	    /* set cilix */
        if(cx->eb_charge_type == CILIX_CONN_EB_BAT_LOW) 
        {
			if (1==gptHWCFG->m_val.bPMIC) {
				if(ricoh619_is_sdp()) {
					cx_ilim = 100;
					cx_ichg = 0;
				} else {
					cx_ilim = 300;
					cx_ichg = 300;
				}
			}
			else if (6==gptHWCFG->m_val.bPMIC) {
				if(bd71828_is_sdp()) {
					cx_ilim = 100;
					cx_ichg = 0;
				} else {
					cx_ilim = 300;
					cx_ichg = 300;
				}
			}
			else {
				cx_ilim = 300;
				cx_ichg = 300;
			}
	    } 
        else if(cx->eb_charge_type == CILIX_CONN_EB_BAT_MED) 
        {
			if (1==gptHWCFG->m_val.bPMIC) {
				if(ricoh619_is_sdp()) {
					cx_ilim = 100;
					cx_ichg = 0;
				} else {
					cx_ilim = 500;
					cx_ichg = 500;
				}
			}
			else if (6==gptHWCFG->m_val.bPMIC) {
				if(bd71828_is_sdp()) {
					cx_ilim = 100;
					cx_ichg = 0;
				} else {
					cx_ilim = 500;
					cx_ichg = 500;
				}
			}
			else {
				cx_ilim = 500;
				cx_ichg = 500;
			}
        }
        else if(cx->eb_charge_type == CILIX_CONN_EB_BAT_HIGH) 
        {
			if (1==gptHWCFG->m_val.bPMIC) {
				if(ricoh619_is_sdp()) {
					cx_ilim = 400;
					cx_ichg = 400;
				} else {
					cx_ilim = 800;
					cx_ichg = 800;
				}
			}
			else if (6==gptHWCFG->m_val.bPMIC) {
				if(bd71828_is_sdp()) {
					cx_ilim = 400;
					cx_ichg = 400;
				} else {
					cx_ilim = 800;
					cx_ichg = 800;
				}
			}
			else {
				cx_ilim = 800;
				cx_ichg = 800;
			}
        }
        else{
            dev_info(cx->dev,"%s(): unknown charge type(0x%04X) skip!\n",__func__, cx->eb_charge_type);
            return;
        }
        if(cx->cur_state == ST_EBCX_CHARGING)
        {
            _cilix_config_sy6974b_paramters(cx, cx_ilim, cx_ichg);
        }
        else 
        {
            dev_info(cx->dev,"%s(): NOT in ST_EBCX_CHARGING, skip!\n",__func__);
        }
    }
	cx->status->in_setting_progress = false;
}

/*print out m_adc and uart test status*/
static void info_print(struct cilix *cx)
{
    int m_adc, uart_comm;
    
    m_adc = cilix_exec_command(cx, CILIX_M_ADC);
	printk("[cilix] m_adc=%d, ", m_adc);

    uart_comm = cilix_exec_command(cx, CILIX_M_MSP430_STAT) & BIT(STAT_UART_COMM);
	printk("uart comm %s\n", uart_comm ? "error" : "good" );

}

static int FN_CX_CHARGE_ON(struct cilix *cx){
	int ret;

	dev_info(cx->dev, "%s entered\n",__func__);
	ret = cilix_exec_command(cx, CILIX_CHARGE_ON);
	if(ret)
		dev_info(cx->dev, "[Warning]%s failed %d\n",__func__,ret);

	return ret;
};

/* ebook does not see a charger in interrupt when usb plug 
 * if cilix was supplying power to ebook. Need a force redetect for ebook
 * to set correct charging parameters
 */
static int FN_CX_CHRG_ON_EB_REDET(struct cilix *cx){
	int ret;

	dev_info(cx->dev, "%s entered\n",__func__);
	ret = cilix_exec_command(cx, CILIX_CHARGE_ON);
    cx->eb_charge_type = CILIX_CHARGER_DISCONN;
    ret += _cilix_config_ebook_parameters(CILIX_CHARGER_DISCONN);
	if(ret)
		dev_info(cx->dev, "[Warning]%s failed %d\n",__func__,ret);

	return ret;
};

static int FN_CX_CHARGE_OFF(struct cilix *cx){
	int ret;

	dev_info(cx->dev, "%s entered\n",__func__);
	ret = cilix_exec_command(cx, CILIX_CHARGE_OFF);
	if(ret)
		dev_info(cx->dev, "[Warning]%s failed %d\n",__func__,ret);

	return ret;
};

static int FN_CX_OTG_ON(struct cilix *cx){
	int ret = 0;
	dev_info(cx->dev, "%s entered\n",__func__);
    cx->eb_charge_type = CILIX_CHARGER;
    queue_delayed_work(system_wq, &cx->set_charge_para_work, 0);
    queue_delayed_work(system_wq, &cx->otg_on_work, 10);

	return ret;
};

static int FN_CX_OTG_05C(struct cilix *cx){
	int ret;

	dev_info(cx->dev, "%s entered\n",__func__);
    cx->eb_charge_type = CILIX_CHARGER_05C;
    queue_delayed_work(system_wq, &cx->set_charge_para_work, 0);
	if(ret)
		dev_info(cx->dev, "[Warning]%s failed %d\n",__func__,ret);

	return ret;
};

static int FN_CX_OTG_02C(struct cilix *cx){
	int ret;

	dev_info(cx->dev, "%s entered\n",__func__);
    cx->eb_charge_type = CILIX_CHARGER_02C;
    queue_delayed_work(system_wq, &cx->set_charge_para_work, 0);
	if(ret)
		dev_info(cx->dev, "[Warning]%s failed %d\n",__func__,ret);

	return ret;
};

static int FN_CX_OTG_OFF(struct cilix *cx){
	int ret;

	dev_info(cx->dev, "%s entered\n",__func__);
	ret = cilix_exec_command(cx, CILIX_OTG_OFF);
	if(ret)
		dev_info(cx->dev, "[Warning]%s failed %d\n",__func__,ret);
	
	return ret;
};

/* 
 * FN_EB_BAT_*_CHARGE funcions 
 * 1. set ebook charging parameters through ricoh619-battery driver
 * 2. set cilix charging parameters
 */
static int FN_EB_BAT_LOW_CHARGE(struct cilix *cx){
	dev_info(cx->dev, "%s entered\n",__func__);
    cx->eb_charge_type = CILIX_CONN_EB_BAT_LOW;
    queue_delayed_work(system_wq, &cx->set_charge_para_work, 0);
}

static int FN_EB_BAT_MED_CHARGE(struct cilix *cx){
	dev_info(cx->dev, "%s entered\n",__func__);
    cx->eb_charge_type = CILIX_CONN_EB_BAT_MED;
    queue_delayed_work(system_wq, &cx->set_charge_para_work, 0);
}

static int FN_EB_BAT_HIGH_CHARGE(struct cilix *cx){
	dev_info(cx->dev, "%s entered\n",__func__);
    cx->eb_charge_type = CILIX_CONN_EB_BAT_HIGH;
    queue_delayed_work(system_wq, &cx->set_charge_para_work, 0);
}

/* to call ricoh619_charger_detect_ex to redetect ebook charger type*/
static int FN_EB_DETECT_CHG_TYPE(struct cilix *cx){
	int ret;

	dev_info(cx->dev, "%s entered\n",__func__);
    cx->eb_charge_type = CILIX_CHARGER_DISCONN;
	ret = _cilix_config_ebook_parameters(CILIX_CHARGER_DISCONN);
	if(ret)
		dev_info(cx->dev, "[Warning]%s failed %d\n",__func__,ret);

	return ret;
};

/* to set cilix charge at 240mA and redetect ebook charger type*/
static int FN_EB_REDETECT_CX_240MA(struct cilix *cx)
{
	int ret;

	dev_info(cx->dev, "%s entered\n",__func__);
    /*set cilix current limit 300mA and charge current 240mA*/
        _cilix_config_sy6974b_paramters(cx, 300, 240);
    /*set book charge current*/
    cx->eb_charge_type = CILIX_CHARGER_DISCONN;
	ret = _cilix_config_ebook_parameters(CILIX_CHARGER_DISCONN);
	if(ret)
		dev_info(cx->dev, "[Warning]%s failed %d\n",__func__,ret);

	return ret;
}

cilix_fsm_table_t cx_fsm_table[] = {
/*INCOMING EVENT			CURRENT STATE			ACTION					NEXT STATE	*/
	{EV_USB_CONN,			ST_EBCX_APART,			NULL,					ST_EB_CHARGING},
	{EV_CILIX_CONN,			ST_EBCX_APART,			NULL,					ST_EBCX_CONN},
	{EV_USB_OUT,			ST_EB_CHARGING,			FN_CX_CHARGE_ON,		ST_EBCX_APART},
	{EV_CILIX_CONN,			ST_EB_CHARGING,			FN_CX_CHARGE_ON,		ST_EBCX_CHARGING},
	{EV_USB_CONN,			ST_EB_CHARGING_CX_OTG,	FN_CX_CHRG_ON_EB_REDET, ST_EBCX_CHARGING},
	{EV_CILIX_OUT,			ST_EB_CHARGING_CX_OTG,	FN_EB_DETECT_CHG_TYPE,	ST_EBCX_APART},
	{EV_EBOOK_BAT_FULL,		ST_EB_CHARGING_CX_OTG,	FN_CX_CHARGE_ON,		ST_EBCX_CONN},
	{EV_CILIX_BAT_OUT,		ST_EB_CHARGING_CX_OTG,	FN_CX_CHARGE_ON,		ST_CX_BAT_CRITICAL},
	{EV_CILIX_BAT_20_49,    ST_EB_CHARGING_CX_OTG,  FN_CX_OTG_05C,          ST_EB_CHARGING_CX_OTG},
	{EV_CILIX_BAT_0_19,     ST_EB_CHARGING_CX_OTG,  FN_CX_OTG_02C,          ST_EB_CHARGING_CX_OTG},
	{EV_USB_CONN,			ST_CX_BAT_CRITICAL,		FN_CX_CHARGE_ON,		ST_EBCX_CHARGING},
	{EV_CILIX_OUT,			ST_CX_BAT_CRITICAL,		FN_EB_DETECT_CHG_TYPE,	ST_EBCX_APART},
	{EV_USB_OUT,			ST_EBCX_CHARGING,		FN_CX_CHARGE_OFF,		ST_EBCX_CONN},
	{EV_CILIX_OUT,			ST_EBCX_CHARGING,		FN_EB_DETECT_CHG_TYPE,	ST_EBCX_APART},
	{EV_EBOOK_BAT_0_49,		ST_EBCX_CHARGING,		FN_EB_BAT_LOW_CHARGE,	ST_EBCX_CHARGING},
	{EV_EBOOK_BAT_50_69,	ST_EBCX_CHARGING,		FN_EB_BAT_MED_CHARGE,	ST_EBCX_CHARGING},
	{EV_EBOOK_BAT_70_79,	ST_EBCX_CHARGING,		FN_EB_BAT_HIGH_CHARGE,	ST_EBCX_CHARGING},
	{EV_EBOOK_BAT_80_99,	ST_EBCX_CHARGING,		FN_EB_BAT_HIGH_CHARGE,	ST_EBCX_CHARGING},
	{EV_EBOOK_BAT_FULL,		ST_EBCX_CHARGING,		FN_EB_BAT_HIGH_CHARGE,	ST_EBCX_CHARGING},
	{EV_CILIX_BAT_70_100,	ST_EBCX_CHARGING,		FN_EB_REDETECT_CX_240MA,ST_CX_BAT_HIGH},
	{EV_USB_CONN,			ST_EBCX_CONN,			FN_CX_CHARGE_ON,		ST_EBCX_CHARGING},
	{EV_CILIX_OUT,			ST_EBCX_CONN,			FN_EB_DETECT_CHG_TYPE,	ST_EBCX_APART},
	{EV_EBOOK_BAT_0_49,		ST_EBCX_CONN,			FN_CX_OTG_ON,			ST_EB_CHARGING_CX_OTG},
	{EV_EBOOK_BAT_50_69,	ST_EBCX_CONN,			FN_CX_OTG_ON,			ST_EB_CHARGING_CX_OTG},
	{EV_EBOOK_BAT_70_79,	ST_EBCX_CONN,			FN_CX_OTG_ON,			ST_EB_CHARGING_CX_OTG},
	{EV_EBOOK_BAT_80_99,	ST_EBCX_CONN,			FN_CX_OTG_ON,			ST_EB_CHARGING_CX_OTG},
	{EV_CILIX_BAT_OUT,		ST_EBCX_CONN,			NULL,					ST_CX_BAT_CRITICAL},
    {EV_USB_OUT,            ST_CX_BAT_HIGH,         NULL,                   ST_EBCX_CONN},
    {EV_CILIX_OUT,          ST_CX_BAT_HIGH,         NULL,                   ST_EBCX_APART},
};

status_t cx_status = {
	.auto_mode = 0,
	.active = false,
	.usb_conn = false,
	.cilix_conn = false,
    .comm_good = true,
    .in_reset_progress = false,
	.ebook_bat_capacity = -1,
	.cilix_bat_capacity = -1,
};

static void run_state(struct cilix *cx, enum cilix_events event)
{
	int num_of_states = sizeof(cx_fsm_table)/sizeof(struct cilix_fsm_table);
	int i=0;
	/* reference ntx_cilix.h for printing enum names*/
	const char* state_names[] = {"ST_EBCX_APART", "ST_EBCX_CHARGING", "ST_EBCX_CONN",\
								 "ST_EB_CHARGING", "ST_EB_CHARGING_CX_OTG", "ST_EB_FULL",\
								 "ST_CX_CRITICAL", "ST_CX_BAT_HIGH"};
	const char* event_names[] = {"NA", "EV_USB_CONN", "EV_USB_OUT", "EV_CILIX_CONN",\
								 "EV_CILIX_OUT", "EV_EBOOK_BAT_FULL", "EV_EBOOK_BAT_80_99",\
								 "EV_EBOOK_BAT_70_79", "EV_EBOOK_BAT_50_69", "EV_EBOOK_BAT_0_49",\
								 "EV_CILIX_BAT_70_100", "EV_CILIX_BAT_20_49", "EV_CILIX_BAT_0_19",
                                 "EV_CILIX_BAT_OUT"};
	/* skip run_state if not active*/
	if(!cx->status->active)
		return;

	dev_info(cx->dev, "%s entered\n",__func__);
	dev_info(cx->dev, "current state[%s](event:%s)\n", state_names[cx->cur_state], event_names[event]);
	
	for(i=0 ; i<num_of_states ; i++){
		if(event == cx_fsm_table[i].cilix_event && 
			cx->cur_state == cx_fsm_table[i].cur_state) {
			/* Event match current state in the fsm table */
			
			if (cx_fsm_table[i].action) {
				/* execute action() */
				cx_fsm_table[i].action(cx);
			}
			else{
				dev_info(cx->dev, "no action\n");
			}

            /*print info when cilix is disconnected with ebook*/
            if (ST_EBCX_APART == cx_fsm_table[i].next_state)
                info_print(cx);

			/* transfer to new state */
			cx->cur_state = cx_fsm_table[i].next_state;
			dev_info(cx->dev, "new state[%s]\n",state_names[cx->cur_state]);
			
			if(cx_fsm_table[i].cur_state != cx_fsm_table[i].next_state) {
				/* when state changed, reset to make sure ebook battery event
				   will be sent again even it is still in the same range*/
				reset_ebook_battery_event();
			}

            if( cx->cur_state  == ST_EBCX_APART ) {
                /* once disconnected, reset comm fail status */
                cx->status->comm_good = 1;
            }

			break;
		} else {
			/* do nothing */
			dev_dbg(cx->dev,"state=%d, table state=%d, table event=%d\n",\
					cx->cur_state, cx_fsm_table[i].cur_state, cx_fsm_table[i].cilix_event);
		}
	}
};

static int cilix_sysfs_init(struct cilix *cx)
{
	int ret=0;
	ret += sysfs_create_group(&cx->misc_dev.this_device->kobj,
			&cilix_sysfs_attr_group);
	ret += sysfs_create_group(&cx->misc_dev.this_device->kobj,
			&sy6974b_sysfs_attr_group);
	ret += sysfs_create_group(&cx->misc_dev.this_device->kobj,
			&eg2805_sysfs_attr_group);
	return ret;
}

static void cilix_sysfs_exit(struct cilix *cx)
{
	sysfs_remove_group(&cx->misc_dev.this_device->kobj, &cilix_sysfs_attr_group);
	sysfs_remove_group(&cx->misc_dev.this_device->kobj, &sy6974b_sysfs_attr_group);
	sysfs_remove_group(&cx->misc_dev.this_device->kobj, &eg2805_sysfs_attr_group);
}

/* The only purpose for the handler is to signal to any waiting threads that
 * the interrupt is currently being asserted. The driver does not do any
 * processing triggered by interrupts, and the chip provides no way to mask at
 * the source (plus that would be slow over I2C). Run the IRQ as a one-shot,
 * this means it cannot be shared. */
static irqreturn_t cilix_int_handler(int dummy, void *dev_id)
{
	struct cilix *cx = dev_id;

	dev_dbg(cx->dev, "%s() enter\n",__func__);

	if(cx->waiting_i2c_response) {
		cx->intrs++;
		dev_dbg(cx->dev,"intr:%d\n",cx->intrs);
		wake_up(&cx->read_queue);
	} else if(!cx->status->auto_mode) {
		dev_dbg(cx->dev,"skipping interrupt if not in auto mode\n");
	} else {
		dev_dbg(cx->dev,"%s():waked up by cilix\n",__func__);
		queue_delayed_work(system_wq, &cx->status_check_work, 0);

	}
	return IRQ_HANDLED;
}

static int cilix_init_pads(struct cilix *cx){

	cx->pinctrl = devm_pinctrl_get_select_default(cx->dev);
	if (IS_ERR_OR_NULL(cx->pinctrl)) {
		dev_err(cx->dev, "pinctrl not configured!\n");
		return -EINVAL;
	}
	return 0;
}

static int cilix_i2c_probe(struct i2c_client *client,
							const struct i2c_device_id *id)
{
	struct cilix *cx;
	enum cilix_command;
	unsigned long irqflags;
	int ret;
	
	cx = devm_kzalloc(&client->dev, sizeof(struct cilix), GFP_KERNEL);
	if (!cx) {
		ret = -ENOMEM;
		goto err_1;
	}

	cx->dev = &client->dev;
	dev_set_drvdata(cx->dev, cx);

	cx->client = client;
	i2c_set_clientdata(client, cx);

    /*set miscdevice*/
    cx->misc_dev.minor = MISC_DYNAMIC_MINOR,
    cx->misc_dev.name  = "cilix",

    /*read device tree*/
	ret = of_property_read_u32(cx->dev->of_node,"detect_interval", &cx->detect_interval);
	if(ret) {
		cx->detect_interval = 60;
		dev_info(cx->dev, "%s() detect_interval property missing in dts,\
							 use default: 60s\n",__func__);
	}

	ret = of_property_read_u32(cx->dev->of_node,"bat_critical_pct", &cx->bat_critical_pct);
	if(ret) {
		cx->bat_critical_pct = 15;
		dev_info(cx->dev, "%s() bat_critical_pct property missing in dts,\
							 use default: 15%%\n",__func__);
	}

	cx->intrs=0;
	cx->cur_intrs=0;
    cx->eb_charge_type = 0;
	cx->waiting_i2c_response = false;

	/*initialize status*/
	cx->status = &cx_status;

	/* Initialize cilix state machine */
	cx->cur_state = 0;

	/* Initilize communication mutex */
	mutex_init(&cx->i2c_mutex);

	/* Initialize work*/
	INIT_DELAYED_WORK(&cx->status_check_work, cilix_check_status_work);
	INIT_DELAYED_WORK(&cx->set_charge_para_work, cilix_set_charge_parameters_work);
	INIT_DELAYED_WORK(&cx->check_comm_work, check_comm_work_func);
	INIT_DELAYED_WORK(&cx->reset_work, reset_work_func);
	INIT_DELAYED_WORK(&cx->otg_on_work, otg_on_func);

	/* Initilialize status check timer */
	hrtimer_init(&cx->status_check_timer, CLOCK_BOOTTIME, HRTIMER_MODE_REL);
	cx->status_check_timer.function = cilix_hrtimer_function;

	/* Setup interrupt if avaiable */
	if(client->irq) {
		cilix_init_pads(cx);
		init_waitqueue_head(&cx->read_queue);
	
		cx->int_gpio = of_get_named_gpio_flags(cx->dev->of_node, "int-gpio", 0, NULL);
		cx->irq = client->irq;
		dev_dbg(cx->dev, "%s() cx->irq\n", __func__);
		irqflags = irq_get_trigger_type(client->irq);
		ret = devm_request_irq(cx->dev, cx->irq,
				      cilix_int_handler,
				      irqflags | IRQF_ONESHOT,
				      "cilix",
				      cx);
		if (ret) {
			dev_err(cx->dev, "%s() Unable to request irq: %d for use\n",
				__func__, cx->irq);
			cx->irq = 0;
		    goto err_1;
		}
	} else {
		dev_err(cx->dev, "no irq registered, wait 100ms for response");
		cx->irq = 0;
	}

	/* Read MSP430 version */
	ret = cilix_exec_command(cx, CILIX_M_MSP430_VER);
	if (ret < 0) {
		dev_err(cx->dev, "%s() Unable to communicate with M_MSP430\n",__func__);
		goto err_2;
	} else {
		dev_info(cx->dev, "%s() M_MSP430 ver: %d.%d.%d\n", __func__, 
                ret>>12, ret>>8&0xf, ret&0xff);
	}

	/* Register misc device */
	ret = misc_register(&cx->misc_dev);
	if (ret) {
		dev_err(cx->dev, "cilix register driver as misc device error!\n");
		goto err_2;
	}

	ret = cilix_sysfs_init(cx);
	if (ret) {
		dev_err(cx->dev, "failed to create sysfs entries: %d\n", ret);
		goto err_3;
	}

    /* probe succeed . set auto_mode on
       start status check to get the correct status */
    gcx = cx;
    cx->status->auto_mode = 1;
	queue_delayed_work(system_wq, &cx->status_check_work, 0);

	dev_info(cx->dev, "driver registered\n");
	return 0;

err_3:
	cilix_sysfs_exit(cx);
	misc_deregister(&cx->misc_dev);
err_2:
    devm_free_irq(cx->dev, cx->irq, cx);
err_1:
	hrtimer_cancel(&cx->status_check_timer);
	cancel_delayed_work_sync(&cx->status_check_work);
	cancel_delayed_work_sync(&cx->set_charge_para_work);
	cancel_delayed_work_sync(&cx->check_comm_work);
	cancel_delayed_work_sync(&cx->reset_work);
	cancel_delayed_work_sync(&cx->otg_on_work);
    mutex_destroy(&cx->i2c_mutex);
    devm_kfree(cx->dev, cx);
	return ret;
}

static int cilix_suspend(struct device *dev)
{
	struct cilix *cx = dev_get_drvdata(dev);
    int ret;

	dev_dbg(cx->dev, "%s():entered\n",__func__);

    if(cx->status->in_reset_progress)
    {
	    dev_info(cx->dev, "%s():msp430 resetting, suspend failed\n",__func__);
        return -EAGAIN;
    }

    if(cx->status->in_setting_progress)
    {
	    dev_info(cx->dev, "%s():waiting to set charge parameters, suspend failed\n",__func__);
        return -EAGAIN;
    }

    if(gSleep_Mode_Suspend)
    {   
	    dev_info(cx->dev, "%s():msp430 i2c monitor disable\n",__func__);
        /*To prevent msp430 reset itself constantly 
          due to i2c block during hibernation*/
        ret = cilix_exec_command(cx, CILIX_I2C_MONITOR_DISABLE);
        if(ret < 0)
        {
            dev_info(cx->dev,"[WARNING] msp430 i2c monitor disable failed\n");
        }
    }

	enable_gpio_wakeup_src(cx->int_gpio);
	dev_info(cx->dev, "%s():leave\n",__func__);
	
	return 0;
}

static int cilix_resume(struct device *dev)
{
	struct cilix *cx = dev_get_drvdata(dev);
    int ret;

	dev_dbg(cx->dev, "%s():entered\n",__func__);
	disable_gpio_wakeup_src(cx->int_gpio);
    
    if(gSleep_Mode_Suspend)
    {
	    dev_info(cx->dev, "%s():msp430 i2c monitor enable\n",__func__);
        cilix_exec_command(cx, CILIX_I2C_MONITOR_ENABLE);
        if(ret < 0)
        {
            dev_info(cx->dev,"[WARNING] msp430 i2c monitor enable failed\n");
        }
    }
	
	return 0;
}

static int cilix_i2c_remove(struct i2c_client *client)
{
	struct cilix *cx = i2c_get_clientdata(client);

	hrtimer_cancel(&cx->status_check_timer);
	cancel_delayed_work_sync(&cx->status_check_work);
	cancel_delayed_work_sync(&cx->set_charge_para_work);
	cancel_delayed_work_sync(&cx->check_comm_work);
	cancel_delayed_work_sync(&cx->reset_work);
	cancel_delayed_work_sync(&cx->otg_on_work);
	misc_deregister(&cx->misc_dev);
	cilix_sysfs_exit(cx);
	i2c_unregister_device(cx->client);

	return 0;
}

static void cilix_shutdown(struct i2c_client *client)
{
	struct cilix *cx = i2c_get_clientdata(client);

    dev_info(cx->dev, "%s() entered\n",__func__);

    if(cx->status->in_reset_progress)
    {
	    dev_info(cx->dev, "%s():msp430 resetting, wait\n",__func__);
        reinit_completion(&msp430_reset_done);
        wait_for_completion(&msp430_reset_done);
	    dev_info(cx->dev, "%s():msp430 reset done\n",__func__);
    }
}


#ifdef CONFIG_OF
static const struct of_device_id cilix_of_match[] = {
	{ .compatible = "ntx,cilix" },
	{ },
};
MODULE_DEVICE_TABLE(of, cilix_of_match);
#endif

static const struct dev_pm_ops cilix_pm_ops = {
	.suspend = cilix_suspend,
	.resume = cilix_resume,
};

static const struct i2c_device_id cilix_i2c_id[] = {
	{"cilix", 0},
	{}
};
MODULE_DEVICE_TABLE(i2c, cilix_i2c_id);

static struct i2c_driver cilix_i2c_driver = {
	.driver = {
		.name = "cilix",
		.pm = &cilix_pm_ops,
		.of_match_table = of_match_ptr(cilix_of_match),
	},
	.probe = cilix_i2c_probe,
	.remove = cilix_i2c_remove,
    .shutdown = cilix_shutdown,
	.id_table = cilix_i2c_id,
};

static int __init cilix_i2c_init(void)
{
	int ret = -ENODEV;

	ret = i2c_add_driver(&cilix_i2c_driver);
	if (ret != 0)
		pr_err("Failed to register I2C driver: %d\n", ret);

	return ret;
}
subsys_initcall(cilix_i2c_init);

static void __exit cilix_i2c_exit(void)
{
	i2c_del_driver(&cilix_i2c_driver);
}
module_exit(cilix_i2c_exit);

MODULE_DESCRIPTION("CILIX i2c trough uart driver");
MODULE_AUTHOR("William Chen <william.chen@netronixinc.com>");
MODULE_LICENSE("GPL v2");