/*
 * Allwinner SoCs display driver.
 *
 * Copyright (C) 2016 Allwinner.
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2.  This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#ifndef DISP_WAVEFORM_H
#define DISP_WAVEFORM_H

#include "../include.h"
#if 0
typedef enum update_mode {
	/* ALL AREA flush mode */
	EINK_INIT_MODE 	= 0x01,
	EINK_DU_MODE 		= 0x02,
	EINK_GC16_MODE 	= 0x04,
	EINK_A2_MODE 		= 0x10,
	EINK_GU16_MODE	= 0x84,

	/* RECTANGLE flush mode */
	EINK_DU_RECT_MODE 	= 0x402,
	EINK_GC16_RECT_MODE 	= 0x404,
	EINK_A2_RECT_MODE 	= 0x410,
	EINK_GC16_LOCAL_RECT_MODE = 0x484
}EINK_UPDATE_MODE;
#endif
typedef enum  {
	ED060SC4 = 0x01,
	ED060SC7 = 0x02,
	OPM060A1 = 0x03,
	ED060XC3 = 0x04,
	ED060XD4 = 0x05
} EINK_PANEL_TYPE;
/*
typedef enum  {
	EINK_BIT_1 = 0x01,
	EINK_BIT_2 = 0x02,
	EINK_BIT_3 = 0x03,
	EINK_BIT_4 = 0x04,
	EINK_BIT_5 = 0x05
}EINK_BIT_NUM;
*/

#if 0
#define DEBUG_WAVEFILE
#ifdef DEBUG_WAVEFILE
#define WF_DBG(msg, fmt...)		pr_warn(msg, ##fmt)
#define WF_INFO(msg, fmt...)		pr_warn(msg, ##fmt)
#define WF_WRN(msg, fmt...)		pr_warn(msg, ##fmt)
#define WF_ERR(msg, fmt...)		pr_err(msg, ##fmt)
#else
#define WF_DBG(msg, fmt...)
#define WF_INFO(msg, fmt...)
#define WF_WRN(msg, fmt...)		pr_warn(msg, ##fmt)
#define WF_ERR(msg, fmt...)		pr_err(msg, ##fmt)
#endif
#endif

extern unsigned int memory_addr_to_wavefile_addr(unsigned int paddr);
extern __s32 init_waveform(const char *path);
extern bool check_valid_waveform_mode(enum eink_update_mode mode);
extern bool is_local_update_mode(enum eink_update_mode mode);
extern int get_eink_panel_type(EINK_PANEL_TYPE *type);
extern int get_eink_panel_bit_num(enum  eink_bit_num *bit_num);
extern int get_waveform_data(enum eink_update_mode mode, u32 temp, u32 *total_frames, u32 *wf_paddr, u32 *wf_vaddr);
extern void free_waveform(void);
#endif

