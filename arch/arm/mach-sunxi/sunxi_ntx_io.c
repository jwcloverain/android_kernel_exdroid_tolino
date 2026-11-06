

#include <linux/errno.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/input.h>
#include <linux/wait.h>
#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/uaccess.h>
#include <linux/miscdevice.h>
#include <linux/power_supply.h>

#include <linux/gpio_keys.h>

#include <linux/gpio/consumer.h>
#include <linux/of_platform.h>
#include <linux/of_gpio.h>

#include <asm/system_misc.h>
#include <linux/regulator/consumer.h>

#include "ntx_hwconfig.h"
#include "ntx_firmware.h"
#include "ntx_led.h"



#define		DEVICE_NAME 		"ntx_io"		// "pvi_io"
#define		DEVICE_MINJOR		190

#define  CM_PLATFORM		164
#define  CM_HWCONFIG		165
#define  CM_SET_HWCONFIG	166

#define	CM_SD_IN				117
#define	AC_IN					118
#define CM_PWR_ON2				112
#define CM_AUDIO_PWR			113
#define	CM_POWER_BTN			110
#define CM_USB_Plug_IN			108
#define CM_AC_CK				109
#define CM_CHARGE_STATUS		204
#define	CM_nLED					101
#define	CM_nLED_CPU				102
#define	POWER_OFF_COMMAND		0xC0	// 192
#define	SYS_RESET_COMMAND 		193		// Joseph 091223
#define	GET_LnBATT_CPU			0XC2		// 194
#define	GET_VBATT_TH			0XC3	// 195
#define	CM_SIGUSR1				104
//kay 20081110 for detecting SD write protect
#define	CM_SD_PROTECT			120
#define	SYS_AUTO_POWER_ON 		0xC4	// 196 Joseph 120620
                            	
//20090216 for detecting controller
#define	CM_CONTROLLER			121

//20090416 for detecting controller
#define	CM_USB_AC_STATUS		122
#define	CM_RTC_WAKEUP_FLAG		123
#define	CM_SYSTEM_RESET			124
#define	CM_USB_HOST_PWR			125
#define	CM_BLUETOOTH_PWR		126
#define	CM_TELLPID				99	
#define CM_LED_BLINK 			127
#define CM_TOUCH_LOCK 			128
#define CM_DEVICE_MODULE 		129
#define CM_BLUETOOTH_RESET 		130
#define CM_DEVICE_INFO 			131
#define CM_GET_WL_PWR_STAT	132

//Joseph 091211 for 3G
#define CM_3G_POWER 			150
#define CM_3G_RF_OFF 			151
#define CM_3G_RESET 			152
#define CM_3G_GET_WAKE_STATUS	153

//Joseph 091209
#define	CM_ROTARY_STATUS 		200	
#define	CM_GET_KEY_STATUS 		201	
#define	CM_GET_WHEEL_KEY_STATUS 202
#define CM_GET_KL25_STATUS		203	
#define CM_GET_KL25_ACTION		199	
#define	POWER_KEEP_COMMAND 		205	
#define	CM_GET_BATTERY_STATUS 	206	
#define	CM_SET_ALARM_WAKEUP	 	207	
#define	CM_WIFI_CTRL	 		208	
#define	CM_ROTARY_ENABLE 		209	
#define	CM_USB_ID_STATUS 		210

#define CM_GET_UP_VERSION 		215
#define	CM_ITE8951_POWER	 		220	
#define	CM_VBUS_POWER	 		221
#define	CM_ITE8951_RESET	 		222	

// gallen 100621
// Audio functions ...
#define CM_AUDIO_GET_VOLUME		230
#define CM_AUDIO_SET_VOLUME		240
#define CM_AMP_INIT				238
#define CM_FL_LM3630_SET		239
#define CM_FRONT_LIGHT_SET		241
#define CM_FRONT_LIGHT_AVAILABLE	242
#define CM_FL_MSP430_R_DUTY		243
#define CM_FL_MSP430_FREQUENCY		244
#define CM_FRONT_LIGHT_R_EN		245
#define CM_FL_HT68F20_SETDUTY		246
#define CM_FL_HT68F20_GETDUTY		247
#define CM_FL_LM3630_TABLE		248
#define CM_FL_MSP430_W_DUTY		249
#define CM_FL_MSP430_W_H_EN		250
#define CM_FL_MSP430_PWM_EN		251

#define CM_GET_KEYS				107



extern int gSleep_Mode_Suspend;
extern volatile NTX_HWCONFIG *gptHWCFG;

typedef enum __DEV_MODULE_NAME{
    EB500=0,
    EB600=1,
    EB600E=2,
    EB600EM=3,
    COOKIE=4,    
}__dev_module_name;

typedef enum __DEV_MODULE_CPU{
    CPU_S3C2410=0,
    CPU_S3C2440=1,
    CPU_S3C2416=2,
    CPU_CORETEX_A8=3,
    CPU_COOKIE=4,    
}__dev_module_cpu;

typedef enum __DEV_MODULE_CONTROLLER{
    CONTROLLER_PVI=0,
    CONTROLLER_EPSON=1,
    CONTROLLER_SW=2,
}__dev_module_controller;

typedef enum __DEV_MODULE_WIFI{
    WIFI_NONE=0,
    WIFI_MARVELL=1,
    WIFI_OTHER=2,
}__dev_module_wifi;

typedef enum __DEV_MODULE_BLUETOOTH{
    BLUETOOTH_NONE=0,
    BLUETOOTH_TI=1,
    BLUETOOTH_CSR=2,
}__devi_module_bluetooth;

