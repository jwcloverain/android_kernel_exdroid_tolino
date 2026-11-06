/*
 * Wacom Penabled Driver for I2C
 *
 * Copyright (c) 2011 - 2013 Tatsunosuke Tobita, Wacom.
 * <tobita.tatsunosuke@wacom.co.jp>
 *
 * This program is free software; you can redistribute it
 * and/or modify it under the terms of the GNU General
 * Public License as published by the Free Software
 * Foundation; either version of 2 of the License,
 * or (at your option) any later version.
 */

#include <linux/module.h>
#include <linux/input.h>
#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/gpio.h>
#include <linux/delay.h>
#include <asm/unaligned.h>
#include "../init-input.h"

// For digitizer report offset, added by andrew 2019 10-03
#include <linux/proc_fs.h>
#include <asm/uaccess.h>	/* for copy_from_user */

#define USE_IN_SUNXI	1
#define HRTIMER_GET_PT	1

#ifdef HRTIMER_GET_PT//[
#include <linux/hrtimer.h>
#include <linux/ktime.h>


//#define HRTIMER_GET_PT_INTERVAL_US		5000 //:= 150 pts/sec
//#define HRTIMER_GET_PT_INTERVAL_US		3000 //:= 250 pts/sec
//#define HRTIMER_GET_PT_INTERVAL_US		2500 //:= 281 pts/sec
//#define HRTIMER_GET_PT_INTERVAL_US		1500 //:= 392 pts/sec
#define HRTIMER_GET_PT_INTERVAL_US		800 //:= 550 pts/sec

//#define PRINT_REPORT_RATE		1
#define CALC_REPORT_RATE		1

#endif //]HRTIMER_GET_PT
#include "../../../arch/arm/mach-sunxi/ntx_hwconfig.h"
extern volatile NTX_HWCONFIG *gptHWCFG;

#define WACOM_CMD_QUERY0	0x04
#define WACOM_CMD_QUERY1	0x00
#define WACOM_CMD_QUERY2	0x33
#define WACOM_CMD_QUERY3	0x02
#define WACOM_CMD_THROW0	0x05
#define WACOM_CMD_THROW1	0x00
#define WACOM_QUERY_SIZE	19

extern int gSleep_Mode_Suspend;
static struct delayed_work wacom_delay_work;
/*
 *	WACOM touch resolution and direction are designed to be determined by DTB
 *	Only E60P22 is now backward supported with runtime change refrencing hwconfig value
 *	FORCE_DISPLAY_RES_MAPPING to be removed later
 */
//#define FORCE_DISPLAY_RES_MAPPING
#define WACOM_PERFORMANCE_IMPROVE		1


/**
 * ================================================================================ HEAD ===
 * proc file for user to control digitizer report offset
 * added by andrew on 2019 10-03
 * ================================================================================
 */
#define PROCFS_NAME 		"digitizer_offset"
#define STRINGLEN 32
#define OFFSET_HEADER 'G'

/*
 * global_buffer definition
 * global_buffer[0] : Header for check whether to add offset
 * global_buffer[1] : x sign
 * global_buffer[2] : x high byte
 * global_buffer[3] : x low byte
 * global_buffer[4] : y sign
 * global_buffer[5] : y high byte
 * global_buffer[6] : y low byte
 * 
 * For debug
 * global_buffer[1] : enable/disable debug message , clear offset
 */
unsigned char global_buffer[STRINGLEN] = "no_offset\n";

int offset_X = 0, offset_Y = 0;
unsigned char show_debug_message = 0;
char gszFW_Ver[2] = {0};

// This structure hold information about the /proc file
static struct proc_dir_entry *Our_Proc_File;


static ssize_t proc_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
        int len;

        if (*ppos > 0)
                return 0;

        len = strlen(global_buffer) + 1;
        if (copy_to_user(buf, global_buffer, len))
                return -EFAULT;

        *ppos += len;

        return len;
}

static ssize_t proc_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos)
{
        int len;

        if (count >= STRINGLEN)
                len = STRINGLEN - 1;
        else
                len = count;

        if (copy_from_user(global_buffer, buf, len))
                return -EFAULT;

        global_buffer[len] = '\0';
        
	if ( global_buffer[0] == OFFSET_HEADER ){
		
		// Disable debug message
		if ( global_buffer[1] == 'r' ){
		        show_debug_message = 0;
		}
		
		// Enable debug message
		else if ( global_buffer[1] == 'd' ){
		        show_debug_message = 1;
		}
		
		// No offset
		else if ( global_buffer[1] == 'n' ){
		        offset_X = 0;
			offset_Y = 0;
		}
		
		// Set offset
		else {
			offset_X = (global_buffer[2] << 8) + global_buffer[3];
			if( global_buffer[1] == 1 ) offset_X *= -1;
			
			offset_Y = (global_buffer[5] << 8) + global_buffer[6];
			if( global_buffer[4] == 1 ) offset_Y *= -1;
		}
	}
        
        if( show_debug_message ){
		int i = 0;
		printk("%s() : line %d ----------------------------------- show digitizer offset ----------------------------------\n", __func__, __LINE__);
		printk("offset_X = %d , offset_Y = %d \n",  offset_X, offset_Y );
		for( ; i < len ; i++ )		printk("digitizer offset global_buffer[%d] = %d \n", i , global_buffer[i] );
	}

        return len;
}

static const struct file_operations proc_fops = {
        .owner = THIS_MODULE,
        .read = proc_read,
        .write = proc_write,
};
/**
 * ================================================================================
 * proc file for user to control digitizer report offset
 * ================================================================================ END ===
 */


struct wacom_features {
	int x_max;
	int y_max;
	int pressure_max;
	char fw_version;
};
struct wacom_features g_features;

struct wacom_i2c {
	struct i2c_client *client;
	struct input_dev *input;
	u8 data[WACOM_QUERY_SIZE];
	bool prox;
	int tool;

#ifdef FORCE_DISPLAY_RES_MAPPING//[
	unsigned long dwXRes,dwYRes;
#endif //]FORCE_DISPLAY_RES_MAPPING

#ifdef WACOM_PERFORMANCE_IMPROVE//[
	int iGotReady;
	int iIs_IRQ_disabled;
#endif //]WACOM_PERFORMANCE_IMPROVE
	int iFailCnt;
#ifdef HRTIMER_GET_PT//[
	struct hrtimer hrtimer_get_pt;
	ktime_t ktime_get_pt;
#endif //]HRTIMER_GET_PT

};

static struct wacom_i2c *g_wac_i2c;
static int gWacomRST = 0;
static int gWacomINT = 0 ;
static int gTouchDown = 0 ;


