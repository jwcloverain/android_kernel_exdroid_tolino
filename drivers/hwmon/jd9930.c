/*
 * Papyrus epaper power control HAL
 *
 *      Copyright (C) 2009 Dimitar Dimitrov, MM Solutions
 *
 *  This file is subject to the terms and conditions of the GNU General Public
 *  License. See the file COPYING in the main directory of this archive for
 *  more details.
 *
 * TPS6518x power control is facilitated using I2C control and WAKEUP GPIO
 * pin control. The other VCC GPIO Papyrus' signals must be tied to ground.
 *
 * TODO:
 * 	- Instead of polling, use interrupts to signal power up/down
 * 	  acknowledge.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/major.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/interrupt.h>
#include <linux/workqueue.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/version.h>
//#include <mach/board.h>
#include <linux/suspend.h>
//#include <linux/earlysuspend.h>
#include <linux/gpio.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
//#include <linux/sys_config.h>
#include <linux/platform_device.h>
//#include <linux/init-input.h>
//#include <linux/input.h>
#include <linux/pinctrl/consumer.h>
#include <linux/pmic.h>

// #define pr_debug pr_info
#define PAPYRUS2_1P1_I2C_ADDRESS		0x18
extern void papyrus_set_i2c_address(int address);

#include <linux/gpio.h>
#include <linux/sunxi-gpio.h>

#include "../../arch/arm/mach-sunxi/ntx_hwconfig.h"
extern volatile NTX_HWCONFIG *gptHWCFG;

#include <linux/ebc.h>

struct pmic_mgr *g_jd9930_mgr = NULL;

#define JD9930_I2C_NAME "jd9930"

#define XON_TIMING_ENABLE		1
#define LIGHTNESS_MODE_PARAMS_FROM_DTS		1

#define PAPYRUS_VCOM_MAX_MV		0
#define PAPYRUS_VCOM_MIN_MV		-5110

#define PAPYRUS_POWER_UP_DELAY_MS   2

/* After waking up from sleep, Papyrus
   waits for VN to be discharged and all
   voltage ref to startup before loading
   the default EEPROM settings. So accessing
   registers too early after WAKEUP could
   cause the register to be overridden by
   default values */
#define PAPYRUS_EEPROM_DELAY_MS 50
/* Papyrus WAKEUP pin must stay low for
   a minimum time */
#define PAPYRUS_SLEEP_MINIMUM_MS 110
/* Temp sensor might take a little time to
   settle eventhough the status bit in TMST1
   state conversion is done - if read too early
   0C will be returned instead of the right temp */
#define PAPYRUS_TEMP_READ_TIME_MS 10

/* Powerup sequence takes at least 24 ms - no need to poll too frequently */
#define HW_GET_STATE_INTERVAL_MS 24

#define INVALID_GPIO -1

#define JD9930_PWRON_READYUS	2000
#define JD9930_WAKEUP0_MS	4
#define JD9930_SLEEP0_MS	4
#define JD9930_VCOM_OFF_US	100
#define JD9930_VCOM_STEP_uV     19607

#define JD9930_INIT_COUNT	20

/*
 * Bitfield macros that use rely on bitfield width/shift information.
 */
#define BITFMASK(field) (((1U << (field ## _WID)) - 1) << (field ## _LSH))
#define BITFVAL(field, val) ((val) << (field ## _LSH))
#define BITFEXT(var, bit) ((var & BITFMASK(bit)) >> (bit ## _LSH))

/* OP MODE (0h )*/
#define RAILS_ON_LSH 7
#define RAILS_ON_WID 1
#define VCOM_MANUAL_LSH 6
#define VCOM_MANUAL_WID 1
#define LIGHTNESS_LSH 5
#define LIGHTNESS_WID 1
#define VDDH_DISABLE_LSH 4
#define VDDH_DISABLE_WID 1
#define VEE_DISABLE_LSH 3
#define VEE_DISABLE_WID 1
#define VPOS_DISABLE_LSH 2
#define VPOS_DISABLE_WID 1
#define VNEG_DISABLE_LSH 1
#define VNEG_DISABLE_WID 1
#define VCOM_DISABLE_LSH 0
#define VCOM_DISABLE_WID 1
// all rails bits .
#define RAILS_DISABLE_LSH 0
#define RAILS_DISABLE_WID 5

//VCOM_ADJ2 (02h) : VCOM2 and VDDH_EXT 
#define VCOM2_B8_LSH	7
#define VCOM2_B8_WID	1
#define VDDH_EXT_LSH	0
#define VDDH_EXT_WID	5



/* VLDO voltage adjustment control (03h)*/
#define VLDO_ADJ_LSH	5 // VPOS & VNEG voltage adjustment . 
#define VLDO_ADJ_WID	3
#define VPDD_ADJ_LSH	0 // VPDD voltage setting at lightness mode . 
#define VPDD_ADJ_WID	5 


/* VPDD LEN Control(04h) */
#define VPDD_LEN_LSH	0
#define VPDD_LEN_WID	5

/* VEE_EXT & VP_EXT Control (05h) */
#define VP_EXT_LSH		5
#define VP_EXT_WID		2
#define VEE_EXT_LSH		0
#define VEE_EXT_WID		5

/* Power On Delay Time (06h)*/
#define TDLY4_LSH 6
#define TDLY4_WID 2
#define TDLY3_LSH 4
#define TDLY3_WID 2
#define TDLY2_LSH 2
#define TDLY2_WID 2
#define TDLY1_LSH 0
#define TDLY1_WID 2

/* FAULT_FLAGS (07h) */
#define FAULTS_LSH 1
#define FAULTS_WID 4
#define PG_LSH 0
#define PG_WID 1

// night mode/day mode selector .
#define NM_LSH   0
#define NM_WID   1 

#define PRINT_REG_WR_EN		1
#ifdef PRINT_REG_WR_EN //[
	#define PRINT_REG_WR(reg_name,reg_val) \
		printk("%s() %s=0x%x\n",__func__,reg_name,reg_val)
#else //][!PRINT_REG_WR_EN
	#define PRINT_REG_WR(reg_name,reg_val) 
#endif //] PRINT_REG_WR_EN
/*
 * PMIC Register Addresses
 */
enum {
    REG_JD9930_TMST_VALUE = 0x0,
    REG_JD9930_VCOM_SET,
    REG_JD9930_VPOS_VNEG_SET,
    REG_JD9930_PWRON_DELAY,
    REG_JD9930_VGH_EXT,
    REG_JD9930_VGL_EXT,
    REG_JD9930_VGHNM_EXT,
    REG_JD9930_VGHNM,
    REG_JD9930_DISA_DELAY,
    REG_JD9930_XON_DELAY,
    REG_JD9930_XON_LEN,
    REG_JD9930_CTRL_REG1,
    REG_JD9930_CTRL_REG2,
    JD9930_REG_NUM,
};


struct jd9930 {
    unsigned char need_reinit;// need reinit registers .
    unsigned char need_re_en;// need re enable jd9930 .
    
    unsigned char regTMST_VALUE;
    unsigned char regVCOM_SET;
    unsigned char regVPOS_VNEG_SET;
    unsigned char regPWRON_DELAY;
    unsigned char regVGH_EXT;
    unsigned char regVGL_EXT;
    unsigned char regVGHNM_EXT;
    unsigned char regVGHNM;
    unsigned char regDISA_DELAY;
    unsigned char regXON_DELAY;
    unsigned char regXON_LEN;
    unsigned char regCTRL_REG1;
    unsigned char regCTRL_REG2;
};

/* Addresses to scan */
static const unsigned short normal_i2c[2] = {PAPYRUS2_1P1_I2C_ADDRESS,I2C_CLIENT_END};

static int gHasPWRALL = -1 ;

/*struct ctp_config_info config_info = {
	.input_type = CTP_TYPE,
	.name = NULL,
	.int_number = 0,
};*/

struct papyrus_sess {
	PMIC_ID id;
	struct i2c_adapter *adap;
	struct i2c_client *client;
	uint8_t enable_reg_shadow;
	uint8_t enable_reg;
	uint8_t vadj;
	uint8_t vcom1;
	uint8_t vcom2;
	uint8_t vcom2off;
	uint8_t int_en1;
	uint8_t int_en2;
	uint8_t tmst1;
	uint8_t tmst2;

	/* Custom power up/down sequence settings */
	struct {
		/* If options are not valid we will rely on HW defaults. */
		bool valid;
		unsigned int dly[8];
	} seq;
	struct timeval standby_tv;
	int power_up_pin;
	int xon_pin;
	int power_all_pin;
	int power_good_pin;
	int en_ts_pin;
	/* True if a high WAKEUP brings Papyrus out of reset. */
	int power_active_high;
	int xon_active_high;
	int en_ts_active_high;
	struct regulator *pmic_power_ldo;
	u32 xon_off_delay_us,xon_off_day_delay_us, xon_on_delay_us , xon_on_1st_delay_us ;
	struct hrtimer hrt_xon_on,hrt_xon_off;
	
};

#define sy7636_SPEED	(400*1000)

// VCOM2_ADJUST

#define PAPYRUS_MV_TO_VCOMREG(MV)	((MV) / 10)

#define V3P3_EN_MASK	0x20
#define PAPYRUS_V3P3OFF_DELAY_MS 10//100

struct papyrus_hw_state {
	uint8_t tmst_value;
	uint8_t int_status1;
	uint8_t int_status2;
	uint8_t pg_status;
};

//PTIME_DECLARE(sy7636_xon_1st_delay);

static uint8_t papyrus2_i2c_addr = PAPYRUS2_1P1_I2C_ADDRESS;

