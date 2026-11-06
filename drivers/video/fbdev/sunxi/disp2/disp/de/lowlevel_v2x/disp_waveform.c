/*
 * Allwinner SoCs display driver.
 *
 * Copyright (C) 2016 Allwinner.
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2.  This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

/*
Description: awf waveform file decoder
Version	: v0.1
Author	: oujunxi
Date	: 2015/07/10
*/

#include "disp_waveform.h"
#include "./../libeink.h"

#include "../../../../../../../../arch/arm/mach-sunxi/ntx_hwconfig.h"
extern volatile NTX_HWCONFIG *gptHWCFG;

//#define WAVE_LOAD_FROM_FILE  /* load waveform form file */

//#define DEBUG_WAVEFILE
#ifdef DEBUG_WAVEFILE
#define WF_DBG(msg, fmt...)		printk(KERN_WARNING msg, ##fmt)
#define WF_INFO(msg, fmt...)		printk(KERN_WARNING msg, ##fmt)
#define WF_WRN(msg, fmt...)		printk(KERN_WARNING msg, ##fmt)
#define WF_ERR(msg, fmt...)		printk(KERN_ERR msg, ##fmt)
#else
#define WF_DBG(msg, fmt...)
#define WF_INFO(msg, fmt...)	printk(msg, ##fmt)
#define WF_WRN(msg, fmt...)		printk(KERN_WARNING msg, ##fmt)
#define WF_ERR(msg, fmt...)		printk(KERN_ERR msg, ##fmt)
#endif


#define    C_HEADER_INFO_OFFSET               		0
#define    C_HEADER_TYPE_ID_OFFSET            		0         	//eink type(eink id)
#define    C_HEADER_VERSION_STR_OFFSET        	1
#define    C_HEADER_INFO_SIZE                 			128		//size of awf file header
#define    C_TEMP_TBL_OFFSET                  			(C_HEADER_INFO_OFFSET+C_HEADER_INFO_SIZE)	//temperature table offset from the beginning of the awf file
#define    C_TEMP_TBL_SIZE                    			32     	// temperature table size

#define    C_MODE_ADDR_TBL_OFFSET             		(C_TEMP_TBL_OFFSET+C_TEMP_TBL_SIZE)             		//mode address table offset
#define    C_MODE_ADDR_TBL_SIZE               		64

#define    C_INIT_MODE_ADDR_OFFSET            		C_MODE_ADDR_TBL_OFFSET
#define    C_GC16_MODE_ADDR_OFFSET            	(C_MODE_ADDR_TBL_OFFSET+4)
#define    C_GC4_MODE_ADDR_OFFSET             		(C_MODE_ADDR_TBL_OFFSET+8)
#define    C_DU_MODE_ADDR_OFFSET              		(C_MODE_ADDR_TBL_OFFSET+12)
#define    C_A2_MODE_ADDR_OFFSET              		(C_MODE_ADDR_TBL_OFFSET+16)
#define    C_GC16_LOCAL_MODE_ADDR_OFFSET    	(C_MODE_ADDR_TBL_OFFSET+20)
#define    C_GC4_LOCAL_MODE_ADDR_OFFSET   	(C_MODE_ADDR_TBL_OFFSET+24)
#define    C_A2_IN_MODE_ADDR_OFFSET 			(C_MODE_ADDR_TBL_OFFSET+28)
#define    C_A2_OUT_MODE_ADDR_OFFSET 		(C_MODE_ADDR_TBL_OFFSET+32)
#define    C_GL16_MODE_ADDR_OFFSET             	(C_MODE_ADDR_TBL_OFFSET+36)
#define    C_GLR16_MODE_ADDR_OFFSET             	(C_MODE_ADDR_TBL_OFFSET+40)
#define    C_GLD16_MODE_ADDR_OFFSET             	(C_MODE_ADDR_TBL_OFFSET+44)
#define    C_GCC16_MODE_ADDR_OFFSET		(C_MODE_ADDR_TBL_OFFSET+48)
#define    C_GCK16_MODE_ADDR_OFFSET             	(C_MODE_ADDR_TBL_OFFSET+52)
#define    C_GLK16_MODE_ADDR_OFFSET             	(C_MODE_ADDR_TBL_OFFSET+56)     

#define    C_INIT_MODE_OFFSET                 			(C_MODE_ADDR_TBL_OFFSET+C_MODE_ADDR_TBL_SIZE)

#define    C_REAL_TEMP_AREA_NUM               		15              //max temperature range number
#define    WF_MAX_COL                         				256		// GC16, 16*16 = 256


typedef struct {
	u8 load_flag;			//when awf has been loaded, init_flag = 1
	char* p_wf_vaddr;			//virtual address of waveform file
	u32 p_wf_paddr;			//phy address of waveform file
	char* p_wf_partial_vaddr;			//virtual address of waveform file
	u32 p_wf_partial_paddr;			//phy address of waveform file
	EINK_PANEL_TYPE eink_panel_type;		//eink type, example PVI, OED and etc
	char wavefile_name[128];
	u8 wf_temp_area_tbl[C_TEMP_TBL_SIZE];	//temperature table

	/*full phy address*/
	u32 p_init_wf;        		//init mode address (include temperature table address)
	u32 p_gc16_wf;           	//gc16 mode address (include temperature table address)
	u32 p_gc4_wf;            	//gc4 mode address (include temperature table address)
	u32 p_du_wf;             		//du mode address (include temperature table address)
	u32 p_A2_wf;             		//A2 mode address (include temperature table address)
	u32 p_gc16_local_wf;	//gc16 local mode address (include temperature table address)
	u32 p_gc4_local_wf;      	//gc4 local mode address (include temperature table address)
	u32 p_A2_in_wf;              	//A2 in mode address (include temperature table address)
	u32 p_A2_out_wf;              	//A2 out mode address (include temperature table address)
	u32 p_gl16_wf;              	//GL16 mode address (include temperature table address)
	u32 p_glr16_wf;              	//GLR16 mode address (include temperature table address)
	u32 p_gld16_wf;              	//GLD16 mode address (include temperature table address)
	u32 p_gcc16_wf;		//GLD16 mode address (include temperature table address)
	u32 p_gck16_wf;              	//GCK16 mode address (include temperature table address)
	u32 p_glk16_wf;              	//GLK16 mode address (include temperature table address)
	
	/*partial phy address*/
	u32 p_gc16_partial_wf;
	u32 p_du_partial_wf;
	u32 p_A2_partial_wf;
	u32 p_gl16_partial_wf;
	u32 p_glr16_partial_wf;
	u32 p_gld16_partial_wf;
	u32 p_gcc16_partial_wf;
	u32 p_gck16_partial_wf;
	u32 p_glk16_partial_wf;
}AWF_WAVEFILE;

static AWF_WAVEFILE g_waveform_file;

static unsigned char *gpbWF_vaddr = NULL;
static unsigned char *gpbWF_paddr = NULL;
static unsigned long gdwWF_size = 0;
static bool gu_mode_exist = true;
static int libeink_version = 0;