#ifdef USE_IN_SUNXI
static struct emr_config_info config_info = {
	.input_type = EMR_TYPE,
	.name = NULL,
	.int_number = 0,
};

static int screen_max_x;
static int screen_max_y;
static int revert_x_flag;
static int revert_y_flag;
static int exchange_x_y_flag;
static int twi_id = -1;
#endif
static struct wacom_features w_features;


#define WACOM_PWRMODE_ON			1
#define WACOM_PWRMODE_SLEEP		0
static int giCurrentPowerMode = WACOM_PWRMODE_ON;
static ssize_t power_mode_write(struct device *dev, struct device_attribute *attr,
		       const char *buf, size_t count)
{
	int ret;
	u8 cmd_sleep[] = { 0x40, 0x00, 0x01, 0x08 };
	u8 cmd_on[] = { 0x40, 0x00, 0x00, 0x08 };
	int iPowerMode;

	struct i2c_msg msgs_sleep[] = {
		{
			.addr = g_wac_i2c->client->addr,
			.flags = 0,
			.len = sizeof(cmd_sleep),
			.buf = cmd_sleep,
		},
	};
	struct i2c_msg msgs_on[] = {
		{
			.addr = g_wac_i2c->client->addr,
			.flags = 0,
			.len = sizeof(cmd_on),
			.buf = cmd_on,
		},
	};
	
	struct i2c_msg *pmsg;
	int iMsgSize;

	if(0==strncmp(buf,"on",2)) {
		pmsg = msgs_on;
		iMsgSize = ARRAY_SIZE(msgs_on);
		iPowerMode = WACOM_PWRMODE_ON;
	}
	else if(0==strncmp(buf,"sleep",5)) {
		pmsg = msgs_sleep;
		iMsgSize = ARRAY_SIZE(msgs_sleep);
		iPowerMode = WACOM_PWRMODE_SLEEP;
	}

	ret = i2c_transfer(g_wac_i2c->client->adapter, pmsg, iMsgSize);
	if (ret < 0)  {
		printk ("[%s-%d] return %d\n",__func__,__LINE__,ret);
		return count;
	}
	if (ret != iMsgSize) {
		printk ("[%s-%d] read %d/%d failed\n",__func__,__LINE__,ret,iMsgSize);
		return count;
	}

	giCurrentPowerMode = iPowerMode ;
	return count;
}
static ssize_t power_mode_read(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	int len;
	if (WACOM_PWRMODE_ON==giCurrentPowerMode) {
		len = sprintf(buf,"on\n");
	}
	else if (WACOM_PWRMODE_SLEEP==giCurrentPowerMode){
		len = sprintf(buf,"sleep\n");
	}
	return len;
}

static DEVICE_ATTR(power_mode,0660,power_mode_read,power_mode_write);


static int giHZ_pts = 0;
static ssize_t HZ_pts_read(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	int len;
	len = sprintf(buf,"%d\n",giHZ_pts);
	return len;
}

static ssize_t FW_ver_read(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	int len,error;

	int ret;
	u8 cmd1[] = { WACOM_CMD_QUERY0, WACOM_CMD_QUERY1,
			WACOM_CMD_QUERY2, WACOM_CMD_QUERY3 };
	u8 cmd2[] = { WACOM_CMD_THROW0, WACOM_CMD_THROW1 };
	u8 data[WACOM_QUERY_SIZE];
	struct i2c_msg msgs[] = {
		{
			.addr = g_wac_i2c->client->addr,
			.flags = 0,
			.len = sizeof(cmd1),
			.buf = cmd1,
		},
		{
			.addr = g_wac_i2c->client->addr,
			.flags = 0,
			.len = sizeof(cmd2),
			.buf = cmd2,
		},
		{
			.addr = g_wac_i2c->client->addr,
			.flags = I2C_M_RD,
			.len = sizeof(data),
			.buf = data,
		},
	};

	ret = i2c_transfer(g_wac_i2c->client->adapter, msgs, ARRAY_SIZE(msgs));
	if (ret < 0)  {
		printk ("[%s-%d] return %d\n",__func__,__LINE__,ret);
		return ret;
	}
	if (ret != ARRAY_SIZE(msgs)) {
		printk ("[%s-%d] read %d/%d failed\n",__func__,__LINE__,ret, ARRAY_SIZE(msgs));
		return -EIO;
	}

	gszFW_Ver[0] = get_unaligned_le16(&data[13]);
	gszFW_Ver[1] = get_unaligned_le16(&data[14]);

	len = sprintf(buf,"0x%02x%02x\n",gszFW_Ver[1],gszFW_Ver[0]);
	return len;
}

static DEVICE_ATTR(HZ_pts,S_IRUGO,HZ_pts_read,NULL);
static DEVICE_ATTR(FW_ver,S_IRUGO,FW_ver_read,NULL);

static const struct attribute *sysfs_attrs[] = {
	&dev_attr_power_mode.attr,
	&dev_attr_HZ_pts.attr,
	&dev_attr_FW_ver.attr,
	NULL,
};


void ntx_wacom_reset(bool on) {
    gpio_direction_output (gWacomRST, on);
}

static int wacom_int_level(void)
{
	unsigned v;
	v = gpio_get_value(config_info.int_number);
        //printk(KERN_ERR"=====%s_%d=====\n",__FUNCTION__,v);
	return v?0:1;
}

#ifdef HRTIMER_GET_PT//[
static enum hrtimer_restart hrtimer_get_pt(struct hrtimer *timer)
{
	//schedule_delayed_work(&wacom_delay_work,0);
	//schedule_delayed_work_on(0,&wacom_delay_work,0);
	queue_work_on(0, system_highpri_wq, &wacom_delay_work.work);
	return HRTIMER_NORESTART;
}
#endif //]HRTIMER_GET_PT


static int wacom_query_device(struct i2c_client *client,
			      struct wacom_features *features)
{
	int ret;
	u8 cmd1[] = { WACOM_CMD_QUERY0, WACOM_CMD_QUERY1,
			WACOM_CMD_QUERY2, WACOM_CMD_QUERY3 };
	u8 cmd2[] = { WACOM_CMD_THROW0, WACOM_CMD_THROW1 };
	u8 data[WACOM_QUERY_SIZE];
	struct i2c_msg msgs[] = {
		{
			.addr = client->addr,
			.flags = 0,
			.len = sizeof(cmd1),
			.buf = cmd1,
		},
		{
			.addr = client->addr,
			.flags = 0,
			.len = sizeof(cmd2),
			.buf = cmd2,
		},
		{
			.addr = client->addr,
			.flags = I2C_M_RD,
			.len = sizeof(data),
			.buf = data,
		},
	};