static int papyrus_resume_flag = 0;

struct jd9930 *g_regJD9930;

extern int gSleep_Mode_Suspend;

extern int jd9930_power_on(void);
extern int jd9930_power_on_ex(unsigned long dwFlags);
extern int jd9930_power_down(void);
extern int jd9930_power_down_ex(unsigned long dwFlags);
extern int jd9930_vcom_get(int *vcom);
extern int jd9930_vcom_disable(void);
extern int jd9930_vcom_disable_ex(unsigned long dwFlags);

static int cfg_VCOMmV = 0;

static int jd9930_EN(struct papyrus_sess *sess, int iEN);
static void papyrus_hw_power_req(struct pmic_sess *pmsess, int up);

static enum hrtimer_restart _hrtint_xon_on(struct hrtimer *timer)
{
	struct papyrus_sess *pmess = 
		container_of(timer, struct papyrus_sess, hrt_xon_on);
#ifdef XON_TIMING_ENABLE //[
	gpio_direction_output(pmess->xon_pin,pmess->xon_active_high);
	printk(KERN_DEBUG"%s() : XON=%d\n",__func__,pmess->xon_active_high);
#else //][!XON_TIMING_ENABLE
	pr_info("%s() : XON(%d) control disabled \n",__func__,pmess->xon_active_high);
#endif //] XON_TIMING_ENABLE

	return HRTIMER_NORESTART;
}
static enum hrtimer_restart _hrtint_xon_off(struct hrtimer *timer)
{
	struct papyrus_sess *pmess = 
		container_of(timer, struct papyrus_sess, hrt_xon_off);

#ifdef XON_TIMING_ENABLE //[
	gpio_direction_output(pmess->xon_pin,!pmess->xon_active_high);
	printk(KERN_DEBUG"%s() : XON=%d\n",__func__,!pmess->xon_active_high);
#else //][!XON_TIMING_ENABLE
	pr_info("%s() : XON(%d) control disabled \n",__func__,!pmess->xon_active_high);
#endif //] XON_TIMING_ENABLE

	return HRTIMER_NORESTART;
}


static int jd9930_papyrus_hw_setreg(struct papyrus_sess *sess, uint8_t regaddr, uint8_t val)
{
	int stat;
	uint8_t txbuf[2] = { regaddr, val };
	struct i2c_msg msgs[] = {
		{
			.addr = sess->client->addr,//papyrus2_i2c_addr,
			.flags = 0,
			.len = 2,
			.buf = txbuf,
			//.scl_rate = sy7636_SPEED,
		}
	};

	if(gpio_is_valid(sess->power_all_pin)) {
		if(!gpio_get_value(sess->power_all_pin)) {
			// en not enabled . 
			pr_debug("%s(): enabling power_all before registers access .\n",__func__);
			//dump_stack();
			gpio_direction_output(sess->power_all_pin, 1);
		}
	}

	//printk ("[%s-%d] reg %d = 0x%X\n", __func__, __LINE__, regaddr, val);
	stat = i2c_transfer(sess->adap, msgs, ARRAY_SIZE(msgs));

	if (stat < 0)
		pr_err("papyrus: I2C send error: %d\n", stat);
	else if (stat != ARRAY_SIZE(msgs)) {
		pr_err("papyrus: I2C send N mismatch: %d\n", stat);
		stat = -EIO;
	} else
		stat = 0;

	return stat;
}


static int jd9930_papyrus_hw_getreg(struct papyrus_sess *sess, uint8_t regaddr, uint8_t *val)
{
	int stat;
	struct i2c_msg msgs[] = {
		{
			.addr = sess->client->addr,////papyrus2_i2c_addr,
			.flags = 0,
			.len = 1,
			.buf = &regaddr,
			//.scl_rate = sy7636_SPEED,
		},
		{
			.addr = sess->client->addr,//papyrus2_i2c_addr,
			.flags = I2C_M_RD,
			.len = 1,
			.buf = val,
			//.scl_rate = sy7636_SPEED,
		}
	};

	if(gpio_is_valid(sess->power_all_pin)) {
		if(!gpio_get_value(sess->power_all_pin)) {
			// en not enabled . 
			pr_debug("%s(): enabling power_all before registers access .\n",__func__);
			//dump_stack();
			gpio_direction_output(sess->power_all_pin, 1);
		}
	}

	stat = i2c_transfer(sess->adap, msgs, ARRAY_SIZE(msgs));
	if (stat < 0) {
		pr_err("papyrus: I2C read error=%d,i2c_addr=0x%x\n", stat, sess->client->addr);
	} else if (stat != ARRAY_SIZE(msgs)) {
		pr_err("papyrus: I2C read N mismatch, ret=%d\n", stat);
		stat = -EIO;
	} else {
		stat = 0;
	}

	//printk ("GET [%s-%d] reg %d = 0x%X\n", __func__, __LINE__, regaddr, *val);
	return stat;
}

#define JD9930_REG_GET(ptJD9930,_regName)		\
	(ptJD9930)->reg##_regName

#define JD9930_REG_SET(ptJD9930,_regName,val)		\
	(ptJD9930)->reg##_regName=val

/*
 * jd9930_to_reg_val(): Creates a register value with new data
 *
 * Creates a new register value for a particular field.  The data
 * outside of the new field is not modified.
 *
 * @cur_reg: current value in register
 * @reg_mask: mask of field bits to be modified
 * @fld_val: new value for register field.
 */
static unsigned int jd9930_to_reg_val(unsigned int cur_reg, unsigned int fld_mask,
							   unsigned int fld_val)
{
	return (cur_reg & (~fld_mask)) | fld_val;
}