/*
Description	: get temperature range index from temperature table, if temperature match
			  TBL[id] <= temperature < TBL[id + 1] condition, then temp range index = id
Input		: temperature -- temperature of eink panel
Output		: None
Return		: 0 & positive -- temperature range index, negative -- fail
*/
static __s32 get_temp_range_index(int temperature, u32 *p_mode_virt_addr)
{
	__s32 index = -EINVAL;
	u32 mode_temp_offset = 0;

	for(index = 0; index < C_TEMP_TBL_SIZE; index++) {

		mode_temp_offset =  *((u32 *)p_mode_virt_addr + index);		
		if(((0 == mode_temp_offset) || (g_waveform_file.wf_temp_area_tbl[index] == 0)) && (index>0)) {
			break;
		}

		if(temperature <= g_waveform_file.wf_temp_area_tbl[index]) {
			break;
		}
	}

	if(index > 0) {
		index -= 1;
	}

	return index;
}

/*
Description	: get mode address according to flush mode
Input		: mode -- flush mode
Output		: None
Return		: NULL -- get mode address fail, others -- get mode address successfully
*/
static u8 *get_mode_virt_address(enum eink_update_mode mode)
{
	u8 *p_wf_file = NULL;
	u32 offset = 0;
    
	switch (mode & 0xff)
	{
		case EINK_INIT_MODE:
		{
			offset = (u32)g_waveform_file.p_init_wf - g_waveform_file.p_wf_paddr;
			if (offset != 0)
				p_wf_file = (u8 *)g_waveform_file.p_wf_vaddr + offset;
			else
				p_wf_file = NULL;
			break;
		}

		case EINK_DU_MODE:
		{
			if (mode & 0x400) {
				offset = (u32)g_waveform_file.p_du_partial_wf - g_waveform_file.p_wf_partial_paddr;
				p_wf_file = (u8 *)g_waveform_file.p_wf_partial_vaddr + offset;
			} else {
				offset = (u32)g_waveform_file.p_du_wf - g_waveform_file.p_wf_paddr;
				p_wf_file = (u8 *)g_waveform_file.p_wf_vaddr + offset;
			}
			if (offset == 0)
				p_wf_file = NULL;
			break;
		}

		case EINK_GC16_MODE:
		{
			if (mode & 0x400) {
				offset = (u32)g_waveform_file.p_gc16_partial_wf - g_waveform_file.p_wf_partial_paddr;
				p_wf_file = (u8 *)g_waveform_file.p_wf_partial_vaddr + offset;
			} else {
				offset = (u32)g_waveform_file.p_gc16_wf - g_waveform_file.p_wf_paddr;
				p_wf_file = (u8 *)g_waveform_file.p_wf_vaddr + offset;
			}
			if (offset == 0)
				p_wf_file = NULL;
			break;
		}

		case EINK_GC4_MODE:
		{
			offset = (u32)g_waveform_file.p_gc4_wf - g_waveform_file.p_wf_paddr;
			if (offset != 0)
				p_wf_file = (u8 *)g_waveform_file.p_wf_vaddr + offset;
			else
				p_wf_file = NULL;
			break;
		}

		case EINK_GC4L_MODE:
		{
			offset = (u32)g_waveform_file.p_gc4_local_wf - g_waveform_file.p_wf_paddr;
			if (offset != 0)
				p_wf_file = (u8 *)g_waveform_file.p_wf_vaddr + offset;
			else
				p_wf_file = NULL;
			break;
		}

		case EINK_A2_MODE:
		{
			if (mode & 0x400) {
				offset = (u32)g_waveform_file.p_A2_partial_wf - g_waveform_file.p_wf_partial_paddr;
				p_wf_file = (u8 *)g_waveform_file.p_wf_partial_vaddr + offset;
			} else {
				offset = (u32)g_waveform_file.p_A2_wf - g_waveform_file.p_wf_paddr;
				p_wf_file = (u8 *)g_waveform_file.p_wf_vaddr + offset;
			}
			if (offset == 0)
				p_wf_file = NULL;
			break;
		}

		case EINK_GU16_MODE:
		{
			if (gu_mode_exist == true)
				offset = (u32)g_waveform_file.p_gc16_local_wf - g_waveform_file.p_wf_paddr;
			else
				offset = (u32)g_waveform_file.p_gc16_local_wf - g_waveform_file.p_wf_partial_paddr;
			if (offset != 0)
				p_wf_file = (u8 *)g_waveform_file.p_wf_vaddr + offset;
			else
				p_wf_file = NULL;
			break;
		}

		case EINK_CLEAR_MODE:
		{
			offset = (u32)g_waveform_file.p_gc16_local_wf - g_waveform_file.p_wf_paddr;
			if (offset != 0)
				p_wf_file = (u8 *)g_waveform_file.p_wf_vaddr + offset;
			else
				p_wf_file = NULL;
			break;
		}
		case EINK_GL16_MODE:
		{
			if (mode & 0x400) {
				offset = (u32)g_waveform_file.p_gl16_partial_wf - g_waveform_file.p_wf_partial_paddr;
				p_wf_file = (u8 *)g_waveform_file.p_wf_partial_vaddr + offset;
			} else {
				offset = (u32)g_waveform_file.p_gl16_wf - g_waveform_file.p_wf_paddr;
				p_wf_file = (u8 *)g_waveform_file.p_wf_vaddr + offset;
			}
			if (offset == 0)
				p_wf_file = NULL;
			break;
		}

		case EINK_GLR16_MODE:
		{
			if (mode & 0x400) {
				offset = (u32)g_waveform_file.p_glr16_partial_wf - g_waveform_file.p_wf_partial_paddr;
				p_wf_file = (u8 *)g_waveform_file.p_wf_partial_vaddr + offset;
			} else {
				offset = (u32)g_waveform_file.p_glr16_wf - g_waveform_file.p_wf_paddr;
				p_wf_file = (u8 *)g_waveform_file.p_wf_vaddr + offset;
			}
			if (offset == 0)
				p_wf_file = NULL;
			break;
		}

		case EINK_GLD16_MODE:
		{
			if (mode & 0x400) {
				offset = (u32)g_waveform_file.p_gld16_partial_wf - g_waveform_file.p_wf_partial_paddr;
				p_wf_file = (u8 *)g_waveform_file.p_wf_partial_vaddr + offset;
			} else {
				offset = (u32)g_waveform_file.p_gld16_wf - g_waveform_file.p_wf_paddr;
				p_wf_file = (u8 *)g_waveform_file.p_wf_vaddr + offset;
			}
			if (offset == 0)
				p_wf_file = NULL;
			break;
		}

		case EINK_GCC16_MODE:
		{
			if (mode & 0x400) {
				offset = (u32)g_waveform_file.p_gcc16_partial_wf - g_waveform_file.p_wf_partial_paddr;
				p_wf_file = (u8 *)g_waveform_file.p_wf_partial_vaddr + offset;
			} else {
				offset = (u32)g_waveform_file.p_gcc16_wf - g_waveform_file.p_wf_paddr;
				p_wf_file = (u8 *)g_waveform_file.p_wf_vaddr + offset;
			}
			if (offset == 0)
				p_wf_file = NULL;
			break;
		}
		
		case EINK_GCK16_MODE:	
		{
			if (mode & 0x400) {
				offset = (u32)g_waveform_file.p_gck16_partial_wf - g_waveform_file.p_wf_partial_paddr;
				p_wf_file = (u8 *)g_waveform_file.p_wf_partial_vaddr + offset;
			} else {
				offset = (u32)g_waveform_file.p_gck16_wf - g_waveform_file.p_wf_paddr;
				p_wf_file = (u8 *)g_waveform_file.p_wf_vaddr + offset;
			}
			if (offset == 0)
				p_wf_file = NULL;
			break;
		}
		
		case EINK_GLK16_MODE:
		{
			if (mode & 0x400) {
				offset = (u32)g_waveform_file.p_glk16_partial_wf - g_waveform_file.p_wf_partial_paddr;
				p_wf_file = (u8 *)g_waveform_file.p_wf_partial_vaddr + offset;
			} else {
				offset = (u32)g_waveform_file.p_glk16_wf - g_waveform_file.p_wf_paddr;
				p_wf_file = (u8 *)g_waveform_file.p_wf_vaddr + offset;
			}
			if (offset == 0)
				p_wf_file = NULL;
			break;
		}
		
		default:
		{
			WF_WRN("unkown mode(0x%x)\n", mode);
			p_wf_file = NULL;
			break;
		}
	}

	return p_wf_file;
}