struct ebook_device_info {
    char device_name;
    char cpu;
    char controller;
    char wifi;
    char bluetooth;
};

static int Driver_Count = -1;
static int gKeepPowerAlive;
int g_wakeup_by_alarm;
static unsigned int g_initialize = 0;


struct gpio_desc *GPIO_WIFI_3V3;
struct gpio_desc *GPIO_WIFI_RST;
struct gpio_desc *GPIO_WIFI_INT;
struct gpio_desc *GPIO_WIFI_DIS;
struct gpio_desc *GPIO_BT_DIS;
struct gpio_desc *GPIO_BT_INT;
static DEFINE_MUTEX(ntx_wifi_power_mutex);
static int gi_wifi_power_status = 0;

static int _Percent_to_NtxBattADC(int iPercent)
{
	int iRet;
	if(0>=iPercent) {
		return 0x8000;
	}
	iRet = 885 + (1023-885)*iPercent/100;
	return iRet;
}


static int giPowerKeyState;
#define MAX_QUEUE 6
char g_PowerKeyQueue[MAX_QUEUE] = {0};
static int g_PKQFront = -1;
static int g_PKQRear = -1;
static int g_PKQFlag = 0 ;
static int PowerKeyQueueIsFull(void)
{
	return (g_PKQRear % MAX_QUEUE) == g_PKQFront ;	
}

static int  PowerKeyQueueIsEmpty(void)
{
	return g_PKQFront == g_PKQRear ;
}

static void PowerKeyEnQueue(int state)
{
	if(PowerKeyQueueIsFull() && g_PKQFlag==1 || g_PKQRear == MAX_QUEUE && g_PKQFront == -1)
	{
		//printk("## PowerKeyQueue is full \r\n");
		return ;
	}
	g_PKQRear = (g_PKQRear + 1) % MAX_QUEUE ;
	g_PowerKeyQueue[g_PKQRear] = state;
	if(g_PKQFront == g_PKQRear)
		g_PKQFlag = 1 ;
}

static int PowerKeyDeQueue(void)
{
	int state = -1 ;
	if(PowerKeyQueueIsEmpty() && g_PKQFlag==0)
	{
		//printk("## PowerKeyQueue is empty \r\n");		
		if(g_PKQRear==-1 && g_PKQFront==-1)
		{
			printk("## Init PowerKeyQueue \r\n");
			return 1;
		}
		return  -1;
	}
	g_PKQFront = (g_PKQFront +1 )% MAX_QUEUE ;
	state = g_PowerKeyQueue[g_PKQFront];
	g_PowerKeyQueue[g_PKQFront] = -1 ;		
	if(g_PKQFront==g_PKQRear)
		g_PKQFlag = 0;
	return state ;
}

static int ntx_power_key_hook(struct gpio_keys_button *I_gpio_btn_data,int state)
{
	printk("%s():state=%d\n",__FUNCTION__,state);
	PowerKeyEnQueue(state);
	if(state) {
		giPowerKeyState = 1;
	}
	else {
		giPowerKeyState = 0;
	}
	return 0;
}

#if 0 // Disable (Ray)
gpio_keys_callback ntx_get_power_key_hookfn(void)
{
	return ntx_power_key_hook;
}
#endif

static int check_hardware_wifi(void)
{
    return  WIFI_NONE;           
}

//check Bluetooth ID
static int check_hardware_bt(void)
{
    return  BLUETOOTH_NONE;           
}

static int check_hardware_cpu(void)
{
    return CPU_S3C2440;
}
int check_hardware_name(void)
{
	static int pcb_id = -1;

	if (0 >= pcb_id) {
		switch(gptHWCFG->m_val.bPCB)
		{
			default:
				pcb_id = gptHWCFG->m_val.bPCB;
				break;	
		}
		printk ("[%s-%d] PCBA ID is %d\n",__func__,__LINE__,pcb_id);
	}

    return pcb_id;      
}
EXPORT_SYMBOL(check_hardware_name);

static int check_hardware_controller(void)
{
    return CONTROLLER_EPSON;
}

static void collect_hardware_info(struct ebook_device_info *info)
{
    info->cpu = check_hardware_cpu();
    info->device_name = check_hardware_name();
    info->controller = check_hardware_controller();
    info->wifi = check_hardware_wifi();
    info->bluetooth = check_hardware_bt();
}