	ret = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
	if (ret < 0)  {
		printk ("[%s-%d] return %d\n",__func__,__LINE__,ret);
		return ret;
	}
	if (ret != ARRAY_SIZE(msgs)) {
		printk ("[%s-%d] read %d/%d failed\n",__func__,__LINE__,ret, ARRAY_SIZE(msgs));
		return -EIO;
	}

#ifdef FORCE_DISPLAY_RES_MAPPING
	features->y_max = get_unaligned_le16(&data[3]);
	features->x_max = get_unaligned_le16(&data[5]);
#else
	features->x_max = get_unaligned_le16(&data[3]);
	features->y_max = get_unaligned_le16(&data[5]);
#endif
	features->pressure_max = get_unaligned_le16(&data[11]);
	features->fw_version = get_unaligned_le16(&data[13]);	// data[13] , data[14] FW version
															// 0x32 , 0x02 -> 0x0232 
	gszFW_Ver[0] = get_unaligned_le16(&data[13]) ;
	gszFW_Ver[1] = get_unaligned_le16(&data[14]) ;

	printk(KERN_ERR"[WACOM] FW: 0x%02x%02x \n",gszFW_Ver[1],gszFW_Ver[0]);

	dev_dbg(&client->dev,
		"x_max:%d, y_max:%d, pressure:%d, fw:%d\n",
		features->x_max, features->y_max,
		features->pressure_max, features->fw_version);
	printk("x_max:%d, y_max:%d, pressure:%d, fw:%d\n",
		features->x_max, features->y_max,
		features->pressure_max, features->fw_version);

	return 0;
}