/*
Description	: get mode address according to flush mode
Input		: mode -- flush mode
Output		: None
Return		: NULL -- get mode address fail, others -- get mode address successfully
*/
static u32 get_mode_phy_address(enum eink_update_mode mode)
{
	u32 phy_addr = 0;

	switch (mode & 0xff)
	{
		case EINK_INIT_MODE:
		{
			phy_addr = g_waveform_file.p_init_wf;
			break;
		}

		case EINK_DU_MODE:
		{
			if (mode & 0x400)
				phy_addr = g_waveform_file.p_du_partial_wf;
			else
				phy_addr = g_waveform_file.p_du_wf;
			break;
		}

		case EINK_GC16_MODE:
		{
			if (mode & 0x400)
				phy_addr = g_waveform_file.p_gc16_partial_wf;
			else
				phy_addr = g_waveform_file.p_gc16_wf;
			break;
		}

		case EINK_GC4_MODE:
		{
			phy_addr = g_waveform_file.p_gc4_wf;
			break;
		}

		case EINK_GC4L_MODE:
		{
			phy_addr = g_waveform_file.p_gc4_local_wf;
			break;
		}

		case EINK_A2_MODE:
		{
			if (mode & 0x400)
				phy_addr = g_waveform_file.p_A2_partial_wf;
			else
				phy_addr = g_waveform_file.p_A2_wf;
			break;
		}

		case EINK_GU16_MODE:
		{
			phy_addr = g_waveform_file.p_gc16_local_wf;
			break;
		}

		case EINK_CLEAR_MODE:
		{
			phy_addr = g_waveform_file.p_gc16_local_wf;
			break;
		}
		
		case EINK_GL16_MODE:
		{
			if (mode & 0x400)
				phy_addr = g_waveform_file.p_gl16_partial_wf;
			else
				phy_addr = g_waveform_file.p_gl16_wf;
			break;
		}

		case EINK_GLR16_MODE:
		{
			if (mode & 0x400)
				phy_addr = g_waveform_file.p_glr16_partial_wf;
			else
				phy_addr = g_waveform_file.p_glr16_wf;
			break;
		}
		
		case EINK_GLD16_MODE:
		{
			if (mode & 0x400)
				phy_addr = g_waveform_file.p_gld16_partial_wf;
			else
				phy_addr = g_waveform_file.p_gld16_wf;
			break;
		}

		case EINK_GCC16_MODE:
		{
			if (mode & 0x400)
				phy_addr = g_waveform_file.p_gcc16_partial_wf;
			else
				phy_addr = g_waveform_file.p_gcc16_wf;
			break;
		}
		
		case EINK_GCK16_MODE:
		{
			if (mode & 0x400)
				phy_addr = g_waveform_file.p_gck16_partial_wf;
			else
				phy_addr = g_waveform_file.p_gck16_wf;
			break;
		}
        
		case EINK_GLK16_MODE:
		{
			if (mode & 0x400)
				phy_addr = g_waveform_file.p_glk16_partial_wf;
			else
				phy_addr = g_waveform_file.p_glk16_wf;
			break;
		}

		default:
		{
			WF_WRN("unkown mode(0x%x)\n", mode);
			phy_addr = 0;
			break;
		}
	}

	return phy_addr;
}

bool check_valid_waveform_mode(enum eink_update_mode mode)
{
	bool ret = false;

	if (EINK_AUTO_MODE == mode) {
		return true;
	}
	
	switch (mode & 0xff)
	{
		case EINK_INIT_MODE:
		case EINK_DU_MODE:
		case EINK_GC16_MODE:
		case EINK_GC4_MODE:
		case EINK_A2_MODE:			
		case EINK_GL16_MODE:
		case EINK_GLR16_MODE:	
		case EINK_GLD16_MODE:
		case EINK_GU16_MODE:
		case EINK_GC4L_MODE:
		case EINK_GCC16_MODE:
		case EINK_GCK16_MODE:	
		case EINK_GLK16_MODE:
		case EINK_CLEAR_MODE:
			ret = true;
			break;
		default:
			ret = false;
			break;
	}
	
	return ret;
}

bool is_local_update_mode(enum eink_update_mode mode)
{
	bool ret = false;
	switch (mode & 0xff) {
		case EINK_DU_MODE:
			ret = true;
			break;
		default:
			ret = false;
			break;
	}
	return ret;
}

unsigned int memory_addr_to_wavefile_addr(unsigned int paddr)
{
	return (unsigned int)(paddr - g_waveform_file.p_wf_paddr);
}