static int ntx_wifi_bt_power_init(void)
{
#if 0 //[ not used in sunxi platform . 
#ifdef CONFIG_MMC
	struct device_node *np;
	struct platform_device *pdev;
	
	np = of_find_node_by_path("/regulators/wifi_regulator");
	if (np)
	{
		pdev = of_find_device_by_node(np);
		if (!pdev) {
			printk(KERN_ERR "%s():device not found !!\n", __FUNCTION__);
			of_node_put(np);
			return 0;
		}
	} else {
		printk(KERN_ERR "%s(): device node not found !!\n", __FUNCTION__);
		return 0;
	}
	
	GPIO_WIFI_3V3 = gpiod_get(&pdev->dev, "wifipwr", GPIOD_ASIS);
	if (IS_ERR(GPIO_WIFI_3V3)) {
		printk("Get GPIO_WIFI_3V3 descripter failed\n");
	}
	gpiod_direction_output(GPIO_WIFI_3V3, 0);
	
	if (0!=gptHWCFG->m_val.bWifi) {
		GPIO_WIFI_DIS = gpiod_get(&pdev->dev, "wifidis", GPIOD_ASIS);
		if (IS_ERR(GPIO_WIFI_DIS)) {
			printk("Get GPIO_WIFI_DIS descripter failed\n");
		}
		gpiod_direction_output(GPIO_WIFI_DIS, 0);
	}
	
	GPIO_WIFI_RST = gpiod_get(&pdev->dev, "wifirst", GPIOD_ASIS);
	if (IS_ERR(GPIO_WIFI_RST)) {
		printk("Get GPIO_WIFI_RST descripter failed\n");
	}
	gpiod_direction_output(GPIO_WIFI_RST, 0);
	
	GPIO_WIFI_INT = gpiod_get(&pdev->dev, "wifiint", GPIOD_ASIS);
	if (IS_ERR(GPIO_WIFI_INT)) {
		printk("Get GPIO_WIFI_INT descripter failed\n");
	}
	gpiod_direction_input(GPIO_WIFI_INT);
	
	if (0!= gptHWCFG->m_val.bBT) {
		GPIO_BT_DIS = gpiod_get(&pdev->dev, "btdis", GPIOD_ASIS);
		if (IS_ERR(GPIO_BT_DIS)) {
			printk("Get GPIO_BT_DIS descripter failed\n");
		}
		//gpiod_direction_output(GPIO_BT_DIS, 0);
	
		GPIO_BT_INT = gpiod_get(&pdev->dev, "btint", GPIOD_ASIS);
		if (IS_ERR(GPIO_BT_INT)) {
			printk("Get GPIO_BT_INT descripter failed\n");
		}
		//gpiod_direction_input(GPIO_BT_INT);
	}
	
	printk("%s() : done \n",__FUNCTION__);
#endif
#endif //]
	return 1;
}

extern void wifi_card_detect(bool);
extern void sunxi_wlan_set_power(bool);
extern void sunxi_mmc_rescan_card(unsigned ids);
extern void sunxi_mmc_set_card_removed(unsigned id);
extern int  sunxi_wlan_get_bus_index(void);
extern void sunxi_bluetooth_set_power(bool on_off);
extern void sunxi_wlan_set_dis(bool on_off);
static void _ntx_wifi_bt_power_ctrl (int power_status)
{
	int bus;
	int iOldStatus;

	iOldStatus = gi_wifi_power_status;

	//if (14 == gptHWCFG->m_val.bCPU)	// B300
	{
		printk("Wifi / BT power control %d\n", power_status);

		if (power_status & 0x3)	{
			printk("wifi/BT power on\n");
			sunxi_wlan_set_power(1);
		} 

		/* Bluetooth power control */
		if (0 != gptHWCFG->m_val.bBT) {
			if ((power_status & 0x2) == (iOldStatus & 0x2)) {  /* Bluetooth dis status */
				printk("Bluetooth already %s.\n",(power_status & 0x2)?"on":"off");
			} else if (power_status & 0x2) {
				printk("Bluetooth On\n");
				sunxi_bluetooth_set_power(1);
				gi_wifi_power_status |= 2;
			} else {
				printk("Bluetooth Off\n");
				gi_wifi_power_status &= ~2;
			}
		}

		/* Wifi power control */
		printk(KERN_ERR"[DEBUG] power_status:%d , iOldStatus:%d \n",power_status,iOldStatus);
		if ((power_status & 0x1) == (iOldStatus & 0x1)) {  /* check Wifi status */
			printk ("Wifi already %s.\n",(power_status & 0x1)?"on":"off");
		} else if (power_status & 0x1) {
			printk("Wifi On\n");
			//gpiod_direction_output(GPIO_WIFI_RST, 1);
			gi_wifi_power_status |= 1;
		} else {
			printk("Wifi Off\n");
			gi_wifi_power_status &= ~1;
		}

		//printk("Wifi/BT bus:%d \n",bus);
		bus = sunxi_wlan_get_bus_index() ;
		if( power_status&0x3 ) {
			if ( (!!power_status) != (!!iOldStatus) ) {
				sunxi_wlan_set_dis(1);	 /* Wifi dis pull high */
				//printk("sunxi_mmc_rescan_card !!\n");
				msleep(300);
				printk(KERN_DEBUG" sunxi_mmc_rescan_card bus%d!!\n",bus);
				sunxi_mmc_rescan_card(bus);
				msleep(300);
			}
		}
		else if ( 0==power_status ) {
			if ( power_status != iOldStatus )
			{
				sunxi_wlan_set_dis(0);	/* Wifi dis pull low */
				printk(KERN_DEBUG" remove card bus%d!!\n",bus);
				sunxi_mmc_set_card_removed(bus);
				msleep(300);
			}
		}

	
		if (0 != gptHWCFG->m_val.bBT) {
			if ( !(power_status&2) )	{
				sunxi_bluetooth_set_power(0);
				//printk("bt off\n");
			}
		}

		if(0==power_status) {
			printk("wifi/bt device power off\n");
			sunxi_wlan_set_power(0);
			sunxi_mmc_set_card_removed(bus);
			msleep(300);
		}

	}
	//printk("%s() : done \n",__FUNCTION__);
#if 0
#ifdef CONFIG_MMC
	int iOldStatus;

	iOldStatus = gi_wifi_power_status;

	if (!g_initialize) {
		printk("%s() : ntx_wifi_power_ctrl return \n",__FUNCTION__);
		return;
	}
	
	printk("%s g_initialize = %d\n", __func__, g_initialize);
	printk("Wifi / BT power control %d\n", power_status);
	
	if (power_status & 0x3)	{
		printk("wifi 3v3 power on\n");
		gpiod_direction_output(GPIO_WIFI_3V3, 1);  /* wifi 3V3 turn on */
	} 
	
	
	/* Bluetooth power control */
	if (11 == gptHWCFG->m_val.bWifi || 12 == gptHWCFG->m_val.bWifi || 14 == gptHWCFG->m_val.bWifi) {
		if ((power_status & 0x2) == (iOldStatus & 0x2)) {  /* Bluetooth dis status */
			printk("Bluetooth already %s.\n",(power_status & 0x2)?"on":"off");
		} else if (power_status & 0x2) {
			printk("Bluetooth On\n");
			gpiod_direction_output(GPIO_BT_DIS, 1);  /* BT dis pull high */
			gi_wifi_power_status |= 2;
		} else {
			printk("Bluetooth Off\n");
			gpiod_direction_output(GPIO_BT_DIS, 0);  /* BT dis pull low */
			gi_wifi_power_status &= ~2;
		}
	}
	
	/* Wifi power control */
	if ((power_status & 0x1) == (iOldStatus & 0x1)) {  /* check Wifi status */
		printk ("Wifi already %s.\n",(power_status & 0x1)?"on":"off");
	} else if (power_status & 0x1) {
		gpiod_direction_output(GPIO_WIFI_RST, 0);
		msleep(20);
		if (11 == gptHWCFG->m_val.bWifi || 12 == gptHWCFG->m_val.bWifi || 14 == gptHWCFG->m_val.bWifi) {
			gpiod_direction_output(GPIO_WIFI_DIS, 1); /* Wifi dis pull high */
			gpiod_direction_input(GPIO_WIFI_INT);
		}
		msleep(20);
		printk("Wifi On\n");
		gpiod_direction_output(GPIO_WIFI_RST, 1);
		
		gi_wifi_power_status |= 1;
	} else {
		gpiod_direction_output(GPIO_WIFI_RST, 0);
		if (11 == gptHWCFG->m_val.bWifi || 12 == gptHWCFG->m_val.bWifi || 14 == gptHWCFG->m_val.bWifi) {
			gpiod_direction_output(GPIO_WIFI_DIS, 0); /* Wifi dis pull low */
			printk("Wifi Off\n");
			gpiod_direction_input(GPIO_WIFI_INT);
		}
		
		gi_wifi_power_status &= ~1;
	}
	
	if ((power_status) && (iOldStatus)) {
		printk("Device already exist !!\n");
	}
	else if (power_status)
		wifi_card_detect(true);
	else
		wifi_card_detect(false);


	if ( !(power_status & 0x3) ){
		printk("wifi 3v3 power off\n");
		gpiod_direction_output(GPIO_WIFI_3V3, 0);  /* wifi 3V3 turn off */
	}

	printk("%s() : done \n",__FUNCTION__);
#endif
#endif
}