static void wacom_work_func(struct work_struct *work)
{
	struct input_dev *input = g_wac_i2c->input;
	u8 *data = g_wac_i2c->data;
	unsigned int x, y, pressure;
	unsigned int wac_x, wac_y;
	unsigned char tsw, f1, f2, ers;
	int error;
	unsigned char inv,rdy;

#ifdef CALC_REPORT_RATE//[
	static int giPtCnt = 0;
	static unsigned long gdwLastJiffies = 0;

	unsigned long dwCurJiffies = jiffies,dwDiffTicks;


	giPtCnt++;

	if(0==gdwLastJiffies) {
		gdwLastJiffies = jiffies;
	}
#endif //]CALC_REPORT_RATE


 
#ifdef HRTIMER_GET_PT //[
	hrtimer_cancel(&g_wac_i2c->hrtimer_get_pt);
#endif //]HRTIMER_GET_PT
	
#ifdef WACOM_PERFORMANCE_IMPROVE//[
	g_wac_i2c->iGotReady = 0;
#endif

	error = i2c_master_recv(g_wac_i2c->client,
				g_wac_i2c->data, sizeof(g_wac_i2c->data));
	if (error < 0) {
		printk ("[%s-%d] return %d\n",__func__,__LINE__,error);
		goto out;
	}
	if (error != sizeof(g_wac_i2c->data)) {
		printk ("[%s-%d] read %d/%d failed\n",__func__,__LINE__,error, sizeof(g_wac_i2c->data));
		error = -1;
		goto out;
	}
	if (0x02 != data[2] && 0x1a != data[2]) {
		printk ("[%s-%d] illegal header %x, %x, %x, %x \n", __func__, __LINE__, data[0],data[1],data[2],data[3]);
		error = -1;
		goto out;
	}

	tsw = data[3] & 0x01;
	ers = data[3] & 0x04;
	f1 = data[3] & 0x02;
	f2 = data[3] & 0x10;
	inv = data[3] & 0x03;
	rdy = data[3] & 0x20;

#ifdef WACOM_PERFORMANCE_IMPROVE//[
	if(rdy) {
		g_wac_i2c->iGotReady = 1;
		if(!g_wac_i2c->iIs_IRQ_disabled) {
			disable_irq_nosync(g_wac_i2c->client->irq);
			g_wac_i2c->iIs_IRQ_disabled=1;
		}
	}
	else {
		g_wac_i2c->iGotReady = 0;
		if(g_wac_i2c->iIs_IRQ_disabled) {
			enable_irq(g_wac_i2c->client->irq);
			g_wac_i2c->iIs_IRQ_disabled=0;
		}
	}
#endif //]WACOM_PERFORMANCE_IMPROVE

#ifdef FORCE_DISPLAY_RES_MAPPING//[
	wac_y = le16_to_cpup((__le16 *)&data[4]);
	wac_x = le16_to_cpup((__le16 *)&data[6]);
#else if 
	wac_x = le16_to_cpup((__le16 *)&data[4]);
	wac_y = le16_to_cpup((__le16 *)&data[6]);
#endif //] FORCE_DISPLAY_RES_MAPPING
	pressure = le16_to_cpup((__le16 *)&data[8]);

	if (g_features.x_max <  wac_x || g_features.y_max < wac_y || g_features.pressure_max < pressure) {
		printk ("[%s-%d] illegal stylus (%d, %d) p %d \n", __func__, __LINE__, wac_x,wac_y,pressure);
		error = -1;
		goto out;
	}
#ifdef FORCE_DISPLAY_RES_MAPPING//[
	if (10 == gptHWCFG->m_val.bDisplayPanel) 
	{
		// 6.8inch Bottom .
		if(78==gptHWCFG->m_val.bPCB) {
			// E60QRX 
			if(1==gptHWCFG->m_val.bUIStyle) // customer
			{
				x = g_wac_i2c->dwXRes -  wac_y * g_wac_i2c->dwXRes / g_features.y_max;
				y = g_wac_i2c->dwYRes - ((wac_x) * g_wac_i2c->dwYRes/g_features.x_max);
			}
			else if(0==gptHWCFG->m_val.bUIStyle) // ebrmain
			{
				x = (wac_y) * g_wac_i2c->dwXRes/g_features.y_max;
				y = (g_features.x_max-wac_x) * g_wac_i2c->dwYRes/g_features.x_max;
			}
			else if(2==gptHWCFG->m_val.bUIStyle) // android
			{
				x = (wac_y) * g_wac_i2c->dwXRes/g_features.y_max;
				y = (wac_x) * g_wac_i2c->dwYRes/g_features.x_max;
			}
		}
                else if(91==gptHWCFG->m_val.bPCB) {
			// E60P22 
			if(1==gptHWCFG->m_val.bUIStyle) // customer
			{
				x = g_wac_i2c->dwXRes -  wac_y * g_wac_i2c->dwXRes / g_features.y_max;
				y = g_wac_i2c->dwYRes - ((wac_x) * g_wac_i2c->dwYRes/g_features.x_max);
			}
			else if(0==gptHWCFG->m_val.bUIStyle) // ebrmain
			{
				x = (g_wac_i2c->dwXRes * wac_y)  /g_features.y_max;
				y = (g_wac_i2c->dwYRes * wac_x) /g_features.x_max;
			}
			else if(2==gptHWCFG->m_val.bUIStyle) // android
			{
				x = (wac_y) * g_wac_i2c->dwXRes/g_features.y_max;
				x = g_wac_i2c->dwXRes - x;
				y = (wac_x) * g_wac_i2c->dwYRes/g_features.x_max;
				y = g_wac_i2c->dwYRes - y;
			}
		}
		else {
			y = (g_features.y_max-wac_y) * g_wac_i2c->dwYRes/g_features.y_max;
			x = (wac_x) * g_wac_i2c->dwXRes/g_features.x_max;
		}
	} 
	else if(35==gptHWCFG->m_val.bDisplayPanel) {
		// 8" Right EPD
		if(2==gptHWCFG->m_val.bUIStyle) {	// android
			x = g_wac_i2c->dwXRes -  wac_y * g_wac_i2c->dwXRes / g_features.y_max;
			y = g_wac_i2c->dwYRes - ((wac_x) * g_wac_i2c->dwYRes/g_features.x_max);
		}
		else if(0==gptHWCFG->m_val.bUIStyle){ // ebrmain
			y = (wac_y) * g_wac_i2c->dwYRes/g_features.y_max;
			x = (wac_x) * g_wac_i2c->dwXRes/g_features.x_max;
			//x = (g_features.y_max-wac_y) * g_wac_i2c->dwYRes/g_features.y_max;
			//y = (wac_x) * g_wac_i2c->dwXRes/g_features.x_max;
		}
		else if(1==gptHWCFG->m_val.bUIStyle){ // Customer UI
			y = (g_features.y_max-wac_y) * g_wac_i2c->dwYRes/g_features.y_max;
			x = (wac_x) * g_wac_i2c->dwXRes/g_features.x_max;
		}
	}	
	else if(28==gptHWCFG->m_val.bDisplayPanel) {
		// 10.3inch Top.
		if(89==gptHWCFG->m_val.bPCB &&2==gptHWCFG->m_val.bUIStyle) {// android
			x = (wac_y) * g_wac_i2c->dwXRes/g_features.y_max;
			y = (wac_x) * g_wac_i2c->dwYRes/g_features.x_max;
		}else if(2==gptHWCFG->m_val.bUIStyle) {	// android
			x = g_wac_i2c->dwXRes -  wac_y * g_wac_i2c->dwXRes / g_features.y_max;
			y = g_wac_i2c->dwYRes - ((wac_x) * g_wac_i2c->dwYRes/g_features.x_max);
		}
		else if(0==gptHWCFG->m_val.bUIStyle){ // ebrmain
			x = (g_features.y_max-wac_y) * g_wac_i2c->dwYRes/g_features.y_max;
			y = (wac_x) * g_wac_i2c->dwXRes/g_features.x_max;
		}
		else if(1==gptHWCFG->m_val.bUIStyle){ // Customer UI
			x = (wac_y) * g_wac_i2c->dwYRes/g_features.y_max;
			y = (wac_x) * g_wac_i2c->dwXRes/g_features.x_max;
		}
	}
	else if(6==gptHWCFG->m_val.bDisplayPanel) {
		// 6ch bottom
		//printk("x=%d,y=%d\n",wac_x,wac_y);
		if(1==gptHWCFG->m_val.bDisplayResolution) {//
			// 1024x758 .
			x =  g_wac_i2c->dwXRes - wac_x * g_wac_i2c->dwXRes / g_features.x_max;
			y =  wac_y * g_wac_i2c->dwYRes/g_features.y_max;
		}
		else {
			// 1448x1072 . 
			//if(2==gptHWCFG->m_val.bUIStyle) 
			{// android
				x =  wac_x * g_wac_i2c->dwXRes / g_features.x_max;
				y =  g_wac_i2c->dwYRes -((wac_y) * g_wac_i2c->dwYRes/g_features.y_max);
			}
		}
	}
	else 
	{
		if(2==gptHWCFG->m_val.bUIStyle) {
			// android
			y = (wac_y) * g_wac_i2c->dwYRes/g_features.y_max;
			x = (g_features.x_max-wac_x) * g_wac_i2c->dwXRes/g_features.x_max;
		}
		else {
			y = (g_features.y_max-wac_y) * g_wac_i2c->dwYRes/g_features.y_max;
			x = (wac_x) * g_wac_i2c->dwXRes/g_features.x_max;
		}
	}
#else //][!FORCE_DISPLAY_RES_MAPPING
	{
		//printk("g_features.screen_max_x=%d, g_features.screen_max_y=%d\n", g_features.x_max, g_features.y_max);
		//printk("screen_max_x=%d, screen_max_y=%d\n", screen_max_x, screen_max_y);
		//printk("revert_x_flag=%d, revert_y_flag=%d,exchange_x_y=%d\n", revert_x_flag, revert_y_flag, exchange_x_y_flag);

		x = wac_x * screen_max_x / g_features.x_max;
		y = wac_y * screen_max_y / g_features.y_max;
		if (revert_x_flag) {
			x = screen_max_x - x;
		}
		if (revert_y_flag) {
			y = screen_max_y - y;
		}
		if (exchange_x_y_flag) {
			int tmp = x;
			x = y;
			y = tmp;
		}
		//printk("X=%d, Y=%d, read_x=%d,read_y=%d\n",x,y,(int)wac_x,(int)wac_y);
	}
#endif //] FORCE_DISPLAY_RES_MAPPING

	if (!g_wac_i2c->prox)
		g_wac_i2c->tool = (data[3] & 0x0c) ?
			BTN_TOOL_RUBBER : BTN_TOOL_PEN;

	g_wac_i2c->prox = data[3] & 0x20;
	
	// Add offset if "the proc entry" start with OFFSET_HEADER
	if ( global_buffer[0] == OFFSET_HEADER ){
		x += offset_X;
		y += offset_Y;
	}

	//printk ("[%s-%d] stylus %02X, (%d, %d) p=%d\n", __func__, __LINE__, data[3], x,y,pressure);
	if(0==gptHWCFG->m_val.bUIStyle) // Ebrmain
	{
		if(tsw)
		{
			gTouchDown = 1 ;
			input_report_key(input, BTN_TOUCH, tsw || ers);		
			input_report_abs(input, ABS_X, x);
			input_report_abs(input, ABS_Y, y);
			input_report_abs(input, ABS_PRESSURE, pressure);
		}
		else{
			if(gTouchDown)
			{
				input_report_key(input, BTN_TOUCH, tsw || ers);		
				input_report_abs(input, ABS_X, x);
				input_report_abs(input, ABS_Y, y);
				input_report_abs(input, ABS_PRESSURE, pressure);
				gTouchDown = 0 ;
				//printk(KERN_ERR"==== Touch Up ==== w:%d \n",pressure);
			}
		}
	}
	else{
		input_report_key(input, BTN_TOUCH, tsw || ers);
		input_report_key(input, g_wac_i2c->tool, g_wac_i2c->prox);
		input_report_key(input, BTN_STYLUS, f1);
		input_report_key(input, BTN_STYLUS2, f2);
		input_report_abs(input, ABS_X, x);
		input_report_abs(input, ABS_Y, y);
		input_report_abs(input, ABS_PRESSURE, pressure);
	}
#if 0
	printk("%s :(%d,%d,%d),tsw=%d,ssw1=%d,ssw2=%d,esr=%d,inv=%d,rdy=%d,wac_x=%d,wac_y=%d\n",
			__func__,(int)x,(int)y,(int)pressure,
			tsw?1:0,f1?1:0,f2?1:0,ers?1:0,inv?1:0,rdy?1:0,
			(int)wac_x,(int)wac_y);
#endif

	input_sync(input);

out:
	if (0 > error) {
		if (5 < g_wac_i2c->iFailCnt++) {
			g_wac_i2c->iFailCnt = 0;
			printk("%s() failed over 5 times, reseting...\n",__func__);
			ntx_wacom_reset(0);
			msleep (50);
			ntx_wacom_reset(1);
		}
#ifdef WACOM_PERFORMANCE_IMPROVE//[
		printk("%s() failed count %d times. (int %d, irq_disabled %d)\n",__func__, g_wac_i2c->iFailCnt, wacom_int_level(), g_wac_i2c->iIs_IRQ_disabled);
#else//][!WACOM_PERFORMANCE_IMPROVE
		printk("%s() failed count %d times. (int %d)\n",__func__, g_wac_i2c->iFailCnt, wacom_int_level());
#endif //]WACOM_PERFORMANCE_IMPROVE
	}
	else
		g_wac_i2c->iFailCnt = 0;

#ifdef WACOM_PERFORMANCE_IMPROVE//[
	if ( wacom_int_level() || g_wac_i2c->iGotReady || g_wac_i2c->iIs_IRQ_disabled) 
	{
#ifdef HRTIMER_GET_PT//[
		g_wac_i2c->hrtimer_get_pt.function = hrtimer_get_pt;
		g_wac_i2c->ktime_get_pt = ktime_set(HRTIMER_GET_PT_INTERVAL_US/1000000,(HRTIMER_GET_PT_INTERVAL_US%1000000)*1000);
		hrtimer_start(&g_wac_i2c->hrtimer_get_pt,g_wac_i2c->ktime_get_pt,HRTIMER_MODE_REL);
#else //][!HRTIMER_GET_PT
		schedule_delayed_work(&wacom_delay_work, (0>error)?1:0);
		//schedule_delayed_work(&wacom_delay_work, (0>error || (jiffies&1))?1:0);
#endif //] HRTIMER_GET_PT
	}
#else//][!WACOM_PERFORMANCE_IMPROVE
	if ( wacom_int_level() )
		schedule_delayed_work(&wacom_delay_work, 1);
#endif //]WACOM_PERFORMANCE_IMPROVE

#ifdef CALC_REPORT_RATE//[
	dwDiffTicks = dwCurJiffies-gdwLastJiffies;
	if(dwDiffTicks>=HZ) {
#ifdef PRINT_REPORT_RATE//[
		printk("%d PTs in %d ms\n",giPtCnt,jiffies_to_msecs(dwDiffTicks));
#endif //] PRINT_REPORT_RATE
		giHZ_pts = giPtCnt;
		giPtCnt = 0;
		gdwLastJiffies = jiffies;
	}
#endif //]CALC_REPORT_RATE
}

