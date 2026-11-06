/*
 * ntx_cilix.h
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef __LINUX_NTX_CILIX_H
#define __LINUX_NTX_CILIX_H

#include <linux/mutex.h>
#include <linux/pm.h>

#define UART_CMD_START	0x53
#define UART_CMD_END	0x50

#define I2C_ACK		0x06
#define I2C_NACK	0x15
/* i2c NACK return error codes */
#define I2C_NACK_SUCCESS                0x00
#define I2C_NACK_COMMAND_NOT_SUPPORT    0x01
#define I2C_NACK_M_MSP430_BUSY          0x02
#define I2C_NACK_BATTERY_CRITICAL       0x03
#define I2C_NACK_NOT_ENOUGH_BUFFER      0x04
#define I2C_NACK_CILIX_DISCONNECTED     0x05
#define I2C_NACK_I2C_ERROR              0x06
#define I2C_NACK_RESP_DATA_CNT_NOT_MATCH 0x07
#define I2C_NACK_OPERATION_NOT_VALID    0x08
#define I2C_NACK_OPERATION_TIMEOUT      0x09
#define I2C_NACK_OPERATION_NOT_FINISHED 0x0A
#define I2C_NACK_C_MSP430_BUSY          0x0B
//#define I2C_ERROR_UNKNOWN               0xFF

#define I2C_ADDRESS_SY6974B 0x6B
#define I2C_ADDRESS_EG2805 0x14
#define I2C_ADDRESS_E80P0M_MSP430_GPIO 0x31
#define I2C_ADDRESS_T80P0C_MSP430_GPIO 0x41

#define MSP430_REG_GPIO_P1			    0x00
#define MSP430_REG_GPIO_P2			    0x01
#define MSP430_REG_STAT_1				0x02
#define MSP430_REG_STAT_2				0x03
#define MSP430_REG_VER_LOW_BYTE		    0x04
#define MSP430_REG_VER_HIGH_BYTE		0x05
#define MSP430_REG_DET_ADC_LOW_BYTE 	0x06
#define MSP430_REG_DET_ADC_HIGH_BYTE	0x07
#define MSP430_REG_FUNC_1				0x08
#define MSP430_REG_BAT_LOW_BYTE	    	0x09
#define MSP430_REG_BAT_HIGH_BYTE    	0x0A
/*Settings*/
#define MSP430_REG_SETTING_VER    		0x40
#define MSP430_REG_SETTING_ADC_LOW 		0x56
#define MSP430_REG_SETTING_ADC_HIGH		0x57
#define MSP430_REG_SETTING_OTG_LATENCY	0x58
#define MSP430_REG_SETTING_WD_TIMER 	0x59
#define MSP430_REG_SETTING_BAT_LOW_BYTE	    0x5A
#define MSP430_REG_SETTING_BAT_HIGH_BYTE	0x5B
#define MSP430_REG_SETTING_TIMER_LOW_BYTE	0x5C
#define MSP430_REG_SETTING_TIMER_HIGH_BYTE	0x5D
#define MSP430_REG_SETTING_WAKEUP_INTERVAL	0x5E
#define MSP430_REG_SETTING_RESERVED     	0x5F
#define MSP430_REG_SETTING_OTG_DISABLE_DELAY	0x60
#define MSP430_REG_SETTING_SHIPPING_MODE_DELAY	0x61
#define MSP430_REG_SETTING_DET_ADC_SWITCH_DELAY	0x62
#define MSP430_REG_SETTING_CONN_CHK_RETRY_CNT	0x63
#define MSP430_REG_SETTING_FEATURES_SWITCH  	0x64
#define MSP430_REG_SETTING_MSP430_RESET_REASON	0x65

/*REG_GPIO_P1*/
#define M_MSP430_BIT_GPIO_GLF72120_EN	3
#define C_MSP430_BIT_GPIO_PIN_CE		3

/*REG_GPIO_P2*/
#define M_MSP430_BIT_GPIO_VUSB_IN		3
#define M_MSP430_BIT_GPIO_VUSBVADP_IN	4

/*REG_STAT_1*/
#define STAT_CILIX_CONN			0
#define STAT_USB_CONN           1
#define STAT_MSP_RESET          2
#define STAT_UART_COMM          3

/*REG_FUNC_1*/
#define FUNC_UART_BUF_CLEAN		0x01
#define FUNC_MSP430_RESET		0x02
#define FUNC_I2C_CHK_ENABLE     0x40
#define FUNC_I2C_CHK_DISABLE    0x41
#define FUNC_COMM_CHK			0x80
#define FUNC_CILIX_OTG_ON		0x81
#define FUNC_CILIX_OTG_OFF		0x82
#define FUNC_CILIX_CHARGE_ON_DEPRECATED	0x83 /* set charge enable and pull low pin CE */
#define FUNC_CILIX_CHARGE_OFF	0x84 /* pull high pin CE only */
#define FUNC_CILIX_CHARGE_ON	0x85 /* 0x83 and set VINDPM threshold 3.9V*/
#define FUNC_CE_PIN_LOW         0x86
#define FUNC_START_WAKEUP       0x87		
#define FUNC_SHIPPING_MODE      0x88
#define FUNC_UART_TO_GPIO       0x89		
#define FUNC_GPIO_TO_UART       0x8A		
#define FUNC_ERASE_CONFIG       0x8B
#define FUNC_PT_MODE_ENTER      0x8C
#define FUNC_PT_MODE_EXIT       0x8D
#define FUNC_SHIPPING_MODE_CANCEL   0x8E