static void print_wavefile_mode_mapping(AWF_WAVEFILE wf)
{
	WF_INFO("INIT mode wavefile offset = 0x%08x\n", wf.p_init_wf - wf.p_wf_paddr);
	WF_INFO("GC16 mode wavefile offset = 0x%08x\n", wf.p_gc16_wf - wf.p_wf_paddr);
	WF_INFO("GC4 mode wavefile offset = 0x%08x\n", wf.p_gc4_wf - wf.p_wf_paddr);
	WF_INFO("DU mode wavefile offset = 0x%08x\n", wf.p_du_wf - wf.p_wf_paddr);
	WF_INFO("A2 mode wavefile offset = 0x%08x\n", wf.p_A2_wf - wf.p_wf_paddr);
	WF_INFO("A2_IN mode wavefile offset = 0x%08x\n", wf.p_A2_in_wf - wf.p_wf_paddr);
	WF_INFO("A2_OUT mode wavefile offset = 0x%08x\n", wf.p_A2_out_wf - wf.p_wf_paddr);
	WF_INFO("GL16 mode wavefile offset = 0x%08x\n", wf.p_gl16_wf - wf.p_wf_paddr);
	WF_INFO("GLR16 mode wavefile offset = 0x%08x\n", wf.p_glr16_wf - wf.p_wf_paddr);
	WF_INFO("GLD16 mode wavefile offset = 0x%08x\n", wf.p_gld16_wf - wf.p_wf_paddr);
	WF_INFO("GCC16 mode wavefile offset = 0x%08x\n", wf.p_gcc16_wf - wf.p_wf_paddr);
	WF_INFO("GCK16 mode wavefile offset = 0x%08x\n", wf.p_gck16_wf - wf.p_wf_paddr);
	WF_INFO("GLK16 mode wavefile offset = 0x%08x\n", wf.p_glk16_wf - wf.p_wf_paddr);
}

static void * _MemoryRequest(void *addr, u32 len, const char * name)
{
	void * mem = NULL;

	do {
		printk(KERN_DEBUG "***%s:%d: request memory region! addr=%p, len=%hd***\n",
			__FUNCTION__, __LINE__, addr, len);

		if (!request_mem_region((u32)addr, len, name)) {
			printk(KERN_CRIT "%s():  request memory region failed! addr=%p, len %hd\n",__FUNCTION__, addr, len);
			break;
		}
		mem = (void *) ioremap_nocache((u32)addr, len);
		if (!mem) {
			printk(KERN_CRIT "***%s:%d: could not ioremap %s***\n", __FUNCTION__, __LINE__, name);
			release_mem_region((u32)addr, len);
			break;
		}
	} while (0);
	
	
	return mem;
}

static int waveform_p_setup(char *str)
{
	gpbWF_paddr = (unsigned char *)simple_strtoul(str,NULL,0);
	if(NULL==gpbWF_vaddr) {
		gpbWF_vaddr = (unsigned char *)_MemoryRequest((u32)gpbWF_paddr, gdwWF_size, "waveform_p");
		if(!gpbWF_vaddr) {
			return 0;
		}
	}
	printk("%s() wf_p=%p,vaddr=%p,size=%lu\n",__FUNCTION__, gpbWF_paddr,gpbWF_vaddr,gdwWF_size);
    
	return 1;
}

static int waveform_size_setup(char *str)
{
	gdwWF_size = (unsigned long)simple_strtoul(str,NULL,0);
	printk("%s() wf_size=%lu\n",__FUNCTION__,gdwWF_size);
	return 1;
}

void parse_waveform_cmdline(void)
{
	static int iParseCnt = 0;
	char *pcPatternStart,*pcPatternVal,*pcPatternValEnd,cTempStore='\0';
	unsigned long ulPatternLen;

	char *szParsePatternA[]={"waveform_sz=", "waveform_p="};
	int ((*pfnDispatchA[])(char *str))={waveform_size_setup, waveform_p_setup};
 
	int i;
	char *pszCmdLineBuf;
 
	if(iParseCnt++>0) {
		printk("%s : epd cmdline parse already done .\n",__FUNCTION__);
		return ;
	}
	printk("%s():cmdline(%d)=%s\n",__FUNCTION__,strlen(saved_command_line),saved_command_line);

	pszCmdLineBuf = kmalloc(strlen(saved_command_line)+1,GFP_KERNEL);
	strcpy(pszCmdLineBuf,saved_command_line);
	printk("%s():cp cmdline=%s\n",__FUNCTION__,pszCmdLineBuf);
    
	for(i=0;i<sizeof(szParsePatternA)/sizeof(szParsePatternA[0]);i++) {
		ulPatternLen = strlen(szParsePatternA[i]);
		pcPatternStart = strstr(pszCmdLineBuf,szParsePatternA[i]);
		if(pcPatternStart) {
			pcPatternVal=pcPatternStart + ulPatternLen ;
			pcPatternValEnd = strchr(pcPatternVal,' ');
			if(pcPatternValEnd) {
				cTempStore = *pcPatternValEnd;
				*pcPatternValEnd = '\0';
			}
			printk("%s():pattern \"%s\" ,val=\"%s\"\n",__FUNCTION__,szParsePatternA[i],pcPatternVal);
			pfnDispatchA[i](pcPatternVal);
			if(pcPatternValEnd) {
				*pcPatternValEnd = cTempStore;
			}
		}
	}

	if(pszCmdLineBuf) {
		kfree(pszCmdLineBuf);
	}

      return;
}

/* for Partial update use (update mode has EINK_PARTIAL_MODE) */
static int init_partial_waveform(u32 src_addr, EINK_PANEL_TYPE bit_num) 
{
    u32 mode_offset;
    u8 *p_mode_virt_addr = NULL;
    u8 *p_mode_temp_addr = NULL;
    u8 *temp_addr = NULL;
    u32 temp;
    u32 temp_offset;
    u32 grey_level = 0;
    int headinfo;
    int frame_nums, frame_size;
    int frm, i, j;
    char wavedata[1024];
    int temp0_frame_nums;
    
    if (src_addr == g_waveform_file.p_wf_partial_paddr)
        return -1;
    
    if (bit_num == EINK_BIT_5)
        grey_level = 32;
    else if (bit_num == EINK_BIT_4)
        grey_level = 16;
    
    for (temp = 0; temp < C_TEMP_TBL_SIZE; temp++) {
        mode_offset = (u32)src_addr - g_waveform_file.p_wf_partial_paddr;
        p_mode_virt_addr = (u8 *)g_waveform_file.p_wf_partial_vaddr + mode_offset;
        temp_offset = *((u32 *)p_mode_virt_addr + temp);
        if (!temp_offset)
            break;
        
        p_mode_temp_addr = (u8 *)p_mode_virt_addr + temp_offset;
        
        memcpy(&headinfo, p_mode_temp_addr, sizeof(int));
        frame_nums = headinfo & 0xffff;
        frame_size = headinfo >> 16;
        WF_DBG("temp %d, frame nums: %d, frame_size = %d\n", temp, frame_nums, frame_size);

	/* E60P22 use allwinner all tool, will let waveform format have something wrong */
        if (gptHWCFG->m_val.bPCB == 0x5b)
            frame_size = 1024;

        
        if (temp == 0)
            temp0_frame_nums = frame_nums;
        else {
            if (frame_nums >= temp0_frame_nums || frame_size <= 0 || frame_size != (grey_level * grey_level)) {
                WF_ERR("get waveform data failed, please check waveform file");
                return -1;
            }
        }
        
        p_mode_temp_addr += 4; /* skip total frame(2 Byte) and table size(2 Byte) */
        
        for (frm = 0; frm < frame_nums; frm++) {
            for (i = 0; i < frame_size; i++)
            {
                if ((i / grey_level) == (i % grey_level))
                    memset(p_mode_temp_addr, 0, sizeof(u8));
                p_mode_temp_addr++;
            }
        }
    }
    WF_DBG("Create partrial waveform finish\n");
    
    return 0;
}