#if 0
static irqreturn_t wacom_i2c_irq(int irq, void *dev_id)
{
	struct wacom_i2c *wac_i2c = dev_id;
	struct input_dev *input = wac_i2c->input;
	u8 *data = wac_i2c->data;
	unsigned int x, y, pressure;
	unsigned char tsw, f1, f2, ers;
	int error;

	error = i2c_master_recv(wac_i2c->client,
				wac_i2c->data, sizeof(wac_i2c->data));
	if (error < 0)
		goto out;

	tsw = data[3] & 0x01;
	ers = data[3] & 0x04;
	f1 = data[3] & 0x02;
	f2 = data[3] & 0x10;
	x = le16_to_cpup((__le16 *)&data[4]);
	y = le16_to_cpup((__le16 *)&data[6]);
	pressure = le16_to_cpup((__le16 *)&data[8]);

	if (!wac_i2c->prox)
		wac_i2c->tool = (data[3] & 0x0c) ?
			BTN_TOOL_RUBBER : BTN_TOOL_PEN;
#ifdef USE_IN_SUNXI
	wac_i2c->prox = data[3] & 0x20;
	x = x * screen_max_x / w_features.x_max;
	y = y * screen_max_y / w_features.y_max;
	if (revert_x_flag) {
		x = screen_max_x - x;
	}
	if (revert_y_flag) {
		y = screen_max_y - y;
	}
	if (exchange_x_y_flag) {
		int tmp = x;
		x = y;
		y = tmp;
	}
#endif

	input_report_key(input, BTN_TOUCH, tsw || ers);
	input_report_key(input, wac_i2c->tool, wac_i2c->prox);
	input_report_key(input, BTN_STYLUS, f1);
	input_report_key(input, BTN_STYLUS2, f2);
	input_report_abs(input, ABS_X, x);
	input_report_abs(input, ABS_Y, y);
	input_report_abs(input, ABS_PRESSURE, pressure);
	input_sync(input);
	//pr_debug("%s: %d, %d\n", __func__, x, y);

out:
	return IRQ_HANDLED;
}
#else
static irqreturn_t wacom_i2c_irq(int irq, void *dev_id)
{
	//schedule_delayed_work(&wacom_delay_work, 0);
	queue_work_on(0, system_highpri_wq, &wacom_delay_work.work);
	return IRQ_HANDLED;
}

#endif

static int wacom_i2c_open(struct input_dev *dev)
{
	printk("%s()\n",__func__);
	enable_irq(g_wac_i2c->client->irq);
	ntx_wacom_reset(1);	
	if ( wacom_int_level() ) {
		//schedule_delayed_work(&wacom_delay_work, 0);
		queue_work_on(0, system_highpri_wq, &wacom_delay_work.work);
  }
	return 0;
}

