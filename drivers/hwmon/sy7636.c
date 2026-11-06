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
#define PAPYRUS2_1P1_I2C_ADDRESS		0x62
extern void papyrus_set_i2c_address(int address);

#include <linux/gpio.h>
#include <linux/sunxi-gpio.h>


#include "../../arch/arm/mach-sunxi/ntx_hwconfig.h"
extern volatile NTX_HWCONFIG *gptHWCFG;

//#include "pmic.h"
//#include <linux/pmic.h>
//#include "../ebc.h"
#include <linux/ebc.h>
/*
struct ebc_pwr_ops
{
	int (*power_on)(void);
	int (*power_down)(void);
	int (*set_vcom_value)(int vcom_mv);
	int (*vcom_enable)(void);
	int (*vcom_disable)(void);
};
*/

struct pmic_mgr *g_pmic_mgr = NULL;

#define TPS65185_I2C_NAME "sy7636"
#define TPS65185_SLAVE_I2C_NAME "sy7636_slave"


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

#define SY7636_PWRON_READYUS	2000
#define SY7636_WAKEUP0_MS	4
#define SY7636_SLEEP0_MS	4
#define SY7636_VCOM_OFF_US	100

#define SY7636_INIT_COUNT	20

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
    REG_SY7636_OPMODE = 0x0,
    REG_SY7636_VCOM_ADJ1,
    REG_SY7636_VCOM_ADJ2,
    REG_SY7636_VLDO_ADJ,
    REG_SY7636_VPDD_LEN,
    REG_SY7636_VEE_VP_EXT,
    REG_SY7636_PWRON_DLY,
    REG_SY7636_FAULTFLAGS,
    REG_SY7636_THERM,
    SY7636_REG_NUM,
};

struct sy7636 {
    unsigned char need_reinit;// need reinit registers .
    unsigned char need_re_en;// need re enable sy7636 .
    
    unsigned char regOPMODE;
    unsigned char regVCOM_ADJ1;
    unsigned char regVCOM_ADJ2;
    unsigned char regVLDO_ADJ;
    unsigned char regVPDD_LEN;
    unsigned char regVEE_VP_EXT;
    unsigned char regPWRON_DLY;
    unsigned char regFAULTFLAGS;
    unsigned char regTHERM;
};

/* Addresses to scan */
static const unsigned short normal_i2c[2] = {PAPYRUS2_1P1_I2C_ADDRESS,I2C_CLIENT_END};
static const unsigned short normal_slave_i2c[2] = {PAPYRUS2_1P1_I2C_ADDRESS,I2C_CLIENT_END};

static int gHasPWRALL = -1 ;

/*struct ctp_config_info config_info = {
	.input_type = CTP_TYPE,
	.name = NULL,
	.int_number = 0,
};*/

struct sy7636_platform_data{
	struct gpio_config wake_up_pin;
	struct gpio_config vcom_ctl_pin;
	struct gpio_config power_up_pin;
	struct gpio_config power_all_pin;
	struct gpio_config power_good_pin;
};

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
	unsigned int v3p3off_time_ms;
	int wake_up_pin;
	int vcom_ctl_pin;
	int power_up_pin;
	int xon_pin;
	int power_all_pin;
	int power_good_pin;
	/* True if a high WAKEUP brings Papyrus out of reset. */
	int wakeup_active_high;
	int vcomctl_active_high;
	int power_active_high;
	int xon_active_high;
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

struct sy7636 *g_regSY7636;

extern int gSleep_Mode_Suspend;

extern int sy7636_power_on(void);
extern int sy7636_power_on_ex(unsigned long dwFlags);
extern int sy7636_power_down(void);
extern int sy7636_power_down_ex(unsigned long dwFlags);
extern int sy7636_vcom_get(int *vcom);
extern int sy7636_vcom_enable(void);
extern int sy7636_vcom_disable(void);
extern int sy7636_vcom_disable_ex(unsigned long dwFlags);

static int cfg_VCOMmV = 0;

static int sy7636_EN(struct papyrus_sess *sess, int iEN);
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