enum cilix_command {
	/* MSP430 FUNCIONS*/
    CILIX_COMM_CHECK,
	CILIX_M_MSP430_VER,
	CILIX_C_MSP430_VER,
	CILIX_M_MSP430_STAT,
	CILIX_M_ADC,
	CILIX_C_ADC,
	CILIX_M_MSP430_RESET,
	CILIX_C_MSP430_RESET,

    CILIX_I2C_MONITOR_ENABLE,
    CILIX_I2C_MONITOR_DISABLE,
	CILIX_OTG_ON,
	CILIX_OTG_OFF,
	CILIX_CHARGE_ON,
	CILIX_CHARGE_OFF,
	CILIX_CONN_STATUS,
	USB_CONN_STATUS,

	CILIX_GLF72120_EN_STATUS,
	CILIX_GLF72120_EN_ENABLE,
	CILIX_GLF72120_EN_DISABLE,

	CILIX_PIN_CE_STATUS,
	CILIX_PIN_CE_HIGH,
	CILIX_PIN_CE_LOW,

	/*SY6974B*/
	SY6974B_WATCHDOG_RESET,
	SY6974B_WATCHDOG_STATUS,
	SY6974B_WATCHDOG_ENABLE,
	SY6974B_WATCHDOG_DISABLE,

	SY6974B_OTG_STATUS,
	SY6974B_OTG_ENABLE,
	SY6974B_OTG_DISABLE,
	
	SY6974B_CHARGE_STATUS, /*charge enable configuration*/
	SY6974B_CHARGE_ENABLE,
	SY6974B_CHARGE_DISABLE,

	CILIX_CHARGE_STATUS, /*charge status*/

    CILIX_SHIPPING_MODE,
    CILIX_SHIPPING_MODE_CANCEL,
    CILIX_PT_MODE_ENTER,
    CILIX_PT_MODE_EXIT,
	/*EG2805*/
	CILIX_BAT_CAPACITY,
	EG2805_AVG_CURRENT,
	EG2805_BAT_TEMPERATURE,
	EG2805_BAT_VOLTAGE,
    EG2805_BAT_TIME_TO_EMPTY,
    EG2805_BAT_TIME_TO_FULL,
};

enum cilix_states {
	ST_EBCX_APART,
	ST_EBCX_CHARGING,
	ST_EBCX_CONN,
	ST_EB_CHARGING,
	ST_EB_CHARGING_CX_OTG,
	ST_EB_FULL,
	ST_CX_BAT_CRITICAL,
	ST_CX_BAT_HIGH,
};

enum cilix_events {
	EV_USB_CONN = 1,
	EV_USB_OUT,
	EV_CILIX_CONN,
	EV_CILIX_OUT,
	EV_EBOOK_BAT_FULL,
	EV_EBOOK_BAT_80_99,
	EV_EBOOK_BAT_70_79,
	EV_EBOOK_BAT_50_69,
	EV_EBOOK_BAT_0_49,
	EV_CILIX_BAT_70_100,
	EV_CILIX_BAT_20_49,
	EV_CILIX_BAT_0_19,
	EV_CILIX_BAT_OUT,
};

typedef struct status {
	int auto_mode;
	
	bool active;
	bool usb_conn;
	bool cilix_conn;
    int comm_good;
    bool in_reset_progress;
    bool in_setting_progress;

	int ebook_bat_capacity;
	int cilix_bat_capacity;
} status_t;

struct cilix {
	struct device			*dev;
	struct mutex			i2c_mutex;
	struct i2c_client		*client;
    struct miscdevice       misc_dev;
	struct hrtimer 			status_check_timer;
	struct delayed_work		status_check_work;
	struct delayed_work     set_charge_para_work;
	struct delayed_work     check_comm_work;
	struct delayed_work     reset_work;
	struct delayed_work     otg_on_work;
	status_t				*status;
    struct pinctrl          *pinctrl;

	unsigned int irq;
	unsigned int int_gpio;
	unsigned int detect_interval;
	unsigned int bat_critical_pct;
	unsigned int intrs;
	unsigned int cur_intrs;
    int eb_charge_type;
	wait_queue_head_t read_queue;
	bool waiting_i2c_response;
	enum cilix_states cur_state;
};

/*cilix finite state machine table*/
typedef struct cilix_fsm_table
{
	enum cilix_events cilix_event;
	enum cilix_states cur_state;
	int (*action)(struct cilix*);
	enum cilix_states next_state;
} cilix_fsm_table_t;

#endif