static void wacom_i2c_close(struct input_dev *dev)
{
	struct wacom_i2c *wac_i2c = input_get_drvdata(dev);
	struct i2c_client *client = wac_i2c->client;
	printk("%s()\n",__func__);

	disable_irq(client->irq);
}

static int wacom_i2c_probe(struct i2c_client *client,
				     const struct i2c_device_id *id)
{
	struct input_dev *input;
	int error;
	
	gWacomINT = config_info.int_number ;
#ifdef NTX
	gWacomINT = of_get_named_gpio(client->dev.of_node, "gpio_intr", 0);
	if (gpio_is_valid(gWacomINT)) {
		if (0 > devm_gpio_request_one(&client->dev, gWacomINT, GPIOF_IN, "WACOM_INT")) 
			dev_err(&client->dev, "request WACOM_INT gpio failed !\n");
		printk(KERN_ERR"==== WACOM INT :%d ====\n",gWacomINT);
	}

	gWacomRST = of_get_named_gpio(client->dev.of_node, "gpio_rst", 0);
	if (gpio_is_valid(gWacomRST)) {
		if (0 > devm_gpio_request_one(&client->dev, gWacomRST, GPIOF_OUT_INIT_HIGH, "WACOM_RST")) 
			dev_err(&client->dev, "request WACOM_RST gpio failed !\n");
		printk(KERN_ERR"==== WACOM RST :%d ====\n",gWacomRST);			
	}
#endif
	
	msleep(10);
	//ntx_wacom_reset(1);
	msleep(40);
	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		dev_err(&client->dev, "i2c_check_functionality error\n");
		return -EIO;
	}

	error = wacom_query_device(client, &g_features);
	if (error)
		return error;

	g_wac_i2c = kzalloc(sizeof(*g_wac_i2c), GFP_KERNEL);
	input = input_allocate_device();
	if (!g_wac_i2c || !input) {
		error = -ENOMEM;
		goto err_free_mem;
	}

	g_wac_i2c->client = client;
	g_wac_i2c->input = input;
#ifdef FORCE_DISPLAY_RES_MAPPING//[
	if(1==gptHWCFG->m_val.bDisplayResolution) {
		// 1024x758 .
		g_wac_i2c->dwXRes=1024;
		g_wac_i2c->dwYRes=758;
	}
	else if(2==gptHWCFG->m_val.bDisplayResolution) {
		// 1024x768
		g_wac_i2c->dwXRes=1024;
		g_wac_i2c->dwYRes=768;
	}
	else if(3==gptHWCFG->m_val.bDisplayResolution) {
		// 1440x1080
		g_wac_i2c->dwXRes=1440;
		g_wac_i2c->dwYRes=1080;
	}
	else if(4==gptHWCFG->m_val.bDisplayResolution) {
		// 1366x768
		g_wac_i2c->dwXRes=1366;
		g_wac_i2c->dwYRes=768;
	}
	else if(5==gptHWCFG->m_val.bDisplayResolution) {
		// 1448x1072
		g_wac_i2c->dwXRes=1448;
		g_wac_i2c->dwYRes=1072;
	}
	else if(6==gptHWCFG->m_val.bDisplayResolution) {
		// 1600x1200
		g_wac_i2c->dwXRes=1600;
		g_wac_i2c->dwYRes=1200;
	}
	else if(14==gptHWCFG->m_val.bDisplayResolution) {
		// 1920x1440
		if(0==gptHWCFG->m_val.bUIStyle || 1==gptHWCFG->m_val.bUIStyle) // ebrmain  or Customer UI
		{
			g_wac_i2c->dwXRes=1920;
			g_wac_i2c->dwYRes=1440;
		}
		else {
			g_wac_i2c->dwXRes=1920;
			g_wac_i2c->dwYRes=1440;
		}
	}
	else if(8==gptHWCFG->m_val.bDisplayResolution) {
		// 1872x1404
		if(28==gptHWCFG->m_val.bDisplayPanel) {
			// 10.3inch Top.
			if(2==gptHWCFG->m_val.bUIStyle) // android
			{
				g_wac_i2c->dwXRes=1872;//1404;
				g_wac_i2c->dwYRes=1404;//1872;
			}
			else if(0==gptHWCFG->m_val.bUIStyle || 1==gptHWCFG->m_val.bUIStyle) // ebrmain  or Customer UI
			{
				g_wac_i2c->dwXRes=1404;
				g_wac_i2c->dwYRes=1872;
			}
		}
		else {
			g_wac_i2c->dwXRes=1872;
			g_wac_i2c->dwYRes=1404;
		}
	}
	else {
		// 800x600 
		g_wac_i2c->dwXRes=800;
		g_wac_i2c->dwYRes=600;
	}
#endif //] FORCE_DISPLAY_RES_MAPPING
#ifdef HRTIMER_GET_PT//[

	g_wac_i2c->hrtimer_get_pt.function = hrtimer_get_pt;
	hrtimer_init(&g_wac_i2c->hrtimer_get_pt,CLOCK_MONOTONIC,HRTIMER_MODE_REL);
	g_wac_i2c->ktime_get_pt = ktime_set(HRTIMER_GET_PT_INTERVAL_US/1000000,(HRTIMER_GET_PT_INTERVAL_US%1000000)*1000);
#endif //]HRTIMER_GET_PT

	client->irq = gpio_to_irq(config_info.int_number);
        //printk(KERN_ERR"=====int:%d irq:%d ===\n",config_info.int_number,client->irq);

	input->name = "Wacom_I2C_Digitizer";
	input->id.bustype = BUS_I2C;
	input->id.vendor = 0x56a;
	input->id.version = g_features.fw_version;
	input->dev.parent = &client->dev;
	input->open = wacom_i2c_open;
	input->close = wacom_i2c_close;

	input->evbit[0] |= BIT_MASK(EV_KEY) | BIT_MASK(EV_ABS);

	__set_bit(BTN_TOOL_PEN, input->keybit);
	__set_bit(BTN_TOOL_RUBBER, input->keybit);
	__set_bit(BTN_STYLUS, input->keybit);
	__set_bit(BTN_STYLUS2, input->keybit);
	__set_bit(BTN_TOUCH, input->keybit);
	__set_bit(INPUT_PROP_DIRECT, input->propbit);

	//printk(KERN_ERR"====screen_max_x: %d , screen_max_y:%d=====\n",screen_max_x,screen_max_y);
	//printk(KERN_ERR"====g_wac_i2c->dwXRes: %d , g_wac_i2c->dwYRes:%d=====\n",g_wac_i2c->dwXRes,g_wac_i2c->dwYRes);