void ntx_bt_power_ctrl(int iIsBTEnable)
{
	if (!g_initialize) {
		g_initialize = ntx_wifi_bt_power_init();
	}

	if (0 != gptHWCFG->m_val.bBT) { 
		mutex_lock(&ntx_wifi_power_mutex);
		if (iIsBTEnable)
			_ntx_wifi_bt_power_ctrl(gi_wifi_power_status | 2);
		else
			_ntx_wifi_bt_power_ctrl(gi_wifi_power_status & ~2);
		mutex_unlock(&ntx_wifi_power_mutex);
	}
}
EXPORT_SYMBOL(ntx_bt_power_ctrl);

void ntx_wifi_power_ctrl(int iIsWifiEnable)
{
	if (!g_initialize) {
		g_initialize = ntx_wifi_bt_power_init();
	}

	if (0 != gptHWCFG->m_val.bWifi) { 
		mutex_lock(&ntx_wifi_power_mutex);
		if (iIsWifiEnable)
			_ntx_wifi_bt_power_ctrl(gi_wifi_power_status | 1);
		else
			_ntx_wifi_bt_power_ctrl(gi_wifi_power_status & ~1);
		mutex_unlock(&ntx_wifi_power_mutex);
	}
}

EXPORT_SYMBOL(ntx_wifi_power_ctrl);

int ntx_esd_cd (void)
{
	struct device_node *np;

	np = of_find_node_by_path("/soc/aips-bus@02100000/usdhc@02194000");
	if(np)
	{
		int cd_gpio = of_get_named_gpio(np, "cd-gpios", 0);
		if (gpio_is_valid(cd_gpio))
			return gpio_get_value (cd_gpio)?0:1;

		of_node_put(np);
	}
	else {
		printk(KERN_ERR"%s(): /soc/aips-bus@02100000/usdhc@02194000 device node not found !!\n",__FUNCTION__);
	}
	return 0;
}


#ifdef CONFIG_MFD_RICOH619 //[
extern int ricoh619_restart(void);
extern int ricoh619_power_off(void);
#endif //]CONFIG_MFD_RICOH619

static void ntx_system_reset(const char *pszDomain)
{

	if(pszDomain) {
		printk("%s() ---%s reset ---\n",__FUNCTION__,pszDomain);
	}


	while (1) {
		gKeepPowerAlive = 0;
#ifdef CONFIG_MFD_RICOH619 //[
		if (1==gptHWCFG->m_val.bPMIC) {
			// RC5T19 .
			printk("%s() --- RC5T19 restarting system ---\n",__FUNCTION__);
			ricoh619_restart();
		}
		else 
#endif //]CONFIG_MFD_RICOH619
		{
			printk("%s():TBD\n",__FUNCTION__);
		}

		msleep(500);
	}
}