/* Prevent customer UI use waveform do not exist */
static int waveform_mode_check(bool regal) 
{
    int ret = 0;
    
    ret |= init_partial_waveform(g_waveform_file.p_gc16_partial_wf, g_waveform_file.eink_panel_type);
    ret |= init_partial_waveform(g_waveform_file.p_du_partial_wf, g_waveform_file.eink_panel_type);
    ret |= init_partial_waveform(g_waveform_file.p_A2_partial_wf, g_waveform_file.eink_panel_type);    
    ret |= init_partial_waveform(g_waveform_file.p_gl16_partial_wf, g_waveform_file.eink_panel_type);
    if (ret) {
	WF_INFO("%s() : create GC16/DU/A2/GL16 partial waveform failed\n", __func__);
        //goto error;
    }
    
	if(g_waveform_file.p_gc16_local_wf==g_waveform_file.p_wf_paddr) {
		if(g_waveform_file.p_gl16_partial_wf!=g_waveform_file.p_wf_partial_paddr) {
			WF_INFO("%s() : use gl16(P) instead of null gc16_local \n",__func__);
			g_waveform_file.p_gc16_local_wf = g_waveform_file.p_gl16_partial_wf;
			gu_mode_exist = false;
		} else if (g_waveform_file.p_gl16_wf!=g_waveform_file.p_wf_paddr) {
			WF_INFO("%s() : use gl16 instead of null gc16_local \n",__func__);
			g_waveform_file.p_gc16_local_wf = g_waveform_file.p_gl16_wf;
			gu_mode_exist = true;
		}
	}
	
	if (g_waveform_file.p_glr16_wf == g_waveform_file.p_wf_paddr || regal == false) {
		if (g_waveform_file.p_gl16_wf != g_waveform_file.p_wf_paddr) {
			WF_INFO("%s() : use gl16 instead of null glr16 \n",__func__);
			g_waveform_file.p_glr16_wf = g_waveform_file.p_gl16_wf;
		}
	}
	
	if (g_waveform_file.p_gld16_wf == g_waveform_file.p_wf_paddr || regal == false) {
		if (g_waveform_file.p_gc16_wf != g_waveform_file.p_wf_paddr) {
			WF_INFO("%s() : use gc16 instead of null gld16 \n",__func__);
			g_waveform_file.p_gld16_wf = g_waveform_file.p_gc16_wf;
		}
	}
	
	if (g_waveform_file.p_gcc16_wf == g_waveform_file.p_wf_paddr) {
		if (g_waveform_file.p_gc16_wf != g_waveform_file.p_wf_paddr) {
			WF_INFO("%s() : use gc16 instead of null gcc16 \n",__func__);
			g_waveform_file.p_gcc16_wf = g_waveform_file.p_gc16_wf;
		}
	}
	
	if (g_waveform_file.p_glk16_wf == g_waveform_file.p_wf_paddr) {
		if (g_waveform_file.p_gl16_wf != g_waveform_file.p_wf_paddr) {
			WF_INFO("%s() : use gl16 instead of null glk16 \n",__func__);
			g_waveform_file.p_glk16_wf = g_waveform_file.p_gl16_wf;
		}
	}
	
	if (g_waveform_file.p_gck16_wf == g_waveform_file.p_wf_paddr) {
		if (g_waveform_file.p_gc16_wf != g_waveform_file.p_wf_paddr) {
			WF_INFO("%s() : use gc16 instead of null gck16 \n",__func__);
			g_waveform_file.p_gck16_wf = g_waveform_file.p_gc16_wf;
		}
	}
	
	if (g_waveform_file.p_glr16_partial_wf == g_waveform_file.p_wf_partial_paddr || regal == false) {
		if (g_waveform_file.p_gl16_partial_wf != g_waveform_file.p_wf_partial_paddr) {
			WF_INFO("%s() : use gl16(P) instead of null glk16(P) \n",__func__);
			g_waveform_file.p_glr16_partial_wf = g_waveform_file.p_gl16_partial_wf;
		}
	} else {
        ret |= init_partial_waveform(g_waveform_file.p_glr16_partial_wf, g_waveform_file.eink_panel_type);
        if (ret)
            goto error;
    }
	
	if (g_waveform_file.p_gld16_partial_wf == g_waveform_file.p_wf_partial_paddr || regal == false) {
		if (g_waveform_file.p_gc16_partial_wf != g_waveform_file.p_wf_partial_paddr) {
			WF_INFO("%s() : use gc16(P) instead of null gld16(P) \n",__func__);
			g_waveform_file.p_gld16_partial_wf = g_waveform_file.p_gc16_partial_wf;
		}
	} else {
        ret |= init_partial_waveform(g_waveform_file.p_gld16_partial_wf, g_waveform_file.eink_panel_type);
        if (ret)
            goto error; 
    }
    
    if (g_waveform_file.p_gcc16_partial_wf == g_waveform_file.p_wf_partial_paddr) {
		if (g_waveform_file.p_gc16_partial_wf != g_waveform_file.p_wf_partial_paddr) {
			WF_INFO("%s() : use gc16(P) instead of null gcc16(P) \n",__func__);
			g_waveform_file.p_gcc16_partial_wf = g_waveform_file.p_gc16_partial_wf;
		}
	} else {
        ret |= init_partial_waveform(g_waveform_file.p_gcc16_partial_wf, g_waveform_file.eink_panel_type);
        if (ret)
            goto error; 
    }
	
	if (g_waveform_file.p_glk16_partial_wf == g_waveform_file.p_wf_partial_paddr) {
		if (g_waveform_file.p_gl16_partial_wf != g_waveform_file.p_wf_partial_paddr) {
			WF_INFO("%s() : use gl16(P) instead of null glk16(P) \n",__func__);
			g_waveform_file.p_glk16_partial_wf = g_waveform_file.p_gl16_partial_wf;
		}
	} else {
        ret |= init_partial_waveform(g_waveform_file.p_glk16_partial_wf, g_waveform_file.eink_panel_type);
        if (ret)
            goto error;
    }
	
	if (g_waveform_file.p_gck16_partial_wf == g_waveform_file.p_wf_partial_paddr) {
		if (g_waveform_file.p_gc16_partial_wf != g_waveform_file.p_wf_partial_paddr) {
			WF_INFO("%s() : use gc16(P) instead of null gck16(P) \n",__func__);
			g_waveform_file.p_gck16_partial_wf = g_waveform_file.p_gc16_partial_wf;
		}
	} else {
        ret |= init_partial_waveform(g_waveform_file.p_gck16_partial_wf, g_waveform_file.eink_panel_type);
        if (ret)
            goto error;
    }
    
error:
	
	return ret;
}