#ifdef FORCE_DISPLAY_RES_MAPPING//[
	input_set_abs_params(input, ABS_X, 0, g_wac_i2c->dwXRes, 0, 0);
	input_set_abs_params(input, ABS_Y, 0, g_wac_i2c->dwYRes, 0, 0);
#else //][FORCE_DISPLAY_RES_MAPPING
	if( exchange_x_y_flag ){
		input_set_abs_params(input, ABS_X, 0, screen_max_y, 0, 0);
		input_set_abs_params(input, ABS_Y, 0, screen_max_x, 0, 0);
	} else {
		input_set_abs_params(input, ABS_X, 0, screen_max_x, 0, 0);
		input_set_abs_params(input, ABS_Y, 0, screen_max_y, 0, 0);
	}
#endif //]FORCE_DISPLAY_RES_MAPPING
	input_set_abs_params(input, ABS_PRESSURE,
			     0, g_features.pressure_max, 0, 0);

	input_set_drvdata(input, g_wac_i2c);

	INIT_DELAYED_WORK(&wacom_delay_work, wacom_work_func);

	error = request_irq(client->irq, wacom_i2c_irq, IRQF_TRIGGER_FALLING, "wacom_i2c", g_wac_i2c);
	if (error) {
		dev_err(&client->dev,
			"Failed to enable IRQ, error: %d\n", error);
		cancel_delayed_work_sync (&wacom_delay_work);
		goto err_free_mem;
	}

	/* Disable the IRQ, we'll enable it in wac_i2c_open() */
	disable_irq(client->irq);

	error = input_register_device(g_wac_i2c->input);
	if (error) {
		dev_err(&client->dev,
			"Failed to register input device, error: %d\n", error);
		goto err_free_irq;
	}

	i2c_set_clientdata(client, g_wac_i2c);

	error = sysfs_create_files(&g_wac_i2c->input->dev.kobj, sysfs_attrs);
	if (error) {
		pr_debug("Can't create sysfs !\n");
		return -ENODEV;
	}


	printk(KERN_ERR"[%s_%d] probe success\n",__FUNCTION__,__LINE__);
	return 0;

err_free_irq:
	free_irq(client->irq, g_wac_i2c);
err_free_mem:
	input_free_device(input);
	kfree(g_wac_i2c);

	return error;
}

static int wacom_i2c_remove(struct i2c_client *client)
{
	free_irq(client->irq, g_wac_i2c);
	input_unregister_device(g_wac_i2c->input);
	kfree(g_wac_i2c);

	return 0;
}
#if 0
extern int gSleep_Mode_Suspend;
extern int enable_gpio_wakeup_src(int para);
extern int disable_gpio_wakeup_src(int para);
static int __maybe_unused wacom_i2c_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);

	printk ("[%s-%d] DG_INT %d\n", __func__, __LINE__, config_info.int_number);
	if (!gSleep_Mode_Suspend) {
		enable_gpio_wakeup_src(config_info.int_number);		// enable DG_INT wakeup
	}
	else {
		disable_irq(client->irq);
		disable_gpio_wakeup_src(config_info.int_number);		// disable DG_INT wakeup
	}

	return 0;
}

static int __maybe_unused wacom_i2c_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);

	if (gSleep_Mode_Suspend)
		enable_irq(client->irq);

	return 0;
}


#else

#endif

#ifdef CONFIG_PM_SLEEP
extern int enable_gpio_wakeup_src(int para);
extern int disable_gpio_wakeup_src(int para);
static int wacom_i2c_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);

	/* wacom wants to send data, trigger a read */
	if (gSleep_Mode_Suspend) {
	}
	else {
		// move to here,[when pen put on panel , press power key the system can't enter hibernation]
		if ( wacom_int_level() ) 
		{
			printk ("[%s-%d] event not processed.\n",__func__,__LINE__);
			schedule_delayed_work(&wacom_delay_work, 0);
			return -1;
		}
	}

	return 0;
}

static int wacom_i2c_suspend_late(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);

	/* wacom wants to send data, trigger a read */
	if (gSleep_Mode_Suspend) {
		//printk(KERN_ERR "==== %s() ====\n",__func__);
		free_irq(g_wac_i2c->client->irq, g_wac_i2c);
	}
	else {
		// move to here,[when pen put on panel , press power key the system can't enter hibernation]
		if ( wacom_int_level() ) 
		{
			printk ("[%s-%d] event not processed.\n",__func__,__LINE__);
			schedule_delayed_work(&wacom_delay_work, 0);
			return -1;
		}
		enable_gpio_wakeup_src(config_info.int_number);
	}

	return 0;
}

static int wacom_i2c_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	if (gSleep_Mode_Suspend) {
		//printk(KERN_ERR "==== %s() ====\n",__func__);
		/*g_wac_i2c->iIs_IRQ_disabled = 0;
		request_irq(g_wac_i2c->client->irq, wacom_i2c_irq, IRQF_TRIGGER_FALLING, "wacom_i2c", g_wac_i2c);*/
	}
	else {
		//disable_irq_wake(g_wac_i2c->client->irq);
		//disable_gpio_wakeup_src(config_info.int_number);
	}
	
	return 0;
}

static int wacom_i2c_resume_early(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	if (gSleep_Mode_Suspend) {
		//printk(KERN_ERR "==== %s() %d ====\n",__func__,__LINE__);
		g_wac_i2c->iIs_IRQ_disabled = 0;
		request_irq(g_wac_i2c->client->irq, wacom_i2c_irq, IRQF_TRIGGER_FALLING, "wacom_i2c", g_wac_i2c);
	}
	else {
		//disable_irq_wake(g_wac_i2c->client->irq);
		disable_gpio_wakeup_src(config_info.int_number);
	}
	
	if ( wacom_int_level() ) 
	{
		//schedule_delayed_work(&wacom_delay_work, 0);
		queue_work_on(0, system_highpri_wq, &wacom_delay_work.work);
	}

	return 0;
}
#endif

static int wacom_detect(struct i2c_client *client, struct i2c_board_info *info)
{
	struct i2c_adapter *adapter = client->adapter;
	int ret = -ENODEV;

	if (!i2c_check_functionality(adapter, I2C_FUNC_SMBUS_BYTE_DATA))
		return ret;

	if (twi_id == adapter->nr) {
		strlcpy(info->type, "wacom_i2c", I2C_NAME_SIZE);
		pr_info("%s: addr= %x\n", __func__, client->addr);
		ret = 0;
	}
	return ret;
}
static const unsigned short normal_i2c[2] = {0x09, I2C_CLIENT_END};