static void ntx_system_poweroff(const char *pszDomain)
{
	if(pszDomain) {
		printk("%s() ---%s poweroff ---\n",__FUNCTION__,pszDomain);
	}

	if (!gKeepPowerAlive) {
#ifdef CONFIG_NTX_LED
		ntx_led_set_auto_blink(0);
		led_green(0);
#endif
		while (1) {
#ifdef CONFIG_MFD_RICOH619 //[
			if(1==gptHWCFG->m_val.bPMIC) {
				printk("RC5T619 ---Power Down ---\n");
				ricoh619_power_off();
			}
			else 
#endif //]CONFIG_MFD_RICOH619
			{
				printk("%s():TBD\n",__FUNCTION__);
			}
			msleep(500);
		}
	}
	else {
		printk("Kernel---in keep alive mode ---\n");
	}
}


void ntx_machine_restart(char mode, const char *cmd)
{	
	if (cmd && ('\0' != *cmd)) {
		printk("%s mode=%c,cmd=%s\n",__FUNCTION__,mode,cmd);
		arm_pm_restart (mode, cmd);
	}	
	else {
		printk ("[%s-%d] do hardware reset...\n",__func__,__LINE__);
		ntx_system_reset(0);
	}
}

void ntx_machine_poweroff(void)
{
	ntx_system_poweroff(__FUNCTION__);
}

static int openDriver(struct inode *inode,struct file *filp)
{
	if(!Driver_Count)
		Driver_Count++;
	return 0;
}
static int releaseDriver(struct inode *inode,struct file *filp)
{
	if(Driver_Count)
		Driver_Count--;
	return 0;
}

#ifdef CONFIG_BACKLIGHT_LM3630A //[
extern int lm3630a_get_FL_current(void);
#else //][!CONFIG_BACKLIGHT_LM3630A
#define lm3630a_get_FL_current()	0
#endif //] CONFIG_BACKLIGHT_LM3630A

#ifdef CONFIG_BACKLIGHT_TLC5947
extern int tlc5947_get_FL_current(void);
#else
#define tlc5947_get_FL_current()	0
#endif


#ifdef CONFIG_LEDS_LM3692X //[
extern int lm36922_get_FL_current(void);
#else //][!CONFIG_LEDS_LM3692X
#define lm36922_get_FL_current()	0
#endif //] CONFIG_LEDS_LM3692X

#ifdef CONFIG_LEDS_AW99703
extern int aw99703_get_FL_current(void);
#else
#define  aw99703_get_FL_current()	0
#endif

//unsigned int  last_FL_duty = 0;

extern int fl_level;		// If FL is on, value is 0-100. If FL is off, value is 0;
extern int fl_current;		// Unit is 1uA. If FL is off, value is 0;
extern int slp_state;		// 0:Suspend, 1:Hibernate
extern int idle_current;	// Unit is 1uA.
extern int sus_current;		// Unit is 1uA.
extern int hiber_current;	// Unit is 1uA.
extern bool bat_alert_req_flg;	// 0:Normal, 1:Re-synchronize request from system

static NTX_FW_percent_current_tab *gptNTX_Percent_curr_tab;


int ntx_get_fl_percent(int *O_piPercent);

void ntx_percent_curr_tab_set(void *pvTable)
{
	gptNTX_Percent_curr_tab = (NTX_FW_percent_current_tab *)pvTable ;
}
#ifdef CONFIG_MFD_RICOH619 //[