#define SY7636_REG_FLD_SET(ptSY7636,_regName,_bFieldName,_bSetVal)		\
({\
	unsigned int fld_mask,fld_val;\
	unsigned int cur_reg,new_reg_val;\
	\
	fld_mask=(((1U << (_bFieldName##_WID)) - 1) << (_bFieldName##_LSH));\
	fld_val=((_bSetVal) << (_bFieldName##_LSH));\
	cur_reg=ptSY7636->reg##_regName;\
	new_reg_val = jd9930_to_reg_val(cur_reg,fld_mask,fld_val);\
	ptSY7636->reg##_regName=(unsigned char)new_reg_val;\
	new_reg_val;\
})

#define papyrus_hw_setreg(sess,_regName,val)		\
({\
	int _iRet;\
	{\
		JD9930_REG_SET(g_regJD9930,_regName,val);\
		PRINT_REG_WR(#_regName,g_regJD9930->reg##_regName);\
		_iRet=jd9930_papyrus_hw_setreg(sess,REG_JD9930_##_regName,val);\
	}\
	_iRet;\
})
#define papyrus_hw_getreg(sess,_regName,val)		\
({\
	int _iRet;\
	{\
		_iRet=jd9930_papyrus_hw_getreg(sess,REG_JD9930_##_regName,val);\
	}\
	_iRet;\
})

#define JD9930_REG_WRITE(sess,_regName)		\
({\
	int _iRet;\
	PRINT_REG_WR(#_regName,g_regJD9930->reg##_regName);\
	_iRet = jd9930_papyrus_hw_setreg(sess,REG_JD9930_##_regName,g_regJD9930->reg##_regName);\
 	_iRet;\
})

static int jd9930_is_power_good(struct papyrus_sess *sess)
{
	/*
	 * XOR of polarity (starting value) and current
	 * value yields whether power is good.
	 */
	return gpio_get_value(sess->power_good_pin);
}

int jd9930_get_power_status(struct papyrus_sess *sess,int iIsGetCached,int *O_piPG,unsigned char *O_pbFaults)
{
	int iPG;
	
	if(O_piPG) {
		*O_piPG = jd9930_is_power_good(sess);
	}
	return 0;
}

const static char * gszFaultInfoStr[] = {
	"no fault event",// 0
	"UVP at VB rail", // 1
	"UVP at VN rail",// 2
	"UVP at VPOS rail", // 3
	"UVP at VNEG rail", // 4
	"UVP at VDDH rail", // 5
	"UVP at VEE rail", // 6
	"SCP at VB rail", // 7
	"SCP at VN rail",// 8
	"SCP at VPOS rail", // 9	
	"SCP at VNEG rail", // A
	"SCP at VDDH rail", // B
	"SCP at VEE rail", // C
	"SCP at VCOM rail", // D
	"UVLO", // E
	"Thermal shutdown", // F
};

static int jd9930_wait_power_good(struct papyrus_sess *sess,int iWaitON)
{
	int i;
	int iPG;
	int iRet = -ETIMEDOUT;

	for (i = 0; i < 20; i++)
	{
		msleep(10);
		iPG = jd9930_is_power_good(sess);

		if(iWaitON) {
			if (iPG) {
				iRet = 0;
				//printk("%s():cnt=%d,PG=%d\n",__FUNCTION__,i,iPG);
				break;
			}
		}
		else {
			if (!iPG) {
				//printk("%s():cnt=%d,PG=%d\n",__FUNCTION__,i,iPG);
				iRet = 0;
				break;
			}
		}

	}

	if(iRet<0) {
		printk(KERN_ERR"%s():wait %d times for PG(%d) timeout,cur PG=%d\n",__FUNCTION__,i,iWaitON,iPG);
	}

	return iRet;
}

static int jd9930_regs_init(struct papyrus_sess *sess)
{
	int iRet = -1, try_count = 0;
	int chip_VCOMmV;
	int stat = 0;
	
	pr_debug("%s(%d) \n",__func__,__LINE__);
	do {
		// write registers . 	

		stat |= JD9930_REG_WRITE(sess,VCOM_SET);
		stat |= JD9930_REG_WRITE(sess,VPOS_VNEG_SET);
		stat |= JD9930_REG_WRITE(sess,PWRON_DELAY);
		stat |= JD9930_REG_WRITE(sess,VGH_EXT);
		stat |= JD9930_REG_WRITE(sess,VGL_EXT);
		stat |= JD9930_REG_WRITE(sess,VGHNM_EXT);
		stat |= JD9930_REG_WRITE(sess,VGHNM);
		stat |= JD9930_REG_WRITE(sess,DISA_DELAY);
		stat |= JD9930_REG_WRITE(sess,XON_DELAY);
		stat |= JD9930_REG_WRITE(sess,XON_LEN);
		stat |= JD9930_REG_WRITE(sess,CTRL_REG1);
		stat |= JD9930_REG_WRITE(sess,CTRL_REG2);

		
		try_count++;
		
		if (stat) {
			pr_info("%s() some register init failed ... retry init\n", __func__);
			stat = 0;
			continue;
		}
		else {
			stat = jd9930_vcom_get(&chip_VCOMmV);
			pr_info("get vcom sucess, chip VCOM %d mV, config VCOM %d mV ... init finish\n", chip_VCOMmV, cfg_VCOMmV);
			return 0;
		}
		
		msleep(10);
		
	} while(try_count < JD9930_INIT_COUNT);

	return iRet;
}

static int jd9930_switch_nightmode(struct papyrus_sess *sess, int iIsNightMode)
{
	int iRet = 0;
	int iChk;
	unsigned int cur_reg,new_reg_val;
	unsigned int fld_mask = 0;	  /* register mask for bitfield to modify */
	unsigned int fld_val = 0;	  /* new bitfield value to write */
	unsigned char bIsNM_Old,bIsNM_New;
	
	iChk = JD9930_REG_GET(g_regJD9930,CTRL_REG1);
	if(iChk>=0) {
		cur_reg = iChk;
	}
	else {
		// read register error .
	}
	bIsNM_Old = BITFEXT(cur_reg,NM);
	bIsNM_New = iIsNightMode?1:0;

	if(bIsNM_Old!=bIsNM_New) {
		fld_mask |= BITFMASK(NM);
		fld_val |= BITFVAL(NM,bIsNM_New);
		new_reg_val = jd9930_to_reg_val(cur_reg, fld_mask, fld_val);
		//DBG_MSG("%s(),set CTRL_REG1 =0x%x\n",__FUNCTION__,new_reg_val);
		//JD9930_REG_WRITE_EX(jd9930,PWRON_DLY,new_reg_val);
		JD9930_REG_SET(g_regJD9930,CTRL_REG1,(unsigned char)new_reg_val);
		JD9930_REG_WRITE(sess,CTRL_REG1);
	}
	
#ifdef GDEBUG //[
	printk(KERN_DEBUG"%s(%d) NM=%d\n",__func__,__LINE__,iIsNightMode);
#endif //] GDEBUG

	return iRet;
}

static void jd9930_setup_timings(struct papyrus_sess *sess, struct pmic_config *config)
{
	unsigned char bOnDly1, bOnDly2, bOnDly3, bOnDly4;
	unsigned int cur_reg,new_reg_val;
	unsigned int fld_mask = 0;	  /* register mask for bitfield to modify */
	unsigned int fld_val = 0;	  /* new bitfield value to write */
	int iChk;

	if( 0xff==config->on_delay1 &&
			0xff==config->on_delay2 &&
			0xff==config->on_delay3 &&
			0xff==config->on_delay4)
	{
		// nothing to do .
		pr_info("%s(),skipped !\n",__FUNCTION__);
		return ;
	}
	pr_debug("%s(%d) \n",__func__,__LINE__);

	iChk = JD9930_REG_GET(g_regJD9930,PWRON_DELAY);
	if(iChk>=0) {
		cur_reg = iChk;
	}

	bOnDly1 = BITFEXT(cur_reg,TDLY1);
	bOnDly2 = BITFEXT(cur_reg,TDLY2);
	bOnDly3 = BITFEXT(cur_reg,TDLY3);
	bOnDly4 = BITFEXT(cur_reg,TDLY4);
	
	if(0xff!=config->on_delay1 && bOnDly1!=config->on_delay1) {
		fld_mask |= BITFMASK(TDLY1);
		fld_val |= BITFVAL(TDLY1, config->on_delay1);
	}
	if(0xff!=config->on_delay2 && bOnDly1!=config->on_delay2) {
		fld_mask |= BITFMASK(TDLY2);
		fld_val |= BITFVAL(TDLY2, config->on_delay2);
	}
	if(0xff!=config->on_delay3 && bOnDly1!=config->on_delay3) {
		fld_mask |= BITFMASK(TDLY3);
		fld_val |= BITFVAL(TDLY3, config->on_delay3);
	}
	if(0xff!=config->on_delay4 && bOnDly1!=config->on_delay4) {
		fld_mask |= BITFMASK(TDLY4);
		fld_val |= BITFVAL(TDLY4, config->on_delay4);
	}
	new_reg_val = jd9930_to_reg_val(cur_reg, fld_mask, fld_val);
	
	if(cur_reg!=new_reg_val) {
		pr_info("%s(),set poweron delay=0x%x\n",__func__,new_reg_val);
		JD9930_REG_SET(g_regJD9930,PWRON_DELAY,(unsigned char)new_reg_val);
	}
	
	if (0xff == config->vpos_vneg)
		JD9930_REG_SET(g_regJD9930,VPOS_VNEG_SET,0);
	else
		JD9930_REG_SET(g_regJD9930,VPOS_VNEG_SET,(unsigned char)config->vpos_vneg);
	
	if (0xff == config->vgh_ext)
		JD9930_REG_SET(g_regJD9930,VGH_EXT,0);
	else
		JD9930_REG_SET(g_regJD9930,VGH_EXT,(unsigned char)config->vgh_ext);
	
	if (0xff == config->vgl_ext)
		JD9930_REG_SET(g_regJD9930,VGL_EXT,0);
	else
		JD9930_REG_SET(g_regJD9930,VGL_EXT,(unsigned char)config->vgl_ext);
	
	if (0xff == config->vghnm_ext)
		JD9930_REG_SET(g_regJD9930,VGHNM_EXT,0);
	else
		JD9930_REG_SET(g_regJD9930,VGHNM_EXT,(unsigned char)config->vghnm_ext);
	
	if (0xff == config->vghnm)
		JD9930_REG_SET(g_regJD9930,VGHNM,0);
	else
		JD9930_REG_SET(g_regJD9930,VGHNM,(unsigned char)config->vghnm);
	
	if (0xff == config->disa_delay)
		JD9930_REG_SET(g_regJD9930,DISA_DELAY,0);
	else
		JD9930_REG_SET(g_regJD9930,DISA_DELAY,(unsigned char)config->disa_delay);
		
	if (0xff == config->xon_delay)
		JD9930_REG_SET(g_regJD9930,XON_DELAY,0);
	else
		JD9930_REG_SET(g_regJD9930,XON_DELAY,(unsigned char)config->xon_delay);
		
	if (0xff == config->xon_len)
		JD9930_REG_SET(g_regJD9930,XON_LEN,0);
	else
		JD9930_REG_SET(g_regJD9930,XON_LEN,(unsigned char)config->xon_len);
		
	return;
}

static void papyrus_hw_send_powerdown(struct papyrus_sess *sess)
{
	int stat;

//	printk ("[%s-%d] \n", __func__, __LINE__);
	/* switch to standby mode, keep 3.3V & VEE & VDDH & VPOS & VNEG alive, 
	 * don't enable vcom buffer
	 */
	return;
}
static int papyrus_hw_read_temperature(struct pmic_sess *pmsess, int *t)
{
	struct papyrus_sess *sess = (struct papyrus_sess *)pmsess->drvpar;
	static uint8_t last_temp=25;
	int stat;
	int ntries = 50;
	int nwait = 10;
	uint8_t tb;

	if (MASTER_PMIC_ID != pmsess->id) {
		*t = -1;
		pr_err("only master pmic have NTC\n");
		return -1;
	}
	
	// get temperture from JD9930 if power was on. return last temperautre if power was off.
	stat = papyrus_hw_getreg(sess, TMST_VALUE, &tb);
	if (0 <= stat) {
		stat = papyrus_hw_getreg(sess, TMST_VALUE, &tb);
		pr_debug ("[%s-%d] return %d(temp=%d)\n", __func__, __LINE__,stat, tb);
		if (0 <= stat) {
			*t = (int)(int8_t)tb;
			last_temp = tb;
		}
		else
			*t = (int)(int8_t)last_temp;
	}
	else {
		pr_debug ("[%s-%d] return last temperature tb=0x%x,%d(temp=%d)\n", __func__, __LINE__,tb, stat, last_temp);
		*t = (int)(int8_t)last_temp;
	}
	
	return stat;
}
static int papyrus_hw_get_revid(struct papyrus_sess *sess)
{
	return 0;
}

static int papyrus_hw_arg_init(struct papyrus_sess *sess)
{
	sess->vadj = 0x66;
	
	sess->vcom1 = (PAPYRUS_MV_TO_VCOMREG(2500) & 0x00FF);
	sess->vcom2 = ((PAPYRUS_MV_TO_VCOMREG(2500) & 0x0100) >> 1);

	return 0;
}

static int papyrus_hw_init(struct papyrus_sess *sess, PMIC_ID pmic_id)
{
	int stat = 0;

	if (MASTER_PMIC_ID != pmic_id) {
		pr_info("not master pmic, do not control gpio\n");
		return 0;
	}

	//pr_info("%s(%d)\n",__func__,__LINE__);
	if(gpio_is_valid(sess->power_up_pin))
		stat |= gpio_request(sess->power_up_pin, "papyrus-power-up");
	else
		stat |= 0x1;
	//pr_info("%s(%d)\n",__func__,__LINE__);
	if(1==gHasPWRALL){
		if(gpio_is_valid(sess->power_all_pin))
			stat |= gpio_request(sess->power_all_pin, "papyrus-power-all");
		else
			stat |= 0x2;
	}

	//pr_info("%s(%d)\n",__func__,__LINE__);
	if(gpio_is_valid(sess->power_good_pin)) {
		if (0 > gpio_request(sess->power_good_pin, "papyrus-power-good"))
			pr_err("PMIC%d: failed request power good gpio %d\n", pmic_id, sess->power_good_pin);
	}
	else
		stat |= 0x4;
    
	//pr_info("%s(%d)\n",__func__,__LINE__);
	if(gpio_is_valid(sess->en_ts_pin))
		stat |= gpio_request(sess->en_ts_pin, "papyrus-en-ts");
	else
		stat |= 0x8;

	//pr_info("%s(%d),xon_pin=%d,valid=%d\n",__func__,__LINE__,sess->xon_pin,gpio_is_valid(sess->xon_pin));
	if(gpio_is_valid(sess->xon_pin)) {
		if(gpio_request(sess->xon_pin, "papyrus-xon")<0) {
			pr_err("PMIC%d: failed request xon gpio %d\n", pmic_id, sess->xon_pin);
		}
	}
	else 
		pr_err("PMIC%d: there is no xon pin declared \n", pmic_id);

	if (stat) {
		pr_err("PMIC%d: fail to request gpio, stat=0x%x\n", pmic_id, stat);
		return -ENODEV;
	}
	
	if(gpio_is_valid(sess->power_all_pin))
		gpio_direction_output(sess->power_all_pin, 1);
	msleep(PAPYRUS_SLEEP_MINIMUM_MS);
	//gpio_direction_output(sess->power_up_pin, sess->power_active_high);
	//gpio_direction_output(sess->en_ts_pin, sess->en_ts_active_high);
	
	gpio_direction_input(sess->power_good_pin);
	msleep(PAPYRUS_EEPROM_DELAY_MS);

	//pr_info("%s(%d)\n",__func__,__LINE__);
    
	if (gpio_is_valid(sess->en_ts_pin)) {
		gpio_direction_output(sess->en_ts_pin, sess->en_ts_active_high);
	}

	if (gpio_is_valid(sess->xon_pin)) {
		//pr_info("%s(%d)\n",__func__,__LINE__);
		gpio_direction_output(sess->xon_pin, sess->xon_active_high);
		printk(KERN_DEBUG"%s() : XON=%d\n",__func__,sess->xon_active_high);
	}
	
	return 0;
	
free_gpios:
	gpio_free(sess->power_up_pin);
	if (gpio_is_valid(sess->xon_pin)) {
		gpio_free(sess->xon_pin);
	}
	if(gpio_is_valid(sess->power_all_pin))
		gpio_free(sess->power_all_pin);
	gpio_free(sess->power_good_pin);
	if (gpio_is_valid(sess->en_ts_pin))
		gpio_free(sess->en_ts_pin);
	pr_err("papyrus: ERROR: could not initialize I2C papyrus!\n");
	return stat;
}

int jd9930_EN(struct papyrus_sess *sess, int iEN)
{
	int iEN_Old_State, iRet;
	
	if (!gpio_is_valid(sess->power_up_pin)) {
		pr_info("%s():epdc pmic powerup pin available\n",__FUNCTION__);
		return -1;
	}
	pr_debug("%s(%d):\n",__FUNCTION__,iEN);
    
	iEN_Old_State = gpio_get_value(sess->power_up_pin);
	
	if(iEN) {
		if (iEN_Old_State != iEN) {
			gpio_direction_output(sess->power_up_pin, sess->power_active_high);
			
			if(0==iEN_Old_State) {
				pr_debug("%s():EN 0->1 .\n",__FUNCTION__);
				mdelay(JD9930_WAKEUP0_MS);
			}
		}
	}
	else {
		if (iEN_Old_State == iEN) {
			return iEN_Old_State;
		}
		gpio_direction_output(sess->power_up_pin, !sess->power_active_high);
		if(1==iEN_Old_State) {
			pr_debug("%s():EM 1->0 .\n",__FUNCTION__);
			udelay(JD9930_PWRON_READYUS/2);
		}
	}
	return iEN;
}

static void papyrus_hw_power_req(struct pmic_sess *pmsess, int up)
{
	struct papyrus_sess *sess = pmsess->drvpar;
	int stat = 0;
	unsigned int cur_reg_val,new_reg_val; /* current register value to modify */
	unsigned int fld_mask;	  /* register mask for bitfield to modify */
	unsigned int fld_val;	  /* new bitfield value to write */
	unsigned char bOP;
	int iRet, iTryCnt=0;
	int iPG;
	
	printk(KERN_DEBUG"papyrus: i2c pwr req: %d\n", up);
	
	if (up>0) {

		if(gpio_is_valid(sess->xon_pin)) {
			hrtimer_cancel(&sess->hrt_xon_off);
			hrtimer_cancel(&sess->hrt_xon_on);
		}
		
#ifdef XON_TIMING_ENABLE //[
		if(gpio_is_valid(sess->xon_pin)) {
			if( -1 != sess->xon_on_1st_delay_us ) {
				printk(KERN_DEBUG"%s(): 1st delay XON %d us,xon=%d\n",
					__func__,sess->xon_on_1st_delay_us,gpio_get_value(sess->xon_pin));
				//udelay(sess->xon_on_1st_delay_us/1000);
				usleep_range(sess->xon_on_1st_delay_us,sess->xon_on_1st_delay_us);
			}
			gpio_direction_output(sess->xon_pin,sess->xon_active_high);
			printk(KERN_DEBUG"%s(): XON=%d\n",__func__,sess->xon_active_high);
		}
#endif //] XON_TIMING_ENABLE

		do {
			++iTryCnt;
			/* enable rail power */
			jd9930_EN(sess,1);
			
			//stat |= papyrus_hw_setreg(sess, VLDO_ADJ, sess->vadj);
			jd9930_get_power_status(sess,0,&iPG,NULL);
			if (iPG) {
				pr_debug("%s() : JD9930 power on rail is ready\n", __FUNCTION__);
				break;
			}
			// commit it, so that we can adjust vcom through "Rk_ebc_power_control_Release_v1.1" 
			//stat |= papyrus_hw_setreg(sess, VCOM_ADJ1, sess->vcom1);
			//stat |= papyrus_hw_setreg(sess, VCOM_ADJ2, sess->vcom2);

			/* wait for power good */
			iRet = jd9930_wait_power_good(sess, 1);
#if 1
			if(iRet<0) {
				printk("%s power on failed %d,retry ...\n",__FUNCTION__,iTryCnt);
#if 1
				printk("reseting JD9930 EN ...\n");
				msleep(JD9930_SLEEP0_MS);
				jd9930_EN(sess, 0);
				jd9930_EN(sess, 1);
#endif
			}
		} while (iTryCnt<=5);
#else
		papyrus_hw_send_powerup(sess);
#endif

		
	} else if( up<=0 ) {
		/* chip need setting to day/night mode power off sequence */
		if (-1==up) {
			jd9930_switch_nightmode(sess, 1);
		}
		else {
			jd9930_switch_nightmode(sess, 0);
		}
		
		/* disable rail power */
		jd9930_EN(sess, 0); 

		/* wait for power good */
		jd9930_wait_power_good(sess, 0);

		if(gpio_is_valid(sess->xon_pin)) {
			hrtimer_cancel(&sess->hrt_xon_on);

			if(-1==up) { 
				// night mode ...

				hrtimer_start(&sess->hrt_xon_on,
					ktime_set(sess->xon_on_delay_us/1000000,(sess->xon_on_delay_us%1000000) * 1000),
					HRTIMER_MODE_REL);
			}
		}
	}
	return;
}


static bool papyrus_hw_power_ack(struct pmic_sess *pmsess)
{
	struct papyrus_sess *sess = pmsess->drvpar;
	int st;
	int retries_left = 10;

	do {
		st = jd9930_is_power_good(sess);
		if (!st) {
			st = -1;	/* not settled yet */
			msleep(HW_GET_STATE_INTERVAL_MS);
		}
		retries_left--;
	} while ((st == -1) && retries_left);

	if ((st == -1) && !retries_left)
		pr_err("papyrus: power up/down settle error (PG = %02x)\n", st);

	return !!st;
}


static void papyrus_hw_cleanup(struct papyrus_sess *sess)
{
	gpio_free(sess->power_up_pin);
	if (gpio_is_valid(sess->xon_pin)) {
		gpio_free(sess->xon_pin);
	}
	if(gpio_is_valid(sess->power_all_pin))
		gpio_free(sess->power_all_pin);
	gpio_free(sess->power_good_pin);
	
	i2c_put_adapter(sess->adap);
}

/* -------------------------------------------------------------------------*/

static int papyrus_set_enable(struct pmic_sess *pmsess, int enable)
{
	struct papyrus_sess *sess = pmsess->drvpar;
	sess->enable_reg = enable;
	return 0;
}

static int papyrus_set_vcom_voltage(struct pmic_sess *pmsess, int vcom_mv)
{
	struct papyrus_sess *sess = pmsess->drvpar;
	sess->vcom1 = (unsigned char)(((-vcom_mv)*1000)/JD9930_VCOM_STEP_uV);
	return 0;
}

static int papyrus_set_vcom1(struct pmic_sess *pmsess, uint8_t vcom1)
{
	struct papyrus_sess *sess = pmsess->drvpar;
	sess->vcom1 = vcom1;
	return 0;
}

static int papyrus_set_vcom2(struct pmic_sess *pmsess, uint8_t vcom2)
{
	struct papyrus_sess *sess = pmsess->drvpar;
	// TODO; Remove this temporary solution to set custom vcom-off mode
	//       Add PMIC setting when this is to be a permanent feature
	pr_debug("papyrus_set_vcom2 vcom2off 0x%02x\n", vcom2);
	sess->vcom2off = vcom2;
	return 0;
}

static int papyrus_set_vadj(struct pmic_sess *pmsess, uint8_t vadj)
{
	return 0;
}

static int papyrus_set_int_en1(struct pmic_sess *pmsess, uint8_t int_en1)
{
	return 0;
}

static int papyrus_set_int_en2(struct pmic_sess *pmsess, uint8_t int_en2)
{
	return 0;
}

static int papyrus_set_upseq0(struct pmic_sess *pmsess, uint8_t upseq0)
{
	return 0;
}

static int papyrus_set_upseq1(struct pmic_sess *pmsess, uint8_t upseq1)
{
	return 0;
}

static int papyrus_set_dwnseq0(struct pmic_sess *pmsess, uint8_t dwnseq0)
{
	return 0;
}

static int papyrus_set_dwnseq1(struct pmic_sess *pmsess, uint8_t dwnseq1)
{
	return 0;
}

static int papyrus_set_tmst1(struct pmic_sess *pmsess, uint8_t tmst1)
{
	return 0;
}

static int papyrus_set_tmst2(struct pmic_sess *pmsess, uint8_t tmst2)
{
	return 0;
}

static int papyrus_vcom_switch(struct pmic_sess *pmsess, bool state)
{
	return 0;
}

static bool papyrus_standby_dwell_time_ready(struct pmic_sess *pmsess)
{
	struct papyrus_sess *sess = pmsess->drvpar;
	struct timeval current_tv;
	long total_secs;

	do_gettimeofday(&current_tv);
	mb();
	total_secs = current_tv.tv_sec - sess->standby_tv.tv_sec;

	if (total_secs < PAPYRUS_STANDBY_DWELL_TIME)
		return false;

	return true;
}

static int nWakeFormIdleMode = 0;

static int papyrus_pm_sleep(struct pmic_sess *sess)
{
	struct papyrus_sess *s = sess->drvpar;

	pr_info("PMIC%d: enter to sleep\n", sess->id);
  

	if(gpio_is_valid(s->xon_pin)) {
		if(hrtimer_active(&s->hrt_xon_off)) {
			pr_warn("waiting for XON off timing ,please retry suspend later !!!\n");
			return -1;
		}
	}

	papyrus_resume_flag = 0;

	return 0;
}

static int papyrus_pm_resume(struct pmic_sess *sess)
{
	struct papyrus_sess *s = sess->drvpar;
	int iRet;

	pr_info("PMIC%d: resume from sleep\n", sess->id);
	
	if (MASTER_PMIC_ID == sess->id) {
		papyrus_resume_flag = 1;
	}
	
	if(g_regJD9930->need_reinit) 
	{
		iRet = jd9930_regs_init(s);
		if (iRet) {
			pr_info("jd9930 register init counts > 20 failed ....\n");
		}
		else {
			g_regJD9930->need_reinit = 0;
		}
	}
	
	
	return 0;
}

static int papyrus_pm_suspend_late(struct pmic_sess *sess)
{
	struct papyrus_sess *s = sess->drvpar;
	
	pr_info("PMIC%d: enter to sleep late for %s \n", sess->id,gSleep_Mode_Suspend?"deep sleep":"sleep");
	


	if (gSleep_Mode_Suspend) {
		if(gpio_is_valid(s->xon_pin)) {
			hrtimer_cancel(&s->hrt_xon_on);
#ifdef XON_TIMING_ENABLE //[
			pr_debug("%s() : XON=%d\n",__func__,!s->xon_active_high);
			gpio_direction_output(s->xon_pin,!s->xon_active_high);
#endif //] XON_TIMING_ENABLE
			
		}
		
		g_regJD9930->need_reinit = 1;
	}
	else {
#if 1
		//if(133==s->power_up_pin) // PE5 確定會被拉到0 .
		{
			// EN會被拉到0 , 這裡只是先行設定對應的狀態.
			g_regJD9930->need_reinit = 1;
		}
#endif
	}
	
	if (gpio_is_valid(s->en_ts_pin)) {
		gpio_direction_output(s->en_ts_pin, !s->en_ts_active_high);
	}
	
	if(gpio_is_valid(s->power_all_pin)){
		// PWRALL 會被拉到0 , 這裡只是先行設定對應的狀態.
		gpio_direction_output(s->power_all_pin, 0);
	}

	return 0;
}

static int papyrus_pm_resume_early(struct pmic_sess *sess)
{
	struct papyrus_sess *s = sess->drvpar;
	
	pr_info("PMIC%d: resume early from sleep\n", sess->id);
	if(gpio_is_valid(s->power_all_pin)) {
		gpio_direction_output(s->power_all_pin, 1);
	}
	
	if (gpio_is_valid(s->en_ts_pin)) {
		gpio_direction_output(s->en_ts_pin, s->en_ts_active_high);
	}
	
	if(gpio_is_valid(s->xon_pin)) {
#ifdef XON_TIMING_ENABLE //[
		pr_debug("%s() : XON=%d\n",__func__,s->xon_active_high);
		gpio_direction_output(s->xon_pin,s->xon_active_high);
#endif //] XON_TIMING_ENABLE
		
	}

	
	return 0;
}

static int papyrus_probe(struct pmic_sess *pmsess, struct pmic_config *config, struct i2c_client *client)
{
	struct papyrus_sess *sess = NULL;
	struct papyrus_sess *tmp_sess = NULL;
	struct pmic_sess *tmp_pmsess = NULL;
	PMIC_ID id = 0;
	int stat;

	if ((NULL == pmsess) || (NULL == config) || (NULL == client)) {
		pr_err("PMIC%d: input param is null\n", id);
		return -1;
	}
	
	sess = kzalloc(sizeof(*sess), GFP_KERNEL);
	if (!sess)
		return -ENOMEM;

	
	sess->client = client;
	sess->adap = client->adapter;	

	sess->id = pmsess->id;
	sess->power_up_pin = config->power_up_pin.gpio;
	sess->xon_pin = config->xon_pin.gpio;
	//pr_info("EN gpio=%d\n",sess->power_up_pin);
	sess->power_all_pin = config->power_all_pin.gpio;
	sess->power_good_pin = config->power_good_pin.gpio;
	sess->en_ts_pin = config->en_ts_pin.gpio;
	sess->power_active_high = 1;
	sess->xon_active_high = 1;
	sess->en_ts_active_high = 1;

	sess->enable_reg_shadow = 0;
	sess->pmic_power_ldo = regulator_get(NULL, (const char *)config->pmic_power_name);
	if (IS_ERR(sess->pmic_power_ldo)) {
		pr_err("cannot get pmic ldo(%s)\n", config->pmic_power_name);
	}

	//printk("%s(%d)\n",__func__,__LINE__);	
	papyrus_hw_arg_init(sess);

	pmsess->drvpar = sess;

	if (MASTER_PMIC_ID == pmsess->id) {		
		//printk("%s(%d)\n",__func__,__LINE__);	
		stat = papyrus_hw_init(sess, pmsess->id);
		if (stat)
			goto free_sess;

		for (id = 0; id < MAX_PMIC_NUM; id++) {
			if (1 != g_jd9930_mgr->config[id].used_flag) {
				continue;
			}

			//printk("%s(%d)\n",__func__,__LINE__);	
			hrtimer_init(&sess->hrt_xon_on,CLOCK_MONOTONIC,HRTIMER_MODE_REL);
			sess->hrt_xon_on.function = _hrtint_xon_on;
			hrtimer_init(&sess->hrt_xon_off,CLOCK_MONOTONIC,HRTIMER_MODE_REL);
			sess->hrt_xon_off.function = _hrtint_xon_off;
			
			tmp_pmsess = &(g_jd9930_mgr->sess[id]);
			tmp_sess = (struct papyrus_sess *)tmp_pmsess->drvpar;

			sess->power_good_pin = config->power_good_pin.gpio;

			sess->xon_off_delay_us = config->xon_off_delay_us;
			sess->xon_off_day_delay_us = config->xon_off_day_delay_us;
			sess->xon_on_delay_us = config->xon_on_delay_us;
			sess->xon_on_1st_delay_us = config->xon_on_1st_delay_us;

			// reset default regiters value . 
			JD9930_REG_SET(g_regJD9930,VCOM_SET,0x40);
#if 0
			JD9930_REG_SET(g_regJD9930,VPOS_VNEG_SET,0);
			JD9930_REG_SET(g_regJD9930,PWRON_DELAY,0);
			JD9930_REG_SET(g_regJD9930,VGH_EXT,0);
			JD9930_REG_SET(g_regJD9930,VGL_EXT,0);
			JD9930_REG_SET(g_regJD9930,VGHNM_EXT,0);
			JD9930_REG_SET(g_regJD9930,VGHNM,0);
			JD9930_REG_SET(g_regJD9930,DISA_DELAY,0);
			JD9930_REG_SET(g_regJD9930,XON_DELAY,0);
			JD9930_REG_SET(g_regJD9930,XON_LEN,0);
#endif

			JD9930_REG_SET(g_regJD9930,CTRL_REG1,0x2);// V3P3_EN .
			JD9930_REG_SET(g_regJD9930,CTRL_REG2,0x0f); // VP 5A limit, VN 5A limit . 
			
			jd9930_setup_timings(sess, config);// power on delay timings setup .
		
			stat |= jd9930_regs_init(tmp_sess);

			if (stat)
				goto free_sess;
		}
	}
	
	return 0;

free_sess:
	if (sess)
		kfree(sess);
	
	return stat;
}

static void papyrus_remove(struct pmic_sess *pmsess)
{	
	struct papyrus_sess *sess = pmsess->drvpar;

	if (MASTER_PMIC_ID == pmsess->id)
		papyrus_hw_cleanup(sess);

	if (sess)
		kfree(sess);
	pmsess->drvpar = 0;

}

const struct pmic_driver pmic_driver_jd9930_i2c = {
	.id = "jd9930-i2c",

	.vcom_min = PAPYRUS_VCOM_MIN_MV,
	.vcom_max = PAPYRUS_VCOM_MAX_MV,
	.vcom_step = 10,

	.hw_read_temperature = papyrus_hw_read_temperature,
	.hw_power_ack = papyrus_hw_power_ack,
	.hw_power_req = papyrus_hw_power_req,

	.set_enable = papyrus_set_enable,
	.set_vcom_voltage = papyrus_set_vcom_voltage,
	.set_vcom1 = papyrus_set_vcom1,
	.set_vcom2 = papyrus_set_vcom2,
	.set_vadj = papyrus_set_vadj,
	.set_int_en1 = papyrus_set_int_en1,
	.set_int_en2 = papyrus_set_int_en2,
	.set_upseq0 = papyrus_set_upseq0,
	.set_upseq1 = papyrus_set_upseq1,
	.set_dwnseq0 = papyrus_set_dwnseq0,
	.set_dwnseq1 = papyrus_set_dwnseq1,
	.set_tmst1 = papyrus_set_tmst1,
	.set_tmst2 = papyrus_set_tmst2,

	.hw_vcom_switch = papyrus_vcom_switch,

	//.hw_init = papyrus_probe,
	.hw_cleanup = papyrus_remove,

	.hw_standby_dwell_time_ready = papyrus_standby_dwell_time_ready,
	.hw_pm_sleep = papyrus_pm_sleep,
	.hw_pm_resume = papyrus_pm_resume,
	.hw_pm_sleep_late = papyrus_pm_suspend_late,
	.hw_pm_resume_early = papyrus_pm_resume_early,
};

static int jd9930_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	int ret = -1;
	PMIC_ID pmic_id = MASTER_PMIC_ID;
	struct pmic_sess *sess;
	struct pmic_config *config;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		pr_err("I2C check functionality failed\n");
		return -ENODEV;
	}
	
	g_regJD9930 = kzalloc(sizeof(struct jd9930), GFP_KERNEL);
	if (!g_regJD9930)
		return -ENOMEM;

	sess = &(g_jd9930_mgr->sess[pmic_id]);
	config = &(g_jd9930_mgr->config[pmic_id]);

	ret = papyrus_probe(sess, config, client);
	if(0 != ret) {
		pr_err("[jd9930_probe] hw_init master pmic failed.");
		return -ENODEV;
	}

	papyrus_resume_flag = 1;
	sess->is_inited = 1;
	
	pr_info("hw_init master pmic ok\n");
	
	return 0;

}

static int jd9930_remove(struct i2c_client *client)
{	
	struct pmic_sess *pmic_sess = &(g_jd9930_mgr->sess[MASTER_PMIC_ID]);
	struct papyrus_sess *sess = NULL;

	sess = (struct papyrus_sess *)pmic_sess->drvpar;
	pmic_driver_jd9930_i2c.hw_cleanup(pmic_sess);
	
	if (!IS_ERR(sess->pmic_power_ldo)) {
		regulator_disable(sess->pmic_power_ldo);
		regulator_put(sess->pmic_power_ldo);
	}
	
	if (sess)
		kfree(sess);
	
	pmic_sess->drvpar = NULL;
	
	return 0;
}

static int jd9930_suspend_late(struct device *dev)
{
	return pmic_driver_jd9930_i2c.hw_pm_sleep_late(&g_jd9930_mgr->sess[MASTER_PMIC_ID]);
}

static int jd9930_suspend(struct device *dev)
{
	return pmic_driver_jd9930_i2c.hw_pm_sleep(&g_jd9930_mgr->sess[MASTER_PMIC_ID]);
}

static int jd9930_resume_early(struct device *dev)
{
	return pmic_driver_jd9930_i2c.hw_pm_resume_early(&g_jd9930_mgr->sess[MASTER_PMIC_ID]);
}

static int jd9930_resume(struct device *dev)
{
	return pmic_driver_jd9930_i2c.hw_pm_resume(&g_jd9930_mgr->sess[MASTER_PMIC_ID]);
}

static int jd9930_i2c_detect(struct i2c_client *client, struct i2c_board_info *info)
{
	struct i2c_adapter *adapter = client->adapter;

	if(g_jd9930_mgr->config[MASTER_PMIC_ID].twi_id == adapter->nr) {
		strlcpy(info->type, JD9930_I2C_NAME, I2C_NAME_SIZE);
		
		pr_info("%s: Detected chip %s at adapter %d, address 0x%02x\n",
			 __func__, JD9930_I2C_NAME, i2c_adapter_id(adapter), client->addr);
		return 0;
	}
	
	return -ENODEV;	
}

static int sy7636_get_sys_param(void)
{
	return 0;
}

static const struct i2c_device_id jd9930_id[] = {
	{ JD9930_I2C_NAME, 0 },
	{ }
};

static const struct dev_pm_ops jd9930_pm_ops = {
	.suspend = jd9930_suspend,
	.suspend_late = jd9930_suspend_late,
	.resume = jd9930_resume,
	.resume_early = jd9930_resume_early,
};

MODULE_DEVICE_TABLE(i2c, jd9930_id);

static struct i2c_driver jd9930_driver = {
	.class = I2C_CLASS_HWMON,
	.probe	= jd9930_probe,
	.remove 	= jd9930_remove,
	.id_table	= jd9930_id,
	.detect   = jd9930_i2c_detect,
	.driver = {
		.name	  = JD9930_I2C_NAME,
		.owner	  = THIS_MODULE,
		.pm		  = &jd9930_pm_ops,
	},
	.address_list   = normal_i2c,
};

static int jd9930_get_config(struct pmic_config *config, int i)
{
	char *pmic_node_array[MAX_PMIC_NUM] = {"jd9930", "jd9930_slave"};
	struct device_node *np = NULL;
	const char *str;
	
	if ((NULL == config) || (i >= MAX_PMIC_NUM)) {
		printk(KERN_WARNING "%s: input param is wrong\n");
		return -1;
	}

	np = of_find_node_by_name(NULL, pmic_node_array[i]);
	if (NULL == np) {
		pr_err("PMIC%d: cannot find config node(%s)\n", i, pmic_node_array[i]);
		return -2;
	}
	
	config->used_flag = 1;

	memset(config->name, 0, sizeof(config->name));
	if (0 != of_property_read_string(np, "jd9930_name", &str))
		pr_err("PMIC%d: get jd9930_name fail\n", i);
	else {
		memcpy((void *)(config->name), str, strlen(str) + 1);
		pr_info("PMIC%d: jd9930_name = %s\n", i, config->name);
	}

	if (0 != of_property_read_u32_array(np, "jd9930_twi_id", &(config->twi_id), 1))
		pr_err("PMIC%d: get jd9930_twi_id fail\n", i);
	else
		pr_info("PMIC%d: jd9930_twi_id = %d\n", i, config->twi_id);

	if (0 != of_property_read_u32_array(np, "jd9930_twi_addr", &(config->twi_addr), 1))
		pr_err("PMIC%d: get jd9930_twi_addr fail\n", i);
	else
		pr_info("PMIC%d: jd9930_twi_addr = 0x%x\n", i, config->twi_addr);
	
	config->power_up_pin.gpio = of_get_named_gpio_flags(np, "jd9930_powerup", 0, (enum of_gpio_flags *)(&(config->power_up_pin)));
	if (!gpio_is_valid(config->power_up_pin.gpio))
		pr_err("PMIC%d: jd9930_powerup is invalid. \n",i );	
	else
		pr_info("PMIC%d: jd9930_powerup = %d\n", i, config->power_up_pin.gpio);
	
	config->xon_pin.gpio = of_get_named_gpio_flags(np, "xon", 0, (enum of_gpio_flags *)(&(config->xon_pin)));
	if (!gpio_is_valid(config->xon_pin.gpio))
		pr_err("PMIC%d: xon is invalid. \n",i );	
	else
		pr_info("PMIC%d: xon = %d\n", i, config->xon_pin.gpio);

	config->power_good_pin.gpio = of_get_named_gpio_flags(np, "jd9930_powergood", 0, (enum of_gpio_flags *)(&(config->power_good_pin)));
	if (!gpio_is_valid(config->power_good_pin.gpio))
		pr_err("PMIC%d: jd9930_powergood is invalid. \n",i );
	else
		pr_info("PMIC%d: jd9930_powergood = %d\n", i, config->power_good_pin.gpio);
    
	config->en_ts_pin.gpio = of_get_named_gpio_flags(np, "jd9930_en_ts", 0, (enum of_gpio_flags *)(&(config->en_ts_pin)));
	if (!gpio_is_valid(config->en_ts_pin.gpio))
		pr_err("PMIC%d: jd9930_en_ts is invalid. \n",i );	
	else
		pr_info("PMIC%d: jd9930_en_ts = %d\n", i, config->en_ts_pin.gpio);

	config->power_all_pin.gpio = of_get_named_gpio_flags(np, "jd9930_pwrall", 0, (enum of_gpio_flags *)(&(config->power_all_pin)));
	if (!gpio_is_valid(config->power_all_pin.gpio)) {
		pr_err("PMIC%d: jd9930_powerall is invalid. \n",i );
		gHasPWRALL = 0;
	}
	else{
		gHasPWRALL = 1 ;
		pr_info("PMIC%d: jd9930_powerall = %d\n", i, config->power_all_pin.gpio);
	}

	memset(config->pmic_power_name, 0, sizeof(config->pmic_power_name));
	if (0 != of_property_read_string(np, "jd9930_ldo", &str))
		pr_err("PMIC%d: get jd9930_ldo fail\n", i);
	else {
		memcpy((void *)(config->pmic_power_name), str, strlen(str) + 1);
		pr_info("PMIC%d: jd9930_ldo = %s\n", i, config->pmic_power_name);
	}

	if (0 != of_property_read_u32_array(np, "jd9930_on_delay1", &(config->on_delay1), 1)) {
		config->on_delay1 = 0xff;
		pr_err("PMIC%d: get jd9930_on_delay1 fail\n", i);
	}
	else {
		pr_info("PMIC%d: jd9930_on_delay1 = %d\n", i, config->on_delay1);
	}
	
	if (0 != of_property_read_u32_array(np, "jd9930_on_delay2", &(config->on_delay2), 1)) {
		config->on_delay2 = 0xff;
		pr_err("PMIC%d: get jd9930_on_delay2 fail\n", i);
	}
	else {
		pr_info("PMIC%d: jd9930_on_delay2 = %d\n", i, config->on_delay2);
	}
	
	if (0 != of_property_read_u32_array(np, "jd9930_on_delay3", &(config->on_delay3), 1)) {
		config->on_delay3 = 0xff;
		pr_err("PMIC%d: get jd9930_on_delay3 fail\n", i);
	}
	else {
		pr_info("PMIC%d: jd9930_on_delay3 = %d\n", i, config->on_delay3);
	}
	
	if (0 != of_property_read_u32_array(np, "jd9930_on_delay4", &(config->on_delay4), 1)) {
		config->on_delay4 = 0xff;
		pr_err("PMIC%d: get jd9930_on_delay4 fail\n", i);
	}
	else {
		pr_info("PMIC%d: jd9930_on_delay4 = %d\n", i, config->on_delay4);
	}
	
	if(0 != of_property_read_u32_array(np,"vpos_vneg", &(config->vpos_vneg), 1)) {
		config->vpos_vneg=0x00;
		pr_err("PMIC%d: get vpos_vneg property failed\n", i);
	}
	else {
		pr_info("PMIC%d: vpos_vneg=0x%x\n", i, config->vgh_ext);
	}
	
	if(0 != of_property_read_u32_array(np,"vgh_ext", &(config->vgh_ext), 1)) {
		config->vgh_ext=0xff;
		pr_err("PMIC%d: get vgh_ext property failed\n", i);
	}
	else {
		pr_info("PMIC%d: vgh_ext=0x%x\n", i, config->vgh_ext);
	}
	
	if(0 != of_property_read_u32_array(np,"vgl_ext", &(config->vgl_ext), 1)) {
		config->vgl_ext=0xff;
		pr_err("PMIC%d: get vgl_ext property failed\n", i);
	}
	else {
		pr_info("PMIC%d: vgl_ext=0x%x\n", i, config->vgl_ext);
	}
	
	if(0 != of_property_read_u32_array(np,"vghnm_ext", &(config->vghnm_ext), 1)) {
		config->vghnm_ext=0xff;
		pr_err("PMIC%d: get vghnm_ext property failed\n", i);
	}
	else {
		pr_info("PMIC%d: vghnm_ext=0x%x\n", i, config->vghnm_ext);
	}
	
	if(0 != of_property_read_u32_array(np,"vghnm", &(config->vghnm), 1)) {
		config->vghnm=0xff;
		pr_err("PMIC%d: get vghnm property failed\n", i);
	}
	else {
		pr_info("PMIC%d: vghnm=0x%x\n", i, config->vghnm);
	}
	
	if(0 != of_property_read_u32_array(np,"disa_delay", &(config->disa_delay), 1)) {
		config->disa_delay=0xff;
		pr_err("PMIC%d: get disa_delay property failed\n", i);
	}
	else {
		pr_info("PMIC%d: disa_delay=0x%x\n", i, config->disa_delay);
	}
	
	if(0 != of_property_read_u32_array(np,"xon_delay", &(config->xon_delay), 1)) {
		config->xon_delay=0xff;
		pr_err("PMIC%d: get xon_delay property failed\n", i);
	}
	else {
		pr_info("PMIC%d: xon_delay=0x%x\n", i, config->xon_delay);
	}
	
	if(0 != of_property_read_u32_array(np,"xon_len", &(config->xon_len), 1)) {
		config->xon_len=0xff;
		pr_err("PMIC%d: get xon_len property failed\n", i);
	}
	else {
		pr_info("PMIC%d: xon_len=0x%x\n", i, config->xon_len);
	}
	
	if (0 != of_property_read_u32_array(np, "xon_off_delay_us", &(config->xon_off_delay_us), 1)) {
		config->xon_off_delay_us = (u32)(-1);
		pr_err("PMIC%d: get xon_off_delay_us fail\n", i);
	}
	else {
		pr_info("PMIC%d: xon_off_delay_us = %d\n", i, (int)config->xon_off_delay_us);
	}

	if (0 != of_property_read_u32_array(np, "xon_off_day_delay_us", &(config->xon_off_day_delay_us), 1)) {
		config->xon_off_day_delay_us = (u32)(-1);
		pr_err("PMIC%d: get xon_off_day_delay_us fail\n", i);
	}
	else {
		pr_info("PMIC%d: xon_off_day_delay_us = %d\n", i, (int)config->xon_off_day_delay_us);
	}

	if (0 != of_property_read_u32_array(np, "xon_on_delay_us", &(config->xon_on_delay_us), 1)) {
		config->xon_on_delay_us = (u32)(-1);
		pr_err("PMIC%d: get xon_on_delay_us fail\n", i);
	}
	else {
		pr_info("PMIC%d: xon_on_delay_us = %d\n", i, (int)config->xon_on_delay_us);
	}

	if (0 != of_property_read_u32_array(np, "xon_on_1st_delay_us", &(config->xon_on_1st_delay_us), 1)) {
		config->xon_on_1st_delay_us = (u32)(-1);
		pr_err("PMIC%d: get xon_on_1st_delay_us fail\n", i);
	}
	else {
		pr_info("PMIC%d: xon_on_1st_delay_us = %d\n", i, (int)config->xon_on_1st_delay_us);
	}

	return 0;
}

static int jd9930_manager_init(void)
{
	int i = 0;
	int ret = -1;
	
	g_jd9930_mgr = kmalloc(sizeof(*g_jd9930_mgr), GFP_KERNEL);
	if (NULL == g_jd9930_mgr) {
		printk(KERN_ERR "%s: fail to alloc memory\n", __func__);
		return -1;
	}

	memset(g_jd9930_mgr, 0, sizeof(*g_jd9930_mgr));
	for (i = 0; i < MAX_PMIC_NUM; i++) {
		g_jd9930_mgr->sess[i].id = i;
		ret = jd9930_get_config(&(g_jd9930_mgr->config[i]), i);
		if (ret) {
			if (i!= 0)
				return 0;
			pr_err("PMIC%d: get config fail, ret=%d\n", i, ret);
			break;
		}
	}
	return ret;
}

static void jd9930_manager_exit(void)
{
	if (g_jd9930_mgr)
		kfree(g_jd9930_mgr);
	return;
}

static int __init jd9930_init(void)
{
	int ret = -1;
	
	ret = jd9930_manager_init();
	if (ret) {
		pr_err("[jd9930_init] pmic manager init fail, ret=%d", ret);
		return -1;
	}
	
	if (gptHWCFG->m_val.bDisplayCtrl != 0x1C)
		return -1;

	if (g_jd9930_mgr->config[MASTER_PMIC_ID].used_flag) {
		ret = i2c_add_driver(&jd9930_driver);
		if (ret) {
			pr_err("[jd9930_init] fail to init master pmic, ret=%d\n", ret);
		} else {
			pr_info("init master pmic ok\n");
		}
	}
	return ret;
}

static void __exit jd9930_exit(void)
{
	if (g_jd9930_mgr->config[MASTER_PMIC_ID].used_flag) {
		i2c_del_driver(&jd9930_driver);
	}

	jd9930_manager_exit();
	
	return;
}

fs_initcall(jd9930_init);
module_exit(jd9930_exit);

MODULE_DESCRIPTION("jd9930 pmic");
MODULE_LICENSE("GPL");

int jd9930_vcom_set(int vcom_mv)
{
	struct pmic_sess *master_pmsess = NULL, *tmp_pmsess = NULL;
	struct papyrus_sess *master_sess = NULL, *tmp_sess = NULL;
	PMIC_ID id = 0;
	uint8_t rev_val = 0;
	int stat = 0;
	int read_vcom_mv = 0;
	int cnt = 20;

	printk("%s: vcom setting %d\n", __func__, vcom_mv);
	
	if(1 != g_jd9930_mgr->sess[MASTER_PMIC_ID].is_inited) {
		//pr_err("master pmic has not initial yet\n");
		return -2;
	}
	
	cfg_VCOMmV = vcom_mv;

	master_pmsess = &(g_jd9930_mgr->sess[MASTER_PMIC_ID]);
	master_sess = (struct papyrus_sess *)master_pmsess->drvpar;
	if(gpio_is_valid(master_sess->power_all_pin))
		gpio_direction_output(master_sess->power_all_pin, 1);

	for (id = 0; id < MAX_PMIC_NUM; id++) {
		if ((1 != g_jd9930_mgr->config[id].used_flag) || (1 != g_jd9930_mgr->sess[id].is_inited)) {
			continue;
		}

		tmp_pmsess = &(g_jd9930_mgr->sess[id]);
		tmp_sess = (struct papyrus_sess *)tmp_pmsess->drvpar;
		
		//papyrus_hw_power_req ((struct pmic_sess *)tmp_pmsess, 1);
		// Set vcom voltage
		pmic_driver_jd9930_i2c.set_vcom_voltage((struct pmic_sess *)tmp_pmsess,vcom_mv);
		stat |= papyrus_hw_setreg(tmp_sess, VCOM_SET,tmp_sess->vcom1);
		pr_debug("sess->vcom = 0x%x\n",tmp_sess->vcom1);
	}

	if (stat)
		pr_err("papyrus: I2C error: %d\n", stat);

	return 0;
}
EXPORT_SYMBOL(jd9930_vcom_set);

int jd9930_vcom_get(int *vcom)
{
	uint8_t vcom_reg_val;
	int iVCOMmV;
	
	struct pmic_sess *master_pmsess = NULL, *tmp_pmsess = NULL;
	struct papyrus_sess *master_sess = NULL, *tmp_sess = NULL;
	PMIC_ID id = 0;
	int stat = 0;

	if(1 != g_jd9930_mgr->sess[MASTER_PMIC_ID].is_inited) {
		//pr_err("master pmic has not initial yet\n");
		return -2;
	}

	pr_debug("%s(%d) \n",__func__,__LINE__);
	master_pmsess = &(g_jd9930_mgr->sess[MASTER_PMIC_ID]);
	master_sess = (struct papyrus_sess *)master_pmsess->drvpar;

	if(gpio_is_valid(master_sess->power_all_pin))
		gpio_direction_output(master_sess->power_all_pin, 1);

	for (id = 0; id < MAX_PMIC_NUM; id++) {
		if ((1 != g_jd9930_mgr->config[id].used_flag) || (1 != g_jd9930_mgr->sess[id].is_inited)) {
			continue;
		}

		tmp_pmsess = &(g_jd9930_mgr->sess[id]);
		tmp_sess = (struct papyrus_sess *)tmp_pmsess->drvpar;
		
		//papyrus_hw_power_req ((struct pmic_sess *)tmp_pmsess, 1);
		// Get vcom voltage
		stat |= papyrus_hw_getreg(tmp_sess, VCOM_SET, &vcom_reg_val);
		
		//papyrus_hw_power_req ((struct pmic_sess *)tmp_pmsess, 0);
	}

	if (stat) {
		pr_err("papyrus: I2C error: %d\n", stat);
		return -1;
	}
	
	iVCOMmV = -((vcom_reg_val*JD9930_VCOM_STEP_uV)/1000);
    
    memcpy(vcom, &iVCOMmV, sizeof(int));
	
	return 0;
}
EXPORT_SYMBOL(jd9930_vcom_get);

int jd9930_power_status(void)
{
	struct pmic_sess *master_pmsess = NULL, *tmp_pmsess = NULL;
	struct papyrus_sess *master_sess = NULL, *tmp_sess = NULL;
	PMIC_ID id = 0;
	int ret = -1;

	int status = -1;

	pr_debug("%s()\n", __func__);
	if (!g_jd9930_mgr->sess[id].is_inited)
		return status;

	for (id == 0; id < MAX_PMIC_NUM; id++) {
		if ((1 != g_jd9930_mgr->config[id].used_flag) || (1 != g_jd9930_mgr->sess[id].is_inited)) {
			continue;
		}
		
		tmp_pmsess = &(g_jd9930_mgr->sess[id]);
		tmp_sess = (struct papyrus_sess *)tmp_pmsess->drvpar;
		
		if (!jd9930_is_power_good(tmp_sess))
			status = 0;
		else
			status = 1;
	}

	return status;
}
EXPORT_SYMBOL(jd9930_power_status);

int jd9930_power_on_ex(unsigned long dwFlags)
{
	struct pmic_sess *master_pmsess = NULL, *tmp_pmsess = NULL;
	struct papyrus_sess *master_sess = NULL, *tmp_sess = NULL;
	PMIC_ID id = 0;
	int ret = -1;

	pr_debug("%s()\n",__func__);

	for (id = 0; id < MAX_PMIC_NUM; id++) {
		if ((1 != g_jd9930_mgr->config[id].used_flag) || (1 != g_jd9930_mgr->sess[id].is_inited)) {
			continue;
		}

		tmp_pmsess = &(g_jd9930_mgr->sess[id]);
		tmp_sess = (struct papyrus_sess *)tmp_pmsess->drvpar;
	
		pmic_driver_jd9930_i2c.hw_power_req((struct pmic_sess *)tmp_pmsess,
				(dwFlags&EBC_PWR_FLAG_XON)?2:1);
	}

	return 0;
}
int jd9930_power_on(void)
{
	return jd9930_power_on_ex(EBC_PWR_FLAG_DEFAULT);
}
int jd9930_power_down_ex(unsigned long dwFlags)
{
	struct pmic_sess *master_pmsess = NULL, *tmp_pmsess = NULL;
	struct papyrus_sess *master_sess = NULL, *tmp_sess = NULL;
	PMIC_ID id = 0;
	int ret = -1;
	
	pr_debug("%s()\n",__func__);
	for (id = 0; id < MAX_PMIC_NUM; id++) {
		if ((1 != g_jd9930_mgr->config[id].used_flag) || (1 != g_jd9930_mgr->sess[id].is_inited)) {
			continue;
		}

		tmp_pmsess = &(g_jd9930_mgr->sess[id]);
		tmp_sess = (struct papyrus_sess *)tmp_pmsess->drvpar;
		pmic_driver_jd9930_i2c.hw_power_req((struct pmic_sess *)tmp_pmsess,
				(dwFlags&EBC_PWR_FLAG_XON)?-1:0);
	}
	
	return 0;
}
int jd9930_power_down(void)
{
	return jd9930_power_down_ex(EBC_PWR_FLAG_DEFAULT);
}

int jd9930_vcom_disable_ex(unsigned long dwFlags)
{
	struct pmic_sess *pmsess = NULL;
	struct papyrus_sess *sess = NULL;
	PMIC_ID id = 0;
	
	for (id = 0; id < MAX_PMIC_NUM; id++) {
		if ((1 != g_jd9930_mgr->config[id].used_flag) || (1 != g_jd9930_mgr->sess[id].is_inited)) {
			continue;
		}

		pmsess = &(g_jd9930_mgr->sess[id]);
		sess = (struct papyrus_sess *)pmsess->drvpar;

#ifdef XON_TIMING_ENABLE //[
		if(gpio_is_valid(sess->xon_pin)) {
			hrtimer_cancel(&sess->hrt_xon_off);

			if(dwFlags&EBC_PWR_FLAG_XON) {
				printk(KERN_DEBUG"XON OFF after %d us\n",sess->xon_off_delay_us);
				hrtimer_start(&sess->hrt_xon_off,
					ktime_set(sess->xon_off_delay_us/1000000,(sess->xon_off_delay_us%1000000) * 1000),
						HRTIMER_MODE_REL);
			}
			else {
				if((u32)-1 != sess->xon_off_day_delay_us) {
					printk(KERN_DEBUG"daymode XON OFF after %d us\n",sess->xon_off_day_delay_us);
					hrtimer_start(&sess->hrt_xon_off,
						ktime_set(sess->xon_off_day_delay_us/1000000,(sess->xon_off_day_delay_us%1000000) * 1000),
						HRTIMER_MODE_REL);
				}
			}
		}
#endif //] XON_TIMING_ENABLE
		
	}
	
	return 0;
}
int jd9930_vcom_disable(void) {
	return jd9930_vcom_disable_ex(EBC_PWR_FLAG_DEFAULT);
}

int jd9930_temperature_get(int *temp)
{
	struct pmic_sess *master_pmsess = NULL;

	master_pmsess = &g_jd9930_mgr->sess[MASTER_PMIC_ID];
	if(master_pmsess->is_inited)
		return pmic_driver_jd9930_i2c.hw_read_temperature(master_pmsess,temp);
	else
		return -1;
}
EXPORT_SYMBOL(jd9930_temperature_get);

int register_ebc_jd9930_pwr_ops(struct ebc_pwr_ops *ops)
{
	if (g_jd9930_mgr->sess[MASTER_PMIC_ID].is_inited) {
		ops->power_on = jd9930_power_on;
		ops->power_on_ex = jd9930_power_on_ex;
		ops->power_down = jd9930_power_down;
		ops->power_down_ex = jd9930_power_down_ex;
		ops->vcom_disable = jd9930_vcom_disable;
		ops->vcom_disable_ex = jd9930_vcom_disable_ex;
		ops->power_status = jd9930_power_status;
	} else 
		return -1;
	
	return 0;
}
EXPORT_SYMBOL(register_ebc_jd9930_pwr_ops);