//static SIMPLE_DEV_PM_OPS(wacom_i2c_pm, wacom_i2c_suspend, wacom_i2c_resume);

static const struct dev_pm_ops wacom_i2c_pm = {
	.suspend = wacom_i2c_suspend,
	.suspend_late = wacom_i2c_suspend_late,
	.resume = wacom_i2c_resume,
	.resume_early = wacom_i2c_resume_early,
};

static const struct i2c_device_id wacom_i2c_id[] = {
	{ "wacom_i2c", 0 },
	{ },
};
MODULE_DEVICE_TABLE(i2c, wacom_i2c_id);

static struct i2c_driver wacom_i2c_driver = {
	.class = I2C_CLASS_HWMON,
	.driver	= {
		.owner = THIS_MODULE,
		.name	= "wacom_i2c",
		.pm	= &wacom_i2c_pm,
	},

	.probe		= wacom_i2c_probe,
	.remove		= wacom_i2c_remove,
	.id_table	= wacom_i2c_id,
	.detect   	= wacom_detect,
	.address_list	= normal_i2c,
};

static void emr_print_info(struct emr_config_info info)
{
	pr_debug("info.emr_used:%d\n", info.emr_used);
	pr_debug("info.emr_name:%s\n", info.name);
	pr_debug("info.twi_id:%d\n", info.twi_id);
	pr_debug("info.screen_max_x:%d\n", info.screen_max_x);
	pr_debug("info.screen_max_y:%d\n", info.screen_max_y);
	pr_debug("info.revert_x_flag:%d\n", info.revert_x_flag);
	pr_debug("info.revert_y_flag:%d\n", info.revert_y_flag);
	pr_debug("info.exchange_x_y_flag:%d\n", info.exchange_x_y_flag);
	pr_debug("info.irq_gpio_number:%d\n", info.irq_gpio.gpio);
	pr_debug("info.wakeup_gpio_number:%d\n", info.wakeup_gpio.gpio);
}

static int emr_wakeup(int status, int ms)
{
	pr_debug("***emr*** %s:status:%d,ms = %d\n", __func__, status, ms);

	if (status == 0) {

		if (ms == 0) {
			__gpio_set_value(config_info.wakeup_gpio.gpio, 0);
		} else {
			__gpio_set_value(config_info.wakeup_gpio.gpio, 0);
			msleep(ms);
			__gpio_set_value(config_info.wakeup_gpio.gpio, 1);
		}
	}
	if (status == 1) {
		if (ms == 0) {
			__gpio_set_value(config_info.wakeup_gpio.gpio, 1);
		} else {
			__gpio_set_value(config_info.wakeup_gpio.gpio, 1);
			msleep(ms);
			__gpio_set_value(config_info.wakeup_gpio.gpio, 0);
		}
	}
	usleep_range(5000, 6000);

	return 0;
}

static int emr_get_system_config(void)
{
	emr_print_info(config_info);

	twi_id = config_info.twi_id;
	if ((config_info.screen_max_x == 0) || (config_info.screen_max_y == 0)) {
		pr_warning("%s:screen_max_x or screen_max_y not defined!\n", __func__);
		return 0;
	}
	screen_max_x = config_info.screen_max_x - 1;
	screen_max_y = config_info.screen_max_y - 1;
	revert_x_flag = config_info.revert_x_flag;
	revert_y_flag = config_info.revert_y_flag;
	exchange_x_y_flag = config_info.exchange_x_y_flag;
	/*
	 * Backward compatible with E60P22 MFG
	 * Do NOT add any new models here
	 * if you need to change x y parameters, do it in dtb
	 */
	if( 91 == gptHWCFG->m_val.bPCB // E60P22
			&& 0 == gptHWCFG->m_val.bPCB_LVL && 0x30 > gptHWCFG->m_val.bPCB_REV // PCB <= A2X
			&& 0 == gptHWCFG->m_val.bUIStyle  // Ebrmain
			&& 3 == gptHWCFG->m_val.bDisplayResolution ) {  //1440x1080
		revert_x_flag = !revert_x_flag;
		revert_y_flag = !revert_y_flag;
		pr_debug("*** emr revert x and y for E60P22A20 MFG!\n");
	}
	/* Do not add models above*/

	/*
	 * Note: Only consider 16Bits_mirror flag now and it invert revert_y_flag
	 * Todo: 1. 8Bits panel with mirror
	 *       2. Future panels with revert_x_flag needed
	 */
	if( 3 == gptHWCFG->m_val.bDisplayBusWidth ) {  //16Bits_mirror
		revert_y_flag = !revert_y_flag;
		pr_debug("*** emr revert y for 16bit mirror panel!\n");
	}

	return 1;
}
static int __init wacom_init(void)
{
	int ret = -1;
	if (input_sensor_startup(&(config_info.input_type))) {
		pr_debug("%s: emr_startup err.\n", __func__);
		return 0;
	} else {
		ret = input_sensor_init(&(config_info.input_type));
		if (ret != 0)
			pr_debug("%s:emr_ops.init err.\n", __func__);
	}
	if (config_info.emr_used == 0) {
		pr_debug("*** emr_used set to 0 !\n");
		pr_debug("if use ctp,please put the sys_config.fex emr_used set to 1.\n");
		return 0;
	}
	if (!emr_get_system_config()) {
		pr_debug("%s:read config fail!\n", __func__);
		return ret;
	}
	if(input_set_power_enable(&(config_info.input_type), 1)>=0) {
		msleep(20);
	}
	emr_wakeup(1, 0);

	ret = i2c_add_driver(&wacom_i2c_driver);
	
	/* Create the /proc file */
	Our_Proc_File = proc_create(PROCFS_NAME, 0644, NULL, &proc_fops);
	
	if (Our_Proc_File == NULL) {
		remove_proc_entry(PROCFS_NAME, NULL);
		printk(KERN_ALERT "Error: Could not initialize /proc/%s\n", PROCFS_NAME);
		return -ENOMEM;
	}
	return ret;
}

static void __exit wacom_exit(void)
{
	pr_debug("==gsl_ts_exit==\n");
	remove_proc_entry(PROCFS_NAME, NULL);
	i2c_del_driver(&wacom_i2c_driver);
	input_sensor_free(&(config_info.input_type));
}

module_init(wacom_init);
module_exit(wacom_exit);

MODULE_AUTHOR("Tatsunosuke Tobita <tobita.tatsunosuke@wacom.co.jp>");
MODULE_DESCRIPTION("WACOM EMR I2C Driver");
MODULE_LICENSE("GPL");