extern int get_elan_touch_tp_source(void);
void ricoh_suspend_state_sync(void)
{
	int  last_FL_duty ;
	int iTemp;

	ntx_get_fl_percent(&last_FL_duty);

	fl_level = last_FL_duty;		// If FL is on, value is 0-100. If FL is off, value is 0;

	fl_current = 0;
	if (gptHWCFG->m_val.bFrontLight) {

		switch(gptHWCFG->m_val.bPCB) {
		default :
			if(gptNTX_Percent_curr_tab) {
				if(last_FL_duty) {
					fl_current = gptNTX_Percent_curr_tab->dwCurrentA[last_FL_duty-1];
					printk("fl_current@%d=%d\n",last_FL_duty,fl_current);
				}
				break;
			}
			else
			if(2==gptHWCFG->m_val.bFL_PWM||4==gptHWCFG->m_val.bFL_PWM||
				5==gptHWCFG->m_val.bFL_PWM||6==gptHWCFG->m_val.bFL_PWM||
				7==gptHWCFG->m_val.bFL_PWM)
			{

				iTemp = lm3630a_get_FL_current();
				if(iTemp>=0) {
					fl_current = iTemp;
				}
				else {
					printk(KERN_WARNING"\n[WARNING]FL current not avalible(PCB=0x%x)\n\n",
							gptHWCFG->m_val.bPCB);
				}
				break;
			}
			else if(8==gptHWCFG->m_val.bFL_PWM)
			{
				iTemp = tlc5947_get_FL_current();
				if(iTemp>=0) {
					fl_current = iTemp;
				}
				else {
					printk(KERN_WARNING"\n[WARNING]FL current not avalible(PCB=0x%x)\n\n",
							gptHWCFG->m_val.bPCB);
				}
				break;
			}
			else if (10==gptHWCFG->m_val.bFL_PWM)
			{
				iTemp = lm36922_get_FL_current();
				if(iTemp>=0) {
					fl_current = iTemp;
				}
				else {
					printk(KERN_WARNING"\n[WARNING]FL current not avalible(PCB=0x%x)\n\n",
							gptHWCFG->m_val.bPCB);
				}
				break;
			}
			else if(0x0C==gptHWCFG->m_val.bFL_PWM){
				iTemp = aw99703_get_FL_current();
				if(iTemp>=0) {
					fl_current = iTemp;
				}
				else {
					printk(KERN_WARNING"\n[WARNING]FL current not avalible(PCB=0x%x)\n\n",
							gptHWCFG->m_val.bPCB);
				}
				break;
			}
			else {
				printk(KERN_WARNING"\n[WARNING]there is no FL current table for this model(0x%x)\n\n",
					gptHWCFG->m_val.bPCB);
			}
		}
	}

	slp_state = gSleep_Mode_Suspend?1:0;		// 0:Suspend, 1:Hibernate

	switch (gptHWCFG->m_val.bPCB) {
	default:
		if(0==sus_current) {
			printk(KERN_WARNING"\n[WARNING]there is no suspend current data for PCB(0x%x)\n\n",gptHWCFG->m_val.bPCB);
		}
		if(0==hiber_current) {
			printk(KERN_WARNING"\n[WARNING]there is no hibernation current data for PCB(0x%x)\n\n",gptHWCFG->m_val.bPCB);
		}
		break;
	case 91://E60P2X .
		idle_current = 154000;	// Unit is 1uA.
		sus_current = 10344;		// Unit is 1uA.
		hiber_current = 1890;	// Unit is 1uA.
		break;
    case 94://EA0P14
        sus_current = 17000;     // Unit is 1uA.
        hiber_current = 1100;    // Unit is 1uA.
        break;
    case 95://E60P42
		if(gptHWCFG->m_val.bEPD_Flags & 0x08 ){	// 0x08 CFA
			sus_current = 2225;     // Unit is 1uA.
			hiber_current = 985;    // Unit is 1uA.
		}
		else{ // BW
			sus_current = 1971;     // Unit is 1uA.
			hiber_current = 1021;   // Unit is 1uA.
		}
        break;
    case 96://E70P14
        sus_current = 17500;     // Unit is 1uA.
        hiber_current = 1059;    // Unit is 1uA.
        break;
	case 98://E80P04
		sus_current = 14982;     // Unit is 1uA.
		hiber_current = 1267;    // Unit is 1uA.
	case 100://E60P54
		if(5==gptHWCFG->m_val.bDisplayResolution) {// BN4
			sus_current = 2890;     // Unit is 1uA.
			hiber_current = 1110;    // Unit is 1uA.
		}
		else if (1==gptHWCFG->m_val.bDisplayResolution) {// BN5
			if(1==get_elan_touch_tp_source())
				sus_current = 7000;     // Unit is 1uA.
			else if(2==get_elan_touch_tp_source())
				sus_current = 5484;     // Unit is 1uA.
			else{
				sus_current = 7000;     // Unit is 1uA.
				printk(KERN_ERR"[ERROR] Unknow tp source , sus_current maybe wrong !!!");
			}

			hiber_current = 1090;    // Unit is 1uA.
		}
		break;
	}
	bat_alert_req_flg = 0;	// 0:Normal, 1:Re-synchronize request from system
}

#endif //]CONFIG_MFD_RICOH619


const char bd7181x_bat_psy_name[] = "battery";
const char rc5t619_bat_psy_name[] = "mc13892_bat";

static int _Is_USB_plugged(void)
{

	char *psy_name=0;
	struct power_supply *psy;
	int iChk;
	int iRet = 0;

	if(1==gptHWCFG->m_val.bPMIC) {
		// Ricoh PMIC .
		psy_name = rc5t619_bat_psy_name;
	}
	else if(6==gptHWCFG->m_val.bPMIC) {
		// Rohm PMIC .
		psy_name = bd7181x_bat_psy_name;
	}

	psy = power_supply_get_by_name(psy_name);

	if(psy) {
		union power_supply_propval psy_propval;
		iChk = power_supply_get_property(psy,POWER_SUPPLY_PROP_ONLINE,&psy_propval);
		if(iChk>=0) {
			iRet = psy_propval.intval;
		}
		else {
			iRet = 0;
			printk(KERN_ERR"%s():%s power supply propval get failed !!\n",
					__FUNCTION__,psy_name);
		}
	}
	else {
		iRet = 0;
		printk(KERN_ERR"%s power supply getting failed !!\n",psy_name);
	}
	return iRet;
}