static int sy7636_papyrus_hw_setreg(struct papyrus_sess *sess, uint8_t regaddr, uint8_t val)
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

	if(sess->power_active_high!=gpio_get_value(sess->power_up_pin)) {
		// en not enabled . 
		pr_debug("%s(): enabling EN before registers access .\n",__func__);
		//dump_stack();
		sy7636_EN(sess, 2);
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


static int sy7636_papyrus_hw_getreg(struct papyrus_sess *sess, uint8_t regaddr, uint8_t *val)
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

	if(sess->power_active_high!=gpio_get_value(sess->power_up_pin)) {
		// en not enabled . 
		pr_debug("%s(): enabling EN before registers access .\n",__func__);
		//dump_stack();
		sy7636_EN(sess, 2);
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

#define SY7636_REG_GET(ptSY7636,_regName)		\
	(ptSY7636)->reg##_regName

#define SY7636_REG_SET(ptSY7636,_regName,val)		\
	(ptSY7636)->reg##_regName=val

/*
 * sy7636_to_reg_val(): Creates a register value with new data
 *
 * Creates a new register value for a particular field.  The data
 * outside of the new field is not modified.
 *
 * @cur_reg: current value in register
 * @reg_mask: mask of field bits to be modified
 * @fld_val: new value for register field.
 */
static unsigned int sy7636_to_reg_val(unsigned int cur_reg, unsigned int fld_mask,
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
	new_reg_val = sy7636_to_reg_val(cur_reg,fld_mask,fld_val);\
	ptSY7636->reg##_regName=(unsigned char)new_reg_val;\
	new_reg_val;\
})

#define papyrus_hw_setreg(sess,_regName,val)		\
({\
	int _iRet;\
	{\
		SY7636_REG_SET(g_regSY7636,_regName,val);\
		PRINT_REG_WR(#_regName,g_regSY7636->reg##_regName);\
		_iRet=sy7636_papyrus_hw_setreg(sess,REG_SY7636_##_regName,val);\
	}\
	_iRet;\
})
#define papyrus_hw_getreg(sess,_regName,val)		\
({\
	int _iRet;\
	{\
		_iRet=sy7636_papyrus_hw_getreg(sess,REG_SY7636_##_regName,val);\
	}\
	_iRet;\
})

#define SY7636_REG_WRITE(sess,_regName)		\
({\
	int _iRet;\
	PRINT_REG_WR(#_regName,g_regSY7636->reg##_regName);\
	_iRet = sy7636_papyrus_hw_setreg(sess,REG_SY7636_##_regName,g_regSY7636->reg##_regName);\
 	_iRet;\
})

static void papyrus_hw_get_pg(struct papyrus_sess *sess,
							  struct papyrus_hw_state *hwst)
{
	int stat;

	stat = papyrus_hw_getreg(sess,
				FAULTFLAGS, &hwst->pg_status);
	if (stat)
		pr_err("papyrus: I2C error: %d\n", stat);
}

/*
static void papyrus_hw_get_state(struct papyrus_sess *sess, struct papyrus_hw_state *hwst)
{
	int stat;

	stat = papyrus_hw_getreg(sess,
				PAPYRUS_ADDR_TMST_VALUE, &hwst->tmst_value);
	stat |= papyrus_hw_getreg(sess,
				PAPYRUS_ADDR_INT_STATUS1, &hwst->int_status1);
	stat |= papyrus_hw_getreg(sess,
				PAPYRUS_ADDR_INT_STATUS2, &hwst->int_status2);
	stat |= papyrus_hw_getreg(sess,
				REG_SY7636_FAULTFLAGS, &hwst->pg_status);
	if (stat)
		pr_err("papyrus: I2C error: %d\n", stat);
}
*/

int sy7636_get_FaultFlags(struct papyrus_sess *sess,int iIsGetCached)
{
	unsigned int dwReg;
	int iRet ;
	char iChk;

	if(!sess) {
		printk("%s():error object !\n",__FUNCTION__);
		return -2;
	}
	iRet = papyrus_hw_getreg(sess, FAULTFLAGS, &iChk);
	if(iRet>=0) {
		iRet = (iChk & 0x1F);
	}
	else {
		iRet = -1;
		printk("%s():reading fault flags failed !\n",__FUNCTION__);
	}
	return iRet;
}

int sy7636_get_power_status(struct papyrus_sess *sess,int iIsGetCached,int *O_piPG,unsigned char *O_pbFaults)
{
	int iRet;
	unsigned char bFaultFlags;
	unsigned char bFaults;
	int iPG;

	iRet = sy7636_get_FaultFlags(sess,iIsGetCached);

	if(iRet<0) {
		bFaults = 0;
		bFaultFlags = 0;
		iRet = -1;
		return -1;
	}
	else {
		bFaultFlags = (unsigned char)iRet;
		bFaults = (bFaultFlags>>1)&0x0F;
		iPG = bFaultFlags&1;
		if(bFaults) {
			iRet = 1;
		}
		else {
			iRet = 0;
		}
	}

	if(O_piPG) {
		*O_piPG = iPG;
	}

	if(O_pbFaults) {
		*O_pbFaults = bFaults;
	}

	return iRet;
}

static int sy7636_is_power_good(struct papyrus_sess *sess)
{
	/*
	 * XOR of polarity (starting value) and current
	 * value yields whether power is good.
	 */
	return gpio_get_value(sess->power_good_pin);
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

static int sy7636_wait_power_good(struct papyrus_sess *sess,int iWaitON)
{
	int i;
	int iChk;
	int iPG;
	unsigned char bFaults = 0;
	int iRet = -ETIMEDOUT;

	for (i = 0; i < 20; i++)
	{
		msleep(10);
		iChk = sy7636_get_power_status(sess,0,&iPG,&bFaults);

		if(iWaitON) {

			if (iPG) {
				if(bFaults) {
					iRet = (int)bFaults;
					printk(KERN_ERR"%s():power good, but fault occurs \"%s\"(0x%02x)\n",
						__FUNCTION__,gszFaultInfoStr[(int)bFaults]);
				}
				else {
					iRet = 0;
					//printk("%s():cnt=%d,PG=%d,%d,faults=0x%x\n",__FUNCTION__,i,gpio_get_value(sess->power_good_pin),iPG,bFaults);
				}
				break;
			}
			else {
				if(bFaults) {
					printk(KERN_ERR"%s():fault occurs \"%s\"\n",
						__FUNCTION__,gszFaultInfoStr[(int)bFaults]);
				}
			}

		}
		else {
			if (!sy7636_is_power_good(sess)) {
				//printk("%s():cnt=%d,PG=%d\n",__FUNCTION__,i,gpio_get_value(sess->power_good_pin));
				iRet = 0;
				break;
			}
		}

	}


	if(iRet<0) {
		printk(KERN_ERR"%s():wait %d times for PG(%d) timeout,cur PG=%d\n",__FUNCTION__,i,iWaitON,gpio_get_value(sess->power_good_pin));
	}

	return iRet;
}

static int sy7636_regs_init(struct papyrus_sess *sess)
{
	int iRet = -1, try_count = 0;
	int chip_VCOMmV;
	int stat = 0;
	
	pr_debug("%s(%d) \n",__func__,__LINE__);
	do {
		// write registers . 	
		stat |= SY7636_REG_WRITE(sess,VPDD_LEN);
		stat |= SY7636_REG_WRITE(sess,VEE_VP_EXT);


		stat |= SY7636_REG_WRITE(sess,VCOM_ADJ1);
		stat |= SY7636_REG_WRITE(sess,VCOM_ADJ2);

		stat |= SY7636_REG_WRITE(sess,VLDO_ADJ);
		stat |= SY7636_REG_WRITE(sess,PWRON_DLY);
		
		try_count++;
		
		if (stat) {
			pr_info("%s() some register init failed ... retry init\n", __func__);
			stat = 0;
			continue;
		}
		else {
			stat = sy7636_vcom_get(&chip_VCOMmV);
			if ((chip_VCOMmV != cfg_VCOMmV) && (cfg_VCOMmV != 0)) {
				pr_info("get vcom failed, chip VCOM %d mV, not match config VCOM %d mV ... retry init\n", chip_VCOMmV, cfg_VCOMmV);
			}
			else {
				pr_info("get vcom sucess, chip VCOM %d mV, match config VCOM %d mV ... init finish\n", chip_VCOMmV, cfg_VCOMmV);
				return 0;
			}
		}
		
		msleep(10);
		
	} while(try_count < SY7636_INIT_COUNT);

	return iRet;
}


static void sy7636_setup_timings(struct papyrus_sess *sess, struct pmic_config *config)
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

	iChk = SY7636_REG_GET(g_regSY7636,PWRON_DLY);
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
	new_reg_val = sy7636_to_reg_val(cur_reg, fld_mask, fld_val);
	
	if(cur_reg!=new_reg_val) {
		pr_info("%s(),set poweron delay=0x%x\n",__func__,new_reg_val);
		papyrus_hw_setreg(sess,PWRON_DLY,(unsigned char)new_reg_val);
	}
}

#if 0
static void papyrus_hw_send_powerup(struct papyrus_sess *sess)
{
	int stat = 0;

	// set VADJ
	stat |= papyrus_hw_setreg(sess, VLDO_ADJ, sess->vadj);
	sy7636_get_power_status(sess,0,NULL,NULL);
	// commit it, so that we can adjust vcom through "Rk_ebc_power_control_Release_v1.1" 
	//stat |= papyrus_hw_setreg(sess, VCOM_ADJ1, sess->vcom1);
	//stat |= papyrus_hw_setreg(sess, VCOM_ADJ2, sess->vcom2);

	/* switch to active mode, keep 3.3V & VEE & VDDH & VPOS & VNEG alive, 
	 * don't enable vcom buffer
	 */
	sess->enable_reg_shadow = 0x80;
	stat |= papyrus_hw_setreg(sess, OPMODE, sess->enable_reg_shadow);
	if (stat)
		pr_err("papyrus: I2C error: %d\n", stat);
	
	sy7636_wait_power_good(sess, 1);
	
	gpio_direction_output(sess->vcom_ctl_pin, sess->vcomctl_active_high);
	return;
}
#endif

static void papyrus_hw_send_powerdown(struct papyrus_sess *sess)
{
	int stat;
	
	gpio_direction_output(sess->vcom_ctl_pin, 0);

//	printk ("[%s-%d] \n", __func__, __LINE__);
	/* switch to standby mode, keep 3.3V & VEE & VDDH & VPOS & VNEG alive, 
	 * don't enable vcom buffer
	 */
	sess->enable_reg_shadow = 0;
	stat = papyrus_hw_setreg(sess, OPMODE, sess->enable_reg_shadow);

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


	papyrus_hw_power_req(pmsess, 1);
	//sy7636_EN(sess,1);

	// get temperture from SY7636 if power was on. return last temperautre if power was off.
	stat = papyrus_hw_getreg(sess, OPMODE, &tb);
	if ((0 <= stat) && (tb & 0x80)) {
		stat = papyrus_hw_getreg(sess, THERM, &tb);
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

	if(gpio_is_valid(sess->wake_up_pin))
		stat |= gpio_request(sess->wake_up_pin, "papyrus-wake_up");
	else
		stat |= 0x1;
	//pr_info("%s(%d)\n",__func__,__LINE__);
	if(gpio_is_valid(sess->vcom_ctl_pin))
		stat |= gpio_request(sess->vcom_ctl_pin, "papyrus-vcom-ctl");
	else
		stat |= 0x2;
	//pr_info("%s(%d)\n",__func__,__LINE__);
	if(gpio_is_valid(sess->power_up_pin))
		stat |= gpio_request(sess->power_up_pin, "papyrus-power-up");
	else
		stat |= 0x4;
	//pr_info("%s(%d)\n",__func__,__LINE__);
	if(1==gHasPWRALL){
		if(gpio_is_valid(sess->power_all_pin))
			stat |= gpio_request(sess->power_all_pin, "papyrus-power-all");
		else
			stat |= 0x8;
	}

	//pr_info("%s(%d)\n",__func__,__LINE__);
	if(gpio_is_valid(sess->power_good_pin)) {
		if (0 > gpio_request(sess->power_good_pin, "papyrus-power-good"))
			pr_err("PMIC%d: failed request power good gpio %d\n", pmic_id, sess->power_good_pin);
	}
	else
		stat |= 0x10;

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
	gpio_direction_output(sess->power_up_pin, sess->power_active_high);
	if (gpio_is_valid(sess->wake_up_pin))
		gpio_direction_output(sess->wake_up_pin, sess->wakeup_active_high);

	gpio_direction_input(sess->power_good_pin);
	msleep(PAPYRUS_EEPROM_DELAY_MS);

	//pr_info("%s(%d)\n",__func__,__LINE__);

	if (gpio_is_valid(sess->xon_pin)) {
		//pr_info("%s(%d)\n",__func__,__LINE__);
		gpio_direction_output(sess->xon_pin, sess->xon_active_high);
	}
	
	return 0;
	
free_gpios:
	if (gpio_is_valid(sess->wake_up_pin))
		gpio_free(sess->wake_up_pin);
	gpio_free(sess->vcom_ctl_pin);
	gpio_free(sess->power_up_pin);
	if (gpio_is_valid(sess->xon_pin)) {
		gpio_free(sess->xon_pin);
	}
	if(gpio_is_valid(sess->power_all_pin))
		gpio_free(sess->power_all_pin);
	gpio_free(sess->power_good_pin);
	pr_err("papyrus: ERROR: could not initialize I2C papyrus!\n");
	return stat;
}

int sy7636_EN(struct papyrus_sess *sess, int iEN)
{
	int iEN_Old_State;
	
	if (!gpio_is_valid(sess->power_up_pin)) {
		pr_info("%s():epdc pmic powerup pin available\n",__FUNCTION__);
		return -1;
	}
	pr_debug("%s(%d):\n",__FUNCTION__,iEN);
	
	iEN_Old_State = gpio_get_value(sess->power_up_pin);

	/*
	if (iEN_Old_State != iEN) {
		pr_debug("%s(%d):skipped the same EN status\n",__FUNCTION__,iEN);
		return iEN_Old_State;
	}
	*/
	

	if(iEN) {
		int iReInitRegs = (0==iEN_Old_State||g_regSY7636->need_reinit) ? 1:0;

		if (iEN_Old_State != iEN) {

			gpio_direction_output(sess->power_up_pin, sess->power_active_high);

			if(0==iEN_Old_State) {
				pr_debug("%s():EN 0->1 .\n",__FUNCTION__);
				mdelay(SY7636_WAKEUP0_MS);
			}
		}

		if(1==iEN && iReInitRegs) {
			g_regSY7636->need_reinit = 0;
			sy7636_regs_init(sess);	
		}
	}
	else {
		if (iEN_Old_State == iEN) {
			return iEN_Old_State;
		}
		g_regSY7636->need_reinit = 1;//need setup vcom/regs again .
		gpio_direction_output(sess->power_up_pin, !sess->power_active_high);
		if(1==iEN_Old_State) {
			pr_debug("%s():EM 1->0 .\n",__FUNCTION__);
			udelay(SY7636_PWRON_READYUS/2);
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
	sy7636_EN(sess,1);

	
	cur_reg_val = SY7636_REG_GET(g_regSY7636,OPMODE);
	bOP = (unsigned char)cur_reg_val;
	
	if (up>0) {

		if(gpio_is_valid(sess->xon_pin)) {
			hrtimer_cancel(&sess->hrt_xon_off);
			hrtimer_cancel(&sess->hrt_xon_on);
		}

		if( 0xff != g_pmic_mgr->config[sess->id].vpdd_len ) {
			if(1==up) {
				// day mode . 
				SY7636_REG_FLD_SET(g_regSY7636,VPDD_LEN,VPDD_LEN,0);
			}
			else {
				// night mode .
				SY7636_REG_FLD_SET(g_regSY7636,VPDD_LEN,VPDD_LEN,
					(unsigned char)g_pmic_mgr->config[sess->id].vpdd_len);	
			}
			SY7636_REG_WRITE(sess,VPDD_LEN);
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
			//stat |= papyrus_hw_setreg(sess, VLDO_ADJ, sess->vadj);
			sy7636_get_power_status(sess,0,&iPG,NULL);
			if (iPG) {
				pr_debug("%s() : SY7636 power on rail is ready\n", __FUNCTION__);
				break;
			}
			// commit it, so that we can adjust vcom through "Rk_ebc_power_control_Release_v1.1" 
			//stat |= papyrus_hw_setreg(sess, VCOM_ADJ1, sess->vcom1);
			//stat |= papyrus_hw_setreg(sess, VCOM_ADJ2, sess->vcom2);

			/* switch to active mode, keep 3.3V & VEE & VDDH & VPOS & VNEG alive, 
			 * don't enable vcom buffer
			 */
			fld_mask = BITFMASK(RAILS_ON) | BITFMASK(VCOM_MANUAL) | BITFMASK(RAILS_DISABLE);
			fld_val = BITFVAL(RAILS_ON,true) | BITFVAL(RAILS_DISABLE,0);
			if (gpio_is_valid(sess->vcom_ctl_pin)) {
				fld_val |= BITFVAL(VCOM_MANUAL,true);
			}
			else 
			{
				fld_val |= BITFVAL(VCOM_MANUAL,false);
			}
			new_reg_val = sy7636_to_reg_val(cur_reg_val, fld_mask, fld_val);
			pr_debug("%s() : set SY7636 OP mode ON 0x%x->0x%x \n", __FUNCTION__,cur_reg_val,new_reg_val);

			sess->enable_reg_shadow = new_reg_val;
			stat |= papyrus_hw_setreg(sess, OPMODE, sess->enable_reg_shadow);
			if (stat)
				pr_err("papyrus: I2C error: %d\n", stat);
		
			/* wait for power good */
			iRet = sy7636_wait_power_good(sess, 1);
#if 1
			if(iRet<0) {
				printk("%s power on failed %d,retry ...\n",__FUNCTION__,iTryCnt);
#if 1
				printk("reseting SY7636 EN ...\n");
				msleep(SY7636_SLEEP0_MS);
				sy7636_EN(sess, 0);
				sy7636_EN(sess, 1);
#endif
			}
			else {
				if(iRet>0) {
					printk("need re-enable SY7636 to clear faults ...\n");
					// sy7636 got faults, need re pull EN to clear faults .
					g_regSY7636->need_re_en = 1;
				}
				break;
			}
		} while (iTryCnt<=5);
#else
		papyrus_hw_send_powerup(sess);
#endif

		
	} else if( up<=0 ) {


		/* disable rail power */
		fld_mask = BITFMASK(RAILS_ON)| BITFMASK(VCOM_MANUAL) | BITFMASK(RAILS_DISABLE);
		fld_val = BITFVAL(RAILS_ON,false) | BITFVAL(RAILS_DISABLE,0);

		if (gpio_is_valid(sess->vcom_ctl_pin)) {
			fld_val |= BITFVAL(VCOM_MANUAL,true);
		}
		else 
		{
			fld_val |= BITFVAL(VCOM_MANUAL,false);
		}

		new_reg_val = sy7636_to_reg_val(cur_reg_val, fld_mask, fld_val);
		pr_debug("%s() : set SY7636 OP mode OFF 0x%x->0x%x \n", __FUNCTION__,cur_reg_val,new_reg_val);
		
		sess->enable_reg_shadow = new_reg_val;
		stat = papyrus_hw_setreg(sess, OPMODE, sess->enable_reg_shadow);
		
		/* wait for power good */
		sy7636_wait_power_good(sess, 0);

		if(gpio_is_valid(sess->xon_pin)) {
			hrtimer_cancel(&sess->hrt_xon_on);

			if(-1==up) { 
				// night mode ...

				hrtimer_start(&sess->hrt_xon_on,
					ktime_set(sess->xon_on_delay_us/1000000,(sess->xon_on_delay_us%1000000) * 1000),
					HRTIMER_MODE_REL);
			}
		}

		if(g_regSY7636->need_re_en)	{
			g_regSY7636->need_re_en = 0;
			sy7636_EN(sess, 0);
		}
		
	}
	return;
}


static bool papyrus_hw_power_ack(struct pmic_sess *pmsess)
{
	struct papyrus_sess *sess = pmsess->drvpar;
	struct papyrus_hw_state hwst;
	int st;
	int retries_left = 10;

	do {
		papyrus_hw_get_pg(sess, &hwst);

		pr_debug("hwst: tmst_val=%d, ist1=%02x, ist2=%02x, pg=%02x\n",
				hwst.tmst_value, hwst.int_status1,
				hwst.int_status2, hwst.pg_status);
		st = hwst.pg_status & 1;
		if (!st) {
			st = -1;	/* not settled yet */
			msleep(HW_GET_STATE_INTERVAL_MS);
		}
		retries_left--;
	} while ((st == -1) && retries_left);

	if ((st == -1) && !retries_left)
		pr_err("papyrus: power up/down settle error (PG = %02x)\n", hwst.pg_status);

	return !!st;
}


static void papyrus_hw_cleanup(struct papyrus_sess *sess)
{
	if (gpio_is_valid(sess->wake_up_pin))
		gpio_free(sess->wake_up_pin);
	gpio_free(sess->vcom_ctl_pin);
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
	sess->vcom1 = (PAPYRUS_MV_TO_VCOMREG(-vcom_mv) & 0x00FF);
	sess->vcom2 = ((PAPYRUS_MV_TO_VCOMREG(-vcom_mv) & 0x0100) >> 1);
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
	struct papyrus_sess *sess = pmsess->drvpar;
	sess->vadj = vadj;
	return 0;
}

static int papyrus_set_int_en1(struct pmic_sess *pmsess, uint8_t int_en1)
{
	struct papyrus_sess *sess = pmsess->drvpar;
	sess->int_en1 = int_en1;
	return 0;
}

static int papyrus_set_int_en2(struct pmic_sess *pmsess, uint8_t int_en2)
{
	struct papyrus_sess *sess = pmsess->drvpar;
	sess->int_en2 = int_en2;
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
	struct papyrus_sess *sess = pmsess->drvpar;
	sess->tmst1 = tmst1;
	return 0;
}

static int papyrus_set_tmst2(struct pmic_sess *pmsess, uint8_t tmst2)
{
	struct papyrus_sess *sess = pmsess->drvpar;
	sess->tmst2 = tmst2;
	return 0;
}

static int papyrus_vcom_switch(struct pmic_sess *pmsess, bool state)
{
	struct papyrus_sess *sess = pmsess->drvpar;
	int stat = 0;

	gpio_direction_output(sess->vcom_ctl_pin, state?1:0);
	return stat;
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


#if 0
	struct papyrus_sess *s = sess->drvpar;
	
//	printk ("[%s-%d] ...\n", __func__, __LINE__);
	
#if 0	
	if (MASTER_PMIC_ID == sess->id) {
		gpio_direction_output(s->vcom_ctl_pin, !s->vcomctl_active_high);
		gpio_direction_output(s->wake_up_pin, !s->wakeup_active_high);
		gpio_direction_output(s->power_up_pin, !s->power_active_high);
		papyrus_resume_flag = 0;
	}
#else
	
	gpio_direction_output(s->wake_up_pin, !s->wakeup_active_high);
	//gpio_direction_output(s->power_up_pin, !s->power_active_high);
#endif
	gpio_direction_output(s->power_up_pin, !s->power_active_high);
	gpio_direction_output(s->power_all_pin, 0);
	g_regSY7636->need_reinit = 1;
#endif

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
#if 0	
	if(g_regSY7636->need_reinit) 
	{
		iRet = sy7636_regs_init(s);
		if (iRet) {
			pr_info("sy7636 register init counts > 20 failed ....\n");
		}
		else {
			g_regSY7636->need_reinit = 0;
		}
	}
#endif
	
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
		if (gpio_is_valid(s->wake_up_pin))
			gpio_direction_output(s->wake_up_pin, !s->wakeup_active_high);
		gpio_direction_output(s->power_up_pin, !s->power_active_high);

		g_regSY7636->need_reinit = 1;
	}
	else {
#if 1
		//if(133==s->power_up_pin) // PE5 確定會被拉到0 .
		{
			// EN會被拉到0 , 這裡只是先行設定對應的狀態.
			gpio_direction_output(s->power_up_pin, !s->power_active_high);
			g_regSY7636->need_reinit = 1;
		}
#endif
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
	if(gpio_is_valid(s->power_all_pin))
		gpio_direction_output(s->power_all_pin, 1);
	
	if (gSleep_Mode_Suspend) {
		
		if (MASTER_PMIC_ID == sess->id) {
			if (gpio_is_valid(s->wake_up_pin))
				gpio_direction_output(s->wake_up_pin, s->wakeup_active_high);
		}
	}
	gpio_direction_output(s->power_up_pin, s->power_active_high);

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
	sess->wake_up_pin = config->wake_up_pin.gpio;
	sess->vcom_ctl_pin = config->vcom_ctl_pin.gpio;
	sess->power_up_pin = config->power_up_pin.gpio;
	sess->xon_pin = config->xon_pin.gpio;
	//pr_info("EN gpio=%d\n",sess->power_up_pin);
	sess->power_all_pin = config->power_all_pin.gpio;
	sess->power_good_pin = config->power_good_pin.gpio;
	sess->wakeup_active_high = 1;
	sess->vcomctl_active_high = 1;
	sess->power_active_high = 1;
	sess->xon_active_high = 1;

	sess->enable_reg_shadow = 0;
	sess->pmic_power_ldo = regulator_get(NULL, (const char *)config->pmic_power_name);
	if (IS_ERR(sess->pmic_power_ldo)) {
		pr_err("cannot get pmic ldo(%s)\n", config->pmic_power_name);
	} /*else {
		//regulator_set_voltage(sess->pmic_power_ldo, 3300*1000, 3300*1000);
		regulator_enable(sess->pmic_power_ldo);
	}*/
	 
	if (pmsess->v3p3off_time_ms == -1)
		sess->v3p3off_time_ms = PAPYRUS_V3P3OFF_DELAY_MS;
	else
		sess->v3p3off_time_ms = pmsess->v3p3off_time_ms;

	//printk("%s(%d)\n",__func__,__LINE__);	
	sy7636_power_on();

	//printk("%s(%d)\n",__func__,__LINE__);	
	papyrus_hw_arg_init(sess);

	pmsess->drvpar = sess;

	if (MASTER_PMIC_ID == pmsess->id) {		
		//printk("%s(%d)\n",__func__,__LINE__);	
		stat = papyrus_hw_init(sess, pmsess->id);
		if (stat)
			goto free_sess;

		for (id = 0; id < MAX_PMIC_NUM; id++) {
			if (1 != g_pmic_mgr->config[id].used_flag) {
				continue;
			}

			//printk("%s(%d)\n",__func__,__LINE__);	
			hrtimer_init(&sess->hrt_xon_on,CLOCK_MONOTONIC,HRTIMER_MODE_REL);
			sess->hrt_xon_on.function = _hrtint_xon_on;
			hrtimer_init(&sess->hrt_xon_off,CLOCK_MONOTONIC,HRTIMER_MODE_REL);
			sess->hrt_xon_off.function = _hrtint_xon_off;
			
			tmp_pmsess = &(g_pmic_mgr->sess[id]);
			tmp_sess = (struct papyrus_sess *)tmp_pmsess->drvpar;

			sess->power_good_pin = config->power_good_pin.gpio;
			sess->xon_off_delay_us = config->xon_off_delay_us;
			sess->xon_off_day_delay_us = config->xon_off_day_delay_us;
			sess->xon_on_delay_us = config->xon_on_delay_us;
			sess->xon_on_1st_delay_us = config->xon_on_1st_delay_us;

#if 1
			// reset default regiters value . 
			SY7636_REG_SET(g_regSY7636,OPMODE,0);
			SY7636_REG_SET(g_regSY7636,VCOM_ADJ1,0x7d);
			SY7636_REG_SET(g_regSY7636,VCOM_ADJ2,0x04);
			SY7636_REG_SET(g_regSY7636,VLDO_ADJ,0x66);
			SY7636_REG_SET(g_regSY7636,VPDD_LEN,0x07);
			SY7636_REG_SET(g_regSY7636,VEE_VP_EXT,0x26);
			SY7636_REG_SET(g_regSY7636,PWRON_DLY,0xaa);
			SY7636_REG_SET(g_regSY7636,FAULTFLAGS,0);
			SY7636_REG_SET(g_regSY7636,THERM,0);

#ifdef LIGHTNESS_MODE_PARAMS_FROM_DTS //[
			// lightness mode [
			
			SY7636_REG_FLD_SET(g_regSY7636,OPMODE,LIGHTNESS,1);	
			if( 0xff == g_pmic_mgr->config[id].vpdd_len ) {
				SY7636_REG_FLD_SET(g_regSY7636,VPDD_LEN,VPDD_LEN,0);	
			}
			else {
				SY7636_REG_FLD_SET(g_regSY7636,VPDD_LEN,VPDD_LEN,
						(unsigned char)g_pmic_mgr->config[id].vpdd_len);	
			}

			if( 0xff == g_pmic_mgr->config[id].vp_ext ) {
				SY7636_REG_FLD_SET(g_regSY7636,VEE_VP_EXT,VP_EXT,0x1);	
			}
			else {
				SY7636_REG_FLD_SET(g_regSY7636,VEE_VP_EXT,VP_EXT,
						(unsigned char)g_pmic_mgr->config[id].vp_ext);	
			}

			if( 0xff == g_pmic_mgr->config[id].vee_ext ) {
				SY7636_REG_FLD_SET(g_regSY7636,VEE_VP_EXT,VEE_EXT,0);	
			}
			else {
				SY7636_REG_FLD_SET(g_regSY7636,VEE_VP_EXT,VEE_EXT,
						(unsigned char)g_pmic_mgr->config[id].vee_ext);	
			}

			
			if( 0xff == g_pmic_mgr->config[id].vddh_ext ) {
				SY7636_REG_FLD_SET(g_regSY7636,VCOM_ADJ2,VDDH_EXT,0);	
			}
			else {
				SY7636_REG_FLD_SET(g_regSY7636,VCOM_ADJ2,VDDH_EXT,
						(unsigned char)g_pmic_mgr->config[id].vddh_ext);	
			}

			if( 0xff == g_pmic_mgr->config[id].vldo ) {
				SY7636_REG_FLD_SET(g_regSY7636,VLDO_ADJ,VLDO_ADJ,0x3);
			}
			else {
				SY7636_REG_FLD_SET(g_regSY7636,VLDO_ADJ,VLDO_ADJ,
						(unsigned char)g_pmic_mgr->config[id].vldo);
			}

			if( 0xff == g_pmic_mgr->config[id].vpdd ) {
				SY7636_REG_FLD_SET(g_regSY7636,VLDO_ADJ,VPDD_ADJ,0x6);
			}
			else {
				SY7636_REG_FLD_SET(g_regSY7636,VLDO_ADJ,VPDD_ADJ,
						(unsigned char)g_pmic_mgr->config[id].vpdd);
			}
#endif //] LIGHTNESS_MODE_PARAMS_FROM_DTS
			//] lightness mode 
			stat |= sy7636_regs_init(tmp_sess);
#else		
			stat |= papyrus_hw_setreg(tmp_sess,VPDD_LEN,0x07);
			stat |= papyrus_hw_setreg(tmp_sess,VEE_VP_EXT,0x26);

			stat |= papyrus_hw_setreg(tmp_sess,VCOM_ADJ1,0x7d);
			stat |= papyrus_hw_setreg(tmp_sess,VCOM_ADJ2,0x04);

			stat |= papyrus_hw_setreg(tmp_sess,VLDO_ADJ,0x66);
			stat |= papyrus_hw_setreg(tmp_sess,PWRON_DLY,0xaa);
#endif

			if (stat)
				goto free_sess;
		}
	}
	
	sy7636_setup_timings(sess, config);// power on delay timings setup .
	
	sy7636_power_down();

	return 0;

free_sess:
	if (sess)
		kfree(sess);
	
	sy7636_power_down();
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

const struct pmic_driver pmic_driver_sy7636_i2c = {
	.id = "sy7636-i2c",

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

static int sy7636_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	int ret = -1;
	PMIC_ID pmic_id = MASTER_PMIC_ID;
	struct pmic_sess *sess;
	struct pmic_config *config;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		pr_err("I2C check functionality failed\n");
		return -ENODEV;
	}
	
	g_regSY7636 = kzalloc(sizeof(struct sy7636), GFP_KERNEL);
	if (!g_regSY7636)
		return -ENOMEM;

	sess = &(g_pmic_mgr->sess[pmic_id]);
	config = &(g_pmic_mgr->config[pmic_id]);
	
	ret = papyrus_probe(sess, config, client);
	if(0 != ret) {
		pr_err("hw_init master pmic failed.");
		return -ENODEV;
	}

	papyrus_resume_flag = 1;
	sess->is_inited = 1;
	
	pr_info("hw_init master pmic ok\n");
	
	return 0;

}

static int sy7636_slave_probe(struct i2c_client *client, const struct i2c_device_id *id)
{	
	int ret = -1;
	PMIC_ID pmic_id = SLAVE_PMIC_ID;
	struct pmic_sess *sess;
	struct pmic_config *config;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		pr_err("I2C check functionality failed\n");
		return -ENODEV;
	}

	sess = &(g_pmic_mgr->sess[pmic_id]);
	config = &(g_pmic_mgr->config[pmic_id]);
	
	ret = papyrus_probe(sess, config, client);
	if(0 != ret) {
		pr_err("hw_init slave pmic failed.");
		return -ENODEV;
	}

	papyrus_resume_flag = 1;
	sess->is_inited = 1;
	
	pr_info("hw_init slave pmic ok\n");
	
	return 0;
}


static int sy7636_remove(struct i2c_client *client)
{	
	struct pmic_sess *pmic_sess = &(g_pmic_mgr->sess[MASTER_PMIC_ID]);
	struct papyrus_sess *sess = NULL;

	sess = (struct papyrus_sess *)pmic_sess->drvpar;
	pmic_driver_sy7636_i2c.hw_cleanup(pmic_sess);
	
	if (!IS_ERR(sess->pmic_power_ldo)) {
		regulator_disable(sess->pmic_power_ldo);
		regulator_put(sess->pmic_power_ldo);
	}
	
	if (sess)
		kfree(sess);
	
	pmic_sess->drvpar = NULL;
	
	return 0;
}

static int sy7636_slave_remove(struct i2c_client *client)
{
	struct pmic_sess *pmic_sess = &(g_pmic_mgr->sess[SLAVE_PMIC_ID]);
	struct papyrus_sess *sess = NULL;

	if (1 != g_pmic_mgr->config[SLAVE_PMIC_ID].used_flag) {
		return;
	}

	sess = (struct papyrus_sess *)pmic_sess->drvpar;
	pmic_driver_sy7636_i2c.hw_cleanup(pmic_sess);
	
	if (!IS_ERR(sess->pmic_power_ldo)) {
		regulator_disable(sess->pmic_power_ldo);
		regulator_put(sess->pmic_power_ldo);
	}

	if (sess)
		kfree(sess);
	
	pmic_sess->drvpar = NULL;
	
	return 0;
}

static int sy7636_suspend_late(struct device *dev)
{
	return pmic_driver_sy7636_i2c.hw_pm_sleep_late(&g_pmic_mgr->sess[MASTER_PMIC_ID]);
}

static int sy7636_suspend(struct device *dev)
{
	return pmic_driver_sy7636_i2c.hw_pm_sleep(&g_pmic_mgr->sess[MASTER_PMIC_ID]);
}

static int sy7636_resume_early(struct device *dev)
{
	return pmic_driver_sy7636_i2c.hw_pm_resume_early(&g_pmic_mgr->sess[MASTER_PMIC_ID]);
}

static int sy7636_resume(struct device *dev)
{
	return pmic_driver_sy7636_i2c.hw_pm_resume(&g_pmic_mgr->sess[MASTER_PMIC_ID]);
}

static int sy7636_slave_suspend(struct device *dev)
{
	return pmic_driver_sy7636_i2c.hw_pm_sleep(&g_pmic_mgr->sess[SLAVE_PMIC_ID]);
}
static int sy7636_slave_resume(struct device *dev)
{
	return pmic_driver_sy7636_i2c.hw_pm_resume(&g_pmic_mgr->sess[SLAVE_PMIC_ID]);
}

static int sy7636_i2c_detect(struct i2c_client *client, struct i2c_board_info *info)
{
	struct i2c_adapter *adapter = client->adapter;

	if(g_pmic_mgr->config[MASTER_PMIC_ID].twi_id == adapter->nr) {
		strlcpy(info->type, TPS65185_I2C_NAME, I2C_NAME_SIZE);
		
		pr_info("%s: Detected chip %s at adapter %d, address 0x%02x\n",
			 __func__, TPS65185_I2C_NAME, i2c_adapter_id(adapter), client->addr);
		return 0;
	}
	
	return -ENODEV;	
}

static int sy7636_slave_i2c_detect(struct i2c_client *client, struct i2c_board_info *info)
{
	struct i2c_adapter *adapter = client->adapter;

	if(g_pmic_mgr->config[SLAVE_PMIC_ID].twi_id == adapter->nr) {
		strlcpy(info->type, TPS65185_SLAVE_I2C_NAME, I2C_NAME_SIZE);
		
		pr_info("%s: Detected chip %s at adapter %d, address 0x%02x\n",
			 __func__, TPS65185_SLAVE_I2C_NAME, i2c_adapter_id(adapter), client->addr);
		return 0;
	}
	
	return -ENODEV;
}

static int sy7636_get_sys_param(void)
{
	return 0;
}

static const struct i2c_device_id sy7636_id[] = {
	{ TPS65185_I2C_NAME, 0 },
	{ }
};


static const struct i2c_device_id sy7636_slave_id[] = {
	{ TPS65185_SLAVE_I2C_NAME, 0 },
	{ }
};

static const struct dev_pm_ops sy7636_pm_ops = {
	.suspend = sy7636_suspend,
	.suspend_late = sy7636_suspend_late,
	.resume = sy7636_resume,
	.resume_early = sy7636_resume_early,
};

static const struct dev_pm_ops sy7636_slave_pm_ops = {
	.suspend = sy7636_slave_suspend,
	.resume = sy7636_slave_resume,
};


MODULE_DEVICE_TABLE(i2c, sy7636_id);

static struct i2c_driver sy7636_driver = {
	.class = I2C_CLASS_HWMON,
	.probe	= sy7636_probe,
	.remove 	= sy7636_remove,
	.id_table	= sy7636_id,
	.detect   = sy7636_i2c_detect,
	.driver = {
		.name	  = TPS65185_I2C_NAME,
		.owner	  = THIS_MODULE,
		.pm             = &sy7636_pm_ops,
	},
	.address_list   = normal_i2c,
};

//MODULE_DEVICE_TABLE(i2c, sy7636_slave_id);

static struct i2c_driver sy7636_slave_driver = {
	.class = I2C_CLASS_HWMON,
	.probe	= sy7636_slave_probe,
	.remove 	= sy7636_slave_remove,
	.id_table	= sy7636_slave_id,
	.detect   = sy7636_slave_i2c_detect,
	.driver = {
		.name	  = TPS65185_SLAVE_I2C_NAME,
		.owner	  = THIS_MODULE,
		.pm             = &sy7636_slave_pm_ops,
	},
	.address_list   = normal_slave_i2c,
};

static int sy7636_get_config(struct pmic_config *config, int i)
{
	char *pmic_node_array[MAX_PMIC_NUM] = {"sy7636", "sy7636_slave"};
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
	if (0 != of_property_read_string(np, "sy7636_name", &str))
		pr_err("PMIC%d: get sy7636_name fail\n", i);
	else {
		memcpy((void *)(config->name), str, strlen(str) + 1);
		pr_info("PMIC%d: sy7636_name = %s\n", i, config->name);
	}

	if (0 != of_property_read_u32_array(np, "sy7636_twi_id", &(config->twi_id), 1))
		pr_err("PMIC%d: get sy7636_twi_id fail\n", i);
	else
		pr_info("PMIC%d: sy7636_twi_id = %d\n", i, config->twi_id);

	if (0 != of_property_read_u32_array(np, "sy7636_twi_addr", &(config->twi_addr), 1))
		pr_err("PMIC%d: get sy7636_twi_addr fail\n", i);
	else
		pr_info("PMIC%d: sy7636_twi_addr = 0x%x\n", i, config->twi_addr);
	
	config->wake_up_pin.gpio = of_get_named_gpio_flags(np, "sy7636_wakeup", 0, (enum of_gpio_flags *)(&(config->wake_up_pin)));
	if (!gpio_is_valid(config->wake_up_pin.gpio))
		pr_err("PMIC%d: sy7636_wakeup is invalid. \n", i );
	else
		pr_info("PMIC%d: sy7636_wakeup = %d\n", i, config->wake_up_pin.gpio);

	config->vcom_ctl_pin.gpio = of_get_named_gpio_flags(np, "sy7636_vcom", 0, (enum of_gpio_flags *)(&(config->vcom_ctl_pin)));
	if (!gpio_is_valid(config->vcom_ctl_pin.gpio))
		pr_err("PMIC%d: sy7636_vcom is invalid. \n",i );	
	else
		pr_info("PMIC%d: sy7636_vcom = %d\n", i, config->vcom_ctl_pin.gpio);

	config->power_up_pin.gpio = of_get_named_gpio_flags(np, "sy7636_powerup", 0, (enum of_gpio_flags *)(&(config->power_up_pin)));
	if (!gpio_is_valid(config->power_up_pin.gpio))
		pr_err("PMIC%d: sy7636_powerup is invalid. \n",i );	
	else
		pr_info("PMIC%d: sy7636_powerup = %d\n", i, config->power_up_pin.gpio);

//#ifdef XON_TIMING_ENABLE //[
	config->xon_pin.gpio = of_get_named_gpio_flags(np, "xon", 0, (enum of_gpio_flags *)(&(config->xon_pin)));
	if (!gpio_is_valid(config->xon_pin.gpio))
		pr_err("PMIC%d: xon is invalid. \n",i );	
	else
		pr_info("PMIC%d: xon = %d\n", i, config->xon_pin.gpio);
//#else //]!XON_TIMING_ENABLE 
//	pr_info("PMIC%d: xon disabled \n", i, config->xon_pin.gpio);
//	config->xon_pin.gpio = INVALID_GPIO;
//#endif //] XON_TIMING_ENABLE

	config->power_good_pin.gpio = of_get_named_gpio_flags(np, "sy7636_powergood", 0, (enum of_gpio_flags *)(&(config->power_good_pin)));
	if (!gpio_is_valid(config->power_good_pin.gpio))
		pr_err("PMIC%d: sy7636_powergood is invalid. \n",i );
	else
		pr_info("PMIC%d: sy7636_powergood = %d\n", i, config->power_good_pin.gpio);

	config->int_pin.gpio = of_get_named_gpio_flags(np, "sy7636_int", 0, (enum of_gpio_flags *)(&(config->int_pin)));
	if (!gpio_is_valid(config->int_pin.gpio))
		pr_err("PMIC%d: sy7636_int is invalid. \n",i );
	else
		pr_info("PMIC%d: sy7636_int = %d\n", i, config->int_pin.gpio);

	config->power_all_pin.gpio = of_get_named_gpio_flags(np, "sy7636_pwrall", 0, (enum of_gpio_flags *)(&(config->power_all_pin)));
	if (!gpio_is_valid(config->power_all_pin.gpio)) {
		pr_err("PMIC%d: sy7636_powerall is invalid. \n",i );
		gHasPWRALL = 0;
	}
	else{
		gHasPWRALL = 1 ;
		pr_info("PMIC%d: sy7636_powerall = %d\n", i, config->power_all_pin.gpio);
	}

	memset(config->pmic_power_name, 0, sizeof(config->pmic_power_name));
	if (0 != of_property_read_string(np, "sy7636_ldo", &str))
		pr_err("PMIC%d: get sy7636_ldo fail\n", i);
	else {
		memcpy((void *)(config->pmic_power_name), str, strlen(str) + 1);
		pr_info("PMIC%d: sy7636_ldo = %s\n", i, config->pmic_power_name);
	}

	if (0 != of_property_read_u32_array(np, "sy7636_on_delay1", &(config->on_delay1), 1)) {
		config->on_delay1 = 0xff;
		pr_err("PMIC%d: get sy7636_on_delay1 fail\n", i);
	}
	else {
		pr_info("PMIC%d: sy7636_on_delay1 = %d\n", i, config->on_delay1);
	}
	
	if (0 != of_property_read_u32_array(np, "sy7636_on_delay2", &(config->on_delay2), 1)) {
		config->on_delay2 = 0xff;
		pr_err("PMIC%d: get sy7636_on_delay2 fail\n", i);
	}
	else {
		pr_info("PMIC%d: sy7636_on_delay2 = %d\n", i, config->on_delay2);
	}
	
	if (0 != of_property_read_u32_array(np, "sy7636_on_delay3", &(config->on_delay3), 1)) {
		config->on_delay3 = 0xff;
		pr_err("PMIC%d: get sy7636_on_delay3 fail\n", i);
	}
	else {
		pr_info("PMIC%d: sy7636_on_delay3 = %d\n", i, config->on_delay3);
	}
	
	if (0 != of_property_read_u32_array(np, "sy7636_on_delay4", &(config->on_delay4), 1)) {
		config->on_delay4 = 0xff;
		pr_err("PMIC%d: get sy7636_on_delay4 fail\n", i);
	}
	else {
		pr_info("PMIC%d: sy7636_on_delay4 = %d\n", i, config->on_delay4);
	}
	
	if (0 != of_property_read_u32_array(np, "vldo", &(config->vldo), 1)) {
		config->vldo = 0xff;
		pr_err("PMIC%d: get vldo fail\n", i);
	}
	else {
		pr_info("PMIC%d: vldo = %d\n", i, config->vldo);
	}


	if (0 != of_property_read_u32_array(np, "vpdd", &(config->vpdd), 1)) {
		config->vpdd = 0xff;
		pr_err("PMIC%d: get vpdd fail\n", i);
	}
	else {
		pr_info("PMIC%d: vpdd = %d\n", i, config->vpdd);
	}


	if (0 != of_property_read_u32_array(np, "vddh_ext", &(config->vddh_ext), 1)) {
		config->vddh_ext = 0xff;
		pr_err("PMIC%d: get vddh_ext fail\n", i);
	}
	else {
		pr_info("PMIC%d: vddh_ext = %d\n", i, config->vddh_ext);
	}


	if (0 != of_property_read_u32_array(np, "vpdd_len", &(config->vpdd_len), 1)) {
		config->vpdd_len = 0xff;
		pr_err("PMIC%d: get vpdd_len fail\n", i);
	}
	else {
		pr_info("PMIC%d: vpdd_len = %d\n", i, config->vpdd_len);
	}

	if (0 != of_property_read_u32_array(np, "vp_ext", &(config->vp_ext), 1)) {
		config->vp_ext = 0xff;
		pr_err("PMIC%d: get vp_ext fail\n", i);
	}
	else {
		pr_info("PMIC%d: vp_ext = %d\n", i, config->vp_ext);
	}


	if (0 != of_property_read_u32_array(np, "vee_ext", &(config->vee_ext), 1)) {
		config->vee_ext = 0xff;
		pr_err("PMIC%d: get vee_ext fail\n", i);
	}
	else {
		pr_info("PMIC%d: vee_ext = %d\n", i, config->vee_ext);
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

static int sy7636_manager_init(void)
{
	int i = 0;
	int ret = -1;
	
	g_pmic_mgr = kmalloc(sizeof(*g_pmic_mgr), GFP_KERNEL);
	if (NULL == g_pmic_mgr) {
		printk(KERN_ERR "%s: fail to alloc memory\n", __func__);
		return -1;
	}

	memset(g_pmic_mgr, 0, sizeof(*g_pmic_mgr));
	for (i = 0; i < MAX_PMIC_NUM; i++) {
		g_pmic_mgr->sess[i].id = i;
		ret = sy7636_get_config(&(g_pmic_mgr->config[i]), i);
		if (ret) {
			if (i!= 0)
				return 0;
			pr_err("PMIC%d: get config fail, ret=%d\n", i, ret);
			break;
		}
	}
	return ret;
}

static void sy7636_manager_exit(void)
{
	if (g_pmic_mgr)
		kfree(g_pmic_mgr);
	return;
}

static int __init sy7636_init(void)
{
	int ret = -1;
	
	ret = sy7636_manager_init();
	if (ret) {
		pr_err("pmic manager init fail, ret=%d", ret);
		return -1;
	}
	
	if (gptHWCFG->m_val.bDisplayCtrl != 0x18)
		return -1;

	if (g_pmic_mgr->config[SLAVE_PMIC_ID].used_flag) {
		ret = i2c_add_driver(&sy7636_slave_driver);
		if (ret) {
			pr_err("fail to init slave pmic, ret=%d\n", ret);
			return -2;
		}
		pr_info("init slave pmic ok\n");
	}

	if (g_pmic_mgr->config[MASTER_PMIC_ID].used_flag) {
		ret = i2c_add_driver(&sy7636_driver);
		if (ret) {
			pr_err("fail to init master pmic, ret=%d\n", ret);
		} else {
			pr_info("init master pmic ok\n");
		}
	}
	return ret;
}

static void __exit sy7636_exit(void)
{
	if (g_pmic_mgr->config[SLAVE_PMIC_ID].used_flag) {
		i2c_del_driver(&sy7636_slave_driver);
	}

	if (g_pmic_mgr->config[MASTER_PMIC_ID].used_flag) {
		i2c_del_driver(&sy7636_driver);
	}

	sy7636_manager_exit();
	
	return;
}

fs_initcall(sy7636_init);
module_exit(sy7636_exit);

MODULE_DESCRIPTION("sy7636 pmic");
MODULE_LICENSE("GPL");

int sy7636_vcom_set(int vcom_mv)
{
	struct pmic_sess *master_pmsess = NULL, *tmp_pmsess = NULL;
	struct papyrus_sess *master_sess = NULL, *tmp_sess = NULL;
	PMIC_ID id = 0;
	uint8_t rev_val = 0;
	int stat = 0;
	int read_vcom_mv = 0;
	int cnt = 20;

	printk("%s: vcom setting %d\n", __func__, vcom_mv);
	
	if(1 != g_pmic_mgr->sess[MASTER_PMIC_ID].is_inited) {
		//pr_err("master pmic has not initial yet\n");
		return -2;
	}

	master_pmsess = &(g_pmic_mgr->sess[MASTER_PMIC_ID]);

	master_sess = (struct papyrus_sess *)master_pmsess->drvpar;
	//wakeup 65185, sleep mode --> standby mode
	if(gpio_is_valid(master_sess->power_all_pin))
		gpio_direction_output(master_sess->power_all_pin, 1);
	sy7636_EN(master_sess,1);
	//gpio_direction_output(master_sess->power_up_pin, 1);
	//msleep(3);
	//gpio_direction_output(master_sess->wake_up_pin, 1);
	//msleep(10);

	//stat |= papyrus_hw_setreg(master_sess, OPMODE, 0);

	for (id = 0; id < MAX_PMIC_NUM; id++) {
		if ((1 != g_pmic_mgr->config[id].used_flag) || (1 != g_pmic_mgr->sess[id].is_inited)) {
			continue;
		}

		tmp_pmsess = &(g_pmic_mgr->sess[id]);
		tmp_sess = (struct papyrus_sess *)tmp_pmsess->drvpar;
		
		//papyrus_hw_power_req ((struct pmic_sess *)tmp_pmsess, 1);
		// Set vcom voltage
		pmic_driver_sy7636_i2c.set_vcom_voltage((struct pmic_sess *)tmp_pmsess,vcom_mv);
		stat |= papyrus_hw_setreg(tmp_sess, VCOM_ADJ1,tmp_sess->vcom1);
		stat |= papyrus_hw_setreg(tmp_sess, VCOM_ADJ2,tmp_sess->vcom2);
		pr_debug("sess->vcom1 = 0x%x sess->vcom2 = 0x%x\n",tmp_sess->vcom1,tmp_sess->vcom2);
		
		stat |= papyrus_hw_setreg(tmp_sess, VCOM_ADJ1,tmp_sess->vcom1);
		// ligntness mode
		//stat |= papyrus_hw_setreg(tmp_sess, VPDD_LEN,7);		
		//stat |= papyrus_hw_setreg(tmp_sess, VEE_VP_EXT,0x26);
		
		//papyrus_hw_power_req ((struct pmic_sess *)tmp_pmsess, 0);
	}

	if (stat)
		pr_err("papyrus: I2C error: %d\n", stat);

	return 0;
}

int tps65185_vcom_set(int vcom_mv)
{
	cfg_VCOMmV = vcom_mv;
	return sy7636_vcom_set(vcom_mv);
}
EXPORT_SYMBOL(tps65185_vcom_set);

int sy7636_vcom_get(int *vcom)
{
	uint8_t reg_val;
	uint8_t vcom_reg_val;
	unsigned short wTemp;
	int iVCOMmV;
	
	struct pmic_sess *master_pmsess = NULL, *tmp_pmsess = NULL;
	struct papyrus_sess *master_sess = NULL, *tmp_sess = NULL;
	PMIC_ID id = 0;
	int stat = 0;

	if(1 != g_pmic_mgr->sess[MASTER_PMIC_ID].is_inited) {
		//pr_err("master pmic has not initial yet\n");
		return -2;
	}

	pr_debug("%s(%d) \n",__func__,__LINE__);
	master_pmsess = &(g_pmic_mgr->sess[MASTER_PMIC_ID]);
	master_sess = (struct papyrus_sess *)master_pmsess->drvpar;

	//wakeup 65185, sleep mode --> standby mode
	if(gpio_is_valid(master_sess->power_all_pin))
		gpio_direction_output(master_sess->power_all_pin, 1);
	//gpio_direction_output(master_sess->power_up_pin, 1);
	//msleep(3);
	//gpio_direction_output(master_sess->wake_up_pin, 1);
	//msleep(10);
	sy7636_EN(master_sess,1);

	//stat |= papyrus_hw_setreg(master_sess, OPMODE, 0);

	for (id = 0; id < MAX_PMIC_NUM; id++) {
		if ((1 != g_pmic_mgr->config[id].used_flag) || (1 != g_pmic_mgr->sess[id].is_inited)) {
			continue;
		}

		tmp_pmsess = &(g_pmic_mgr->sess[id]);
		tmp_sess = (struct papyrus_sess *)tmp_pmsess->drvpar;
		
		//papyrus_hw_power_req ((struct pmic_sess *)tmp_pmsess, 1);
		// Get vcom voltage
		stat |= papyrus_hw_getreg(tmp_sess, VCOM_ADJ1, &vcom_reg_val);
		stat |= papyrus_hw_getreg(tmp_sess, VCOM_ADJ2, &reg_val);
		
		//papyrus_hw_power_req ((struct pmic_sess *)tmp_pmsess, 0);
	}

	if (stat) {
		pr_err("papyrus: I2C error: %d\n", stat);
		return -1;
	}
	
	wTemp = vcom_reg_val;
	if(reg_val&0x80) {
		wTemp |= 0x100;
	}
	else {
		wTemp &= ~0x100;
	}
	iVCOMmV = -(wTemp*10);
	
    memcpy(vcom, &iVCOMmV, sizeof(int));
    
	return 0;
}

int tps65185_vcom_get(int *vcom)
{
	return sy7636_vcom_get(vcom);
}
EXPORT_SYMBOL(tps65185_vcom_get);

int sy7636_power_status(void)
{
	struct pmic_sess *master_pmsess = NULL, *tmp_pmsess = NULL;
	struct papyrus_sess *master_sess = NULL, *tmp_sess = NULL;
	PMIC_ID id = 0;
	int ret = -1;

	int status = -1;

	pr_debug("%s()\n", __func__);
	
	if(!g_pmic_mgr->sess[id].is_inited)
		return status;

	for (id == 0; id < MAX_PMIC_NUM; id++) {
		if ((1 != g_pmic_mgr->config[id].used_flag) || (1 != g_pmic_mgr->sess[id].is_inited)) {
			continue;
		}

		tmp_pmsess = &(g_pmic_mgr->sess[id]);
		tmp_sess = (struct papyrus_sess *)tmp_pmsess->drvpar;

		//status = pmic_driver_sy7636_i2c.hw_power_ack((struct pmic_sess *)tmp_pmsess);
		if (!sy7636_is_power_good(tmp_sess))
			status = 0;
		else
			status = 1;
	}

	return status;
}
EXPORT_SYMBOL(sy7636_power_status);


int sy7636_power_on_ex(unsigned long dwFlags)
{
	struct pmic_sess *master_pmsess = NULL, *tmp_pmsess = NULL;
	struct papyrus_sess *master_sess = NULL, *tmp_sess = NULL;
	PMIC_ID id = 0;
	int ret = -1;

	pr_debug("%s()\n",__func__);

	for (id = 0; id < MAX_PMIC_NUM; id++) {
		if ((1 != g_pmic_mgr->config[id].used_flag) || (1 != g_pmic_mgr->sess[id].is_inited)) {
			continue;
		}

		tmp_pmsess = &(g_pmic_mgr->sess[id]);
		tmp_sess = (struct papyrus_sess *)tmp_pmsess->drvpar;
	
		pmic_driver_sy7636_i2c.hw_power_req((struct pmic_sess *)tmp_pmsess,
				(dwFlags&EBC_PWR_FLAG_XON)?2:1);
	}

	return 0;
}
int sy7636_power_on(void)
{
	return sy7636_power_on_ex(EBC_PWR_FLAG_DEFAULT);
}
int sy7636_power_down_ex(unsigned long dwFlags)
{
	struct pmic_sess *master_pmsess = NULL, *tmp_pmsess = NULL;
	struct papyrus_sess *master_sess = NULL, *tmp_sess = NULL;
	PMIC_ID id = 0;
	int ret = -1;
	
	pr_debug("%s()\n",__func__);
	for (id = 0; id < MAX_PMIC_NUM; id++) {
		if ((1 != g_pmic_mgr->config[id].used_flag) || (1 != g_pmic_mgr->sess[id].is_inited)) {
			continue;
		}

		tmp_pmsess = &(g_pmic_mgr->sess[id]);
		tmp_sess = (struct papyrus_sess *)tmp_pmsess->drvpar;
		pmic_driver_sy7636_i2c.hw_power_req((struct pmic_sess *)tmp_pmsess,
				(dwFlags&EBC_PWR_FLAG_XON)?-1:0);
	}
	
	return 0;
}
int sy7636_power_down(void)
{
	return sy7636_power_down_ex(EBC_PWR_FLAG_DEFAULT);
}
int sy7636_vcom_enable(void)
{
	struct pmic_sess *pmsess = NULL;
	struct papyrus_sess *sess = NULL;
	PMIC_ID id = 0;
	
	for (id = 0; id < MAX_PMIC_NUM; id++) {
		if ((1 != g_pmic_mgr->config[id].used_flag) || (1 != g_pmic_mgr->sess[id].is_inited)) {
			continue;
		}

		pmsess = &(g_pmic_mgr->sess[id]);
		sess = (struct papyrus_sess *)pmsess->drvpar;
	
		/* enable vcom control */
		if (gpio_is_valid(sess->vcom_ctl_pin))
		{
			gpio_direction_output(sess->vcom_ctl_pin, sess->vcomctl_active_high);
		}
		else {
			printk("%s vcom controlled autimatically \n",__FUNCTION__);
		}
	}
		
	return 0;
}
int sy7636_vcom_disable_ex(unsigned long dwFlags)
{
	struct pmic_sess *pmsess = NULL;
	struct papyrus_sess *sess = NULL;
	PMIC_ID id = 0;
	
	for (id = 0; id < MAX_PMIC_NUM; id++) {
		if ((1 != g_pmic_mgr->config[id].used_flag) || (1 != g_pmic_mgr->sess[id].is_inited)) {
			continue;
		}

		pmsess = &(g_pmic_mgr->sess[id]);
		sess = (struct papyrus_sess *)pmsess->drvpar;
	
		if (gpio_is_valid(sess->vcom_ctl_pin)) {
			/* wait data trans */
			//usleep_range(SY7636_VCOM_OFF_US, SY7636_VCOM_OFF_US*2);
			udelay(SY7636_VCOM_OFF_US*2);
		
			/* disable vcom control */
			gpio_direction_output(sess->vcom_ctl_pin, !sess->vcomctl_active_high);

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

			
		}
		else {
			printk("%s vcom controlled autimatically \n",__FUNCTION__);
		}
#ifdef XON_TIMING_ENABLE //[
		/*
		if(dwFlags&EBC_PWR_FLAG_XON) {
			if(gpio_is_valid(sess->xon_pin)) {
				msleep(2);
				gpio_direction_output(sess->xon_pin,!sess->xon_active_high);
				pr_debug("%s() : XON=%d\n",__func__,!sess->xon_active_high);
			}
		}
		*/
#endif //] XON_TIMING_ENABLE
		
	}
	
	return 0;
}
int sy7636_vcom_disable(void) {
	return sy7636_vcom_disable_ex(EBC_PWR_FLAG_DEFAULT);
}

int sy7636_temperature_get(int *temp)
{
	struct pmic_sess *master_pmsess = NULL;

	master_pmsess = &g_pmic_mgr->sess[MASTER_PMIC_ID];
	if(master_pmsess->is_inited)
		return pmic_driver_sy7636_i2c.hw_read_temperature(master_pmsess,temp);
	else
		return -1;
}
int tps65185_temperature_get(int *temp)
{
	return sy7636_temperature_get(temp);
}
EXPORT_SYMBOL(tps65185_temperature_get);

int register_ebc_pwr_ops(struct ebc_pwr_ops *ops)
{
	if (g_pmic_mgr->sess[MASTER_PMIC_ID].is_inited) {
		ops->power_on = sy7636_power_on;
		ops->power_on_ex = sy7636_power_on_ex;
		ops->power_down = sy7636_power_down;
		ops->power_down_ex = sy7636_power_down_ex;
//		ops->set_vcom_value = sy7636_vcom_set;
		ops->vcom_enable = sy7636_vcom_enable;
		ops->vcom_disable = sy7636_vcom_disable;
		ops->vcom_disable_ex = sy7636_vcom_disable_ex;
		ops->power_status = sy7636_power_status;
	} else
		return -1;
	
	return 0;
}
EXPORT_SYMBOL(register_ebc_pwr_ops);


#ifdef CONFIG_EPD_TPS65185_SENSOR
int register_ebc_temp_ops(struct ebc_temperateure_ops *ops)
{
	ops->temperature_get = sy7636_temperature_get;
	return 0;
}

#ifdef CONFIG_PROC_FS
#include <linux/proc_fs.h>
#include <linux/seq_file.h>

static int proc_lm_show(struct seq_file *s, void *v)
{
	u32 value;
	sy7636_temperature_get(&value);
	seq_printf(s, "%d\n", value);

	return 0;
}

static int proc_lm_open(struct inode *inode, struct file *file)
{
	return single_open(file, proc_lm_show, NULL);
}

static const struct file_operations proc_lm_fops = {
	.open		= proc_lm_open,
	.read		= seq_read,
	.llseek 	= seq_lseek,
	.release	= single_release,
};

static int __init lm_proc_init(void)
{
	proc_create("epdsensor", 0, NULL, &proc_lm_fops);
	return 0;

}
late_initcall(lm_proc_init);
#endif
#endif