/*
Description	: load waveform file to memory, and save each flush mode of address. This function
			  should be called before waveform data has been used.
Input		: path -- path of waveform file
Output		: None
Return		: 0 -- load waveform ok, others -- fail
*/
extern void *disp_eink_malloc(u32 num_bytes, void *phy_addr);
extern void disp_eink_cache_sync(const void *start_addr, const void *end_addr);
extern void disp_eink_free(void *virt_addr, void* phys_addr, u32 num_bytes);
__s32 init_waveform(const char *path)
{
	struct file *fp = NULL;
	__s32 file_len = 0;
	__s32 read_len = 0;
	mm_segment_t fs;
	loff_t pos;
	__u32 wf_buffer_len = 0;           //the len of waveform file
	u32 *pAddr = NULL;
	__s32 ret = -EINVAL;
    bool regal = false;
	
#ifdef WAVE_LOAD_FROM_FILE
	if (NULL == path) {
		WF_ERR("path is null\n");
		return -EINVAL;
	}

	WF_DBG("starting to load awf waveform file(%s)\n", path);
	fp = filp_open(path, O_RDONLY, 0);
	if(IS_ERR(fp))	{
		WF_ERR("fail to open waveform file(%s)", path);
		return -EBADF;
	}
#endif

	if (g_waveform_file.load_flag == 1)
		return 0;

	memset(&g_waveform_file, 0, sizeof(g_waveform_file));
	
#ifdef WAVE_LOAD_FROM_FILE
	fs = get_fs();
	set_fs(KERNEL_DS);
	pos = 0;
	file_len = fp->f_path.dentry->d_inode->i_size;
	//wf_buffer_len = file_len+1023;

	g_waveform_file.p_wf_vaddr= (char *)disp_eink_malloc(file_len, (u32 *)(&g_waveform_file.p_wf_paddr));
	if(NULL == g_waveform_file.p_wf_vaddr) {
		WF_ERR("fail to alloc memory for waveform file, len=%d\n", file_len);
		ret = -ENOMEM;
		goto error;
	}

	read_len = vfs_read(fp, (char *)g_waveform_file.p_wf_vaddr, file_len, &pos);
	if(read_len != file_len) {
		WF_ERR("maybe miss some data(read=%d byte, file=%d byte) when reading waveform file\n", read_len, file_len);
		ret = -EAGAIN;
		goto error;
	}
	
#else
	/* parse kernel command line to get waveform size and waveform paddr */
	parse_waveform_cmdline();
	
	if (gdwWF_size == 0 || gpbWF_paddr == NULL || gpbWF_vaddr == NULL) {
		WF_ERR("can't get waveform data offset from boot command line!\n");
		g_waveform_file.load_flag = 0;
		return -1;
	}
	
	g_waveform_file.p_wf_vaddr= (char *)disp_eink_malloc(gdwWF_size, (u32 *)(&g_waveform_file.p_wf_paddr));
	if(NULL == g_waveform_file.p_wf_vaddr) {
		WF_ERR("fail to alloc memory for waveform data, len=%d\n", gdwWF_size);
		ret = -ENOMEM;
		goto error;
	}
	
	g_waveform_file.p_wf_partial_vaddr= (char *)disp_eink_malloc(gdwWF_size, (u32 *)(&g_waveform_file.p_wf_partial_paddr));
	if(NULL == g_waveform_file.p_wf_partial_vaddr) {
		WF_ERR("fail to alloc memory for waveform data, len=%d\n", gdwWF_size);
		ret = -ENOMEM;
		goto error;
	}
	
	memcpy(g_waveform_file.p_wf_vaddr, gpbWF_vaddr, gdwWF_size);
	memcpy(g_waveform_file.p_wf_partial_vaddr, gpbWF_vaddr, gdwWF_size);
	
#endif
    
    ret = EInk_Init(g_waveform_file.p_wf_vaddr);
    if(ret) {
        WF_ERR("EInk_Init failed, the regal will be invalid, ret=%d\n", ret);
    } else
        regal = true;

	g_waveform_file.eink_panel_type = *((u8 *)g_waveform_file.p_wf_vaddr);
	WF_DBG("eink type=0x%x\n", g_waveform_file.eink_panel_type);

	memset(g_waveform_file.wavefile_name, 0, sizeof(g_waveform_file.wavefile_name));
	memcpy(g_waveform_file.wavefile_name, (g_waveform_file.p_wf_vaddr + 1), (sizeof(g_waveform_file.wavefile_name) -1));
	WF_DBG("wavefile info: %s\n", g_waveform_file.wavefile_name);

	//starting to load data
	memcpy(g_waveform_file.wf_temp_area_tbl,(g_waveform_file.p_wf_vaddr+C_TEMP_TBL_OFFSET),C_TEMP_TBL_SIZE);

	pAddr = (u32 *)(g_waveform_file.p_wf_vaddr + C_INIT_MODE_ADDR_OFFSET);
	g_waveform_file.p_init_wf = (u32)(g_waveform_file.p_wf_paddr+ *pAddr);

	pAddr = (u32 *)(g_waveform_file.p_wf_vaddr + C_GC16_MODE_ADDR_OFFSET);
	g_waveform_file.p_gc16_wf = (u32)(g_waveform_file.p_wf_paddr + *pAddr);
    
	pAddr = (u32 *)(g_waveform_file.p_wf_partial_vaddr + C_GC16_MODE_ADDR_OFFSET); /* GC16 patrial */
	g_waveform_file.p_gc16_partial_wf = (u32)(g_waveform_file.p_wf_partial_paddr + *pAddr);

	pAddr = (u32 *)(g_waveform_file.p_wf_vaddr + C_GC4_MODE_ADDR_OFFSET);
	g_waveform_file.p_gc4_wf = (u32)(g_waveform_file.p_wf_paddr + *pAddr);

	pAddr = (u32 *)(g_waveform_file.p_wf_vaddr + C_DU_MODE_ADDR_OFFSET);
	g_waveform_file.p_du_wf = (u32)(g_waveform_file.p_wf_paddr + *pAddr);
    
	pAddr = (u32 *)(g_waveform_file.p_wf_partial_vaddr + C_DU_MODE_ADDR_OFFSET); /* DU patrial */
	g_waveform_file.p_du_partial_wf = (u32)(g_waveform_file.p_wf_partial_paddr + *pAddr);

	pAddr = (u32 *)(g_waveform_file.p_wf_vaddr + C_A2_MODE_ADDR_OFFSET);
	g_waveform_file.p_A2_wf = (u32)(g_waveform_file.p_wf_paddr + *pAddr);
    
	pAddr = (u32 *)(g_waveform_file.p_wf_partial_vaddr + C_A2_MODE_ADDR_OFFSET); /* A2 patrial */
	g_waveform_file.p_A2_partial_wf = (u32)(g_waveform_file.p_wf_partial_paddr + *pAddr);

	pAddr = (u32 *)(g_waveform_file.p_wf_vaddr + C_GC16_LOCAL_MODE_ADDR_OFFSET);
	g_waveform_file.p_gc16_local_wf = (u32)(g_waveform_file.p_wf_paddr + *pAddr);

	pAddr = (u32 *)(g_waveform_file.p_wf_vaddr + C_GC4_LOCAL_MODE_ADDR_OFFSET);
	g_waveform_file.p_gc4_local_wf = (u32)(g_waveform_file.p_wf_paddr + *pAddr);

	pAddr = (u32 *)(g_waveform_file.p_wf_vaddr + C_A2_IN_MODE_ADDR_OFFSET);
	g_waveform_file.p_A2_in_wf = (u32)(g_waveform_file.p_wf_paddr + *pAddr);

	pAddr = (u32 *)(g_waveform_file.p_wf_vaddr + C_A2_OUT_MODE_ADDR_OFFSET);
	g_waveform_file.p_A2_out_wf = (u32)(g_waveform_file.p_wf_paddr + *pAddr);

	pAddr = (u32 *)(g_waveform_file.p_wf_vaddr + C_GL16_MODE_ADDR_OFFSET);
	g_waveform_file.p_gl16_wf = (u32)(g_waveform_file.p_wf_paddr + *pAddr);
    
	pAddr = (u32 *)(g_waveform_file.p_wf_partial_vaddr + C_GL16_MODE_ADDR_OFFSET); /* GL16 parital */
	g_waveform_file.p_gl16_partial_wf = (u32)(g_waveform_file.p_wf_partial_paddr + *pAddr);
	
	pAddr = (u32 *)(g_waveform_file.p_wf_vaddr + C_GLR16_MODE_ADDR_OFFSET);
	g_waveform_file.p_glr16_wf = (u32)(g_waveform_file.p_wf_paddr + *pAddr);
    
	pAddr = (u32 *)(g_waveform_file.p_wf_partial_vaddr + C_GLR16_MODE_ADDR_OFFSET); /* GLR16 partial */
	g_waveform_file.p_glr16_partial_wf = (u32)(g_waveform_file.p_wf_partial_paddr + *pAddr);

	pAddr = (u32 *)(g_waveform_file.p_wf_vaddr + C_GLD16_MODE_ADDR_OFFSET);
	g_waveform_file.p_gld16_wf = (u32)(g_waveform_file.p_wf_paddr + *pAddr);
    
	pAddr = (u32 *)(g_waveform_file.p_wf_partial_vaddr + C_GLD16_MODE_ADDR_OFFSET); /* GLD16 partial */
	g_waveform_file.p_gld16_partial_wf = (u32)(g_waveform_file.p_wf_partial_paddr + *pAddr);
    
	pAddr = (u32 *)(g_waveform_file.p_wf_vaddr + C_GCC16_MODE_ADDR_OFFSET);
	g_waveform_file.p_gcc16_wf = (u32)(g_waveform_file.p_wf_paddr + *pAddr);
    
	pAddr = (u32 *)(g_waveform_file.p_wf_partial_vaddr + C_GCC16_MODE_ADDR_OFFSET); /* GCC16 partial */
	g_waveform_file.p_gcc16_partial_wf = (u32)(g_waveform_file.p_wf_partial_paddr + *pAddr);
    
	pAddr = (u32 *)(g_waveform_file.p_wf_vaddr + C_GCK16_MODE_ADDR_OFFSET);
	g_waveform_file.p_gck16_wf = (u32)(g_waveform_file.p_wf_paddr + *pAddr);
    
	pAddr = (u32 *)(g_waveform_file.p_wf_partial_vaddr + C_GCK16_MODE_ADDR_OFFSET); /* GCK16 partial */
	g_waveform_file.p_gck16_partial_wf = (u32)(g_waveform_file.p_wf_partial_paddr + *pAddr);

	pAddr = (u32 *)(g_waveform_file.p_wf_vaddr + C_GLK16_MODE_ADDR_OFFSET);
	g_waveform_file.p_glk16_wf = (u32)(g_waveform_file.p_wf_paddr + *pAddr);
    
	pAddr = (u32 *)(g_waveform_file.p_wf_partial_vaddr + C_GLK16_MODE_ADDR_OFFSET); /* GLK16 partial */
	g_waveform_file.p_glk16_partial_wf = (u32)(g_waveform_file.p_wf_partial_paddr + *pAddr);
    
    print_wavefile_mode_mapping(g_waveform_file);

	ret = waveform_mode_check(regal);
    if (ret) {
        WF_ERR("waveform_mode_check fail please check waveform file\n");
        //return ret;
    }
    
    if (regal == true) {
        libeink_version = eink_get_version();
        WF_INFO("eink library version = 0x%x\n", libeink_version);
    }
	
#ifdef WAVE_LOAD_FROM_FILE
	if(fp) {
		filp_close(fp, NULL);
		set_fs(fs);
	}
#endif

	g_waveform_file.load_flag = 1;

	WF_DBG("load waveform file(%s) successfully\n", path);
	return 0;

error:
	g_waveform_file.load_flag = 0;
	
	if(NULL != g_waveform_file.p_wf_vaddr)   {
		vfree(g_waveform_file.p_wf_vaddr);
		g_waveform_file.p_wf_vaddr = NULL;
	}

#ifdef WAVE_LOAD_FROM_FILE
	if(fp) {
		filp_close(fp, NULL);
		set_fs(fs);
	}
#endif

	return ret;
}