static long  ioctlDriver(struct file *filp, unsigned int command, unsigned long arg)
{
      
	unsigned long i = 0, temp;
	unsigned int p = arg;//*(unsigned int *)arg;
	unsigned long dwChk;
  //struct ebook_device_info info; 
#ifdef CONFIG_FB_MXC_EINK_PANEL//[
	extern void mxc_epdc_fb_shutdown_proc(void);
#endif //]CONFIG_FB_MXC_EINK_PANEL
	char *psy_name=0;

	if(!Driver_Count){
		printk("%s : do not open\n",DEVICE_NAME);
		return -1;	
	}

	if(1==gptHWCFG->m_val.bPMIC) {
		// Ricoh PMIC .
		psy_name = rc5t619_bat_psy_name;
	}
	else if(6==gptHWCFG->m_val.bPMIC) {
		// Rohm PMIC .
		psy_name = bd7181x_bat_psy_name;
	}

	switch(command)
	{
		case POWER_OFF_COMMAND:
			{
#ifdef CONFIG_FB_MXC_EINK_PANEL//[
				mxc_epdc_fb_shutdown_proc();
#endif //]CONFIG_FB_MXC_EINK_PANEL
				ntx_system_poweroff("POWER_OFF_COMMAND");
			}
			break;

		case SYS_RESET_COMMAND:
#ifdef CONFIG_FB_MXC_EINK_PANEL//[
			mxc_epdc_fb_shutdown_proc();
#endif //]CONFIG_FB_MXC_EINK_PANEL
			ntx_system_reset("SYS_RESET_COMMAND");
			break;
			
		case SYS_AUTO_POWER_ON:
			printk("%s():SYS_AUTO_POWER_ON\n",__FUNCTION__);
			ntx_system_reset("SYS_AUTO_POWER_ON");
			break;
			
		case POWER_KEEP_COMMAND:
			printk("Kernel---System Keep Alive --- %d\n",p);
			gKeepPowerAlive=p;
			if(1==gptHWCFG->m_val.bPMIC||6==gptHWCFG->m_val.bPMIC)
				break;
			if (gKeepPowerAlive) {
#ifdef CONFIG_NTX_LED
				ntx_led_set_auto_blink(1);
#endif
			}
			break;
		

		case CM_GET_BATTERY_STATUS:
			{
				struct power_supply *psy = power_supply_get_by_name(psy_name);
				int iChk;
				if(psy) {
					union power_supply_propval psy_propval;
					iChk = power_supply_get_property(psy,POWER_SUPPLY_PROP_CAPACITY,&psy_propval);
					if(iChk>=0) {
						i = _Percent_to_NtxBattADC(psy_propval.intval);
						//printk(KERN_INFO"%s power supply capacity=%d\n",psy_name,psy_propval.intval);
					}
					else {
						i = _Percent_to_NtxBattADC(100);
						printk(KERN_ERR"%s power supply propval get failed !!\n",psy_name);
					}
				}
				else {
					i = _Percent_to_NtxBattADC(100);
					printk(KERN_ERR"%s power supply getting failed !!\n",psy_name);
				}
				dwChk = copy_to_user((void __user *)arg, &i, sizeof(unsigned long));
			}
			break;
			
		case AC_IN:
			printk("%s() : AC_IN\n",__FUNCTION__);
			i = 0;
			dwChk = copy_to_user((void __user *)arg, &i, sizeof(unsigned long));
			break;	

		case CM_SD_IN:
			i = ntx_esd_cd();
			dwChk = copy_to_user((void __user *)arg, &i, sizeof(unsigned long));
			break;
			
		case GET_LnBATT_CPU:
			break;
		case GET_VBATT_TH:
			break;
		case CM_AC_CK:
			break;
		case CM_USB_Plug_IN:
		case CM_CHARGE_STATUS:
			i = _Is_USB_plugged();
			dwChk = copy_to_user((void __user *)arg, &i, sizeof(unsigned long));
			break;
		case CM_PWR_ON2:
			break;
		case CM_AUDIO_PWR:
			break;
		case CM_nLED:
			//printk("CM_nLED %d\n",p);
#ifdef CONFIG_NTX_LED
			led_green(p?1:0);
#endif
			break;			
			
		case CM_nLED_CPU:
			break;			
			
		case CM_SD_PROTECT:
			i = 0;
			dwChk = copy_to_user((void __user *)arg, &i, sizeof(unsigned long));
			break;
			
		case CM_CONTROLLER:
			i = 2;	// 2: Epson controller. for micro window
			dwChk = copy_to_user((void __user *)arg, &i, sizeof(unsigned long));
			break;
			
		case CM_USB_AC_STATUS:
			//printk("%s() : CM_USB_AC_STATUS\n",__FUNCTION__);
			i = 0;
			dwChk = copy_to_user((void __user *)arg, &i, sizeof(unsigned long));
			break;
			
		case CM_RTC_WAKEUP_FLAG:
	        i = g_wakeup_by_alarm;		// Joseph 091221 for slide show test.
	        g_wakeup_by_alarm = 0;
			dwChk = copy_to_user((void __user *)arg, &i, sizeof(unsigned long));
			break;
			
		case CM_SYSTEM_RESET:
#ifdef CONFIG_FB_MXC_EINK_PANEL//[
			mxc_epdc_fb_shutdown_proc();
#endif //]CONFIG_FB_MXC_EINK_PANEL
			ntx_system_reset("CM_SYSTEM_RESET");
			break;
			
		case CM_USB_HOST_PWR:
			printk("%s() : CM_USB_HOST_PWR\n",__FUNCTION__);
			break;
			
		case CM_BLUETOOTH_PWR:
			ntx_bt_power_ctrl(p);
			break;
			
		case CM_TELLPID:
			printk("%s() : CM_TELLPID\n",__FUNCTION__);
			break;
			
		case CM_LED_BLINK:
#ifdef CONFIG_NTX_LED
			ntx_led_set_auto_blink(p);	
#endif
			break;
	      
		case CM_TOUCH_LOCK:
			printk("%s() : CM_TOUCH_LOCK\n",__FUNCTION__);
			break;  
      
		case CM_DEVICE_MODULE:
			i = check_hardware_name();
			dwChk = copy_to_user((void __user *)arg, &i, sizeof(unsigned long));
			break;
              	          	
		case CM_BLUETOOTH_RESET:
			printk("%s() : CM_BLUETOOTH_RESET\n",__FUNCTION__);
			break;
			
		case CM_DEVICE_INFO:
			printk("%s() : CM_DEVICE_INFO\n",__FUNCTION__);
			break;	
      		
		case CM_ROTARY_STATUS:
			//printk("%s() : CM_ROTARY_STATUS\n",__FUNCTION__);
   		break;	
      		
		case CM_ROTARY_ENABLE:	
			printk("%s() : CM_ROTARY_ENABLE\n",__FUNCTION__);
     	break;

		case CM_USB_ID_STATUS:
			printk("%s() : CM_USB_ID_STATUS\n",__FUNCTION__);
			i = 0;
			dwChk = copy_to_user((void __user *)arg, &i, sizeof(unsigned long));
     	break;

		case CM_GET_KEYS:
			printk("%s() : CM_GET_KEYS\n",__FUNCTION__);
			i = 0;
			dwChk = copy_to_user((void __user *)arg, &i, sizeof(unsigned long));
			break;
		case CM_POWER_BTN:
		case CM_GET_KEY_STATUS:	
			//printk("%s() : CM_POWER_BTN/CM_GET_KEY_STATUS\n",__FUNCTION__);
			//i=giPowerKeyState;	
			
			i = PowerKeyDeQueue();			
			if(i==-1)
			{
				i= giPowerKeyState;
			}					
			dwChk = copy_to_user((void __user *)arg, &i, sizeof(unsigned long));
     	break;	
      		
		case CM_GET_WHEEL_KEY_STATUS:
			i=0;
			dwChk = copy_to_user((void __user *)arg, &i, sizeof(unsigned long));
   		break;
      		
		case CM_GET_KL25_STATUS:
			i=0;	
			dwChk = copy_to_user((void __user *)arg, &i, sizeof(unsigned long));
			break;
	
		case CM_GET_KL25_ACTION:
			i=0 ;	
			dwChk = copy_to_user((void __user *)arg, &i, sizeof(unsigned long));
			break;

		case CM_3G_POWER:
			break;	
					
		case CM_3G_RF_OFF:
			break;	
					
		case CM_3G_RESET:
			break;	
					
        case CM_WIFI_CTRL:
			ntx_wifi_power_ctrl (p);
			break;	
		case CM_GET_WL_PWR_STAT:
			i = gi_wifi_power_status;
			dwChk = copy_to_user((void __user *)arg, &i, sizeof(unsigned long));
			break;
		case CM_3G_GET_WAKE_STATUS:	
			i = 0;	
			dwChk = copy_to_user((void __user *)arg, &i, sizeof(unsigned long));
    	break;	
	    	
		case CM_SET_ALARM_WAKEUP:
	    break;	
	    	
		case CM_GET_UP_VERSION:
			i = 0;
			dwChk = copy_to_user((void __user *)arg, &i, sizeof(unsigned long));
    	break;

		case CM_AUDIO_GET_VOLUME:
			break;
			
		case CM_AUDIO_SET_VOLUME:
			break;

#ifdef CONFIG_BACKLIGHT_CLASS_DEVICE //[
		case CM_FRONT_LIGHT_SET:
			{
#ifdef CONFIG_BACKLIGHT_LM3630A
				extern int ntx_set_fl_percent(int percent);

				ntx_set_fl_percent(p);
#endif
			}
			break;

		case CM_FL_LM3630_SET:
			break;
		case CM_FL_LM3630_TABLE:
			break;

		case CM_FL_HT68F20_GETDUTY:
			break;

		case CM_FL_HT68F20_SETDUTY:
			break;

		case CM_FRONT_LIGHT_AVAILABLE:
			{
			 	i = (unsigned long) (gptHWCFG->m_val.bFrontLight?1:0) ;
				dwChk = copy_to_user((void __user *)arg, &i, sizeof(unsigned long));
			}
			break;

		case CM_FL_MSP430_R_DUTY:
			break;

		case CM_FL_MSP430_W_DUTY:
			break;

		case CM_FL_MSP430_FREQUENCY:
			break;

		case CM_FRONT_LIGHT_R_EN:
			break;
		case CM_FL_MSP430_W_H_EN:
			break;
		case CM_FL_MSP430_PWM_EN:
			break;
#endif //]CONFIG_BACKLIGHT_CLASS_DEVICE 
		case CM_PLATFORM:
			printk("%s() : CM_PLATFORM\n",__FUNCTION__);
			break;
		case CM_HWCONFIG:
			printk("%s() : CM_HWCONFIG\n",__FUNCTION__);
			break;
		case CM_SET_HWCONFIG:
			if (!capable(CAP_SYS_ADMIN)) 
				return -EPERM;
			break;

#ifdef TOUCH_HOME_LED
		case CM_HOME_LED_ONOFF:
			if(0x03==gptHWCFG->m_val.bUIConfig){
				extern void homeled_onoff(int iIsON); 
				switch(p){
					case 0:
						homeled_onoff(0);
						break;
		
					case 1:
						homeled_onoff(1);
						break;

					default:
						homeled_onoff(1);
						break;
				}
			}
			break;
#endif //#ifdef TOUCH_HOME_LED

		default:
			printk("pvi_io : do not get the command [%d]\n", command);
			return -1;
	}
	return 0;
}
	