/*
Description: get eink panel type, for example, 3 stands for ED060XC3(4-bit), 4 stands for ED060XD4(5-bit)
Input: None
Output: type -- save the type of eink panel
Return: 0 -- get eink panel type successfully, others -- fail
*/
int get_eink_panel_type(EINK_PANEL_TYPE *type)
{
	if (1 != g_waveform_file.load_flag) {
		WF_ERR("waveform hasn't init yet, pls init first\n");
		return -EAGAIN;
	}

	if (NULL == type) {
		WF_ERR("input param is null\n");
		return -EINVAL;
	}

	*type = g_waveform_file.eink_panel_type;
	return 0;
}

/*
Description: get bit number of eink panel, 4-bit stands for 16 grayscale, 5-bit stands for 32 grayscale
Input: None
Output: bit_num -- save the bit number of eink panel
Return: 0 -- get eink panel type successfully, others -- fail
*/
int get_eink_panel_bit_num(enum  eink_bit_num *bit_num)
{
	if (1 != g_waveform_file.load_flag) {
		WF_ERR("waveform hasn't init yet, pls init first\n");
		return -EAGAIN;
	}

	if (NULL == bit_num) {
		WF_ERR("input param is null\n");
		return -EINVAL;
	}

	if (ED060XD4 == g_waveform_file.eink_panel_type) {
		*bit_num = EINK_BIT_5;
	} else {
		*bit_num = EINK_BIT_4;
	}

	return 0;
}


/*
Description: get waveform data address according to mode and temperature
Input: None
Output: type -- save the type of eink panel
Return: 0 -- get eink panel type successfully, others -- fail
*/
int get_waveform_data(enum eink_update_mode mode, u32 temp, u32 *total_frames, u32 *wf_paddr, u32 *wf_vaddr)
{
	u32 p_mode_phy_addr = 0;
	u8 *p_mode_virt_addr = NULL;
	u32 mode_temp_offset = 0;
	u8 *p_mode_temp_addr = NULL;
	//u16 *p_pointer = NULL;
	u32 temp_range_id = 0;

	WF_DBG("mode=0x%x, temp=%d\n", mode, temp);
	
	if (g_waveform_file.load_flag != 1) {
		WF_ERR("waveform hasn't init yet, pls init first\n");
		return -EAGAIN;
	}

	if ((total_frames == NULL) || (wf_paddr == NULL) || (wf_vaddr == NULL)) {
		WF_ERR("input param is null\n");
		return -EINVAL;
	}

	p_mode_virt_addr = get_mode_virt_address(mode);
	if (p_mode_virt_addr == NULL) {
		WF_ERR("get mode virturl address fail, mode=0x%x\n", mode);
		return -EINVAL;
	}
	WF_DBG("p_mode_virt_addr=0x%x\n", p_mode_virt_addr);

	p_mode_phy_addr = get_mode_phy_address(mode);
	if (p_mode_phy_addr == 0) {
		WF_ERR("get mode phy address fail, mode=0x%x\n", mode);
		return -EINVAL;
	}
	WF_DBG("p_mode_phy_addr=0x%x\n", p_mode_phy_addr);

	temp_range_id = get_temp_range_index(temp, (u32 *)p_mode_virt_addr);
	if (temp_range_id < 0) {
		WF_ERR("get temp range index fail, temp=0x%x\n", temp);
		return -EINVAL;
	}

	WF_DBG("temp_range_id=%d\n", temp_range_id);

	mode_temp_offset =  *((u32 *)p_mode_virt_addr + temp_range_id);
	p_mode_temp_addr = (u8 *)p_mode_virt_addr + mode_temp_offset;

	WF_DBG("mode_temp_offset=%d, p_mode_temp_addr:0x%x\n", mode_temp_offset, p_mode_temp_addr);

	*total_frames = *((u16 *)p_mode_temp_addr);
	WF_DBG("total_frames:%d\n", *total_frames);

	mode_temp_offset = mode_temp_offset + 4; //skip total frame(2 Byte) and dividor(2 Byte)
	WF_DBG("mode_temp_offset:%d\n", mode_temp_offset);

	*wf_paddr = p_mode_phy_addr + mode_temp_offset;
	*wf_vaddr = p_mode_virt_addr + mode_temp_offset;

	WF_DBG("mode=0x%x, temp=%d, temp_id=%d, temp_offset=0x%08x, total=%d, mode_offset=0x%08x\n",\
		mode, temp, temp_range_id, (mode_temp_offset - 4), *total_frames, (*wf_paddr - g_waveform_file.p_wf_paddr));

	return 0;
}

ssize_t get_waveform_name(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%s\n", g_waveform_file.wavefile_name);
}

ssize_t get_libeink_version(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "0x%x\n", libeink_version);
}

char *get_waveform_paddr(void)
{
	return g_waveform_file.p_wf_vaddr;
}

int regal_waveform_init(void)
{
	u32 *pAddr = NULL;
	__s32 ret = -EINVAL;
	
	pAddr = (u32 *)(g_waveform_file.p_wf_vaddr + C_GLR16_MODE_ADDR_OFFSET);
	g_waveform_file.p_glr16_wf = (u32)(g_waveform_file.p_wf_paddr + *pAddr);
    
	pAddr = (u32 *)(g_waveform_file.p_wf_partial_vaddr + C_GLR16_MODE_ADDR_OFFSET); /* GLR16 partial */
	g_waveform_file.p_glr16_partial_wf = (u32)(g_waveform_file.p_wf_partial_paddr + *pAddr);

	pAddr = (u32 *)(g_waveform_file.p_wf_vaddr + C_GLD16_MODE_ADDR_OFFSET);
	g_waveform_file.p_gld16_wf = (u32)(g_waveform_file.p_wf_paddr + *pAddr);
    
	pAddr = (u32 *)(g_waveform_file.p_wf_partial_vaddr + C_GLD16_MODE_ADDR_OFFSET); /* GLD16 partial */
	g_waveform_file.p_gld16_partial_wf = (u32)(g_waveform_file.p_wf_partial_paddr + *pAddr);
	
	pAddr = (u32 *)(g_waveform_file.p_wf_vaddr + C_GCK16_MODE_ADDR_OFFSET);
	g_waveform_file.p_gck16_wf = (u32)(g_waveform_file.p_wf_paddr + *pAddr);
    
	pAddr = (u32 *)(g_waveform_file.p_wf_partial_vaddr + C_GCK16_MODE_ADDR_OFFSET); /* GCK16 partial */
	g_waveform_file.p_gck16_partial_wf = (u32)(g_waveform_file.p_wf_partial_paddr + *pAddr);

	pAddr = (u32 *)(g_waveform_file.p_wf_vaddr + C_GLK16_MODE_ADDR_OFFSET);
	g_waveform_file.p_glk16_wf = (u32)(g_waveform_file.p_wf_paddr + *pAddr);
    
	pAddr = (u32 *)(g_waveform_file.p_wf_partial_vaddr + C_GLK16_MODE_ADDR_OFFSET); /* GLK16 partial */
	g_waveform_file.p_glk16_partial_wf = (u32)(g_waveform_file.p_wf_partial_paddr + *pAddr);
    
	ret = waveform_mode_check(true);
	if (ret) {
		WF_ERR("waveform_mode_check fail please check waveform file\n");
		//return ret;
	}
	
	print_wavefile_mode_mapping(g_waveform_file);
	
	return ret;
}

int regal_waveform_deinit(void)
{
	__s32 ret = -EINVAL;
	
	if (libeink_version != 0) {
		WF_INFO("orginal libeink exist, no need reset waveform!\n");
		return 0;
	}
		
	ret = waveform_mode_check(false);
	if (ret) {
		WF_ERR("waveform_mode_check fail please check waveform file\n");
		//return ret;
	}
	
	print_wavefile_mode_mapping(g_waveform_file);
	
	return ret;
}

/*
Description 	: free memory that used by waveform file, after that, waveform data cannot
			  be used until init_waveform function has been called. This function should
			  be called when unload module.
Input		: None
Output		: None
Return		: None
*/
void free_waveform(void)
{
	if(NULL != g_waveform_file.p_wf_vaddr)   {
#ifdef WAVE_LOAD_FROM_FILE
		vfree(g_waveform_file.p_wf_vaddr);
		g_waveform_file.p_wf_vaddr = NULL;
#else
		disp_eink_free(g_waveform_file.p_wf_vaddr, g_waveform_file.p_wf_paddr, gdwWF_size);
#endif
	}

	g_waveform_file.load_flag = 0;
	return;
}