static struct file_operations driverFops= {
    .owner  	=   THIS_MODULE,
    .open   	=   openDriver,
    .unlocked_ioctl	=   ioctlDriver,
    .release    =   releaseDriver,
};

static struct miscdevice driverDevice = {
	.minor		= DEVICE_MINJOR,
	.name		= DEVICE_NAME,
	.fops		= &driverFops,
};

extern void gpio_keys_report_event(unsigned int type, unsigned int code, int value);
void ntx_report_event(unsigned int type, unsigned int code, int value)
{
	gpio_keys_report_event(type,code,value);
}





static int __init initDriver(void)
{
	int ret;
        


#if 0  // Disable (Ray)
#if 1
	pm_power_off = ntx_machine_poweroff;	// this is defined in bd7181x-power.c
	arm_pm_restart = ntx_machine_restart;	// use WDOG to reboot.
#endif
#endif
	ret = misc_register(&driverDevice);
	if (ret < 0) {
		printk("pvi_io: can't get major number\n");
		return ret;
	}

	return 0;
}
static void __exit exitDriver(void) {
	misc_deregister(&driverDevice);

}
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Gallen");
MODULE_VERSION("2016-12-29");
MODULE_DESCRIPTION ("NTX_IO driver");
module_init(initDriver);
module_exit(exitDriver);

