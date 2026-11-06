/*
 * Copyright (C) 2015 Allwinnertech, z.q <zengqi@allwinnertech.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#include "../../../../../../../arch/arm/mach-sunxi/ntx_hwconfig.h"
extern volatile NTX_HWCONFIG *gptHWCFG;

#include <linux/freezer.h>
#include <linux/encode.h>

#include"disp_eink.h"

#ifdef SUPPORT_EINK
#include"include.h"
#include "disp_private.h"
#include "libeink.h"
#include "libcfa.h"
#include "Kaleido_libcfa_table.h"

//#define DEBUG_VER

extern struct format_manager *disp_get_format_manager(unsigned int id);
extern struct index_manager *disp_get_index_manager(unsigned int id);
extern void *disp_eink_malloc(u32 num_bytes, void *phy_addr);
extern void  disp_eink_free(void *virt_addr, void *phy_addr, u32 num_bytes);
extern void disp_eink_cache_sync(const void *start_addr, const void *end_addr);

extern int new_process_calc_index_for_overlap_part_for_du_5bit(struct disp_eink_manager *manager, struct pipe_info *pinfo, struct area_info *collision_area);
extern int new_process_calc_index_for_overlap_part_for_du_4bit(struct disp_eink_manager *manager, struct pipe_info *pinfo, struct area_info *collision_area);

extern int calc_index_for_non_overlap_part(struct disp_eink_manager *manager, struct pipe_info *pinfo);
extern int calc_index_for_overlap_part_for_du_5bit(struct disp_eink_manager *manager, struct pipe_info *pinfo, struct area_info *collision_area);
extern int calc_index_for_overlap_part_for_du_4bit(struct disp_eink_manager *manager, struct pipe_info *pinfo, struct area_info *collision_area);
extern int calc_index_for_overlap_part_for_4bit(struct disp_eink_manager *manager, struct pipe_info *pinfo, struct area_info *collision_area);
extern int calc_index_for_overlap_part_for_5bit(struct disp_eink_manager *manager, struct pipe_info *pinfo, struct area_info *collision_area);

extern void trace_input_image_buffer(u32 order, char *buf, u32 width, u32 height,char * prefix);

extern struct disp_eink_manager *eink_manager;
static  unsigned int dequeue_order,queue_order;


static unsigned long old_index_data_paddr, new_index_data_paddr;
static unsigned long old_index_data_vaddr, new_index_data_vaddr;
static struct image_buffer_node *g_last_2nd_image_node = NULL;
static struct image_buffer_node *g_last_image_node = NULL;
static struct image_buffer_node *g_cur_image_node = NULL;
static struct timeval cur_update_timer, last_update_timer;
static unsigned int continue_gu16_cnt = 0; 


static unsigned int du_cnt = 0;
static struct image_buffer_node *first_node = NULL;
static struct eink_8bpp_image *process_img = NULL;

static struct y8_area_info *g_bg_setting = NULL;
static struct cube_data g_cube_data;

/* regal process use */
struct workqueue_struct *regal_process_workqueue;
struct delayed_work regal_process_work;

static bool image_proc_enable = false;
/*
r r g g b b
g b b r r g
*/
static const int cfa_table[2][6] = {
{2, 2, 1, 1, 0, 0},
{1, 0, 0, 2, 2, 1},
};

/* g2d process use */
static g2d_blt_flags_h g_g2d_flag_type;

static const int giDither8[16][16] = {
  {   1,235, 59,219, 15,231, 55,215,  2,232, 56,216, 12,228, 52,212},
  { 129, 65,187,123,143, 79,183,119,130, 66,184,120,140, 76,180,116},
  {  33,193, 17,251, 47,207, 31,247, 34,194, 18,248, 44,204, 28,244},
  { 161, 97,145, 81,175,111,159, 95,162, 98,146, 82,172,108,156, 92},
  {   9,225, 49,209,  5,239, 63,223, 10,226, 50,210,  6,236, 60,220},
  { 137, 73,177,113,133, 69,191,127,138, 74,178,114,134, 70,188,124},
  {  41,201, 25,241, 37,197, 21,255, 42,202, 26,242, 38,198, 22,252},
  { 169,105,153, 89,165,101,149, 85,170,106,154, 90,166,102,150, 86},
  {   3,233, 57,217, 13,229, 53,213,  0,234, 58,218, 14,230, 54,214},
  { 131, 67,185,121,141, 77,181,117,128, 64,186,122,142, 78,182,118},
  {  35,195, 19,249, 45,205, 29,245, 32,192, 16,250, 46,206, 30,246},
  { 163, 99,147, 83,173,109,157, 93,160, 96,144, 80,174,110,158, 94},
  {  11,227, 51,211,  7,237, 61,221,  8,224, 48,208,  4,238, 62,222},
  { 139, 75,179,115,135, 71,189,125,136, 72,176,112,132, 68,190,126},
  {  43,203, 27,243, 39,199, 23,253, 40,200, 24,240, 36,196, 20,254},
  { 171,107,155, 91,167,103,151, 87,168,104,152, 88,164,100,148, 84} 
};

static const u32 gamma22[] = {
  0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,1,1,1,1,1,
  1,2,2,2,2,2,3,3,3,3,
  3,4,4,4,4,5,5,5,6,6,
  6,7,7,7,8,8,8,9,9,9,
  10,10,11,11,11,12,12,13,13,14,
  14,14,15,15,16,16,17,17,18,19,
  19,20,20,21,21,22,23,23,24,24,
  25,26,26,27,28,28,29,30,30,31,
  32,32,33,34,35,35,36,37,38,39,
  39,40,41,42,43,43,44,45,46,47,
  48,49,50,50,51,52,53,54,55,56,
  57,58,59,60,61,62,63,64,65,66,
  67,68,70,71,72,73,74,75,76,77,
  79,80,81,82,83,84,86,87,88,89,
  91,92,93,94,96,97,98,100,101,102,
  104,105,106,108,109,111,112,113,115,116,
  118,119,121,122,124,125,127,128,130,131,
  149,150,152,154,155,157,159,160,162,164,
  165,167,169,171,172,174,176,178,180,182,
  183,185,187,189,191,193,195,196,198,200,
  202,204,206,208,210,212,214,216,218,220,
  222,224,226,228,230,232,235,237,239,241,
  222,224,226,228,230,232,235,237,239,241,
  222,224,226,228,230,232,235,237,239,241,
  243,245,247,250,252,254
};

//#define TRACK_UPDATE_BUFFER
//#define TEST_QUEUE_BUFFER_TIME

#define COMBINE_THREADHOLD	(IMAGE_BUF_NUM + 1)	//never combine
#define DEFAULT_GC_COUNTER	0

#ifdef TEST_QUEUE_BUFFER_TIME
struct timeval  queue_image_stimer, queue_image_etimer;
struct timeval  image_proc_stimer, image_proc_etimer;
#endif
struct timeval  image_calc_stimer, image_calc_etimer;


//added by tommy 2019-12-26 Begin
#define __USE_FAST_MEMCPY
//added by tommy 2019-12-26 End

extern void dump_pixels(char * title, void *pixels, int x_offset, int y_offset, int x_length, int y_length, int stride, int byteperpixel);

extern int g2d_module_open(void);
extern int g2d_module_release(void);
extern int g2d_module_blit_h(g2d_blt_h *para);


static int global_H;
static int global_S;
static int global_V;
static int global_L;
static int global_Sr;
static int global_Sg;
static int global_Sb;
static int global_Sy;
static int global_custom;
static int global_test1;
static int global_method;
static long global_ch1;
static long global_ch2;
static long global_ch3;
static long global_ch4;
static long global_ch5;
static long global_ch6;
static long global_ch7;
static long global_ch8;
static long global_ch9;
static int global_SF1;
static int global_VF1;
static int global_SF2;
static int global_VF2;
static int global_mode;	//0 RedYellowGreen Enhance ; 1 Face Enhance


static u32 force_update_mode = 0x00;

ssize_t EINK_forceupdate_get(struct device *dev, struct device_attribute *attr, char *buf) 
{
	return sprintf(buf, "force update mode: 0x%x\n", force_update_mode);
}

ssize_t EINK_forceupdate_set(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	int err;
	u32 update_mode;

	err = kstrtoint(buf, 16, &update_mode);
	if (err) {
		pr_warn("Invalid size\n");
		return err;
	}
	
	force_update_mode = (u32) update_mode;
	return count;
}

ssize_t EINK_nightmode_get(struct device *dev, struct device_attribute *attr, char *buf) 
{
	struct disp_eink_manager* manager = &eink_manager[0];
	return sprintf(buf, "night mode: 0x%x\n", manager->nightmode_test_enable );
}

ssize_t EINK_nightmode_set(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	int err;
	u32 night_mode;
	struct disp_eink_manager* manager = &eink_manager[0];

	err = kstrtoint(buf, 16, &night_mode);
	if (err) {
		pr_warn("Invalid size\n");
		return err;
	}

	if ( night_mode == 1 ) {
		manager->nightmode_test_enable = true;
	} else {
		manager->nightmode_test_enable = false;
	}

	return count;
}

ssize_t get_cfa_method(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", global_method);
}

ssize_t set_cfa_method(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	int err;
	char cfa_param[128] = {0};

	printk("cfa arg count  = %d\n", count );
	if (count >= sizeof(cfa_param))
		return 0;
/*
	if (copy_from_user(cfa_param, buf, count)) {
		pr_warn("copy_from_user fail\n");
		return 0;
	}
*/
	sprintf(cfa_param, "%s", buf);

	if (cfa_param[count - 1] == 0x0A)
		cfa_param[count - 1] = 0;
	else
		cfa_param[count] = 0;

    //terry
    char *tosearch;
    char *token;


    tosearch = cfa_param;
    token = strsep(&tosearch, " ");
    kstrtou32(token, 10, &global_method );
    printk("global_method   = %d\n", global_method );

    if (global_method == DEFAULT_CFA_METHOD){

    } else if (global_method == HSV) {
        token = strsep(&tosearch, " ");
        kstrtou32(token, 10, &global_custom  );
        printk("global_custom   = %d\n", global_custom );
        if ( global_custom == CUSTOM1 ) {

            token = strsep(&tosearch, " ");
            kstrtou32(token, 10, &global_H  );
            printk("global_H   = %d\n", global_H );

            token = strsep(&tosearch, " ");
            kstrtou32(token, 10, &global_S  );
            printk("global_S   = %d\n", global_S );

            token = strsep(&tosearch, " ");
            kstrtou32(token, 10, &global_V  );
            printk("global_V   = %d\n", global_V );

            token = strsep(&tosearch, " ");
            kstrtou32(token, 10, &global_SF1  );
            printk("global_SF1   = %d\n", global_SF1 );

            token = strsep(&tosearch, " ");
            kstrtou32(token, 10, &global_VF1  );
            printk("global_VF1   = %d\n", global_VF1 );

            token = strsep(&tosearch, " ");
            kstrtou32(token, 10, &global_SF2  );
            printk("global_SF2   = %d\n", global_SF2 );

            token = strsep(&tosearch, " ");
            kstrtou32(token, 10, &global_VF2  );
            printk("global_VF2   = %d\n", global_VF2 );

            token = strsep(&tosearch, " ");
            kstrtou32(token, 10, &global_mode  );
            printk("global_mode   = %d\n", global_mode );
        }
    } else if (global_method == HSL) {
        token = strsep(&tosearch, " ");
        kstrtou32(token, 10, &global_custom  );
        printk("global_custom   = %d\n", global_custom );
        if ( global_custom == CUSTOM1 ) {
            token = strsep(&tosearch, " ");
            kstrtou32(token, 10, &global_H  );
            printk("global_H   = %d\n", global_H );

            token = strsep(&tosearch, " ");
            kstrtou32(token, 10, &global_S  );
            printk("global_S   = %d\n", global_S );

            token = strsep(&tosearch, " ");
            kstrtou32(token, 10, &global_L  );
            printk("global_L   = %d\n", global_L );
        }

    } else if (global_method == CCM) {
        token = strsep(&tosearch, " ");
        kstrtou32(token, 10, &global_custom  );
        printk("global_custom   = %d\n", global_custom );
        if ( global_custom == CUSTOM1 ) {
            token = strsep(&tosearch, " ");
            kstrtol(token, 10, &global_ch1 );
            printk("global_ch1  = %d\n", global_ch1);

            token = strsep(&tosearch, " ");
            kstrtol(token, 10, &global_ch2 );
            printk("global_ch2  = %d\n", global_ch2);

            token = strsep(&tosearch, " ");
            kstrtol(token, 10, &global_ch3 );
            printk("global_ch3  = %d\n", global_ch3);

            token = strsep(&tosearch, " ");
            kstrtol(token, 10, &global_ch4 );
            printk("global_ch4  = %d\n", global_ch4);

            token = strsep(&tosearch, " ");
            kstrtol(token, 10, &global_ch5 );
            printk("global_ch5  = %d\n", global_ch5);

            token = strsep(&tosearch, " ");
            kstrtol(token, 10, &global_ch6 );
            printk("global_ch6  = %d\n", global_ch6);

            token = strsep(&tosearch, " ");
            kstrtol(token, 10, &global_ch7 );
            printk("global_ch7  = %d\n", global_ch7);

            token = strsep(&tosearch, " ");
            kstrtol(token, 10, &global_ch8 );
            printk("global_ch8  = %d\n", global_ch8);

            token = strsep(&tosearch, " ");
            kstrtol(token, 10, &global_ch9 );
            printk("global_ch9  = %d\n", global_ch9);
        }

    } else if (global_method == XYZ_CCM) {
        token = strsep(&tosearch, " ");
        kstrtou32(token, 10, &global_custom  );
        printk("global_custom   = %d\n", global_custom );
        if ( global_custom == CUSTOM1 ) {

            token = strsep(&tosearch, " ");
            kstrtol(token, 10, &global_ch1 );
            printk("global_ch1  = %d\n", global_ch1);

            token = strsep(&tosearch, " ");
            kstrtol(token, 10, &global_ch2 );
            printk("global_ch2  = %d\n", global_ch2);

            token = strsep(&tosearch, " ");
            kstrtol(token, 10, &global_ch3 );
            printk("global_ch3  = %d\n", global_ch3);

            token = strsep(&tosearch, " ");
            kstrtol(token, 10, &global_ch4 );
            printk("global_ch4  = %d\n", global_ch4);

            token = strsep(&tosearch, " ");
            kstrtol(token, 10, &global_ch5 );
            printk("global_ch5  = %d\n", global_ch5);

            token = strsep(&tosearch, " ");
            kstrtol(token, 10, &global_ch6 );
            printk("global_ch6  = %d\n", global_ch6);

            token = strsep(&tosearch, " ");
            kstrtol(token, 10, &global_ch7 );
            printk("global_ch7  = %d\n", global_ch7);

            token = strsep(&tosearch, " ");
            kstrtol(token, 10, &global_ch8 );
            printk("global_ch8  = %d\n", global_ch8);

            token = strsep(&tosearch, " ");
            kstrtol(token, 10, &global_ch9 );
            printk("global_ch9  = %d\n", global_ch9);
        }

    } else if (global_method == YCbCr_CONVERT) {
        token = strsep(&tosearch, " ");
        kstrtou32(token, 10, &global_custom  );
        printk("global_custom   = %d\n", global_custom );
        if ( global_custom == CUSTOM1 ) {
            token = strsep(&tosearch, " ");
            kstrtou32(token, 10, &global_Sr  );
            printk("global_Sr   = %d\n", global_Sr );

            token = strsep(&tosearch, " ");
            kstrtou32(token, 10, &global_Sg  );
            printk("global_Sg   = %d\n", global_Sg );

            token = strsep(&tosearch, " ");
            kstrtou32(token, 10, &global_Sb  );
            printk("global_Sb   = %d\n", global_Sb );

            token = strsep(&tosearch, " ");
            kstrtou32(token, 10, &global_Sy  );
            printk("global_Sy   = %d\n", global_Sy );
        }

    }else if (global_method == HSVYCbCr) {
        token = strsep(&tosearch, " ");
        kstrtou32(token, 10, &global_custom  );
        printk("global_custom   = %d\n", global_custom );
        if ( global_custom == CUSTOM1 ) {
			token = strsep(&tosearch, " ");
            kstrtou32(token, 10, &global_H  );
            printk("global_H   = %d\n", global_H );

            token = strsep(&tosearch, " ");
            kstrtou32(token, 10, &global_S  );
            printk("global_S   = %d\n", global_S );

            token = strsep(&tosearch, " ");
            kstrtou32(token, 10, &global_V  );
            printk("global_L   = %d\n", global_V );

            token = strsep(&tosearch, " ");
            kstrtou32(token, 10, &global_SF1  );
            printk("global_SF1   = %d\n", global_SF1 );

            token = strsep(&tosearch, " ");
            kstrtou32(token, 10, &global_VF1  );
            printk("global_VF1   = %d\n", global_VF1 );

            token = strsep(&tosearch, " ");
            kstrtou32(token, 10, &global_SF2  );
            printk("global_SF2   = %d\n", global_SF2 );

            token = strsep(&tosearch, " ");
            kstrtou32(token, 10, &global_VF2  );
            printk("global_VF2   = %d\n", global_VF2 );

			token = strsep(&tosearch, " ");
            kstrtou32(token, 10, &global_Sr  );
            printk("global_Sr   = %d\n", global_Sr );

            token = strsep(&tosearch, " ");
            kstrtou32(token, 10, &global_Sg  );
            printk("global_Sg   = %d\n", global_Sg );

            token = strsep(&tosearch, " ");
            kstrtou32(token, 10, &global_Sb  );
            printk("global_Sb   = %d\n", global_Sb );

            token = strsep(&tosearch, " ");
            kstrtou32(token, 10, &global_Sy  );
            printk("global_Sy   = %d\n", global_Sy );

            token = strsep(&tosearch, " ");
            kstrtou32(token, 10, &global_mode  );
            printk("global_mode   = %d\n", global_mode );
        };
    }

    return count;
}


s32 get_delt_ms_timer(struct timeval start_timer, struct timeval end_timer)
{
	return ((end_timer.tv_sec - start_timer.tv_sec) * 1000 + (end_timer.tv_usec - start_timer.tv_usec)/1000);
}
EXPORT_SYMBOL(get_delt_ms_timer);

s32 get_delt_us_timer(struct timeval start_timer, struct timeval end_timer)
{
	return ((end_timer.tv_sec - start_timer.tv_sec) * 1000000 + (end_timer.tv_usec - start_timer.tv_usec));
}
EXPORT_SYMBOL(get_delt_us_timer);

bool rect_is_overlap(struct area_info a_area, struct area_info b_area)
{
	bool overlap_flag = true;

	if (rectangle_is_zero(a_area) || rectangle_is_zero(b_area)) {
		return false;
	}
	
	if ((max(a_area.x_top, b_area.x_top) <= min(a_area.x_bottom, b_area.x_bottom))
		&& (max(a_area.y_top, b_area.y_top) <= min(a_area.y_bottom, b_area.y_bottom))) {
		overlap_flag = true;
	} else {
		overlap_flag = false;
	}

	return overlap_flag;
}


//added by tommy 2019-12-26 Begin
#ifdef __USE_FAST_MEMCPY
void fast_memcpy(void *dest, const void *src, size_t count){

    void *res = dest;
    size_t count_len = 0;

    count_len = count & ~(sizeof(uint64_t) - 1);
    uint64_t *src_u64 = (uint64_t *)src;
    uint64_t *dst_u64 = (uint64_t *)dest;

    while(count_len){
        *dst_u64++ = *src_u64++;
        count_len -= sizeof(uint64_t);
    }

    count_len = count & (sizeof(uint64_t) - 1);
    uint8_t *src_u8 = (uint8_t *)src;
    uint8_t *dst_u8 = (uint8_t *)dest;

    while( count_len-- ){
        (*dst_u8++ = *src_u8++);
    }

    return res;
}
#endif
//added by tommy 2019-12-26 End

/*
rectangle A include rectangle B, then return true;otherwise, return false
*/
static bool rect_include_judge(struct area_info a_rect, struct area_info b_rect)
{
	if ((a_rect.x_top <= b_rect.x_top) &&
		(a_rect.x_bottom >= b_rect.x_bottom) &&
		(a_rect.y_top <= b_rect.y_top) &&
		(a_rect.y_bottom >= b_rect.y_bottom)) {
		return true;
	} else {
		return false;
	}
}


int rect_subtract_judge(struct area_info org_area, struct area_info sub_area, struct area_info *output_area)
{
	bool partial_overlap = false;
	
	if (NULL == output_area) {
		__wrn("input param is NULL\n");
		return -1;
	}

	//is org_area and sub_area overlap?
	if (false == rect_is_overlap(org_area, sub_area)) {
		__debug("two area is no overlap, cannot subtract\n");
		return -2;
	}

	//is sub_area include org_area?
	if (rect_include_judge(sub_area, org_area)) {
		__debug("whole area is included,ORG:(%d,%d)~(%d,%d),SUB:(%d,%d)~(%d,%d)\n",
			org_area.x_top, org_area.y_top, org_area.x_bottom, org_area.y_bottom,
			sub_area.x_top, sub_area.y_top, sub_area.x_bottom, sub_area.y_bottom);
		return 0;
	}

	//is partial overlap
	if ((org_area.x_top == sub_area.x_top) && (org_area.x_bottom== sub_area.x_bottom)) {
		output_area->x_top = org_area.x_top;
		output_area->x_bottom = org_area.x_bottom;
		if (org_area.y_top == sub_area.y_top) {			
			output_area->y_top = sub_area.y_bottom + 1;
			output_area->y_bottom = org_area.y_bottom;
			return 1;
		}

		if (org_area.y_bottom== sub_area.y_bottom) {
			output_area->y_top = org_area.y_top;
			output_area->y_bottom = sub_area.y_top - 1;
			return 1;
		}
	}

	
	if ((org_area.y_top == sub_area.y_top) && (org_area.y_bottom== sub_area.y_bottom)) {
		output_area->y_top = org_area.y_top;
		output_area->y_bottom = org_area.y_bottom;
		if (org_area.x_top == sub_area.x_top) {			
			output_area->x_top = sub_area.x_bottom + 1;
			output_area->x_bottom = org_area.x_bottom;
			return 1;
		}

		if (org_area.x_bottom== sub_area.x_bottom) {
			output_area->x_top = org_area.x_top;
			output_area->x_bottom = sub_area.x_top - 1;
			return 1;
		}
	}

	__debug("partial area cannot subtract\n");
	return -3;
}

bool rect_merge_check(struct area_info new_rect, struct area_info old_rect)
{
	bool ret = false;
	if (false == rect_is_overlap(new_rect, old_rect)) {
		return false;
	}
	
	if (rect_include_judge(new_rect, old_rect)) {
		ret = true;
	} else {
		if (((new_rect.x_top == old_rect.x_top) && (new_rect.x_bottom == old_rect.x_bottom)) &&
			((new_rect.y_top == old_rect.y_top) && (new_rect.y_bottom == old_rect.y_bottom))) {
			ret = true;
		} else {
			ret = false;
		}
	}

	return ret;
}

enum eink_update_mode merge_mode(enum eink_update_mode a_mode, enum eink_update_mode b_mode)
{
	enum eink_update_mode anode_mode, bnode_mode, ret_mode;
	enum eink_update_mode anode_flag, bnode_flag, conbine_flag;

	anode_mode = a_mode & 0xff;
	anode_flag = a_mode & 0xffffff00;
	bnode_mode = b_mode & 0xff;
	bnode_flag = b_mode & 0xffffff00;
	conbine_flag = bnode_flag | anode_flag;
	
	if (anode_mode == EINK_A2_MODE) {
		//if ((bnode_mode == EINK_GU16_MODE) || (bnode_mode == EINK_GL16_MODE)) {
        if (bnode_mode == EINK_A2_MODE) {
			ret_mode = anode_mode;
		} else {
			ret_mode = bnode_mode;
		}
	} else {
		if (anode_mode == EINK_DU_MODE) {
			if (bnode_mode == EINK_A2_MODE) {
				ret_mode = anode_mode;
			} else {
				ret_mode = bnode_mode;
			}
		} else {
			if (anode_mode == EINK_GU16_MODE || anode_mode == EINK_GL16_MODE) {
				if ((bnode_mode == EINK_A2_MODE) || (bnode_mode == EINK_DU_MODE)) {
					ret_mode = anode_mode;
				} else {
					ret_mode = bnode_mode;
				}
			} else {
				ret_mode = anode_mode;
			}
		}
	}

	return (ret_mode | conbine_flag);
}

bool __is_ring_queue_full(struct eink_buffer_manager *buffer_mgr)
{
	bool ret;
	unsigned int in_index, out_index;

	mutex_lock(&buffer_mgr->mlock);

	if (list_empty(&buffer_mgr->image_free_list)) {
		ret = true;
	} else {
		ret = false;
	}

	mutex_unlock(&buffer_mgr->mlock);

	return ret;
}

bool __is_ring_queue_empty(struct eink_buffer_manager *buffer_mgr)
{
	bool ret;

	mutex_lock(&buffer_mgr->mlock);

	if (list_empty(&buffer_mgr->image_used_list)) {
		ret = true;
	} else {
		ret = false;
	}

	mutex_unlock(&buffer_mgr->mlock);

	return ret;
}

bool __is_collision_list_empty(struct eink_buffer_manager *buffer_mgr)
{
	bool ret;

	mutex_lock(&buffer_mgr->mlock);

	if (list_empty(&buffer_mgr->image_collision_list)) {
		ret = true;
	} else {
		ret = false;
	}

	mutex_unlock(&buffer_mgr->mlock);

	return ret;
}

static struct image_buffer_node *try_to_merge_image(struct image_buffer_node *anode, struct image_buffer_node *bnode)
{
	struct eink_8bpp_image *a_img = NULL, *b_img = NULL;
	struct image_buffer_node *tmp_anode = NULL, *tmp_bnode = NULL;
	unsigned int a_update_mode = 0, b_update_mode = 0;
	
	if ((NULL == anode) || (NULL == bnode)) {
		__wrn("input param is null\n");
		return NULL;
	}

	if ((NULL == anode->img) || (NULL == bnode->img)) {
		__wrn("input image is null\n");
		return NULL;
	}

	tmp_anode = (anode->update_order > bnode->update_order) ? anode:bnode;	//new update
	tmp_bnode = (tmp_anode == anode)?bnode:anode;						//old update

	/* old update image is specify no merge, then return NULL*/
	if (IS_NO_MERGE(tmp_bnode->img->update_mode)) {
		__debug("image specity no merge, mode=0x%x, order=%d\n", 
			tmp_bnode->img->update_mode, tmp_bnode->update_order);
		return NULL;
	}
	
	if ((false == rect_merge_check(tmp_anode->img->update_area, tmp_bnode->img->update_area))) {
		return NULL;
	}

	/*if ((tmp_bnode->img->update_mode & 0xff) == EINK_DU_MODE || (tmp_bnode->img->update_mode & 0xff) == EINK_A2_MODE) {
		__wrn("du try merge\n");
	}*/
	/*if ((tmp_anode->img->update_mode & 0xff) != (tmp_bnode->img->update_mode & 0xff))
		return NULL;*/

	tmp_anode->collision_flag |= tmp_bnode->collision_flag;
	
	tmp_anode->img->update_mode = merge_mode(tmp_anode->img->update_mode, tmp_bnode->img->update_mode);

	tmp_anode->img->update_area.x_top = min(tmp_anode->img->update_area.x_top, tmp_bnode->img->update_area.x_top);
	tmp_anode->img->update_area.x_bottom = max(tmp_anode->img->update_area.x_bottom, tmp_bnode->img->update_area.x_bottom);
	tmp_anode->img->update_area.y_top = min(tmp_anode->img->update_area.y_top, tmp_bnode->img->update_area.y_top);
	tmp_anode->img->update_area.y_bottom = max(tmp_anode->img->update_area.y_bottom, tmp_bnode->img->update_area.y_bottom);

	return tmp_anode;
}

static int __add_image_to_collision_list(struct eink_buffer_manager *buffer_mgr, struct image_buffer_node * img_node)
{
	int ret = -1;
	struct image_buffer_node *cur_img_node = NULL, *tmp_img_node = NULL;
	enum eink_update_mode mode = 0;

	if ((NULL == buffer_mgr) || (NULL == img_node)) {
		__wrn("input param is null\n");
		return -1;
	}
	
	mutex_lock(&buffer_mgr->mlock);
	list_for_each_entry_safe(cur_img_node, tmp_img_node, &buffer_mgr->image_collision_list, node) {
		if (img_node == cur_img_node) {	//already add
			__debug("image node is already here\n");
			ret = 0;
			goto out;
		}

		if (cur_img_node->img != NULL) {
			mode = GET_UPDATE_MODE(cur_img_node->img->update_mode);
			if ((EINK_GL16_MODE == mode) || (EINK_GC16_MODE == mode) || (EINK_GU16_MODE == mode)) {
				//cur_img_node->img->update_mode = EINK_GU16_MODE;
				//cur_img_node->img->update_mode = EINK_GL16_MODE;
			}
		}
	}
	
	list_move_tail(&img_node->node, &buffer_mgr->image_collision_list);

out:
	mutex_unlock(&buffer_mgr->mlock);

	return ret;
}

static int __remove_image_from_collision_list(struct eink_buffer_manager *buffer_mgr, struct image_buffer_node * img_node)
{
	int ret = -1;
	struct image_buffer_node *cur_img_node = NULL, *tmp_img_node = NULL;
	unsigned int tmp_order = 0;
	
	if ((NULL == buffer_mgr) || (NULL == img_node)) {
		__wrn("input param is null\n");
		return -1;
	}
	
	mutex_lock(&buffer_mgr->mlock);
	if (list_empty(&buffer_mgr->image_collision_list)) {
		__wrn("no collision image can be free\n");
		ret = 0;
		goto out;
	}

	list_for_each_entry_safe(cur_img_node, tmp_img_node, &buffer_mgr->image_collision_list, node) {
		if (img_node == cur_img_node) {
			tmp_order = cur_img_node->update_order;
			if (NULL != cur_img_node->img) {
				cur_img_node->img->state = FREE;
				cur_img_node->img->flash_mode = GLOBAL_MODE;
				cur_img_node->img->update_mode = EINK_INIT_MODE;
				cur_img_node->img->window_calc_enable = true;
			}
			__debug("remove image node from collision list, order=%d\n", tmp_order);
			list_move_tail(&cur_img_node->node, &buffer_mgr->image_free_list);
			ret = 0;
			break;
		}
	}
out:
	mutex_unlock(&buffer_mgr->mlock);

	return ret;
}

static bool __update_collision_state(struct eink_buffer_manager *buffer_mgr, u32 state)
{
	bool valid_flag = false;
	struct image_buffer_node *cur_img_node = NULL, *tmp_img_node = NULL;
	
	if (NULL == buffer_mgr) {
		return false;
	}
	
	mutex_lock(&buffer_mgr->mlock);

	list_for_each_entry_safe(cur_img_node, tmp_img_node, &buffer_mgr->image_collision_list, node) {
		if (NULL != cur_img_node) {
			cur_img_node->collision_flag &= state;
			if (0 == cur_img_node->collision_flag) {
				valid_flag = true;
			}
		}
	}
	mutex_unlock(&buffer_mgr->mlock);

	return valid_flag;
}


static bool __update_soft_pipe_collision_state(struct eink_buffer_manager *buffer_mgr, u32 state)
{
	bool valid_flag = false;
	struct image_buffer_node *cur_img_node = NULL, *tmp_img_node = NULL;
	
	if (NULL == buffer_mgr) {
		return false;
	}
	
	mutex_lock(&buffer_mgr->mlock);

	list_for_each_entry_safe(cur_img_node, tmp_img_node, &buffer_mgr->image_collision_list, node) {
		if (NULL != cur_img_node) {
			cur_img_node->collision_flag &= (~state);
			if (0 == cur_img_node->collision_flag) {
				valid_flag = true;
			}
		}
	}
	mutex_unlock(&buffer_mgr->mlock);

	return valid_flag;
}



static void __cancel_collision_update(struct eink_buffer_manager *buffer_mgr, u32 update_order, enum eink_update_mode mode, struct area_info update_area)
{
	struct image_buffer_node *img_node = NULL, *combine_node = NULL, *free_node = NULL;
	struct image_buffer_node *cur_img_node = NULL, *tmp_img_node = NULL;
	struct area_info output_area;
	u32 tmp_order = 0;
	int ret = 0;

	if (NULL == buffer_mgr) {
		__wrn("input param is null\n");
		return;
	}

	if (rectangle_is_zero(update_area)) {
		__debug("update area is zero\n");
		return;
	}

	memset(&output_area, 0, sizeof(output_area));
	
	mutex_lock(&buffer_mgr->mlock);
	if (list_empty(&buffer_mgr->image_collision_list)) {
		__debug("collision list is empty\n");
		mutex_unlock(&buffer_mgr->mlock);
		return;
	}

	
	list_for_each_entry_safe(cur_img_node, tmp_img_node, &buffer_mgr->image_collision_list, node) {
		if (update_order < cur_img_node->update_order) {
			__debug("collision order=%d, update_order=%d\n", cur_img_node->update_order, update_order);
			continue;
		}

		ret = rect_subtract_judge(cur_img_node->img->update_area, update_area, &output_area);
		switch (ret) {
		case 0:	//whole area is cancel
			tmp_order = cur_img_node->update_order;
			if (NULL != cur_img_node->img) {
				cur_img_node->img->state = FREE;
				cur_img_node->img->flash_mode = GLOBAL_MODE;
				cur_img_node->img->update_mode = EINK_INIT_MODE;
				cur_img_node->img->window_calc_enable = true;
			}
			list_move_tail(&cur_img_node->node, &buffer_mgr->image_free_list);
			__debug("whole area cancel, free image(order=%d)\n", tmp_order);
			break;
		case 1:	//partial area is cancel
			tmp_order = cur_img_node->update_order;
			if (NULL != cur_img_node->img) {
				memcpy(&cur_img_node->img->update_area, &output_area, sizeof(output_area));
			}
			__debug("partial cancel(order=%d), area(%d,%d)~(%d,%d)\n", tmp_order,
				output_area.x_top, output_area.y_top, output_area.x_bottom, output_area.y_bottom);
			break;
		default:	//no cancel
			break;
		}
	}
	mutex_unlock(&buffer_mgr->mlock);

	return;
}

static struct image_buffer_node *__get_current_image_from_collision_list(struct eink_buffer_manager *buffer_mgr)
{
	struct image_buffer_node *img_node = NULL, *combine_node = NULL, *free_node = NULL;
	struct image_buffer_node *cur_img_node = NULL, *tmp_img_node = NULL;
	unsigned int tmp_order = 0;
	int ret = 0;
	
	if (NULL == buffer_mgr) {
		return NULL;
	}

	mutex_lock(&buffer_mgr->mlock);

	if (list_empty(&buffer_mgr->image_collision_list)) {
		mutex_unlock(&buffer_mgr->mlock);
		return NULL;
	}

	//merge image node
	list_for_each_entry_safe(cur_img_node, tmp_img_node, &buffer_mgr->image_collision_list, node) {		
		if (cur_img_node->collision_flag == 0) {
			if (NULL == img_node) {
				img_node = cur_img_node;
			} else {
				//NO MERGE
				combine_node = try_to_merge_image(img_node, cur_img_node);
				if (NULL == combine_node) {
					__debug("merge block\n");
					//break;
				} else {
					//merge success, free older image
					if (combine_node == img_node) {
						free_node = cur_img_node;
					} else {
						free_node = img_node;
						img_node = combine_node;
					}
					
					tmp_order = free_node->update_order;
					if (NULL != free_node->img) {
						img_node->collision_flag |= free_node->collision_flag;
						
						free_node->img->state = FREE;
						free_node->img->flash_mode = GLOBAL_MODE;
						free_node->img->update_mode = EINK_INIT_MODE;
						free_node->img->window_calc_enable = true;
						free_node->collision_flag = 0;
					}
					list_move_tail(&free_node->node, &buffer_mgr->image_free_list);
					__debug("merge successfully, free image(order=%d)\n", tmp_order);
				}
			}
		}
	}

	img_node = NULL;
	list_for_each_entry_safe(cur_img_node, tmp_img_node, &buffer_mgr->image_collision_list, node) {		
		if (0 == cur_img_node->collision_flag) {
			img_node = cur_img_node;
			break;
		}
	}

	if (NULL != img_node) {
		if ((img_node->img== NULL) || (img_node->img->state != USED)) {
			__wrn("something is wrong: img_node state != USED\n");
		} else {
			//__debug("get image node from collision list, order=%d\n", img_node->update_order);
		}
	} else {
		//__debug("no valid collision node, please wait\n");
	}
	mutex_unlock(&buffer_mgr->mlock);

	return img_node;
}

struct image_buffer_node *__get_current_image(struct eink_buffer_manager *buffer_mgr)
{
	struct image_buffer_node *cur_img_node = NULL, *last_img_node = NULL, *tmp_img_node = NULL;
	struct image_buffer_node *img_node = NULL, *combine_node = NULL, *free_node = NULL;
	unsigned int tmp_order = 0;
	unsigned char overlap = 0;
	int ret = 0;
	
	mutex_lock(&buffer_mgr->mlock);
	if (list_empty(&buffer_mgr->image_used_list)) {
		mutex_unlock(&buffer_mgr->mlock);
		return NULL;
	}

	list_for_each_entry_safe(cur_img_node, tmp_img_node, &buffer_mgr->image_used_list, node) {
		if (NULL == img_node) {
			img_node = cur_img_node;
		} else {
			combine_node = try_to_merge_image(img_node, cur_img_node);
			if(bsp_disp_get_print_level() == 3){
				//printk("a_node(0x%x)  b_node(0x%x) \n",img_node->img->update_mode,cur_img_node->img->update_mode);
			}
			if (NULL == combine_node) {
				__debug("merge block\n");
				//break;
			} else {
				//merge success, free older image
				if (combine_node == img_node) {
					free_node = cur_img_node;
				} else {
					free_node = img_node;
					img_node = combine_node;
				}

				tmp_order = free_node->update_order;
				if (NULL != free_node->img) {
					free_node->img->state = FREE;
					free_node->img->flash_mode = GLOBAL_MODE;
					free_node->img->update_mode = EINK_INIT_MODE;
					free_node->img->window_calc_enable = true;
				}
				list_move_tail(&free_node->node, &buffer_mgr->image_free_list);
				__debug("merge successully, free image(order=%d)\n", tmp_order);
			}
		}
	}

	if (NULL != img_node) {
		if ((img_node->img== NULL) || (img_node->img->state != USED)) {
			__wrn("something is wrong: img_node state != USED\n");
			img_node = NULL;
			goto out;
		} else {
			//check with collision list
			if (list_empty(&buffer_mgr->image_collision_list)) {
				//no collision
			} else {
				//if ((img_node->img->update_mode & 0xff) != EINK_DU_MODE) {
					list_for_each_entry_safe(cur_img_node, tmp_img_node, &buffer_mgr->image_collision_list, node) {
						//if (rect_is_overlap(img_node->img->update_area, cur_img_node->img->update_area) &&
						if (rect_include_judge(img_node->img->update_area, cur_img_node->img->update_area) &&  
							(img_node->update_order > cur_img_node->update_order)) {
							combine_node = try_to_merge_image(img_node, cur_img_node);
							free_node = cur_img_node;
							
							tmp_order = free_node->update_order;
							if (NULL != free_node->img) {
								img_node->collision_flag |= free_node->collision_flag;
								
								free_node->img->state = FREE;
								free_node->img->flash_mode = GLOBAL_MODE;
								free_node->img->update_mode = EINK_INIT_MODE;
								free_node->img->window_calc_enable = true;
								free_node->collision_flag = 0;
							}
							list_move_tail(&free_node->node, &buffer_mgr->image_free_list);
						} else if (rect_is_overlap(img_node->img->update_area, cur_img_node->img->update_area) && 
							(img_node->update_order > cur_img_node->update_order)) {
							overlap = cur_img_node->update_order;
						}
					}

					if (overlap) {
						img_node = NULL;
						__debug("current update overlap with collision list\n");
					}
				//}
			}
		}
	} else {
		//__debug("no valid image node for update\n");
	}

out:
	mutex_unlock(&buffer_mgr->mlock);

	return img_node;
}


#define MAX_GRAY_LEVEL	16
#define UNIT_GRAY_VALUE	(256/MAX_GRAY_LEVEL)
static void trace_input_raw_8bit_image_buffer(u32 order, char *buf, u32 width, u32 height);

int _rgb2cfa(int x, int y, u32 color)
{
	int r, g, b;
	int m, m2;
	int temp;
	r = (color >> 16) & 0xff;
	g = (color >> 8) & 0xff;
	b = color & 0xff;
	
	m = y % 9;
	m2 = x % 9;
			
	if (m == 0 || m == 3 || m == 6) {
		if(m2 == 0 || m2 == 1 || m2 == 2){
			temp = g;
		}
		else if (m2 < 6 && m2 > 2){
			temp = b;
		}
		else{
			temp = r;
		}
	}else if(m == 1 || m == 4 || m == 7){
		if(m2 == 0 || m2 == 1 || m2 == 2){
			temp = b;
		}
		else if (m2 < 6 && m2 > 2){
			temp = r;
		}
		else{
			temp = g;
		}
	}else{
		if(m2 == 0 || m2 == 1 || m2 == 2){
			temp = r;
		}
		else if (m2 < 6 && m2 > 2){
			temp = g;
		}
		else{
			temp = b;
		}
	}
	
	return temp;
}

int _rgb2cfa_ver2( int x, int y, u32 color) {
    int r, g, b;
    int mx, my;
    int temp;
	r = (color >> 16) & 0xff;
	g = (color >> 8) & 0xff;
	b = color & 0xff;
    my= y%2;
    mx= x%6;

    switch( my ) {
    case 0:
        if(mx == 0 || mx == 1) 
        	temp = r;
        else if(mx == 2 || mx == 3) 
        	temp = g;
        else if(mx == 4 || mx == 5)
        	temp = b;
        break;
    case 1:
        if(mx == 0 || mx == 5) temp = g;
        else if(mx == 1 || mx == 2) temp = b;
        else temp = r;
        break;
    }
    return temp;
}

int getRGBA(int x, int y, int z, int size, u8 *data) {
    int rgb =0;
    rgb = data[(z*size*size + y*size + x)*3+2] | 
         (data[(z*size*size + y*size + x)*3+1] << 8) | 
         (data[(z*size*size + y*size + x)*3] << 16);

    return rgb;
}

/*
 *  color_buffer : RGB image (BGR)
 *  gray_buffer : output gray image, you can change format here by yourselves to fit EPD input
 *  width : source image width
 *  height : source image height
 *  (*rgbcfa): function pointer for gen1 and gen2/gen3 switch
 */
void ntx_imageRGB_to_cfa_grayscale(int width, int height, u8 *color_buffer, u8 *gray_buffer, int (*rgbcfa)(int, int, u32)) {
	u8 *cur_color_buffer = color_buffer;
	u8 *cur_gray_buffer = gray_buffer;

	int x=0, y=0, RGB_pixel=0, offset=0;
	u32 cur_R=0, cur_G=0, cur_B=0;
	u32 redValue=0, greenValue=0, blueValue=0;

	__debug("start covert RGB to CFA\n");
	for (y = 0; y < height; y++) {
		for (x = 0; x < width; x++) {
			cur_R = cur_color_buffer[4 * x];
			cur_G = cur_color_buffer[4 * x + 1];
			cur_B = cur_color_buffer[4 * x + 2];

			if (g_cube_data.mode==IMAGE_MODE_LUT) {
				redValue = cur_R*(LUT_3D_SIZE-1)/255;
				greenValue = cur_G*(LUT_3D_SIZE-1)/255;
				blueValue = cur_B*(LUT_3D_SIZE-1)/255;
				RGB_pixel = getRGBA(redValue, greenValue, blueValue, LUT_3D_SIZE, g_cube_data.vaddr);
			} else {
				RGB_pixel = cur_R << 16 | cur_G << 8 | cur_B;
			}

            		*cur_gray_buffer = (u8) rgbcfa(x, y, RGB_pixel);
			cur_gray_buffer++; 
		}
		cur_color_buffer += width * 4; 
	}
}

void xyz_process(struct eink_8bpp_image *cur_img) {

   int x, y, i;
    struct area_info update_area;

    struct st_abgr *abgr_src;
    u32 offset;

    int rgb2xyz[] = { 412391, 357584, 180481, 212639, 715169, 72192, 19331, 119195, 950532 }; //D65
    int xyz2rgb[] = { 3240970, -1537383, -498611, -969244, 1875968, 41555, 55630, -203977, 1056972 }; //D65
/*
[[ 1.34417685e+00 -1.10816567e+00 -4.71623751e-01]
 [-3.26347168e-01  2.20385967e+00 -2.52739883e-01]
 [ 1.08078529e-01 -1.62046622e-03  1.85254545e+00]
 [-5.95428100e-02 -1.03655040e-01 -7.77936848e-02]]*/
    //int pocketbook_inv[] = {1344176, -1108165, -471623, -326347, 2203859, -252739, 108078, -1620, 1852545};
    int pocketbook_inv[] = {1344176, -326347, 108078, -1108165, 2203859, -1620, -471623, -252739 , 1852545};
    int pocketbook_const[] = {-59542, -103655, -77793};
/*
[[ 1.24896786 -1.27391015 -0.59601591]
 [-0.31289486  2.22727811 -0.23516421]
 [ 0.07805754 -0.05388248  1.81332252]*/

    int pocketbook_inv2[] = {1248967, -312894, 78057, -1273910, 2227278, -53882, -596015, -235164 , 1813322};

    int custom_inv[9];

    for ( i = 0 ; i < 9 ; i++ ) {
       custom_inv[i] = 0;
    }

    if ( global_custom == CUSTOM1 ) {
        custom_inv[0] += 1000000 + global_ch1;
        custom_inv[1] += global_ch2;
        custom_inv[2] += global_ch3;
        custom_inv[3] += global_ch4;
        custom_inv[4] += 1000000 + global_ch5;
        custom_inv[5] += global_ch6;
        custom_inv[6] += global_ch7;
        custom_inv[7] += global_ch8;
        custom_inv[8] += 1000000 + global_ch9;
    } else if ( global_custom == V1) {
        for ( i = 0 ; i < 9 ; i++ ) {
            custom_inv[i] = pocketbook_inv2[i];
        }
    }

    int divider = 1000000;
    struct st_xyz new_pixel, converted_xyz_pixel;
    int new_R = 0, new_G = 0, new_B = 0;


    memcpy(&update_area, &cur_img->update_area, sizeof(struct area_info));

    offset = cur_img->size.width * cur_img->update_area.y_top + cur_img->update_area.x_top;
    abgr_src = ((struct st_abgr *) cur_img->cfa_vaddr) + offset;
    for (y = update_area.y_top; y <= update_area.y_bottom; y++) {
        offset = cur_img->size.width * y + cur_img->update_area.x_top;
        abgr_src = ((struct st_abgr *) cur_img->cfa_vaddr) + offset;

        for (x = update_area.x_top; x <= update_area.x_bottom; x++) {
            if ( x == 100 && y == 100 ) {
                printk("rgb [%s](%d %d): (%d %d %d)\n",__func__, x , y , abgr_src->red, abgr_src->green, abgr_src->blue);
            }

            new_pixel.x = (rgb2xyz[0] * abgr_src->red + rgb2xyz[1] * abgr_src->green + rgb2xyz[2] * abgr_src->blue) / divider;
            new_pixel.y = (rgb2xyz[3] * abgr_src->red + rgb2xyz[4] * abgr_src->green + rgb2xyz[5] * abgr_src->blue) / divider;
            new_pixel.z = (rgb2xyz[6] * abgr_src->red + rgb2xyz[7] * abgr_src->green + rgb2xyz[8] * abgr_src->blue) / divider;

            if ( x == 100 && y == 100 ) {
                printk("rgb->xyz [%s](%d %d): (%d %d %d)\n",__func__, x , y , new_pixel.x, new_pixel.y, new_pixel.z);
            }


            converted_xyz_pixel.x = (custom_inv[0] * new_pixel.x + custom_inv[1] * new_pixel.y + custom_inv[2] * new_pixel.z) / divider;
            converted_xyz_pixel.y = (custom_inv[3] * new_pixel.x + custom_inv[4] * new_pixel.y + custom_inv[5] * new_pixel.z) / divider;
            converted_xyz_pixel.z = (custom_inv[6] * new_pixel.x + custom_inv[7] * new_pixel.y + custom_inv[8] * new_pixel.z) / divider;

/*            converted_xyz_pixel.x = (custom_inv[0] * new_pixel.x + custom_inv[1] * new_pixel.y + custom_inv[2] * new_pixel.z + 255 * pocketbook_const[0]) / divider;
            converted_xyz_pixel.y = (custom_inv[3] * new_pixel.x + custom_inv[4] * new_pixel.y + custom_inv[5] * new_pixel.z + 255 * pocketbook_const[1]) / divider;
            converted_xyz_pixel.z = (custom_inv[6] * new_pixel.x + custom_inv[7] * new_pixel.y + custom_inv[8] * new_pixel.z + 255 * pocketbook_const[2]) / divider;*/


            if ( x == 100 && y == 100 ) {
                printk("xyz->pocketbook xyz [%s](%d %d): (%d %d %d)\n",__func__, x , y , converted_xyz_pixel.x, converted_xyz_pixel.y, converted_xyz_pixel.z);
            }


/*            abgr_src->red = new_pixel.red;
            abgr_src->green = new_pixel.green;
            abgr_src->blue = new_pixel.blue;*/

/*            abgr_src->red = (xyz2rgb[0] * new_pixel.x + xyz2rgb[1] * new_pixel.y + xyz2rgb[2] * new_pixel.z) / divider;
            abgr_src->green = (xyz2rgb[3] * new_pixel.x + xyz2rgb[4] * new_pixel.y + xyz2rgb[5] * new_pixel.z) / divider;
            abgr_src->blue = (xyz2rgb[6] * new_pixel.x + xyz2rgb[7] * new_pixel.y + xyz2rgb[8] * new_pixel.z) / divider;*/

/*            abgr_src->red = (xyz2rgb[0] * converted_xyz_pixel.x + xyz2rgb[1] * converted_xyz_pixel.y + xyz2rgb[2] * converted_xyz_pixel.z) / divider;
            abgr_src->green = (xyz2rgb[3] * converted_xyz_pixel.x + xyz2rgb[4] * converted_xyz_pixel.y + xyz2rgb[5] * converted_xyz_pixel.z) / divider;
            abgr_src->blue = (xyz2rgb[6] * converted_xyz_pixel.x + xyz2rgb[7] * converted_xyz_pixel.y + xyz2rgb[8] * converted_xyz_pixel.z) / divider;*/

            new_R = (xyz2rgb[0] * converted_xyz_pixel.x + xyz2rgb[1] * converted_xyz_pixel.y + xyz2rgb[2] * converted_xyz_pixel.z) / divider;
            new_G = (xyz2rgb[3] * converted_xyz_pixel.x + xyz2rgb[4] * converted_xyz_pixel.y + xyz2rgb[5] * converted_xyz_pixel.z) / divider;
            new_B = (xyz2rgb[6] * converted_xyz_pixel.x + xyz2rgb[7] * converted_xyz_pixel.y + xyz2rgb[8] * converted_xyz_pixel.z) / divider;

            new_R = ( new_R > 255 ) ? 255 : new_R;
            new_R = ( new_R < 0 ) ? 0 : new_R;
            new_G = ( new_G > 255 ) ? 255 : new_G;
            new_G = ( new_G < 0 ) ? 0 : new_G;
            new_B = ( new_B > 255 ) ? 255 : new_B;
            new_B = ( new_B < 0 ) ? 0 : new_B;

            abgr_src->red = new_R;
            abgr_src->green = new_G;
            abgr_src->blue = new_B;


            if ( x == 100 && y == 100 ) {
                printk("xyz -> rgb [%s](%d %d): (%d %d %d)\n",__func__, x , y , abgr_src->red, abgr_src->green, abgr_src->blue);
            }



/*    M = np.array([[0.412391, 0.357584, 0.180481],
                  [0.212639, 0.715169, 0.072192],
                  [0.019331, 0.119195, 0.950532]])*/
			*abgr_src++;
        }
    }

}

void XYZToRGB(struct eink_8bpp_image *cur_img) {
/*# D65
    xyz2rgb = (3.240970, -1.537383, -0.498611, 0,
               -0.969244, 1.875968, 0.041555, 0,
               0.055630, -0.203977, 1.056972, 0)*/


   int x, y, i;
    struct area_info update_area;

    struct st_abgr *abgr_src;
    u32 offset;

    int xyz2rgb[] = { 3240970, -1537383, -498611, -969244, 1875968, 41555, 55630, -203977, 1056972 }; //D65
    int divider = 1000000;
    struct st_abgr new_pixel;


    memcpy(&update_area, &cur_img->update_area, sizeof(struct area_info));

    offset = cur_img->size.width * cur_img->update_area.y_top + cur_img->update_area.x_top;
    abgr_src = ((struct st_abgr *) cur_img->cfa_vaddr) + offset;
    for (y = update_area.y_top; y <= update_area.y_bottom; y++) {
        offset = cur_img->size.width * y + cur_img->update_area.x_top;
        abgr_src = ((struct st_abgr *) cur_img->cfa_vaddr) + offset;

        for (x = update_area.x_top; x <= update_area.x_bottom; x++) {

            new_pixel.red = (xyz2rgb[0] * abgr_src->red + xyz2rgb[1] * abgr_src->green + xyz2rgb[2] * abgr_src->blue) / divider;
            new_pixel.green = (xyz2rgb[3] * abgr_src->red + xyz2rgb[4] * abgr_src->green + xyz2rgb[5] * abgr_src->blue) / divider;
            new_pixel.blue = (xyz2rgb[6] * abgr_src->red + xyz2rgb[7] * abgr_src->green + xyz2rgb[8] * abgr_src->blue) / divider;
            abgr_src->red = new_pixel.red;
            abgr_src->green = new_pixel.green;
            abgr_src->blue = new_pixel.blue;

            if ( x == 100 && y == 100 ) {
                printk("rgb [%s](%d %d): (%d %d %d)\n",__func__, x , y , abgr_src->red, abgr_src->green, abgr_src->blue);
            }



/*    M = np.array([[0.412391, 0.357584, 0.180481],
                  [0.212639, 0.715169, 0.072192],
                  [0.019331, 0.119195, 0.950532]])*/
			*abgr_src++;
        }
    }

}

void color_correction_matrix_process(struct eink_8bpp_image *cur_img) {
    int x, y, i;
    struct area_info update_area;

    struct st_abgr *abgr_src;
    u32 offset;
    int r,g,b;

    struct st_abgr new_pixel;

    int C00 = 100000;
    int C01 = 0;
    int C02 = 0;
    int C10 = 0;
    int C11 = 100000;
    int C12 = 0;
    int C20 = 0;
    int C21 = 0;
    int C22 = 100000;

    int divider = 100000;


    if ( global_custom == CUSTOM1 ) {
        C00 += global_ch1;
        C01 += global_ch2;
        C02 += global_ch3;
        C10 += global_ch4;
        C11 += global_ch5;
        C12 += global_ch6;
        C20 += global_ch7;
        C21 += global_ch8;
        C22 += global_ch9;
    } else if ( global_custom == V1 ) {
        //ntx optimal, base on AND9334 Gen1 CFA Devices
        C00 = 175815 + 0;
        C01 = -49027  + 0;
        C02 = -26788  + 0;
        C10 = -19781  - 80000;
        C11 = 136582  + 80000;
        C12 = -16800  + 20000;
        C20 =  21167  - 80000;
        C21 = -84224  + 0;
        C22 = 163057  + 80000;


    } else if ( global_custom == V2 ){
        int pocketbook_inv[] = {166971, -90394, 28551, -54605, 188971, -42780, -23979, -39234 , 173013};
        C00 = pocketbook_inv[0];
        C01 = pocketbook_inv[1];
        C02 = pocketbook_inv[2];
        C10 = pocketbook_inv[3];
        C11 = pocketbook_inv[4];
        C12 = pocketbook_inv[5];
        C20 = pocketbook_inv[6];
        C21 = pocketbook_inv[7];
        C22 = pocketbook_inv[8];
    }


    memcpy(&update_area, &cur_img->update_area, sizeof(struct area_info));

    offset = cur_img->size.width * cur_img->update_area.y_top + cur_img->update_area.x_top;
    abgr_src = ((struct st_abgr *) cur_img->cfa_vaddr) + offset;
    for (y = update_area.y_top; y <= update_area.y_bottom; y++) {
        offset = cur_img->size.width * y + cur_img->update_area.x_top;
        abgr_src = ((struct st_abgr *) cur_img->cfa_vaddr) + offset;

        for (x = update_area.x_top; x <= update_area.x_bottom; x++) {

/*            new_pixel.red = (xyz2rgb[0] * abgr_src->red + xyz2rgb[1] * abgr_src->green + xyz2rgb[2] * abgr_src->blue) / divider;
            new_pixel.green = (xyz2rgb[3] * abgr_src->red + xyz2rgb[4] * abgr_src->green + xyz2rgb[5] * abgr_src->blue) / divider;
            new_pixel.blue = (xyz2rgb[6] * abgr_src->red + xyz2rgb[7] * abgr_src->green + xyz2rgb[8] * abgr_src->blue) / divider;
            abgr_src->red = new_pixel.red;
            abgr_src->green = new_pixel.green;
            abgr_src->blue = new_pixel.blue;*/

            r = (C00 * abgr_src->red + C01 * abgr_src->green + C02 * abgr_src->blue) / divider;
            g = (C10 * abgr_src->red + C11 * abgr_src->green + C12 * abgr_src->blue) / divider;
            b = (C20 * abgr_src->red + C21 * abgr_src->green + C22 * abgr_src->blue) / divider;


            if ( x > 600 && x < 610 && y == 110 ) {
                printk("CCM r g b: %d %d %d, %d %d %d\n",abgr_src->red , abgr_src->green, abgr_src->blue, r, g, b);
            }//Damon Test


            if ( r > 0xff )
                r = 0xff;

            if ( g > 0xff )
                g = 0xff;

            if ( b > 0xff )
                b = 0xff;

            if ( r < 0 )
                r = 0;
            if ( g < 0 )
                g = 0;
            if ( b < 0 )
                b = 0;

            abgr_src->red = r;
            abgr_src->green = g;
            abgr_src->blue = b;

            *abgr_src++;
        }
    }

}

int image_ntx_process_YCbCr(struct eink_8bpp_image *cur_img) {

    int x, y, i;
    struct area_info update_area;

    struct st_abgr *abgr_src;
    u32 offset;

    int divider = 1000000;
    struct st_abgr new_pixel;
    int Y = 0, Sr = 100, Sg = 100, Sb = 100, Sy = 100;
    int r,g,b;
    int Sr_new, Sg_new, Sb_new, Sy_new;
    int rgbMin, rgbMax, delta;

    if ( global_custom == CUSTOM1 ) {
		if(global_Sr == 0) global_Sr=100;
		Sr_new = Sr * global_Sr / 100;
		Sr = Sr_new;
 /*       Sr += global_Sr;
        Sg += global_Sg;
        Sb += global_Sb;
        Sy += global_Sy;
 */
		if(global_Sg == 0 ) global_Sg=100;
		Sg_new = Sg * global_Sg / 100;
		Sg = Sg_new;

		if(global_Sb == 0) global_Sb=100;
		Sb_new = Sb * global_Sb / 100;
		Sb = Sb_new;

		if(global_Sy == 0) global_Sy=100;
		Sy_new = Sy * global_Sy / 100;
		Sy = Sy_new;

    } else {

    }


    memcpy(&update_area, &cur_img->update_area, sizeof(struct area_info));

    offset = cur_img->size.width * cur_img->update_area.y_top + cur_img->update_area.x_top;
    abgr_src = ((struct st_abgr *) cur_img->cfa_vaddr) + offset;
    for (y = update_area.y_top; y <= update_area.y_bottom; y++) {
        offset = cur_img->size.width * y + cur_img->update_area.x_top;
        abgr_src = ((struct st_abgr *) cur_img->cfa_vaddr) + offset;

        for (x = update_area.x_top; x <= update_area.x_bottom; x++) {

            rgbMin = abgr_src->red < abgr_src->green?(abgr_src->red < abgr_src->blue?abgr_src->red:abgr_src->blue):(abgr_src->green < abgr_src->blue? abgr_src->green:abgr_src->blue);
            rgbMax = abgr_src->red > abgr_src->green?(abgr_src->red > abgr_src->blue?abgr_src->red:abgr_src->blue):(abgr_src->green > abgr_src->blue? abgr_src->green:abgr_src->blue);
            delta = rgbMax - rgbMin;



            Y = (77*abgr_src->red + 150*abgr_src->green + 29*abgr_src->blue)>>8; //CN102254540A 0077~0081

            r = (Sr * (abgr_src->red - Y)) / 100 + Y * Sy / 100;
            g = (Sg * (abgr_src->green - Y)) / 100 + Y * Sy / 100;
            b = (Sb * (abgr_src->blue - Y)) / 100 + Y * Sy / 100;


            if ( r > 0xff )
                r = 0xff;

            if ( g > 0xff )
                g = 0xff;

            if ( b > 0xff )
                b = 0xff;

            if ( r < 0 )
                r = 0;
            if ( g < 0 )
                g = 0;
            if ( b < 0 )
                b = 0;

			if((abgr_src->red == 255)&&(abgr_src->green == 255)&&(abgr_src->blue == 255)){

			}else if((rgbMax >= 208) && (delta <= 12)){

			}else{

            abgr_src->red = r;
            abgr_src->green = g;
            abgr_src->blue = b;
			}


			*abgr_src++;
        }
    }

}


void eink_image_to_cfa_grayscale(int width, int height,unsigned char *color_buffer, unsigned char *gray_buffer)
{
	int i, j, m, m2;
	int temp;
	int  r, g, b;
	unsigned char *gray = gray_buffer;
	unsigned char *color = color_buffer;

	//For CFA short stride
	for (j = 0; j < height; j++) {
		// In case image width != pandel width
		color = color_buffer + (3 * width * j);
		m2 = j % 3;
		for (i = 0; i < width; i++) {
			m = i % 9;
			
			b = *color++;
			g = *color++;
			r = *color++;
			
			//May change because of panel starting point
			if (m == 0 || m == 1 || m == 2) {
				if(m2 == 0){
					temp = b;
				}
				else if (m2 == 1){
					temp = r;
				}
				else{
					temp = g;
				}
			}else if(m == 3 || m == 4 || m == 5){
				if(m2 == 0){
					temp = r;
				}
				else if (m2 == 1){
					temp = g;
				}
				else{
					temp = b;	
				}
			}else{
				if(m2 == 0){
					temp = g;
				}
				else if (m2 == 1){
					temp = b;
				}
				else{
					temp = r;	
				}
			}
			
/*			if(i<300&&j<100)
				temp = 0;*/
			
			*gray++ = temp;
		}
	}
}

static s32 copy_8bpp_to_img(struct disp_layer_config_inner *config,
	unsigned int layer_num, struct eink_8bpp_image *dest_img, struct area_info update_area,u32 order, u32 width, u32 height)
{
	s32 ret = 0;
	u32 layer_id = 0;
	u32 h = 0, w = 0, offset = 0, len = 0;
	u32 layer_x_offset = 0,layer_y_offset = 0;
	u32 layer_width = 0,layer_height = 0;
	u32 fd_width = 0,fd_height = 0;
	u32 src_offset = 0,dst_offset = 0;
	struct dma_buf *dmabuf = NULL;
	struct ion_buffer *buffer = NULL;
	struct ion_client *client = NULL;
	struct disp_layer_config_inner *tmp_config;
	struct file *file;
	char *src_base_addr = NULL, *src = NULL, *dest = NULL;
	u32 fd = 0;
	u32 gray = 0, step = 0;
	if ((NULL == config) || (NULL == dest_img)) {
		__wrn("input param is null\n");
		return -1;
	}

	tmp_config = config + layer_num;
	fd = tmp_config->info.fb.y8_fd;
	layer_x_offset = tmp_config->info.screen_win.x;
	layer_y_offset = tmp_config->info.screen_win.y;
	layer_height = tmp_config->info.screen_win.height;
	layer_width = tmp_config->info.screen_win.width;
	fd_width = tmp_config->info.fb.size[0].width;
	fd_height = tmp_config->info.fb.size[0].height;
	if ((0 != fd)) {
		__debug("raw update_area(%d,%d)(%d,%d),layer_offset(%d,%d),screent size (%d,%d) ,buffer_size(%d,%d)  fd(%d)\n",update_area.x_top,update_area.y_top,
			update_area.x_bottom,update_area.y_bottom,layer_x_offset,layer_y_offset,layer_width,layer_height,fd_width,fd_height,fd);
		dmabuf = dma_buf_get(fd);
		if (IS_ERR(dmabuf)) {
			__wrn("fail to get dma buffer, fd=%d\n", fd);
			ret = -2;
			goto exit;
		}
		src_base_addr = dma_buf_kmap(dmabuf, 0);
		if (NULL == src_base_addr) {
			__wrn("fail to get dma kernel addr, fd=%d\n",fd);
			ret = -3;
			goto exit;
		}
		
		flush_cache_all();
		outer_flush_all();

		__debug("fd=%d, vaddr=0x%08x\n",fd, src_base_addr);
		__debug("h=%d,w=%d, area(%d,%d)~(%d,%d)\n", height, width,
			update_area.x_top, update_area.y_top, update_area.x_bottom, update_area.y_bottom);
#if 1
		if ((0 == update_area.x_top) && (0 == update_area.y_top) &&
			(width <= (update_area.x_bottom + 1)) && (height <= (update_area.y_bottom + 1))) {
			memcpy((char *)(dest_img->vaddr), src_base_addr, (width * height));
		} else {
			len = update_area.x_bottom - update_area.x_top + 1;
			for (h = update_area.y_top; h <= update_area.y_bottom; h++) {
				dst_offset = h * width + update_area.x_top ;
				src_offset = (h - layer_y_offset) * fd_width + update_area.x_top - layer_x_offset;
				src = (char *)src_base_addr + src_offset;
				dest = (char *)(dest_img->vaddr) + dst_offset;
				memcpy(dest, src, len);
			}
		}		

#else
	memcpy((char *)(dest_img->vaddr), src_base_addr, (layer_width * layer_height));
#endif
	}

exit:
	if (src_base_addr)
		dma_buf_kunmap(dmabuf, 0, src_base_addr);
	if (!IS_ERR(dmabuf)) {
		dma_buf_put(dmabuf);
	}
	return ret;
}

static s32 copy_32bpp_to_8bpp_img(struct disp_layer_config_inner *config,
	unsigned int layer_num, struct eink_8bpp_image *dest_img, struct area_info update_area,u32 order, 
	u32 width, u32 height, int cfa_use, int handwrite)
{
	s32 ret = 0;
	u32 h = 0, w = 0, offset = 0, len = 0;
	u32 layer_x_offset = 0,layer_y_offset = 0;
	u32 layer_width = 0,layer_height = 0;
	u32 fd_width = 0,fd_height = 0;
	u32 src_offset = 0,dst_offset = 0;
	struct dma_buf *dmabuf = NULL;
	struct disp_layer_config_inner *tmp_config;
	char *src_base_addr = NULL, *dest = NULL;
    struct st_argb *argb_src = NULL;
	struct st_abgr *abgr_src = NULL, *cfa_dest = NULL;
	u32 fd = 0;
    unsigned int x, y;
     
	if ((NULL == config) || (NULL == dest_img)) {
		__wrn("input param is null\n");
		return -1;
	}

	tmp_config = config + layer_num;
	fd = tmp_config->info.fb.fd;
	layer_x_offset = tmp_config->info.screen_win.x;
	layer_y_offset = tmp_config->info.screen_win.y;
	layer_height = tmp_config->info.screen_win.height;
	layer_width = tmp_config->info.screen_win.width;
	fd_width = tmp_config->info.fb.size[0].width;
	fd_height = tmp_config->info.fb.size[0].height;
	
	if ((0 != fd)) {
		dmabuf = dma_buf_get(fd);
		if (IS_ERR(dmabuf)) {
			__wrn("fail to get dma buffer, fd=%d\n", fd);
			ret = -2;
			goto exit;
		}
		src_base_addr = dma_buf_kmap(dmabuf, 0);
		
		if (NULL == src_base_addr) {
			__wrn("fail to get dma kernel addr, fd=%d\n",fd);
			ret = -3;
			goto exit;
		}
		
		flush_cache_all();
		outer_flush_all();

		/* cfa use */
		if ((gptHWCFG->m_val.bEPD_Flags & 0x08) && cfa_use) {
			for (h = update_area.y_top; h <= update_area.y_bottom; h++) {
				dst_offset = h * width + update_area.x_top ;
				src_offset = (h - layer_y_offset) * fd_width + update_area.x_top - layer_x_offset;
				abgr_src = ((struct st_abgr *) src_base_addr) + src_offset;
				dest = (char *)(dest_img->vaddr) + dst_offset;
				cfa_dest = (struct st_abgr *)dest_img->cfa_vaddr + dst_offset;
				memcpy(cfa_dest, abgr_src, sizeof(struct st_abgr)*(update_area.x_bottom-update_area.x_top));
				y=h%2;
			
				for (w = update_area.x_top; w <= update_area.x_bottom; w++) {
					x=w%6;
#if 1
					switch(cfa_table[y][x]) {
						case 0:
							*dest = cfa_dest->blue;
							break;
						case 1:
							*dest = cfa_dest->green;
							break;
						case 2:
						default:
							*dest = cfa_dest->red;
							break;
					}
#else
					if (cfa_table[y][x] == 0)
						*dest = cfa_dest->blue;
					else if (cfa_table[y][x] == 1)
						*dest = cfa_dest->green;
					else
						*dest = cfa_dest->red;
#endif
					cfa_dest++;
					dest++;
				}
			}
		} else {
			/* non cfa handwrite use neon argb to y8 */
			if (handwrite) {
				kernel_neon_begin();
				neon_argb_to_gray(src_base_addr, dest_img->vaddr, &update_area, layer_x_offset, layer_y_offset, width);
				kernel_neon_end();
			} else {
				for (h = update_area.y_top; h <= update_area.y_bottom; h++) {
					dst_offset = h * width + update_area.x_top ;
					src_offset = (h - layer_y_offset) * fd_width + update_area.x_top - layer_x_offset;
					argb_src = ((struct st_argb *) src_base_addr) + src_offset;
					dest = (char *)(dest_img->vaddr) + dst_offset;
				
					for (w = update_area.x_top; w <= update_area.x_bottom; w++) {
						*dest = (15 * argb_src->blue + 75 * argb_src->green + 38 * argb_src->red) >> 7;
						argb_src++;
						dest++;
					}
				}
			}
		}
	}

exit:
	if (src_base_addr)
		dma_buf_kunmap(dmabuf, 0, src_base_addr);
	if (!IS_ERR(dmabuf)) {
		dma_buf_put(dmabuf);
	}
	return ret;
}

#ifdef SUPPORT_WB
static s32 convert_32bpp_to_8bpp(struct eink_buffer_manager *buffer_mgr,
			struct disp_layer_config_inner *config,
			unsigned int layer_num,
			unsigned long dest_paddr,
			unsigned int process_width, unsigned int process_height)
{
	struct format_manager * cmgr;
	struct image_format src, dest;
	s32 ret = -1;

	cmgr = disp_get_format_manager(0);
	memset((void*)&src, 0, sizeof(struct image_format));
	src.format = DISP_FORMAT_ARGB_8888;
	//src.addr1 = (unsigned long)src_image;
	src.width = process_width;
	src.height = process_height;

	memset((void*)&dest, 0, sizeof(struct image_format));
	dest.format = DISP_FORMAT_8BIT_GRAY;
	//dest.addr1 = (unsigned long)buffer_mgr->image_slot[in_index].paddr;
	dest.addr1 = dest_paddr;
	dest.addr2 = old_index_data_paddr;	//no used buffer, but you cannot set to 0 when IOMMU is enable
	dest.addr3 = new_index_data_paddr;	//no used buffer, but you cannot set to 0 when IOMMU is enable
	dest.width = process_width;
	dest.height = process_height;

	//used DE hardware to convert 32bpp to 8bpp
	if (NULL != config)
		ret = cmgr->start_convert(0, config, layer_num, &dest);
	
	return ret;
}

static s32 convert_32bpp_to_rgb888(struct eink_buffer_manager *buffer_mgr,
			struct disp_layer_config_inner *config,
			unsigned int layer_num,
			unsigned long dest_paddr,
			unsigned int process_width, unsigned int process_height)
{
	struct format_manager * cmgr;
	struct image_format src, dest;
	s32 ret = -1;

	cmgr = disp_get_format_manager(0);
	memset((void*)&src, 0, sizeof(struct image_format));
	src.format = DISP_FORMAT_ABGR_8888;
	//src.addr1 = (unsigned long)src_image;
	src.width = process_width;
	src.height = process_height;

	memset((void*)&dest, 0, sizeof(struct image_format));
	dest.format = DISP_FORMAT_ABGR_8888;
	//dest.format = DISP_FORMAT_RGB_888;
	//dest.addr1 = (unsigned long)buffer_mgr->image_slot[in_index].paddr;
	dest.addr1 = dest_paddr;
	dest.addr2 = old_index_data_paddr;	//no used buffer, but you cannot set to 0 when IOMMU is enable
	dest.addr3 = new_index_data_paddr;	//no used buffer, but you cannot set to 0 when IOMMU is enable
	dest.width = process_width;
	dest.height = process_height;

	//used DE hardware to convert 32bpp to 8bpp
	if (NULL != config)
		ret = cmgr->start_convert(0, config, layer_num, &dest);
	
	return ret;
}
#else
static int __conver_32bit_bmp_to_8bit(u8 *src_image_data, u32 width, u32 height, u8 *dest_image_data)
{
	struct st_argb *tmp_src_data = NULL;
	u8 *tmp_dest_data = NULL;
	u32 wi = 0, hi = 0, co = 0;

	if ((NULL == src_image_data) || (NULL == dest_image_data)) {
		__wrn("%s: input param is null\n", __func__);
		return -1;
	}

	tmp_dest_data = (u8 *)dest_image_data;
	hi = height;
	while (hi > 0) {
		tmp_src_data = (struct st_argb *)src_image_data + (height - hi) * width;
		for (wi = 0; wi < width; wi++) {
			*tmp_dest_data = (306*tmp_src_data->red + \
								601*tmp_src_data->green + \
								117 * tmp_src_data->blue + 0x200)>>10;
			tmp_dest_data++;
			tmp_src_data++;
			co++;
		}
		hi--;
	}
	__debug("%s: size = %u.\n", __func__, co);

	return 0;
}


static s32 convert_32bpp_to_8bpp(struct eink_buffer_manager *buffer_mgr,
			struct disp_layer_config_inner *config,
			unsigned int layer_num,
			unsigned long dest_paddr)
{
	//__conver_32bit_bmp_to_8bit(u8 *src_image_data, u32 width, u32 height, u8 *dest_image_data)
	return -1;
}

static s32 convert_32bpp_to_rgb888(struct eink_buffer_manager *buffer_mgr,
			struct disp_layer_config_inner *config,
			unsigned int layer_num,
			unsigned long dest_paddr)
{
	//__conver_32bit_bmp_to_8bit(u8 *src_image_data, u32 width, u32 height, u8 *dest_image_data)
	return -1;
}
#endif

#ifdef DITHER_HW
/*
 * Dithering algorithm: from 256(8-bit) gray level to 2(1-bit) gray level 
 */
static void dithering_Y8_to_Y1(
		unsigned char *update_region_virt_ptr,
		dma_addr_t update_region_phys_ptr,
		struct mxcfb_rect *update_region,
		unsigned long update_region_stride,
		int *err_dist)
{
	return;
}

/*
 * Dithering algorithm: simple dithering from 256(8-bit) gray level to 2(1-bit) gray level 
 */
static void dithering_simple_Y8_to_Y1(
		unsigned char *update_region_virt_ptr,
		dma_addr_t update_region_phys_ptr,
		struct mxcfb_rect *update_region,
		unsigned long update_region_stride,
		int *err_dist)
{
	return;
}

/*
 * Dithering algorithm: ntx dithering from 256(8-bit) gray level to 2(1-bit) gray level 
 */
static void dithering_ntx_Y8_to_Y1(
		unsigned char *update_region_virt_ptr,
		dma_addr_t update_region_phys_ptr,
		struct mxcfb_rect *update_region,
		unsigned long update_region_stride,
		int *err_dist)
{
	return;
}

/*
 * Dithering algorithm: from 256(8-bit) gray level to 16(4-bit) gray level 
 */
static void dithering_Y8_to_Y4(
		unsigned char *update_region_virt_ptr,
		dma_addr_t update_region_phys_ptr,
		struct mxcfb_rect *update_region,
		unsigned long update_region_stride,
		int *err_dist)
{
	return;
}
#else
/*
 * Dithering algorithm: from 256(8-bit) gray level to 2(1-bit) gray level
 * it takes about 4Xms in 1440x1080 panel
 */
static void dithering_Y8_to_Y1(
		unsigned char *update_region_virt_ptr,
		dma_addr_t update_region_phys_ptr,
		struct area_info *update_region,
		unsigned long update_region_stride,
		int *err_dist)
{

	/* create a temp error distribution array */
	int bwPix;
	int y;
	int col;
	int *err_dist_l0, *err_dist_l1, *err_dist_l2, distrib_error;
	int width = 0, height = 0;
	int width_3 = 0;
	char *y8buf;
	int x_offset = 0;

	__debug("dither Y8->Y1");
	width = abs(update_region->x_bottom - update_region->x_top);
	height = abs(update_region->y_bottom - update_region->y_top);
	width_3 = width + 3;

	/* prime a few elements the error distribution array */
	for (y = 0; y < height; y++) {
		/* Dithering the Y8 in sbuf to BW suitable for A2 waveform */
		err_dist_l0 = err_dist + (width_3) * (y % 3);
		err_dist_l1 = err_dist + (width_3) * ((y + 1) % 3);
		err_dist_l2 = err_dist + (width_3) * ((y + 2) % 3);

		y8buf = update_region_virt_ptr + x_offset;

		/* scan the line and convert the Y8 to BW */
		for (col = 1; col <= width; col++) {
			bwPix = *(err_dist_l0 + col) + *y8buf;

if (global_test1 == 0) {
			if (bwPix >= 128) {
				*y8buf++ = 0xff;
				distrib_error = (bwPix - 255) >> 3;
			} else {
				*y8buf++ = 0;
				distrib_error = bwPix >> 3;
			}
} else if (global_test1 == 2) {
            if (bwPix >= 0xc0) {
				*y8buf++ = 0xff;
				distrib_error = (bwPix - 0xff) >> 3;
            } else if (bwPix >= 0x80 && bwPix < 0xc0) {
				*y8buf++ = 0xcf;
				distrib_error = (bwPix - 0xcf) >> 3;
            } else if (bwPix >= 0x40 && bwPix < 0x80) {
				*y8buf++ = 0x4f;
				distrib_error = (bwPix - 0x4f) >> 3;
			} else {
				*y8buf++ = 0;
				distrib_error = bwPix >> 3;
			}
} else if (global_test1 == 3) {
            if (bwPix >= 0x80) {
				*y8buf++ = 0xff;
				distrib_error = (bwPix - 0xff) >> 3;
            } else if (bwPix >= 0x40 && bwPix < 0x80) {
				*y8buf++ = 0x4f;
				distrib_error = (bwPix - 0x4f) >> 3;
			} else {
				*y8buf++ = 0;
				distrib_error = bwPix >> 3;
			}

} else if ( global_test1 == 4) {

		if (bwPix > 255)
				bwPix = 255;
			else if (bwPix < 0)
				bwPix = 0;

            if ( (bwPix > 0x00 && bwPix <= 0xef) && (bwPix & 0x0f) != 0) {
				*y8buf++ = bwPix + 0x10;
				distrib_error = (bwPix - (bwPix + 0x10)) >> 3;
            } else {
				*y8buf++ = bwPix;
                distrib_error = 0;
            }
}
			/* modify the error distribution buffer */
			*(err_dist_l0 + col + 2) += distrib_error;
			*(err_dist_l1 + col + 1) += distrib_error;
			*(err_dist_l0 + col + 1) += distrib_error;
			*(err_dist_l1 + col - 1) += distrib_error;
			*(err_dist_l1 + col) += distrib_error;
			*(err_dist_l2 + col) = distrib_error;
		}
		x_offset += update_region_stride;
	}

	flush_cache_all();
	outer_flush_range(update_region_phys_ptr, update_region_phys_ptr +
			height * width);
}

/*
 * Dithering algorithm: simple dither from 256(8-bit) gray level to 2(1-bit) gray level
 * it takes about 17ms in 1440x1080 panel
 */
static void dithering_simple_Y8_to_Y1(
		unsigned char *update_region_virt_ptr,
		dma_addr_t update_region_phys_ptr,
		struct area_info *update_region,
		unsigned long update_region_stride,
		int *err_dist)
{

	/* create a temp error distribution array */
	int bwPix;
	int y;
	int col;
	int width = 0, height = 0;
	char *y8buf;
	int x_offset = 0;

	width = abs(update_region->x_bottom - update_region->x_top) + 1;
	height = abs(update_region->y_bottom - update_region->y_top) + 1;
	
	__debug("simple dither Y8->Y1, w=%d, h=%d\n", width,height);

	for (y = 0; y < height; y++) {
		y8buf = update_region_virt_ptr + x_offset;
		for (col = 0; col < width; col++) {
			bwPix = *y8buf;
			if ((0x00 != bwPix) && (0xff != bwPix)) {
if (global_test1 == 0) {
				if (bwPix >= 128) {
					*y8buf++ = 0xff;
				} else {
					*y8buf++ = 0;
				}

} else if (global_test1 == 2) {
            if (bwPix >= 0xc0) {
				*y8buf++ = 0xff;
            } else if (bwPix >= 0x80 && bwPix < 0xc0) {
				*y8buf++ = 0xcf;
            } else if (bwPix >= 0x40 && bwPix < 0x80) {
				*y8buf++ = 0x4f;
			} else {
				*y8buf++ = 0;
			}
}

			} else {
				y8buf++;
			}
		}
		x_offset += update_region_stride;
	}
}

/*
 * Dithering algorithm: ntx dither from 256(8-bit) gray level to 2(1-bit) gray level
 * it takes about 2Xms in 1440x1080 panel
 */
static void dithering_ntx_Y8_to_Y1(
		unsigned char *update_region_virt_ptr,
		dma_addr_t update_region_phys_ptr,
		struct area_info *update_region,
		unsigned long update_region_stride,
		int *err_dist)
{
	/* create a temp error distribution array */
	int bwPix;
	int y;
	int col;
	int width = 0, height = 0;
	char *y8buf;
	int x_offset = 0;

	width = abs(update_region->x_bottom - update_region->x_top) + 1;
	height = abs(update_region->y_bottom - update_region->y_top) + 1;
	
	__debug("ntx dither Y8->Y1, w=%d, h=%d\n", width,height);

	for (y = 0; y < height; y++) {
		y8buf = update_region_virt_ptr + x_offset;
		for (col = 0; col < width; col++) {
			bwPix = *y8buf;
			if ((0x00 != bwPix) && (0xff != bwPix)) {
				if (bwPix >= giDither8[col%16][y%16]) {
					*y8buf++ = 0xff;
				} else {
					*y8buf++ = 0;
				}
			} else {
				y8buf++;
			}
		}
		x_offset += update_region_stride;
	}
}

/*
 * Dithering algorithm: from 256(8-bit) gray level to 16(4-bit) gray level 
 * * it takes about 4Xms in 1440x1080 panel
 */
static void dithering_Y8_to_Y4(
		unsigned char *update_region_virt_ptr,
		dma_addr_t update_region_phys_ptr,
		struct area_info *update_region,
		unsigned long update_region_stride,
		int *err_dist)
{

	/* create a temp error distribution array */
	int gcPix;
	int y;
	int col;
	int *err_dist_l0, *err_dist_l1, *err_dist_l2, distrib_error;
	int width = 0, height = 0;
	int width_3 = 0;
	char *y8buf;
	int x_offset = 0;

	__debug("dither Y8->Y4");
	width = abs(update_region->x_bottom - update_region->x_top);
	height = abs(update_region->y_bottom - update_region->y_top);
	width_3 = width + 3;
 
	/* prime a few elements the error distribution array */
	for (y = 0; y < height; y++) {
		/* Dithering the Y8 in sbuf to Y4 */
		err_dist_l0 = err_dist + (width_3) * (y % 3);
		err_dist_l1 = err_dist + (width_3) * ((y + 1) % 3);
		err_dist_l2 = err_dist + (width_3) * ((y + 2) % 3);

		y8buf = update_region_virt_ptr + x_offset;

		/* scan the line and convert the Y8 to Y4 */
		for (col = 1; col <= width; col++) {
			gcPix = *(err_dist_l0 + col) + *y8buf;

			if (gcPix > 255)
				gcPix = 255;
			else if (gcPix < 0)
				gcPix = 0;

			*y8buf++ = gcPix & 0xf0;
			distrib_error = (gcPix - (gcPix & 0xf0)) >> 3;

			/* modify the error distribution buffer */
			*(err_dist_l0 + col + 2) += distrib_error;
			*(err_dist_l1 + col + 1) += distrib_error;
			*(err_dist_l0 + col + 1) += distrib_error;
			*(err_dist_l1 + col - 1) += distrib_error;
			*(err_dist_l1 + col) += distrib_error;
			*(err_dist_l2 + col) = distrib_error;
		}
		x_offset += update_region_stride;
	}

	flush_cache_all();
	outer_flush_range(update_region_phys_ptr, update_region_phys_ptr +
			height * width);
}

/*
 * Dithering algorithm: from 256(8-bit) gray level to 4(2-bit) gray level
 * * it takes about 4Xms in 1440x1080 panel
 */
static void dithering_Y8_to_Y2(
		unsigned char *update_region_virt_ptr,
		dma_addr_t update_region_phys_ptr,
		struct area_info *update_region,
		unsigned long update_region_stride,
		int *err_dist)
{

	/* create a temp error distribution array */
	int gcPix;
	int y;
	int col;
	int *err_dist_l0, *err_dist_l1, *err_dist_l2, distrib_error;
	int width = 0, height = 0;
	int width_3 = 0;
	char *y8buf;
	int x_offset = 0;

	__debug("dither Y8->Y2");
	width = abs(update_region->x_bottom - update_region->x_top);
	height = abs(update_region->y_bottom - update_region->y_top);
	width_3 = width + 3;

	/* prime a few elements the error distribution array */
	for (y = 0; y < height; y++) {
		/* Dithering the Y8 in sbuf to Y4 */
		err_dist_l0 = err_dist + (width_3) * (y % 3);
		err_dist_l1 = err_dist + (width_3) * ((y + 1) % 3);
		err_dist_l2 = err_dist + (width_3) * ((y + 2) % 3);

		y8buf = update_region_virt_ptr + x_offset;

		/* scan the line and convert the Y8 to Y4 */
		for (col = 1; col <= width; col++) {
			gcPix = *(err_dist_l0 + col) + *y8buf;

			if (gcPix > 255)
				gcPix = 255;
			else if (gcPix < 0)
				gcPix = 0;

			distrib_error = (*y8buf - (gcPix & 0xf0)) >> 3;

			*y8buf++ = gcPix & 0xf0;

			/* modify the error distribution buffer */
			*(err_dist_l0 + col + 2) += distrib_error;
			*(err_dist_l1 + col + 1) += distrib_error;
			*(err_dist_l0 + col + 1) += distrib_error;
			*(err_dist_l1 + col - 1) += distrib_error;
			*(err_dist_l1 + col) += distrib_error;
			*(err_dist_l2 + col) = distrib_error;
		}
		x_offset += update_region_stride;
	}
	
	flush_cache_all();
	outer_flush_range(update_region_phys_ptr, update_region_phys_ptr +
			height * width);
}

#endif


static void gamma_process_ntx(u32 *gamma_tbl,
		unsigned char *update_region_virt_ptr,
		dma_addr_t update_region_phys_ptr,
		u32 width,
		u32 height,
		unsigned long update_region_stride,
		int *err_dist)
{

	/* create a temp error distribution array */
	int y;
	int col;
	int width_3 = 0;
	char *y8buf;
	int x_offset = 0;

	__debug("gamma process  width  %d  height  %d\n",width,height);
	width_3 = width + 3;

	/* prime a few elements the error distribution array */
	for (y = 0; y < height; y++) {
		/* Dithering the Y8 in sbuf to BW suitable for A2 waveform */

		y8buf = update_region_virt_ptr + x_offset;

		/* scan the line and convert the Y8 to BW */
		for (col = 0; col < width; col++) {
                *y8buf = gamma_tbl[*y8buf];
                y8buf++;
		}
		x_offset += update_region_stride;
	}

	flush_cache_all();
	outer_flush_range(update_region_phys_ptr, update_region_phys_ptr +
			height * width);
}

static s32 gamma_process(u32 *gamma_tbl, struct eink_8bpp_image *cur_img)
{
	int *err_dist = NULL;
	u32 screen_width = 0, screen_height = 0;

	/*if (EINK_NO_DITHER == type) {
		return 0;
	}*/

	if (NULL == cur_img) {
		__wrn("input param is null\n");
		return -1;
	}
 
	screen_width = cur_img->size.width;
	screen_height = cur_img->size.height;
	
	err_dist = kzalloc((screen_width + 3) * 3 * sizeof(int), GFP_KERNEL);
	if (NULL == err_dist) {
		__wrn("alloc memory fail\n");
		return -2;
	}
	
	gamma_process_ntx(gamma_tbl,(char *)cur_img->vaddr,
				cur_img->paddr,
				screen_width,
				screen_height,
				ALIGN(screen_width,8),
				err_dist);

	kfree(err_dist);
	
	return 0;		
}	



static s32 dither_process(eink_dither_type type, struct eink_8bpp_image *cur_img)
{
	int *err_dist = NULL;
	u32 offset = 0;
	u32 screen_width = 0, screen_height = 0;
    
	if (EINK_NO_DITHER == type) {
		return 0;
	}
	
	if (NULL == cur_img) {
		__wrn("input param is null\n");
		return -1;
	}

	screen_width = cur_img->size.width;
	screen_height = cur_img->size.height;
	
	err_dist = kzalloc((screen_width + 3) * 3 * sizeof(int), GFP_KERNEL);
	if (NULL == err_dist) {
		__wrn("alloc memory fail\n");
		return -2;
	}
	
	offset = screen_width * cur_img->update_area.y_top + cur_img->update_area.x_top;

	switch (type)
	{
		case EINK_DITHER_Y8_Y1:
			dithering_Y8_to_Y1(
				(char *)cur_img->vaddr + offset,
				cur_img->paddr + offset,
				&cur_img->update_area,
				ALIGN(screen_width,8),
				err_dist);
			break;

		case EINK_SIMPLE_DITHER_Y8_Y1:
		case EINK_MONOCHROME:
			dithering_simple_Y8_to_Y1(
				(char *)cur_img->vaddr + offset,
				cur_img->paddr + offset,
				&cur_img->update_area,
				ALIGN(screen_width,8),
				err_dist);
			break;

		case EINK_DITHER_Y8_Y4:
			dithering_Y8_to_Y4(
				(char *)cur_img->vaddr + offset,
				cur_img->paddr + offset,
				&cur_img->update_area,
				ALIGN(screen_width,8),
				err_dist);
			break;
			
		case EINK_NTX_DITHER_Y8_Y1:
			dithering_ntx_Y8_to_Y1(
				(char *)cur_img->vaddr + offset,
				cur_img->paddr + offset,
				&cur_img->update_area,
				ALIGN(screen_width,8),
				err_dist);
			break;
			
		default:
			__wrn("unkown dithering type\n");
			break;
	}
	
	kfree(err_dist);
	
	return 0;
}

static void g2d_process(g2d_blt_flags_h g2d_flag, struct eink_8bpp_image *src_img, struct eink_8bpp_image *dst_img, u32 src_width, u32 src_height, u32 dst_width, u32 dst_height, int cfa_use)
{
	g2d_blt_h g2d_para;
	
	memset(&g2d_para, 0, sizeof(g2d_blt_h));
	
	g2d_para.flag_h = g2d_flag;
	
	g2d_para.src_image_h.width = src_width;
	g2d_para.src_image_h.height = src_height;
	g2d_para.src_image_h.align[0] = 0;
	g2d_para.src_image_h.align[1] = g2d_para.src_image_h.align[0];
	g2d_para.src_image_h.align[2] = g2d_para.src_image_h.align[0];
	if ((gptHWCFG->m_val.bEPD_Flags & 0x08) && cfa_use)
		g2d_para.src_image_h.format = G2D_FORMAT_ABGR8888;
	else
		g2d_para.src_image_h.format = G2D_FORMAT_Y8;
	g2d_para.src_image_h.clip_rect.w = src_width;
	g2d_para.src_image_h.clip_rect.h = src_height;
	g2d_para.src_image_h.clip_rect.x = 0;
	g2d_para.src_image_h.clip_rect.y = 0;
	if ((gptHWCFG->m_val.bEPD_Flags & 0x08) && cfa_use)
		g2d_para.src_image_h.laddr[0] = src_img->cfa_paddr;
	else
		g2d_para.src_image_h.laddr[0] = src_img->paddr;
			
	g2d_para.dst_image_h.width = dst_width;
	g2d_para.dst_image_h.height = dst_height;
	g2d_para.dst_image_h.align[0] = 0;
	g2d_para.dst_image_h.align[1] = g2d_para.src_image_h.align[0];
	g2d_para.dst_image_h.align[2] = g2d_para.src_image_h.align[0];
	if ((gptHWCFG->m_val.bEPD_Flags & 0x08) && cfa_use)
		g2d_para.dst_image_h.format = G2D_FORMAT_ABGR8888;
	else
		g2d_para.dst_image_h.format = G2D_FORMAT_Y8;
	g2d_para.dst_image_h.clip_rect.w = dst_width;
	g2d_para.dst_image_h.clip_rect.h = dst_height;
	g2d_para.dst_image_h.clip_rect.x = 0;
	g2d_para.dst_image_h.clip_rect.y = 0;
	if ((gptHWCFG->m_val.bEPD_Flags & 0x08) && cfa_use)
		g2d_para.dst_image_h.laddr[0] = dst_img->cfa_paddr;
	else
		g2d_para.dst_image_h.laddr[0] = dst_img->paddr;
	
	g2d_module_open();
	g2d_module_blit_h(&g2d_para);
	g2d_module_release();
		
	if ((gptHWCFG->m_val.bEPD_Flags & 0x08) && cfa_use)
		disp_eink_cache_sync(dst_img->cfa_vaddr, ((char *)dst_img->cfa_vaddr + (dst_width * dst_height * 4)));
	else
		disp_eink_cache_sync(dst_img->vaddr, ((char *)dst_img->vaddr + (dst_width * dst_height)));
}

static void adjust_coordinates(g2d_blt_flags_h g2d_flag, u32 screen_width, u32 screen_height, struct area_info *src_update_area, struct area_info *dst_update_area)
{
	int region_width = 0, region_height = 0, temp = 0;
	
	region_width = abs(src_update_area->x_bottom - src_update_area->x_top) + 1;
	region_height = abs(src_update_area->y_bottom - src_update_area->y_top) + 1;
	
	/* Rotate */
	switch(g2d_flag & 0xf00) {
		case G2D_ROT_90:
			dst_update_area->x_top = screen_width - (src_update_area->y_top + region_height);
			dst_update_area->y_top = src_update_area->x_top;
			dst_update_area->x_bottom = screen_width - src_update_area->y_top - 1;
			dst_update_area->y_bottom = src_update_area->x_bottom;
			break;
			
		case G2D_ROT_180:
			dst_update_area->x_top = screen_width - (src_update_area->x_top + region_width);
			dst_update_area->y_top = screen_height - (src_update_area->y_top + region_height);
			dst_update_area->x_bottom = screen_width - src_update_area->x_top - 1;
			dst_update_area->y_bottom = screen_height - src_update_area->y_top - 1;
			break;
		  
		case G2D_ROT_270:
			dst_update_area->x_top = src_update_area->y_top;
			dst_update_area->y_top = screen_height - (src_update_area->x_top + region_width);
			dst_update_area->x_bottom = src_update_area->y_bottom;
			dst_update_area->y_bottom = screen_height - src_update_area->x_top - 1;
			break;
			
		default:
			dst_update_area->x_top = src_update_area->x_top;
			dst_update_area->y_top = src_update_area->y_top;
			dst_update_area->x_bottom = src_update_area->x_bottom;
			dst_update_area->y_bottom = src_update_area->y_bottom;
			break;
	}
	
	region_width = abs(dst_update_area->x_bottom - dst_update_area->x_top) + 1;
	region_height = abs(dst_update_area->y_bottom - dst_update_area->y_top) + 1;
	
	/* Flip */
	if ((g2d_flag & 0xf000) == G2D_ROT_V) {
		dst_update_area->x_bottom = dst_update_area->x_bottom;
		dst_update_area->y_bottom = screen_height - dst_update_area->y_top - 1;
		dst_update_area->x_top = dst_update_area->x_top;
		dst_update_area->y_top = screen_height - region_height - dst_update_area->y_top;
	} else if ((g2d_flag & 0xf000) == G2D_ROT_H) {
		dst_update_area->y_bottom = dst_update_area->y_bottom - 1;
		dst_update_area->x_bottom = screen_width - dst_update_area->x_top - 1;
		dst_update_area->y_top = dst_update_area->y_top;
		dst_update_area->x_top = screen_width - region_width - dst_update_area->x_top;
	}
	
	__debug("adjust coordinates: area(%d,%d)~(%d,%d)\n", dst_update_area->x_top, dst_update_area->y_top, \
							      dst_update_area->x_bottom, dst_update_area->y_bottom);
}

#define MAX_STATISTICS_GRAY_LEVEL			16
#define Y1_THRESHOLD 		90
#define Y4_THRESHOLD 		90
#define WHITE_THRESHOLD 	50
struct timeval ams_stimer, ams_etimer;

static u32 local_mode_cnt = 0;
/*
* Auto mode selection for image, it takes about 45ms for 1024x758
*/
static s32 auto_mode_select(struct eink_8bpp_image *img, enum eink_update_mode *mode, int full_region)
{
	struct area_info *update_area = NULL;	
	enum eink_update_mode auto_mode = EINK_INIT_MODE;
	u32 x = 0, y = 0, offset = 0;
	u32 area_width = 0, area_height = 0, area_size = 0;
	u32 scn_width = 0, scn_height = 0, scn_area_size = 0;
	u32 *data32_ptr = NULL;
	u8 *data8_prt = NULL;
	s32 x_top_align = 0, x_bottom_align = 0;
	u32 start_offset = 0, end_offset = 0;
	u32 data32_1 = 0, data32_2 = 0;
	u8 data1 = 0, data2=0, data3=0, data4 = 0;
	u32 gray_level = 0, tmp_gray_level = 0, valid_gray_level = 0;
	u32 image_histogram_table[256] = {0};	
	u32 image_histogram_table16[16] = {0};
	u32 gray_ratio = 0;
	u32 total_pixel = 0, Y1_total_pixel = 0, Y4_total_pixel = 0,white_total_pixel = 0;
	u32 Y1_ratio = 0, Y4_ratio = 0, white_ratio = 0;
	u32 pixel_sum = 0;

	if ((NULL == img) || (NULL == mode)) {
		__wrn("input param is null\n");
		return -1;
	}

	do_gettimeofday(&ams_stimer);

	update_area = &img->update_area;
	area_width = abs(update_area->x_bottom - update_area->x_top) + 1;
	area_height = abs(update_area->y_bottom - update_area->y_top) + 1;
	area_size = area_width * area_height;
	
	scn_width = img->size.width;
	scn_height = img->size.height;
	scn_area_size = scn_width * scn_height;

	if (0 == (update_area->x_top % 8)) {
		x_top_align = update_area->x_top;
	} else {
		x_top_align = ALIGN(update_area->x_top, 8);
	}

	if (0 == ((update_area->x_bottom + 1) % 8)) {	//x_bottom no-align 8 bytes
		x_bottom_align = update_area->x_bottom;	//include x_bottom col
	} else {
		x_bottom_align = ALIGN((update_area->x_bottom + 1), 8) - 9;
	}
	
	if ((area_width == img->size.width) && (0 == (img->size.width%8))) {
		//stripe len = screen width, use one loop to deal with it
		//__debug("stripe len = screen width, use one loop to deal with it\n");
		start_offset = img->size.width * update_area->y_top;
		end_offset = img->size.width * update_area->y_bottom + img->size.width -1;
		data32_ptr = (u32 *)(img->vaddr) + (start_offset>>2);
		for (offset = start_offset; offset < end_offset; offset += 8) {
			data32_1 = *data32_ptr++;	// 4 bytes
			data32_2 = *data32_ptr++;	// 4 bytes

			data1 = data32_1 & 0xff;
			data2 = (data32_1 >> 8) & 0xff;
			data3 = (data32_1 >> 16) & 0xff;
			data4 = (data32_1 >> 24) & 0xff;
			image_histogram_table[data1]++;
			image_histogram_table[data2]++;
			image_histogram_table[data3]++;
			image_histogram_table[data4]++;

			data1 = data32_2 & 0xff;
			data2 = (data32_2 >> 8) & 0xff;
			data3 = (data32_2 >> 16) & 0xff;
			data4 = (data32_2 >> 24) & 0xff;
			image_histogram_table[data1]++;
			image_histogram_table[data2]++;
			image_histogram_table[data3]++;
			image_histogram_table[data4]++;
		}
	} else if (x_top_align >= x_bottom_align) {
		//width of area is smaller than 8, calculate directly
		//__debug("width of area is smaller than 8, calculate directly\n");
		for (y = update_area->y_top; y <= update_area->y_bottom; y++) {
			offset = y * img->size.width + update_area->x_top;
			data8_prt = (char *)(img->vaddr) + offset;
			for (x=update_area->x_top; x <= update_area->x_bottom; x++) {
				image_histogram_table[*data8_prt]++;
				data8_prt++;
			}
		}
	} else {
		/*
		*split area to 3 part: x_top -> x_top_align->x_bottom_align->x_bottom
		*/

		//__debug("split into 3 part area\n");
		//calc 1st part: (x_top,y_top)~(x_top_align, y_bottom), not align
		if (update_area->x_top < x_top_align) {
			//__debug("caculate 1st part\n");
			for (y = update_area->y_top; y <= update_area->y_bottom; y++) {
				offset = y * img->size.width + update_area->x_top;
				data8_prt = (char *)(img->vaddr) + offset;
				for (x=update_area->x_top; x < x_top_align; x++) {
					image_histogram_table[*data8_prt]++;
					data8_prt++;
				}
			}
		}

		//__debug("caculate 2nd part\n");
		//calc 2nd part: (x_top_align,y_top)~(x_bottom_align, y_bottom), 8byte align
		for (y = update_area->y_top; y <= update_area->y_bottom; y++) {
			offset = y * img->size.width + x_top_align;
			data32_ptr = (u32 *)(img->vaddr) + (offset>>2);

			// 8 bytes per cycle
			for (x = x_top_align; x <= x_bottom_align; x += 8) {
				data32_1 = *data32_ptr++;	// 4 bytes
				data32_2 = *data32_ptr++;	// 4 bytes

				data1 = data32_1 & 0xff;
				data2 = (data32_1 >> 8) & 0xff;
				data3 = (data32_1 >> 16) & 0xff;
				data4 = (data32_1 >> 24) & 0xff;
				image_histogram_table[data1]++;
				image_histogram_table[data2]++;
				image_histogram_table[data3]++;
				image_histogram_table[data4]++;

				data1 = data32_2 & 0xff;
				data2 = (data32_2 >> 8) & 0xff;
				data3 = (data32_2 >> 16) & 0xff;
				data4 = (data32_2 >> 24) & 0xff;
				image_histogram_table[data1]++;
				image_histogram_table[data2]++;
				image_histogram_table[data3]++;
				image_histogram_table[data4]++;
			}
		}

		//calc 3th part: (x_bottom_align,y_top)~(x_bottom, y_bottom), not align
		if (update_area->x_bottom > x_bottom_align) {
			//__debug("caculate 3th part\n");
			for (y = update_area->y_top; y <= update_area->y_bottom; y++) {
				offset = y * img->size.width + x_bottom_align;
				data8_prt = (char *)(img->vaddr) + offset;
				for (x=x_bottom_align; x < update_area->x_bottom; x++) {
					image_histogram_table[*data8_prt]++;
					data8_prt++;
				}
			}
		}
	}

	//calculate 256 gray level to 16 level, and process 16 gray level histogram
	total_pixel = area_width * area_height;
	pixel_sum = 0;
	memset(image_histogram_table16, 0, sizeof(image_histogram_table16));
	for (gray_level=0; gray_level < 256; gray_level++) {
		tmp_gray_level = gray_level >> 4;
		image_histogram_table16[tmp_gray_level] += image_histogram_table[gray_level];
		pixel_sum += image_histogram_table[gray_level];
	}

	if (pixel_sum != total_pixel) {
		__wrn("pixel total(%d) != sum(%d)\n", total_pixel, pixel_sum);
	}

	for (tmp_gray_level = G0;tmp_gray_level<=G15;tmp_gray_level++) {
		gray_ratio = image_histogram_table16[tmp_gray_level];
		if (gray_ratio != 0)
			valid_gray_level++;
		//__debug("G%d ratio = %d\n", tmp_gray_level, image_histogram_table16[tmp_gray_level]);
	}

	/*Y1_total_pixel = image_histogram_table16[G0] + image_histogram_table16[G15];
	Y4_total_pixel = image_histogram_table16[G0] + image_histogram_table16[G5] +
	image_histogram_table16[G10] + image_histogram_table16[G15];
	white_total_pixel = image_histogram_table16[G15];*/
	//printk(KERN_WARNING"Y1=%d,Y4=%d,White=%d\n",Y1_total_pixel,Y4_total_pixel,white_total_pixel);

	/*Y1_ratio = Y1_total_pixel * 100 /total_pixel;
	Y4_ratio = Y4_total_pixel * 100 /total_pixel;
	white_ratio = white_total_pixel * 100/total_pixel;*/
	
	if ((valid_gray_level <= 2)/* && (Y1_ratio >= Y1_THRESHOLD)*/) {
		if (full_region) {
			auto_mode = EINK_DU_MODE;
		} else {
			auto_mode = EINK_PARTIAL_MODE | EINK_DU_MODE;
		}
		//auto_mode = EINK_GU16_MODE;
		local_mode_cnt++;
	} else {
		if (full_region) {
			auto_mode = EINK_GC16_MODE;
		} else {
			auto_mode = EINK_PARTIAL_MODE | EINK_GL16_MODE;
		}

		local_mode_cnt++;
		/*if (white_ratio >= WHITE_THRESHOLD) {
			//auto_mode = EINK_GU16_MODE;
			auto_mode = EINK_GU16_MODE;
			local_mode_cnt++;
		} else {
			//auto_mode = EINK_GU16_MODE;
			auto_mode = EINK_GU16_MODE;
			local_mode_cnt++;
		}*/
	}

#ifdef DEBUG_VER
	if (local_mode_cnt >= 15) {
		if ((auto_mode == EINK_GL16_MODE) || (auto_mode  == EINK_GU16_MODE) || 
			(auto_mode == EINK_GLR16_MODE) || (auto_mode == EINK_GLD16_MODE)) {
			auto_mode = EINK_GC16_MODE;
			local_mode_cnt = 0;
			update_area->x_top = 0;
			update_area->y_top = 0;
			update_area->x_bottom = img->size.width -1;
			update_area->y_bottom = img->size.height -1;
		}
	} else {
		#if 0
		//large area update
		area_size = area_width * area_height;
		if ((area_size >= (scn_area_size >> 1)) && ((auto_mode == EINK_GL16_MODE) || (auto_mode  == EINK_GU16_MODE))) {
			auto_mode = EINK_GC16_MODE;
			local_mode_cnt = 0;
		}
		#endif
	}
#endif

	*mode = auto_mode;

	do_gettimeofday(&ams_etimer);

	//__debug("valid_gray_level=%d, Y1_ratio=%d, Y4_ratio=%d, white_ratio=%d, mode = 0x%x\n", 
	//	valid_gray_level, Y1_ratio, Y4_ratio, white_ratio, *mode);

	__debug("auto mode take %d ms\n", get_delt_ms_timer(ams_stimer, ams_etimer));
	return 0;
}

static s32 calculate_update_area(struct eink_8bpp_image *last_img, struct eink_8bpp_image *cur_img)
{
	struct index_manager *idx_mgr = NULL;
	s32 ret = -1;
	
	idx_mgr = disp_get_index_manager(0);
	if ((NULL == last_img) || (NULL == cur_img) || (NULL == idx_mgr)) {
		__wrn("input param is null\n");
		return -1;
	}

    	memset(&cur_img->update_area, 0, sizeof(struct area_info));
	if (idx_mgr->start_calc_index) {
		ret = idx_mgr->start_calc_index(0, old_index_data_paddr, new_index_data_paddr, last_img, cur_img);
	}

	__debug("get update area: <%d, %d>,<%d, %d>\n", \
				cur_img->update_area.x_top,
				cur_img->update_area.y_top,
				cur_img->update_area.x_bottom,
				cur_img->update_area.y_bottom);

	return ret;
}

//#define TRACK_UPDATE_BUFFER
void trace_input_image_buffer(u32 order, char *buf, u32 width, u32 height,char* prefix)
{
	char file_name[256] = {0};

	if (NULL == buf) {
		__wrn("input param is null\n");
		return;
	}
	
	memset(file_name, 0, sizeof(file_name));
//	snprintf(file_name, sizeof(file_name), "/data/bmp/%d_%s.bmp", order, prefix);
	if(global_method == HSL){
		snprintf(file_name, sizeof(file_name), "/data/bmp/%dHSL_%s_%d_%d.bmp", order, prefix, global_S,global_L);//Damon Test
	}
	else if(global_method == HSV){
		snprintf(file_name, sizeof(file_name), "/data/bmp/%dHSV_%s_%d_%d_%d.bmp", order, prefix, global_H,global_S,global_V);//Damon Test
	}
	else if(global_method == CCM){
		snprintf(file_name, sizeof(file_name), "/data/bmp/%dCCM_%s_%d_%d_%d.bmp", order, prefix, global_ch1,global_ch5,global_ch9);//Damon Test
	}
	else if(global_method == HSVYCbCr){
		snprintf(file_name, sizeof(file_name), "/data/bmp/%dHSVY_%s_%d_%d_%d.bmp", order, prefix, global_S,global_V,global_Sy);//Damon Test
	}
	else{
		snprintf(file_name, sizeof(file_name), "/data/bmp/%dOTHER_%s_%d_%d_%d.bmp", order, prefix, global_S,global_V,global_Sy);//Damon Test
	}


	save_gray_image_as_bmp_24bit((u8 *)buf, file_name, width, height);
	//save_gray_image_as_bmp_8bit((u8 *)buf, file_name, width, height);
	
	//save_bin_file((u8 *)buf,file_name,(width * height), 0);
}


static struct timeval index_stimer, index_etimer;

int eink_new_process(struct disp_eink_manager *manager,struct image_buffer_node *img_node)
{

	int ret = -1;
	unsigned long flags = 0;
	int pipe_id = -1;
	int i;
	unsigned int update_master = 0;
	unsigned int index_paddr = 0;
	unsigned char *index_vaddr = NULL;
	unsigned int index_buffer_size = 0;
	struct eink_buffer_manager *buf_mgr = NULL;
	struct eink_8bpp_image *img = NULL;
	struct pipeline_manager *pipe_mgr = NULL;
	struct index_manager *idx_mgr = NULL;
	
	struct pipe_info* soft_pipe;
	struct pipe_info pinfo;
	struct area_info max_area;
	unsigned int active_fail = 0;
	unsigned int overlap_size = 0;
	int temperature = 0;
	unsigned int waveform_vaddr = 0;
	unsigned int power_cnt = 0;
	unsigned int free_pipe_state = 0;
	enum e_power_state power_state = 0;
	enum eink_update_mode update_mode = 0;
	unsigned int fail_request_cnt = 0, fail_start_cnt = 0, fail_init_cnt = 0;
	struct area_info overlap_area_align;
	bool valid_update_flag = false;
	u32 order = 0;
	struct area_info collision_area;
	struct frame_cnt frame;
	struct timeval cur_update_timer;
	int delt_update_time = 0;
	enum eink_update_mode cur_update_mode = 0;
	char *working_buffer_vaddr = NULL;
	char working_buffer_size = 0;
	
	bool ntx_handwrite_solution = manager->ntx_handwrite_solution;

	if (NULL == manager) {
		__wrn("eink manager is not initial\n");
		return -1;
	}
	buf_mgr = manager->buffer_mgr;
	pipe_mgr = manager->pipeline_mgr;
	idx_mgr = manager->idx_mgr;

	img = img_node->img;
	order = img_node->update_order;
	memset(&pinfo, 0, sizeof(pinfo));
	pinfo.img = img;
	memcpy(&pinfo.org_update_area, &img->update_area, sizeof(img->update_area));
	pinfo.mode = img->update_mode;
	pinfo.update_order = order;
	ret = pipe_mgr->soft_pipe_get_collision_relationship(pipe_mgr,&pinfo);

	if (pinfo.collision_flag) {
		img_node->collision_flag = pinfo.collision_flag;
		buf_mgr->add_image_to_collision_list(buf_mgr, img_node);
	}		
	if (rectangle_is_zero(pinfo.overlap_area) && rectangle_is_zero(pinfo.update_area) &&
		(pinfo.img->empty_frame_flag != true)) {
		__debug("whole area collision, overlap type=%d\n", pinfo.overlap_type);
		if (NULL != pinfo.update_image_mask) {
			vfree(pinfo.update_image_mask);
			pinfo.update_image_mask = NULL;
			}
			return 0;		
		}

	do_gettimeofday(&index_stimer);

	__debug("\n");
	//ret = calc_index_for_non_overlap_part(manager, &pinfo);


	if (ret) {
		//TODO: deal with fail
		__wrn("calc index by HW fail, ret=%d\n", ret);
	}
	
	__debug("\n");
	if (false == rectangle_is_zero(pinfo.overlap_area)) {
		if (NULL == pinfo.update_image_mask) {
			__wrn("update_image_mask is null, something is wrong\n");
		} else {
			if (manager->panel_info.bit_num > EINK_BIT_4) {
				if ((pinfo.mode & 0xff) == EINK_DU_MODE || (pinfo.mode & 0xff) == EINK_A2_MODE) {
                    
                    __debug("sw >>>>> Y8->IB\n");
					ret = new_process_calc_index_for_overlap_part_for_du_5bit(manager, &pinfo, &collision_area);
                    __debug("sw <<<<< Y8->IB\n");
				}else{
					ret = calc_index_for_overlap_part_for_5bit(manager, &pinfo, &collision_area);
				}
			} else {
				if ((pinfo.mode & 0xff) == EINK_DU_MODE || (pinfo.mode & 0xff) == EINK_A2_MODE)
					ret = new_process_calc_index_for_overlap_part_for_du_4bit(manager, &pinfo, &collision_area);
				else
					ret = calc_index_for_overlap_part_for_4bit(manager, &pinfo, &collision_area);
			}
			if (pinfo.collision_flag) {
				__debug("collision area(%d,%d)~(%d,%d) in overlap area(%d,%d)~(%d,%d)\n", \
					collision_area.x_top, collision_area.y_top, collision_area.x_bottom, collision_area.y_bottom, \
					pinfo.overlap_area.x_top, pinfo.overlap_area.y_top, pinfo.overlap_area.x_bottom, pinfo.overlap_area.y_bottom);
				memcpy(&(pinfo.img->update_area), &collision_area, sizeof(collision_area));
				img_node->collision_flag = pinfo.collision_flag;
				buf_mgr->add_image_to_collision_list(buf_mgr, img_node);
				if (rect_include_judge(collision_area, pinfo.overlap_area)) {
					pinfo.overlap_type = OVERLAP_WHOLE_COLLISION;
					if ((NULL != pinfo.update_image_mask)) {
						vfree(pinfo.update_image_mask);
						pinfo.update_image_mask = NULL;
					}
					memset(&pinfo.overlap_area, 0, sizeof(pinfo.overlap_area));
				} else {
					pinfo.overlap_type = OVERLAP_PARTIAL_COLLISION;
				}
			}
		}
	}

	
	__debug("\n");
	index_vaddr = manager->index_buffer.index_vaddr[manager->index_buffer.cur_index];
	index_buffer_size = manager->index_buffer.index_buf_size;
	//disp_eink_cache_sync(index_vaddr, (index_vaddr + index_buffer_size));
#ifdef TRACE_INDEX_BUFFER_HW
    if((pinfo.mode & 0xFF) == EINK_DU_MODE) {
        int x = pinfo.org_update_area.x_top;
        int y = pinfo.org_update_area.y_top;
        int width = pinfo.org_update_area.x_bottom - x + 1;
        int height = pinfo.org_update_area.y_bottom - y + 1;

        dump_pixels("after Y8->IB: index_vaddr", index_vaddr , x,y,width,height,manager->panel_info.width*2,2);
    }
#endif

    __debug("\n");
	working_buffer_vaddr = manager->work_buffer.workbuffer_vaddr;
	working_buffer_size = manager->work_buffer.workbuffer_size;
	//disp_eink_cache_sync(working_buffer_vaddr, (working_buffer_vaddr + working_buffer_size));
    __debug("\n");
	do_gettimeofday(&index_etimer);
	
	__debug("order=%d, update index&WB for overlap takes %d ms\n",	order, get_delt_ms_timer(index_stimer, index_etimer));

	//whole screen update too fast
	do_gettimeofday(&cur_update_timer);
	delt_update_time = get_delt_ms_timer(manager->last_update_timer, cur_update_timer);
	cur_update_mode = GET_UPDATE_MODE(pinfo.mode);

	
	manager->last_update_mode = cur_update_mode;
	memcpy(&manager->last_update_area_info, &pinfo.update_area, sizeof(pinfo.update_area));
	memcpy(&manager->last_update_timer, &cur_update_timer, sizeof(cur_update_timer));

	temperature = manager->get_temperature(manager);
	disp_al_get_waveform_data(manager->disp, GET_UPDATE_MODE(pinfo.mode), 
		temperature, &pinfo.total_frames, &pinfo.waveform_paddr, &pinfo.waveform_vaddr);
	__debug("order=%d, temp=%d, mode=0x%x, total=%d, waveform_paddr=0x%08x, waveform_vaddr=0x%08x\n",
		order, temperature, pinfo.mode, pinfo.total_frames, pinfo.waveform_paddr, pinfo.waveform_vaddr);
		
#ifdef TRACE_INDEX_BUFFER
#if SUPPORT_NEON_ALGORITHM
		if ((pinfo.mode & 0xff) == EINK_DU_MODE) {
			align_area(pinfo.overlap_area, &overlap_area_align, NEON_DATA_ALIGN);
		} else {
			align_area(pinfo.overlap_area, &overlap_area_align, 8);
		}
#else
		align_area(pinfo.overlap_area, &overlap_area_align, 8);
#endif
		overlap_size = (overlap_area_align.x_bottom - overlap_area_align.x_top + 1) *
					(overlap_area_align.y_bottom - overlap_area_align.y_top + 1);
		save_update_mask_buffer(order, pinfo.update_image_mask, overlap_size);
		trace_new_buffer(order, manager);
		save_waveform(order, pinfo.waveform_vaddr, pinfo.total_frames);
#endif
		
	__debug("order=%d,mode=0x%x,empty=%d,ORG:(%d,%d)~(%d,%d),OVP:(%d,%d)~(%d,%d),UPD:(%d,%d)~(%d,%d)\n",
		order,pinfo.mode, pinfo.img->empty_frame_flag,
		pinfo.org_update_area.x_top,pinfo.org_update_area.y_top,
		pinfo.org_update_area.x_bottom,pinfo.org_update_area.y_bottom,
		pinfo.overlap_area.x_top,pinfo.overlap_area.y_top,
		pinfo.overlap_area.x_bottom,pinfo.overlap_area.y_bottom,
		pinfo.update_area.x_top,pinfo.update_area.y_top,
		pinfo.update_area.x_bottom,pinfo.update_area.y_bottom);


	fail_request_cnt = 0;
	fail_start_cnt = 0;
	fail_init_cnt = 0;
	pipe_id = -1;
		
	do {
		ret = pipe_mgr->image_update_start();
		if (ret) {
			fail_start_cnt++;
			msleep(5);
			continue;
		}

		if (pipe_id < 0) {
			pipe_id = pipe_mgr->request_soft_pipe(pipe_mgr);
			if (pipe_id < 0) {
				fail_request_cnt++;
				pipe_mgr->image_update_end();
				msleep(5);
				continue;
			}
		}
		
		pipe_mgr->init_soft_pipe(pipe_mgr,pipe_id, pinfo);
		index_paddr = manager->index_buffer.index_paddr[manager->index_buffer.cur_index];
		index_vaddr = manager->index_buffer.index_vaddr[manager->index_buffer.cur_index];

		ret = pipe_mgr->update_and_active_soft_pipe(pipe_mgr, pipe_id, index_paddr, index_vaddr, pinfo, &frame);
		if (ret) {
			pipe_mgr->image_update_end();
			__wrn("[DEBUG]soft pipe(%d) too late to insert(t=%d,d=%d,f=%d), wait for power off\n", 
				pipe_id, frame.all_total_frames, frame.all_decode_frame_cnt, frame.all_fresh_frame_cnt);
			pipe_mgr->wait_for_eink_power_off(pipe_mgr);
			fail_init_cnt++;
		}
	}while((0 != ret) && (fail_start_cnt < 200) && (fail_request_cnt < 200) && (fail_init_cnt < 20));

	__debug("order=%d, update soft pipe(%d) ok, wait for power on\n", order, pipe_id);
	
	if (fail_init_cnt < 20) {
		//init pipe ok, and wait for eink power on
		ret = pipe_mgr->wait_for_eink_power_on(pipe_mgr);
		__debug("wait for power on %s\n", (ret==0)?"ok":"timeout");
	} else {
		__wrn("active pipe fail for a long time, ignore this update\n");
	}
	
	//index addr has updated to pipeline manager, switch old index and new index buffer

	/*if (ntx_handwrite_solution == false)
		manager->index_buffer.cur_index = (manager->index_buffer.cur_index)?0:1;*/

	pipe_mgr->image_update_end();
	img_node->img->state = FREE;
	list_move_tail(&img_node->node, &buf_mgr->image_free_list);
		
	return ret;

}

static s32 eink_handwrite_background(g2d_blt_flags_h g2d_flag, struct eink_8bpp_image *src_img, struct eink_8bpp_image *dst_img, int screen_width, int screen_height, int cfa_use)
{
	int x, y, width, height;
	u32 offset;
	char *src_buf, *dst_buf;
	struct area_info get_region;
	g2d_blt_flags_h g2d_flag_type = G2D_BLT_NONE_H;
	
	/* check g2d flip */
	if (g2d_flag > G2D_BLT_NONE_H) {
		if (g2d_flag & G2D_ROT_V) 
			g2d_flag_type = G2D_ROT_V;
		else if (g2d_flag & G2D_ROT_H)
			g2d_flag_type = G2D_ROT_H;
		  
		if (g2d_flag_type > G2D_BLT_NONE_H)
			g2d_process(g2d_flag_type, src_img, dst_img, screen_width, screen_height, screen_width, screen_height, src_img->cfa_use);
		else
			dst_img = src_img;
	} else {
		dst_img = src_img;
	}
	
	/* save background to dma buffer */
	memcpy(&get_region, &g_bg_setting->get_region, sizeof(struct area_info));
	
	width = g_bg_setting->width;
	height = g_bg_setting->height;
	
	if (NULL == dst_img) {
		__wrn("input param is null\n");
		return -1;
	}

	if ((gptHWCFG->m_val.bEPD_Flags & 0x08) && cfa_use) {
		if (g_bg_setting->vaddr) {
			offset = get_region.x_top*4 + get_region.y_top * ALIGN(screen_width * 4, 8);
			src_buf = (char *)dst_img->cfa_vaddr + offset;
			dst_buf = (char *)g_bg_setting->vaddr;
		
			for (y = 0; y < height; y++) {
				memcpy(dst_buf, src_buf, ALIGN(width * 4, 8));
				src_buf += ALIGN(screen_width * 4, 8);
				dst_buf += ALIGN(width * 4, 8);
			}
		}
	} else {
		if (g_bg_setting->vaddr) {
			offset = get_region.x_top + get_region.y_top * screen_width;
			src_buf = (char *)dst_img->vaddr + offset;
			dst_buf = (char *)g_bg_setting->vaddr;
		
			for (y = 0; y < height; y++) {
				memcpy(dst_buf, src_buf, width);
				src_buf += screen_width;
				dst_buf += width;
			}
		}
	}
	
#ifdef TRACK_UPDATE_BUFFER
	trace_input_image_buffer(2, dst_img->vaddr, buffer_mgr->width, buffer_mgr->height, "bg_src");
	trace_input_image_buffer(2, g_bg_setting->vaddr, width, height, "bg_back");
#endif
	return 0;
}

static bool update_area_check(struct eink_buffer_manager *buffer_mgr, struct area_info update_area)
{
	if (rectangle_is_zero(update_area))
		return true;
	
	if (update_area.x_top < 0 || update_area.y_top < 0 || update_area.x_bottom < 0 || update_area.y_bottom < 0)
		return false;
	if (update_area.x_top >= buffer_mgr->width || update_area.y_top >= buffer_mgr->height || 
	    update_area.x_bottom >= buffer_mgr->width || update_area.y_bottom >= buffer_mgr->height)
		return false;
	
	return true;
}

static bool update_area_inside(struct area_info update_area_org, struct area_info update_area_calc)
{
	if ((update_area_org.x_top < update_area_calc.x_top) &&
        (update_area_org.y_top < update_area_calc.y_top) &&
        (update_area_org.x_bottom > update_area_calc.x_bottom) &&
        (update_area_org.y_bottom > update_area_calc.y_bottom))
        return false;
	
	return true;
}

s32 reflush_working_buffer_data(bool pmic_pg_status)
{
    s32 ret = 0;
    struct disp_eink_manager* manager;
    struct pipeline_manager *pipe_mgr = NULL;
    struct eink_buffer_manager *buf_mgr = NULL;
    struct image_buffer_node *cur_img_node = NULL, *last_img_node = NULL, *tmp_img_node = NULL, *ttmp_img_node = NULL;
	struct eink_8bpp_image *cur_img = NULL, *last_img = NULL;
	int i, j, width, height;
	struct area_info last_region;
	int temperature = 0;
	
	manager = disp_get_eink_manager(0);
	if (NULL == manager) {
		return ret;
	}
	
	buf_mgr = manager->buffer_mgr;
	pipe_mgr = manager->pipeline_mgr;

	if (pmic_pg_status == false)  {
		for (j = 0; j < 2; j++) {
			for (i = 0; i < 3; i++) {
				mutex_lock(&buf_mgr->mlock);
				if (bsp_disp_get_eink_method() == 1) {
					if (list_empty(&buf_mgr->image_free_list)) {
						__wrn("!!!!! drop image, order(%d) area(%d,%d)-(%d,%d)\n", g_last_image_node->update_order,
						g_last_image_node->img->update_area.x_top,g_last_image_node->img->update_area.y_top,
						g_last_image_node->img->update_area.x_bottom,g_last_image_node->img->update_area.y_bottom);
						g_cur_image_node = g_last_image_node;
						g_last_image_node = g_last_2nd_image_node;
					} else {
						tmp_img_node = g_cur_image_node;
						list_for_each_entry_safe(g_cur_image_node, ttmp_img_node, &buf_mgr->image_free_list, node) {
							if (NULL != g_cur_image_node) {	//get the first node
								break;
							}
						}
						g_last_2nd_image_node = g_last_image_node;
						g_last_image_node = tmp_img_node;
					}
 				}

				cur_img_node = g_cur_image_node;
				last_img_node = g_last_image_node;
			    	if ((NULL == cur_img_node) || (NULL == last_img_node) || (NULL == cur_img_node->img) || (NULL == last_img_node->img)) {
					__wrn("fail to get last and current image node\n");
					ret = -EINVAL;
					goto out;
				}
				
				__list_del_entry(&cur_img_node->node);	//remove from free list
				
				cur_img_node->update_order = buf_mgr->update_order++;
				last_img = last_img_node->img;
				last_region.x_top = 0;
				last_region.y_top = 0;
				last_region.x_bottom = buf_mgr->width - 1;
				last_region.y_bottom = buf_mgr->height - 1;
				
				cur_img = cur_img_node->img;
				cur_img->state = USED;
				
				cur_img->update_mode = EINK_NO_MERGE | EINK_GCC16_MODE;
				
				cur_img->update_area.x_top = last_region.x_top + (buf_mgr->width / 3) * i;
				cur_img->update_area.y_top = last_region.y_top + (buf_mgr->height / 2) * j;
				cur_img->update_area.x_bottom = last_region.x_top + (buf_mgr->width / 3) * (i + 1) - 1;
				cur_img->update_area.y_bottom = last_region.y_top + (buf_mgr->height / 2) * (j + 1) - 1;
	
				memcpy(cur_img->vaddr, last_img->vaddr, buf_mgr->width * buf_mgr->height);
	
				disp_eink_cache_sync(cur_img->vaddr, ((char *)cur_img->vaddr + (buf_mgr->width * buf_mgr->height)));
 	
				//trace_input_image_buffer(buf_mgr->update_order, manager->work_buffer.workbuffer_vaddr, buf_mgr->width, buf_mgr->height, "reflush");
				/*__debug("voltage drop: add to update list order=%d, mode=0x%x, (%d,%d)~(%d,%d), empty=%d\n", cur_img_node->update_order, cur_img->update_mode, cur_img->update_area.x_top, cur_img->update_area.y_top, cur_img->update_area.x_bottom, cur_img->update_area.y_bottom, cur_img->empty_frame_flag);*/
				
				list_add_tail(&cur_img_node->node, &buf_mgr->image_used_list);
				wake_up_process(manager->pixel_process_task);
				mutex_unlock(&buf_mgr->mlock);
			}
		}


	} else {
		if (gptHWCFG->m_val.bDisplayResolution == 14 && gptHWCFG->m_val.bUIStyle == 2) {
			temperature = manager->get_temperature(manager);
			if (temperature <= 6) {
				return 0;
			}
		}
		mutex_lock(&buf_mgr->mlock);
		if(bsp_disp_get_eink_method() == 1){		
			if (list_empty(&buf_mgr->image_free_list)) {
				__debug("!!!!! drop image, order(%d) area(%d,%d)-(%d,%d)\n", g_last_image_node->update_order,
					g_last_image_node->img->update_area.x_top,g_last_image_node->img->update_area.y_top,
					g_last_image_node->img->update_area.x_bottom,g_last_image_node->img->update_area.y_bottom);
				g_cur_image_node = g_last_image_node;
				g_last_image_node = g_last_2nd_image_node;
			} else {
				tmp_img_node = g_cur_image_node;
				list_for_each_entry_safe(g_cur_image_node, ttmp_img_node, &buf_mgr->image_free_list, node) {
					if (NULL != g_cur_image_node) {	//get the first node
						break;
					}
				}
				g_last_2nd_image_node = g_last_image_node;
				g_last_image_node = tmp_img_node;
			}
		}
		
		cur_img_node = g_cur_image_node;
		last_img_node = g_last_image_node;
     
    	if ((NULL == cur_img_node) || (NULL == last_img_node) || (NULL == cur_img_node->img) || (NULL == last_img_node->img)) {
			__wrn("fail to get last and current image node\n");
			ret = -EINVAL;
			goto out;
		}
		
		__list_del_entry(&cur_img_node->node);	//remove from free list
		
		cur_img_node->update_order = buf_mgr->update_order++;
		
		cur_img = cur_img_node->img;
		cur_img->state = USED;
		
		cur_img->update_mode = EINK_NO_MERGE | EINK_GC16_MODE;
     
		cur_img->update_area.x_top = 0;
		cur_img->update_area.y_top = 0;
		cur_img->update_area.x_bottom = buf_mgr->width - 1;
		cur_img->update_area.y_bottom = buf_mgr->height - 1;
    
		memcpy(cur_img->vaddr, manager->work_buffer.workbuffer_vaddr, buf_mgr->width * buf_mgr->height);
    
		disp_eink_cache_sync(cur_img->vaddr, ((char *)cur_img->vaddr + (buf_mgr->width * buf_mgr->height)));
    
		//trace_input_image_buffer(buf_mgr->update_order, manager->work_buffer.workbuffer_vaddr, buf_mgr->width, buf_mgr->height, "reflush");
	
		list_add_tail(&cur_img_node->node, &buf_mgr->image_used_list);
		wake_up_process(manager->pixel_process_task);
		mutex_unlock(&buf_mgr->mlock);
	}
	
out:
	if ((NULL != cur_img) && (NULL != cur_img_node)) {
		if (USED == cur_img->state) {
			__debug("add to update list: order=%d, mode=0x%x, (%d,%d)~(%d,%d), empty=%d\n",
					cur_img_node->update_order, cur_img->update_mode,
					cur_img->update_area.x_top, cur_img->update_area.y_top,
					cur_img->update_area.x_bottom, cur_img->update_area.y_bottom,
					cur_img->empty_frame_flag);
		}
	}else{	
		mutex_unlock(&buf_mgr->mlock);
		return ret;
		
	}
    
    return ret;
}

static void calc_image_update_region(struct eink_8bpp_image *cur_img, char *working_buf, int width, int height)
{
    int x, y, w, h;
    u32 offset = 0;
    int p1_x, p1_y, p2_x, p2_y;
    
    u8 *cur_y8_buf, *last_y8_buf;
    
    p1_x = width - 1;
    p1_y = height - 1;
    p2_x = 0;
    p2_y = 0;
    
    for (y = cur_img->update_area.y_top; y <= cur_img->update_area.y_bottom; y++) {
        offset = y * width + cur_img->update_area.x_top;
        cur_y8_buf = cur_img->vaddr + offset;
        last_y8_buf = working_buf + offset;

        for (x = cur_img->update_area.x_top; x <= cur_img->update_area.x_bottom; x++) {
            if (*cur_y8_buf != *last_y8_buf) {
                p1_x = min(p1_x, x);
                p1_y = min(p1_y, y);
                break;
            }
            cur_y8_buf++;
            last_y8_buf++;
        }
    }
    
    for (y = cur_img->update_area.y_bottom; y > cur_img->update_area.y_top; y--) {
        offset = y * width + cur_img->update_area.x_bottom;
        cur_y8_buf = cur_img->vaddr + offset;
        last_y8_buf = working_buf + offset;
        for (x = cur_img->update_area.x_bottom; x > cur_img->update_area.x_top; x--) {
            if (*cur_y8_buf != *last_y8_buf) {
                p2_x = max(p2_x, x);
                p2_y = max(p2_y, y);
                break;
            }
            cur_y8_buf--;
            last_y8_buf--;
        }
    }
    
    cur_img->update_area.x_top = min(p1_x, p2_x);
    cur_img->update_area.y_top = min(p1_y, p2_y);
    cur_img->update_area.x_bottom = max(p1_x, p2_x);
    cur_img->update_area.y_bottom = max(p1_y, p2_y);
    
    return;
}

static int image_ntx_process_HSV(struct eink_8bpp_image *cur_img)
{
    int h, s, v, region, remainder, p, q, t;
    int x, y, i;
    int rgbMin, rgbMax, delta;
    struct area_info update_area;
	int faceDetect = 0;
	int s_org ,h_old;
	int hsvTest = 0 , hsvTest2 = 0, LeafTest=0;
	int redTest = 0 , redTest2 = 0;

    struct st_abgr *abgr_src;
    u32 offset;

    memcpy(&update_area, &cur_img->update_area, sizeof(struct area_info));

    offset = cur_img->size.width * cur_img->update_area.y_top + cur_img->update_area.x_top;
    abgr_src = ((struct st_abgr *) cur_img->cfa_vaddr) + offset;

    for (y = update_area.y_top; y <= update_area.y_bottom; y++) {
        offset = cur_img->size.width * y + cur_img->update_area.x_top;
        abgr_src = ((struct st_abgr *) cur_img->cfa_vaddr) + offset;

            for (x = update_area.x_top; x <= update_area.x_bottom; x++) {
            rgbMin = abgr_src->red < abgr_src->green?(abgr_src->red < abgr_src->blue?abgr_src->red:abgr_src->blue):(abgr_src->green < abgr_src->blue? abgr_src->green:abgr_src->blue);
            rgbMax = abgr_src->red > abgr_src->green?(abgr_src->red > abgr_src->blue?abgr_src->red:abgr_src->blue):(abgr_src->green > abgr_src->blue? abgr_src->green:abgr_src->blue);
            delta = rgbMax - rgbMin;

            v = rgbMax;
            if (v == 0) {
                h = 0;
                s = 0;
                goto RGB_test;
            }

            s = 255 * (rgbMax - rgbMin) / v;
            if (s == 0) {
                h = 0;
                goto RGB_test;
            }

            if((rgbMax >= 208) && (delta <= 12))
				goto RGB_end;

/*            if (rgbMax == abgr_src->red)
                h = 0 + 43 * (abgr_src->green - abgr_src->blue) / (rgbMax - rgbMin);
            else if (rgbMax == abgr_src->green)
                h = 85 + 43 * (abgr_src->blue - abgr_src->red) / (rgbMax - rgbMin);
            else
                h = 171 + 43 * (abgr_src->red - abgr_src->green) / (rgbMax - rgbMin);*/

            if (rgbMax == abgr_src->red){
                if (abgr_src->green >= abgr_src->blue)
			h = 0 + 43 * (abgr_src->green - abgr_src->blue) / (rgbMax - rgbMin);
                else if (abgr_src->green < abgr_src->blue)
			h = 255 + 43 * (abgr_src->green - abgr_src->blue) / (rgbMax - rgbMin);
            } else if (rgbMax == abgr_src->green)
                h = 85 + 43 * (abgr_src->blue - abgr_src->red) / (rgbMax - rgbMin);
            else
                h = 170 + 43 * (abgr_src->red - abgr_src->green) / (rgbMax - rgbMin);


/*			if ( x > 100 && x < 102 && y == 100 ) {
				printk("r g b: %d %d %d, %d %d %d\n",abgr_src->red , abgr_src->green, abgr_src->blue, h, s, v);
			}*/

RGB_face:
			if((h > 0) && (h < 50) && (s >= 23) && (s <= 184)){
				if((abgr_src->red > 90) && (abgr_src->green > 40) && (abgr_src->blue > 20)){

					if((abgr_src->red > abgr_src->green) && (abgr_src->red > abgr_src->blue) && (((abgr_src->green - abgr_src->red )>15)||((abgr_src->red - abgr_src->green)>15))){
						faceDetect = 1;
					}
				}

			}

RGB_test:

            if (s == 0) {
                abgr_src->red = v;
                abgr_src->green = v;
                abgr_src->blue = v;
                goto RGB_end;
            }

			s_org = s;
			h_old = h;
/*            s = 255;
            v = 255;*/

            if (( global_custom == CUSTOM1 ) && (global_mode == 1)) {

                h += global_H;
                //s += global_S;
				if(global_S == 0) global_S=50;
				s = (s * global_S) / 50;
                //l += global_L;
				if(global_V == 0) global_V=50;
				v = (v * global_V) / 50;
				if((faceDetect == 1)){
					if((h>=10)&&(h<=22)){
						s = (s*global_SF1)/50;
						v = (v*global_VF1)/50;
						hsvTest = 1;
					}else if((h > 0)&&(h<=9)){
						h += 5;
						s = (s*global_SF2)/50;
						v = (v*global_VF2)/50;
						hsvTest2 = 1;
					}
				}


			}else{
				h += global_H;
                //s += global_S;
				if(global_S == 0) global_S=50;
				s = (s * global_S) / 50;
                //l += global_L;
				if(global_V == 0) global_V=50;
				v = (v * global_V) / 50;


			}

//Red Enhance
			if(global_mode == 0){
				if((h_old>=10)&&(h_old<=22)&&(s_org>100)){
					h += 246;
			        if (h > 255) {
						h -= 255;
					}
					s = (s*65)/50;
					v = (v*65)/50;
					redTest = 1;
				}else if((h_old > 0)&&(h_old<=9)&&(s_org>100)){
					h = 0;
					s = (s*65)/50;
					v = (v*65)/50;
					redTest2 = 1;
				}

			}

//Yellow enhance
			if ((h_old>=33)&&(h_old<=35)&&(s_org>180)){
				h += 8;
				s = (s*66)/50;
				v = (v*53)/50;
			}

//Green enhance
			if((h_old>=46)&&(h_old<=85)&&(s_org>45)){
				LeafTest = 1;
				h += 248;//global_H;
				s = (s*100)/50;
				v = (v*65)/50;
			}


            if (h > 255) {
                h -= 255;
            }

            if ( s > 0xff ) {
                s = 0xff;
            }

            if ( v > 0xff ) {
                v = 0xff;
            }


/*            if (s >= 224)
                s = 255;
            else if (s < 224 && s >= 192)
                s = 224;
            else if (s < 192 && s >= 160)
                s = 192;
            else if (s < 160 && s >= 128)
                s = 160;
            else if (s < 128 && s >= 96)
                s = 128;
            else if (s < 96 && s >= 64)
                s = 96;
            else if (s < 64 && s >= 32)
                s = 64;
            else if (s < 32 && s >= 16)
                s = 32;
            else
                s = 0;*/
           /*
            if (v >= 224)
                v = 255;
            else if (v < 224 && v >= 192)
                v = 224;
            else if (v < 192 && v >= 160)
                v = 192;
            else if (v < 160 && v >= 128)
                v = 160;
            else if (v < 128 && v >= 96)
                v = 128;
            else if (v < 96 && v >= 64)
                v = 96;
            else if (v < 64 && v >= 32)
                v = 64;
            else if (v < 32 && v >= 16)
                v = 32;
            else
                v = 0;*/

/*            if (  h < 22 && h > -20 )
                h = 0;
            else if ( h >= 22 && h < 64 )
                h = 43;
            else if ( h >= 64 && h < 107)
                h = 85;
            else if ( h >= 107 && h < 149)
                h = 128;
            else if ( h >= 149 && h < 192)
                h = 170;
            else if ( h >= 192 && h < 234)
                h = 213;*/

            region = (h / 43) % 6;
            remainder = (h - (region * 43)) * 6;


            p = (v * (255 - s)) >> 8;
            q = (v * (255 - ((s * remainder) >> 8))) >> 8;
            t = (v * (255 - ((s * (255 - remainder)) >> 8))) >> 8;

            switch (region)
            {
                case 0:
                    abgr_src->red = v; abgr_src->green = t; abgr_src->blue = p;
                    break;

                case 1:
                    abgr_src->red = q; abgr_src->green = v; abgr_src->blue = p;
                    break;

                case 2:
                    abgr_src->red = p; abgr_src->green = v; abgr_src->blue = t;
                    break;

                case 3:
                    abgr_src->red = p; abgr_src->green = q; abgr_src->blue = v;
                    break;

                case 4:
                    abgr_src->red = t; abgr_src->green = p; abgr_src->blue = v;
                    break;

                default:
                    abgr_src->red = v; abgr_src->green = p; abgr_src->blue = q;
                    break;
            }


/*			if ( x > 100 && x < 102 && y == 100 ) {
				printk("f:r g b: %d %d %d, %d %d %d\n",abgr_src->red , abgr_src->green, abgr_src->blue, h, s, v);
			}*/

			if (faceDetect == 1){
				if(hsvTest == 1){
					hsvTest = 0;
//					abgr_src->red = 255;
//					abgr_src->green = 255;
//					abgr_src->blue = 255;
				}
				if(hsvTest2 == 1){
					hsvTest2 = 0;
//					abgr_src->red = 0;
//					abgr_src->green = 255;
//					abgr_src->blue = 0;
				}
				faceDetect = 0;
			}

			if (LeafTest == 1){
				LeafTest = 0;
//				abgr_src->red = 255;
//				abgr_src->green = 0;
//				abgr_src->blue = 0;
			}

			if (redTest == 1){
				redTest = 0;
			}
			if (redTest2 == 1){
				redTest2 = 0;
			}


RGB_end:
            *abgr_src++;
        }
    }

    return 0;
}

//NTX HSV color mapping feature
static int image_ntx_process_HSL(struct eink_8bpp_image *cur_img)
{
    int h, s, v,l, region, remainder, p, q, t;
   int x, y, i;
    int rgbMin, rgbMax;
    struct area_info update_area;
    
    struct st_abgr *abgr_src;
    u32 offset;
    
    int delta, value;
    int diff,tc;
    int h_old = 0;
    int boost_high_th_s = 10;
    int boost_high_th_l = 224;
    int boost_low_th_l = 32;
    int boost_rate_s = 250;
    int boost_rate_l = 105;
    
    memcpy(&update_area, &cur_img->update_area, sizeof(struct area_info));
    
    offset = cur_img->size.width * cur_img->update_area.y_top + cur_img->update_area.x_top;
    abgr_src = ((struct st_abgr *) cur_img->cfa_vaddr) + offset;
    for (y = update_area.y_top; y <= update_area.y_bottom; y++) {
        offset = cur_img->size.width * y + cur_img->update_area.x_top;
        abgr_src = ((struct st_abgr *) cur_img->cfa_vaddr) + offset;
        
        for (x = update_area.x_top; x <= update_area.x_bottom; x++) {
            rgbMin = abgr_src->red < abgr_src->green?(abgr_src->red < abgr_src->blue?abgr_src->red:abgr_src->blue):(abgr_src->green < abgr_src->blue? abgr_src->green:abgr_src->blue);
            rgbMax = abgr_src->red > abgr_src->green?(abgr_src->red > abgr_src->blue?abgr_src->red:abgr_src->blue):(abgr_src->green > abgr_src->blue? abgr_src->green:abgr_src->blue);
        	// convert to HSL space  
            delta = (rgbMax - rgbMin);
            value = (rgbMax + rgbMin);
            
            l = value /2;
            
            if (rgbMax == abgr_src->red){
				if (abgr_src->green >= abgr_src->blue)
					h = 0 + 60 * (abgr_src->green - abgr_src->blue) / (rgbMax - rgbMin);
				else if (abgr_src->green < abgr_src->blue)
					h = 360 + 60 * (abgr_src->green - abgr_src->blue) / (rgbMax - rgbMin);
			} else if (rgbMax == abgr_src->green)
			   h = 120 + 60 * (abgr_src->blue - abgr_src->red) / (rgbMax - rgbMin);
			else
			   h = 240 + 60 * (abgr_src->red - abgr_src->green) / (rgbMax - rgbMin);
			
            if (delta == 0) {
            	h =0;
            	s = 0;
            }else if(l < 128){
            	s = 255 * delta / value; 
            }else{
            	s = (255 * delta) / (510 - value); 
            }

/*            if ( x > 600 && x < 610 && y == 110 ) {
                printk("=================================\n");
                printk("r g b: %d %d %d, %d %d %d\n",abgr_src->red , abgr_src->green, abgr_src->blue, h, s, l);
            }
*/
            h_old = h;

            if ( global_custom == CUSTOM1 ) {
				if ( s > boost_high_th_s && l > boost_low_th_l && l < boost_high_th_l ) {
					h += global_H;
                //s += global_S;
					if(global_S == 0) global_S=1;
					s = s * global_S / 50;
                //l += global_L;
					if ( l < 128 ) {
						l = l * global_L/50;
						if ( l > 128 )
						l = 128;
					} else {
						l = ((l - 255) * global_L /50) + 255 ;
						if ( l < 128 )
						l = 128;
					}
					if((h >= 55) && (h<=64)){
						s = s * 2;
						l = l*105/100;
					}
				}
            } else if ( global_custom == V1 ){
                if ( s > boost_high_th_s && l > boost_low_th_l && l < boost_high_th_l ) {
                    /*
                       if ( h % 60 != 0) {
                       if ( (h / 30) % 2 == 0 ) {
                       h -= global_ch1;
                       } else {
                       h += global_ch1;
                       }

                       if (h < 0) {
                       h += 360;
                       }
                       if (h >= 360) {
                       h -= 360;
                       }

                       if ( (h_old / 30) != (h / 30)) {
                       if ( h > h_old )
                       h = (h / 60) * 60;
                       else
                       h = (h_old / 60) * 60;
                       }

                       }*/

                    s = s * boost_rate_s/100;

                    if ( l < 128 ) {
                        l = l * boost_rate_l/100;
                        if ( l > 128 )
                            l = 128;
                    } else {
                        l = ((l - 255) * boost_rate_l / 100) + 255 ;
                        if ( l < 128 )
                            l = 128;
                    }
                }
            }

            
            if (h < 0) {   
                h += 360;  
            }  
            if (h >= 360) {  
                h -= 360;  
            }
            
            if( s < 0) {  
                s = 0;  
            }  
            if( s > 255) {  
                s = 255;  
            }  
            if( l < 0) {  
                 l = 0;  
            }  
			if( l > 255) {  
				l = 255;  
			}

            // conversion back to RGB space here!!  
            if (s == 0) {  
            	abgr_src->red = l;  
            	abgr_src->green = l;  
            	abgr_src->blue = l;  
		   } else { 
               if (l < 128) {  
                 q = (l * (255 + s))/255;  
               } else {  
                  q = l + s -  ((s * l)/255);  
               }
               
               p = 2 * l - q;  
               diff = q - p;
               
               tc = h + 120;  
               if (tc >= 360)   
            	   tc -= 360;
               
               if (tc < 60) {  
            	   abgr_src->red = (p + (diff * tc) /60);  
               }  
               else if (tc < 180) {  
            	   abgr_src->red = q;  
               }  
               else if (tc < 240) {  
            	   abgr_src->red = (p + diff * (4*60 - tc)/60);  
               }  
               else {  
            	   abgr_src->red = p;  
               }  
                 
               tc = h;  
               if (tc < 60) {  
            	   abgr_src->green = (p + (diff * tc) /60);  
               }  
               else if (tc < 180) {  
            	   abgr_src->green = q;  
               }   
               else if (tc < 240) {  
            	   abgr_src->green = (p + diff * (4*60 - tc )/60);  
               }  
               else {  
            	   abgr_src->green = p;  
               }  
                 
               tc = h - 120;  
               if (tc < 0) {  
            	   tc += 360;  
               }  
               if (tc < 60) {  
            	   abgr_src->blue = (p + (diff * tc) /60);  
               }  
               else if (tc < 180) {  
            	   abgr_src->blue = q;  
               }  
               else if (tc < 240) {  
            	   abgr_src->blue = (p + diff * (4*60 - tc )/60);  
               }  
               else {  
            	   abgr_src->blue = p;  
               } 
		   }
            /*if ( x > 600 && x < 610 && y == 110 ) {
                printk("r g b: %d %d %d, %d %d %d\n",abgr_src->red , abgr_src->green, abgr_src->blue, h, s, l);
            }*/

			*abgr_src++;
        }
    }
    
   return 0;
}


static int ntx_mode1_set_arg(void)
{
	global_H = 0;
	global_S = 50;
	global_L = 50;
	global_custom = V1;
	return 0;
}

static int ntx_mode2_set_arg(void)
{
//	echo "6 2 246 80 55 65 65 65 65 100 100 100 100 0" > /d/dispdbg/param
	global_H = 0;
	global_S = 80;
	global_V = 55;
	global_SF1 = 65;
	global_VF1 = 65;
	global_SF2 = 65;
	global_VF2 = 65;

	global_custom = CUSTOM1;

	global_Sr = 100;
	global_Sg = 100;
	global_Sb = 100;
	global_Sy = 100;
	global_mode = 0;
	return 0;
}

static int ntx_mode3_set_arg(void)
{
//	echo "6 2 0 50 50 103 74 76 66 100 100 100 100 1" > /d/dispdbg/param
	global_H = 5;
	global_S = 50;
	global_V = 50;
	global_SF1 = 107;
	global_VF1 = 70;
	global_SF2 = 107;
	global_VF2 = 70;

	global_custom = CUSTOM1;

	global_Sr = 100;
	global_Sg = 100;
	global_Sb = 100;
	global_Sy = 100;
	global_mode = 1;
	return 0;
}
static int ntx_mode4_set_arg(void)
{

	global_H = 0;
	global_S = 50;
	global_L = 50;
	global_custom = V1;
	return 0;
}

static int ntx_mode5_set_arg(void)
{
	global_H = 0;
	global_S = 50;
	global_L = 50;
	global_custom = V1;
	return 0;
}

static int eink_image_process_thread(void *parg)
{
    int ret = 0;
    struct disp_eink_manager* manager = &eink_manager[0];
    struct eink_buffer_manager* buf_mgr = manager->buffer_mgr;
    struct image_buffer_node *img_node = NULL;
    struct image_buffer_node *cur_img_node = NULL, *tmp_img_node = NULL;
    struct eink_8bpp_image *img = NULL;
    enum eink_update_mode auto_mode = 0;
    struct area_info tmp_update_area;
    int cfa_use = 0, width = 0, height = 0;
    char *working_buf = NULL;
    
    u32 index;
    u8 *vaddr;
    
    for (;;) {
#ifdef TEST_QUEUE_BUFFER_TIME
        do_gettimeofday(&image_proc_stimer);
#endif
        if (kthread_should_stop()) {
            break;
        }
        
        if(bsp_disp_get_eink_method() == 1){
            try_to_freeze();
            set_current_state(TASK_INTERRUPTIBLE);
        }

        if (list_empty(&buf_mgr->image_process_list)) {
            image_proc_enable = false;
            schedule();
            continue;
        }
        
        mutex_lock(&buf_mgr->mlock);
        
        img_node = NULL;
        list_for_each_entry_safe(cur_img_node, tmp_img_node, &buf_mgr->image_process_list, node) {
            if (NULL == img_node) {
                img_node = cur_img_node;
                break;
            }
        }
        
        img = img_node->img;
        cfa_use = img->cfa_use;
        
        memcpy(&tmp_update_area, &img->update_area, sizeof(struct area_info));
        
        if (rectangle_is_zero(img->update_area) == true || img_node->update_order == 0) {
            img->update_area.x_top = 0;
            img->update_area.y_top = 0;
            img->update_area.x_bottom= buf_mgr->width - 1;
            img->update_area.y_bottom= buf_mgr->height - 1;
        } else {
            adjust_coordinates(g_g2d_flag_type, buf_mgr->width, buf_mgr->height, &tmp_update_area, &img->update_area);
        }
        
        /* initial ret value */
        ret = 0;
        
        /*
         * dither process to make image more smooth, but it will take 200~300ms(depend to resolution) to process.
         * in some case, such as show image. you may more concern about view effect, we recomend you choose EINK_DITHER_Y8_Y4
         * but in most case, we 
         * care about speed, so we set dither_type to EINK_NO_DITHER
         * 1440x1080:
         *     EINK_DITHER_Y8_Y1        -> 4Xms
         *     EINK_SIMPLE_DITHER_Y8_Y1 -> 17ms
         *     EINK_NTX_DITHER_Y8_Y1    -> 2Xms
         *     EINK_DITHER_Y8_Y4        -> 4Xms
         */
        if ((gptHWCFG->m_val.bEPD_Flags & 0x08) && cfa_use) {
            if (GET_UPDATE_INFO(img->update_mode) && GET_UPDATE_INFO(img->update_mode) != EINK_NO_MERGE) {
                /* dithering */
                if (GET_UPDATE_INFO(img->update_mode) & EINK_DITHERING_Y1) {
                    
                }
                
                if (ret)
                    __wrn("dither fail, ret=%d\n", ret);
                
                /* gamma */
                if (GET_UPDATE_INFO(img->update_mode) & EINK_GAMMA_CORRECT) {
                    
                }
                
                disp_eink_cache_sync(img->cfa_vaddr, ((char *)img->cfa_vaddr + (buf_mgr->width * buf_mgr->height * 4)));
#ifdef TRACK_UPDATE_BUFFER
                //trace_input_image_buffer(cur_img_node->update_order, img->cfa_vaddr, buf_mgr->width, buf_mgr->height, "process");
#endif
            }
        } else {
            if (GET_UPDATE_INFO(img->update_mode) && GET_UPDATE_INFO(img->update_mode) != EINK_NO_MERGE) {
                /* dithering */
                if (GET_UPDATE_INFO(img->update_mode) & EINK_DITHERING_Y1)
                    ret = dither_process(EINK_DITHER_Y8_Y1, img);
                else if (GET_UPDATE_INFO(img->update_mode) & EINK_DITHERING_Y4)
                    ret = dither_process(EINK_DITHER_Y8_Y4, img);
                else if (GET_UPDATE_INFO(img->update_mode) & EINK_DITHERING_SIMPLE)
                    ret = dither_process(EINK_SIMPLE_DITHER_Y8_Y1, img);
                else if (GET_UPDATE_INFO(img->update_mode) & EINK_DITHERING_NTX_Y1)
                    ret = dither_process(EINK_NTX_DITHER_Y8_Y1, img);

                if (ret)
                    __wrn("dither fail, ret=%d\n", ret);

                if (GET_UPDATE_INFO(img->update_mode) & EINK_GAMMA_CORRECT) {
                    gamma_process(buf_mgr->gamma_table, img);
                }

                if (GET_UPDATE_INFO(img->update_mode) & EINK_MONOCHROME) {
                    ret = dither_process(EINK_MONOCHROME, img);
                    img->update_mode &= ~(EINK_MONOCHROME);
                }

                disp_eink_cache_sync(img->vaddr, ((char *)img->vaddr + (buf_mgr->width * buf_mgr->height)));
#ifdef TRACK_UPDATE_BUFFER
                trace_input_image_buffer(cur_img_node->update_order, img->vaddr, buf_mgr->width, buf_mgr->height, "process");
#endif
            }
        }
        
        /* 
        * handwrite layer needs get a merge display image for backgroud use
        */
        if (g_bg_setting->y8_fd > 0 && g_bg_setting->enable == true) {
            eink_handwrite_background(g_g2d_flag_type, img, process_img, buf_mgr->width, buf_mgr->height, cfa_use);
        }
        
        if ((gptHWCFG->m_val.bEPD_Flags & 0x08) && cfa_use && !buf_mgr->new_process_flag) {
            /* need check cfa gen1 / gen2 / other and resolution for switch cfa_to_gray functions */
            if (gptHWCFG->m_val.bEPD_CFA_REV == 0) { // cfa gen1
                __debug("CFA Gen1\n");
                switch(gptHWCFG->m_val.bDisplayResolution) {

                    case 5: // 1448x1072  
                        if (GET_UPDATE_INFO(img->update_mode) & EINK_DITHERING_Y1){
                    		ntx_imageRGB_to_cfa_grayscale(buf_mgr->width, buf_mgr->height, img->cfa_vaddr, img->vaddr,_rgb2cfa);         // ntx cfa gen1  
                            	ret = dither_process(EINK_DITHER_Y8_Y1, img);
                        }else{
								if (g_cube_data.mode == IMAGE_MODE_EINK) {
                            		image_to_cfa_grayscale_gen1_g_HSL(buf_mgr->width, buf_mgr->height, img->cfa_vaddr, img->vaddr, cfa_test_lut_20201016_bin);
                            	}else if (g_cube_data.mode == IMAGE_MODE_NTX_HSL){
//									printk("===ProcessNTX HSL==\n");
									global_custom = V1;
									image_ntx_process_HSL(img);
					ntx_imageRGB_to_cfa_grayscale(buf_mgr->width, buf_mgr->height, img->cfa_vaddr, img->vaddr,_rgb2cfa);   // ntx cfa gen1
				}else if (g_cube_data.mode == IMAGE_MODE_NTX_MODE1){
//									printk("===ProcessNTX Mode1==\n");
									ntx_mode1_set_arg();
									image_ntx_process_HSL(img);
//									trace_input_image_buffer(img_node->update_order, img->cfa_vaddr, buf_mgr->width, buf_mgr->height, "Mode1");
					ntx_imageRGB_to_cfa_grayscale(buf_mgr->width, buf_mgr->height, img->cfa_vaddr, img->vaddr,_rgb2cfa);   // ntx cfa gen1
				}else if (g_cube_data.mode == IMAGE_MODE_NTX_MODE2){
//									printk("===ProcessNTX Mode2==\n");
									ntx_mode2_set_arg();
									image_ntx_process_HSV(img);
									image_ntx_process_YCbCr(img);
					ntx_imageRGB_to_cfa_grayscale(buf_mgr->width, buf_mgr->height, img->cfa_vaddr, img->vaddr,_rgb2cfa);   // ntx cfa gen1
				}else if (g_cube_data.mode == IMAGE_MODE_NTX_MODE3){
//									printk("===ProcessNTX Mode3==\n");
									ntx_mode3_set_arg();
									image_ntx_process_HSV(img);
									image_ntx_process_YCbCr(img);
					ntx_imageRGB_to_cfa_grayscale(buf_mgr->width, buf_mgr->height, img->cfa_vaddr, img->vaddr,_rgb2cfa);   // ntx cfa gen1
				}else if (g_cube_data.mode == IMAGE_MODE_NTX_MODE4){
//									printk("===ProcessNTX Mode4==\n");
									ntx_mode4_set_arg();
									image_ntx_process_HSL(img);
					ntx_imageRGB_to_cfa_grayscale(buf_mgr->width, buf_mgr->height, img->cfa_vaddr, img->vaddr,_rgb2cfa);   // ntx cfa gen1
				}else if (g_cube_data.mode == IMAGE_MODE_NTX_MODE5){
//									printk("===ProcessNTX Mode5==\n");
									ntx_mode5_set_arg();
									image_ntx_process_HSL(img);
                            		ntx_imageRGB_to_cfa_grayscale(buf_mgr->width, buf_mgr->height, img->cfa_vaddr, img->vaddr,_rgb2cfa);   // ntx cfa gen1
                            	}else {
                                    if ( global_method == HSV ) {
                                        image_ntx_process_HSV(img);
//										printk("===ProcessHSV_==\n");
                                    } else if ( global_method == HSL ) {
                                        image_ntx_process_HSL(img);
//										printk("===ProcessHSL_2==\n");
                                    } else if ( global_method == CCM ) {
                                        color_correction_matrix_process(img);
                                    } else if ( global_method == XYZ_CCM ) {
                                        xyz_process(img);
                                    } else if ( global_method == YCbCr_CONVERT ) {
                                        image_ntx_process_YCbCr(img);
                                    } else if (	global_method == HSVYCbCr) {
										image_ntx_process_HSV(img);
										image_ntx_process_YCbCr(img);
//										printk("===ProcessHSVYCbCr==\n");
									}

//									if((global_S != 0) || (global_L!=0))
//									trace_input_image_buffer(img_node->update_order, img->cfa_vaddr, buf_mgr->width, buf_mgr->height, "Test1");

                            		ntx_imageRGB_to_cfa_grayscale(buf_mgr->width, buf_mgr->height, img->cfa_vaddr, img->vaddr,_rgb2cfa);   // ntx cfa gen1
                            	}
                        }
                        break;

                    default:
                        __wrn("unknown resolution!!\n");
                        break;
                }
            } else if (gptHWCFG->m_val.bEPD_CFA_REV == 1 || gptHWCFG->m_val.bEPD_CFA_REV == 2) { // cfa gen2/gen3
                __debug("CFA Gen2/Gen3\n");
                switch(gptHWCFG->m_val.bDisplayResolution) {
                    case 8: // 1872x1404
                    	//ntx_imageRGB_to_cfa_grayscale(buf_mgr->width, buf_mgr->height, img->cfa_vaddr, img->vaddr);
                        if (GET_UPDATE_INFO(img->update_mode) & EINK_DITHERING_Y1){                  		
				ntx_imageRGB_to_cfa_grayscale(buf_mgr->width, buf_mgr->height, img->cfa_vaddr, img->vaddr,_rgb2cfa_ver2);         // ntx cfa gen2/gen3  
                    		ret = dither_process(EINK_DITHER_Y8_Y1, img);
                        }else{                						
				if (g_cube_data.mode == IMAGE_MODE_EINK) {
	                                image_to_cfa_grayscale_gen2_r_HSL(buf_mgr->width, buf_mgr->height, img->cfa_vaddr, img->vaddr, cfa_test_lut_20201016_bin);
                            	}else if (g_cube_data.mode == IMAGE_MODE_NTX_HSL){
                            		image_ntx_process_HSL(img); 
                            		ntx_imageRGB_to_cfa_grayscale(buf_mgr->width, buf_mgr->height, img->cfa_vaddr, img->vaddr,_rgb2cfa_ver2);   // ntx cfa gen2/gen3
                            	}else {
                                	ntx_imageRGB_to_cfa_grayscale(buf_mgr->width, buf_mgr->height, img->cfa_vaddr, img->vaddr,_rgb2cfa_ver2);   // ntx cfa gen2/gen3
                            	}
                        }                    	
			
                        
                        break;
                        
                    default:
                        __wrn("unknown resolution!!\n");
                        break;
                }
            } else {
                __wrn("unknown cfa type!!\n");
                //disp_eink_cache_sync(img->cfa_vaddr, ((char *)img->cfa_vaddr + (buf_mgr->width * buf_mgr->height * 4)));
                //ntx_imageRGB_to_cfa_grayscale(src_width, src_height, cfa_process_vaddr, tmp_img->vaddr);// ntx cfa v1
                //eink_image_to_cfa_grayscale(src_width, src_height, cfa_process_vaddr, tmp_img->vaddr);  // eink cfa v1
            }
            
            disp_eink_cache_sync(img->vaddr, ((char *)img->vaddr + (buf_mgr->width * buf_mgr->height)));
            
        }
        
        if (manager->nightmode_test_enable == true || (GET_UPDATE_INFO(img->update_mode) & EINK_NEGATIVE_MODE)) {
            for (index = 0; index < buf_mgr->width * buf_mgr->height; index++) {
                vaddr = (u8 *)img->vaddr + index;
                *vaddr = 255 - *vaddr;
            }
            disp_eink_cache_sync(img->vaddr, ((char *)img->vaddr + (buf_mgr->width * buf_mgr->height)));
        }

#if 0
        if(bsp_disp_get_eink_method() == 1 && buf_mgr->new_process_flag){
            ret = eink_new_process(manager, img_node);
#ifdef TEST_QUEUE_BUFFER_TIME
            do_gettimeofday(&image_proc_etimer);
            printk("image process take %d us\n", get_delt_us_timer(image_proc_stimer, image_proc_etimer));
#endif
            mutex_unlock(&buf_mgr->mlock);
            continue;
        }
#endif
        
#ifdef TRACK_UPDATE_BUFFER
        trace_input_image_buffer(img_node->update_order, img->vaddr, buf_mgr->width, buf_mgr->height, "flush");
#endif
        
        /*
        * update mode information
        */
        if (IS_AUTO_MODE(img->update_mode)) {	//auto mode selection
            ret = auto_mode_select(img, &auto_mode, IS_PARTIAL_UPDATE(img->update_mode));
            if (0 == ret) {
                img->update_mode = GET_UPDATE_INFO(img->update_mode) | auto_mode;
            } else {
                __wrn("auto select mode fail, set default mode to GL16\n");
                img->update_mode = GET_UPDATE_INFO(img->update_mode) | EINK_GU16_MODE;
            }

            __msg("auto select info: order=%d, mode=0x%x, area(%d,%d)-(%d,%d)\n",img_node->update_order, img->update_mode, 
                                img->update_area.x_top, img->update_area.y_top,
                                img->update_area.x_bottom, img->update_area.y_bottom);
        }

        if (0 != buf_mgr->global_clean_cnt) {
            if ((img->update_mode & 0xFF) == EINK_GU16_MODE) {
                continue_gu16_cnt++;
                if (continue_gu16_cnt >= buf_mgr->global_clean_cnt) {
                    continue_gu16_cnt = 0;
                    img->update_mode = EINK_GC16_MODE;
                }
            } else {
                continue_gu16_cnt = 0;
            }
        } else {
            continue_gu16_cnt = 0;
        }

        //for debug
        if (false == disp_al_check_valid_waveform_mode(GET_UPDATE_MODE(img->update_mode))) {
            __wrn("unkown mode, update_mode=0x%x\n", img->update_mode);
        }
        
        if (GET_UPDATE_MODE(img->update_mode) != EINK_INIT_MODE && manager->soft_calc_region) {
            //do_gettimeofday(&image_calc_stimer);
            working_buf = manager->work_buffer.workbuffer_vaddr;
            calc_image_update_region(img, working_buf, buf_mgr->width, buf_mgr->height);
            //do_gettimeofday(&image_calc_etimer);
            //printk("image calc update region take %d us\n", get_delt_us_timer(image_calc_stimer, image_calc_etimer));
        }
        
        width = abs(img->update_area.x_bottom - img->update_area.x_top) + 1;
        height = abs(img->update_area.y_bottom - img->update_area.y_top) + 1;
        if (width == buf_mgr->width && height == buf_mgr->height)
            img->flash_mode = GLOBAL_MODE;
        else
            img->flash_mode = LOCAL_MODE;
    
        list_move_tail(&img_node->node, &buf_mgr->image_used_list);
        ret = wake_up_process(manager->pixel_process_task);
        
#ifdef TEST_QUEUE_BUFFER_TIME
        do_gettimeofday(&image_proc_etimer);
        printk("image process take %d us\n", get_delt_us_timer(image_proc_stimer, image_proc_etimer));
#endif
        mutex_unlock(&buf_mgr->mlock);
    }
    
    if(bsp_disp_get_eink_method() == 1){
        __set_current_state(TASK_RUNNING);
    }
    
    return ret;
}

static int auto_mode_cnt = 0;

s32 __queue_image(struct eink_buffer_manager *buffer_mgr,
			struct disp_layer_config_inner *config,
			unsigned int layer_num,
			u32 mode,
			struct area_info update_area,
			unsigned int g2d_flag,
			unsigned int cfa_use)
{
	int i = 0;
	int msleep_cnt = 0;
	bool use_dma_composite = true;
	bool queue_is_full;
	int ret = 0;
	unsigned int in_index, out_index, last_in_index;
	unsigned int buffer_used_num = 0;
	unsigned char combine_flag = 0;
	unsigned char drop_flag = 0;
	unsigned int buf_size;
	enum eink_update_mode update_mode = 0, auto_mode = 0;
	struct eink_8bpp_image *last_img = NULL;
	struct eink_8bpp_image *cur_img = NULL;
	struct eink_8bpp_image *tmp_img = NULL;
	struct image_buffer_node *cur_img_node = NULL, *last_img_node = NULL, *tmp_img_node = NULL, *ttmp_img_node = NULL;
	unsigned long flags = 0;
	unsigned int order = 0;
	bool full_region_flush = false;	// when update resgion is zero
	
	unsigned long tmp_vaddr;
	//void *tmp_vaddr;
	
	struct area_info tmp_update_area;
	
	struct disp_eink_manager* manager = &eink_manager[0];

	struct pipeline_manager* pipe_mgr = manager->pipeline_mgr;
	
	bool ntx_handwrite_solution = manager->ntx_handwrite_solution;
    bool nightmode_test_enable = manager->nightmode_test_enable;
    
    int minimum_pixels_for_dma = manager->minimum_pixels_for_dma;

	u32 area_width = 0, area_height = 0, area_size = 0;
	u32 src_width = 0, src_height = 0;

	if (NULL == buffer_mgr) {
		__wrn("buffer_mgr is null, please check input parameter\n");
		return (-EINVAL);
	}

#ifdef TEST_QUEUE_BUFFER_TIME
	do_gettimeofday(&queue_image_stimer);
#endif
    
	if (false == disp_al_check_valid_waveform_mode(GET_UPDATE_MODE(mode))) {
		__wrn("unkown mode(0x%08x), set to AUTO mode\n", mode);
		update_mode = GET_UPDATE_INFO(mode) | EINK_AUTO_MODE;
	} else {
		update_mode = mode;
	}
	
	if ((NULL == config) && (EINK_INIT_MODE != GET_UPDATE_MODE(update_mode))) {
		__wrn("config is null and not EINK_INIT_MODE, please check input parameter\n");
		return (-EINVAL);
	}

    	__msg("caller info: order=%d, mode=0x%x, area(%d,%d)~(%d,%d)\n",buffer_mgr->update_order, update_mode,
		update_area.x_top, update_area.y_top,
		update_area.x_bottom, update_area.y_bottom);
	
	if (true == rectangle_is_zero(update_area))
		full_region_flush = true;
	
#ifdef DEBUG_VER
	if (false == IS_AUTO_MODE(update_mode)) {
		//mode = mode | EINK_NO_MERGE;
		update_mode = GET_UPDATE_INFO(mode) | EINK_AUTO_MODE;
	}
#endif

    if (nightmode_test_enable == true || (GET_UPDATE_INFO(update_mode) & EINK_NEGATIVE_MODE)) {
        if ((update_mode & 0xFF) == EINK_GL16_MODE || (update_mode & 0xFF) == EINK_GLR16_MODE ||
            (update_mode & 0xFF) == EINK_GU16_MODE)
            update_mode = (update_mode & 0xffffff00) | EINK_GLK16_MODE;
        else if ((update_mode & 0xFF) == EINK_GC16_MODE || (update_mode & 0xFF) == EINK_GLD16_MODE)
            update_mode = (update_mode & 0xffffff00) | EINK_GCK16_MODE;
    }

	mutex_lock(&buffer_mgr->mlock);

	if(bsp_disp_get_eink_method() == 1){		
		if(((update_mode & 0xFF) == EINK_DU_MODE || (update_mode & 0xFF) == EINK_A2_MODE) && ntx_handwrite_solution == true){
			if(buffer_mgr->new_process_flag == 0){
				mutex_unlock(&buffer_mgr->mlock);
				manager->eink_wait_for_hardpipe_complete(manager);
				mutex_lock(&buffer_mgr->mlock);
				buffer_mgr->new_process_flag = 1;
			}
		}else{
			if(buffer_mgr->new_process_flag == 1){
				if(bsp_disp_get_print_level() == 3){
					__debug("---- wake up process \n");
				}
				mutex_unlock(&buffer_mgr->mlock);
				manager->eink_wait_for_softpipe_complete(manager);
				mutex_lock(&buffer_mgr->mlock);
				ret = wake_up_process(manager->pixel_process_task);
				__debug("set new_process_flag to 0");
				buffer_mgr->new_process_flag = 0;
			}
			
			if((update_mode & 0xFF) == EINK_CLEAR_MODE ){
				msleep(35);
			}	
		}
	}

	if(buffer_mgr->new_process_flag == 1){
		//g_cur_image_node = first_node;
		
	}else{	
		if (list_empty(&buffer_mgr->image_free_list)) {
			__debug("!!!!! drop image, order(%d) area(%d,%d)-(%d,%d)\n", g_last_image_node->update_order,
				g_last_image_node->img->update_area.x_top,g_last_image_node->img->update_area.y_top,
				g_last_image_node->img->update_area.x_bottom,g_last_image_node->img->update_area.y_bottom);
			g_cur_image_node = g_last_image_node;
			g_last_image_node = g_last_2nd_image_node;
		} else {
			tmp_img_node = g_cur_image_node;
			list_for_each_entry_safe(g_cur_image_node, ttmp_img_node, &buffer_mgr->image_free_list, node) {
				if (NULL != g_cur_image_node) {	//get the first node
					break;
				}
			}
			g_last_2nd_image_node = g_last_image_node;
			g_last_image_node = tmp_img_node;
		}
	}
	
	cur_img_node = g_cur_image_node;
	last_img_node = g_last_image_node;


	if ((NULL == cur_img_node) || (NULL == last_img_node) || (NULL == cur_img_node->img) || (NULL == last_img_node->img)) {
		__wrn("fail to get last and current image node\n");
		ret = -EINVAL;
		goto out;
	}
	
	__list_del_entry(&cur_img_node->node);	//remove from free list
	buf_size = buffer_mgr->buf_size;
	
	cur_img_node->update_order = buffer_mgr->update_order++;
	order =  cur_img_node->update_order;

	if ((0 == cur_img_node->update_order) && (GET_UPDATE_MODE(mode) != EINK_INIT_MODE)) {
		update_mode = GET_UPDATE_INFO(mode) | EINK_GC16_MODE;
	}
	
	if (force_update_mode != 0x00) {
		update_mode = GET_UPDATE_INFO(mode) | force_update_mode;
	}
	
	last_img = last_img_node->img;
	cur_img = cur_img_node->img;
	cur_img->state = USED;
	
	if (gptHWCFG && 3 == gptHWCFG->m_val.bDisplayBusWidth)
		g_g2d_flag_type = G2D_ROT_V;
	else
		g_g2d_flag_type = G2D_BLT_NONE_H;
	
	if (g2d_flag > 0 || g_g2d_flag_type != G2D_BLT_NONE_H)
		tmp_img = process_img;
	else
		tmp_img = cur_img;
	
	switch(g2d_flag) {
		case 90:
			g_g2d_flag_type = g_g2d_flag_type | G2D_ROT_90;
			src_width = buffer_mgr->height;
			src_height = buffer_mgr->width;
			break;
			
		case 180:
			g_g2d_flag_type = g_g2d_flag_type | G2D_ROT_180;
			src_width = buffer_mgr->width;
			src_height = buffer_mgr->height;
			break;
			
		case 270: 
			g_g2d_flag_type = g_g2d_flag_type | G2D_ROT_270;
			src_width = buffer_mgr->height;
			src_height = buffer_mgr->width;
			break;
			
		default:
			src_width = buffer_mgr->width;
			src_height = buffer_mgr->height;
			break;
	}
	
	flush_cache_all();
    outer_flush_all();
	
	/*
	* convert 32bpp(ARGB8888) to 8bpp(gray level)
	*/
	if (EINK_INIT_MODE == GET_UPDATE_MODE(update_mode)) {
		memset(cur_img->vaddr, 0xff, (buffer_mgr->width * buffer_mgr->height));
		ret = 0;
	} else {
		area_width = abs(update_area.x_bottom - update_area.x_top) + 1;
		area_height = abs(update_area.y_bottom - update_area.y_top) + 1;
		area_size = area_width * area_height;
		if (full_region_flush)
			area_size = buffer_mgr->width * buffer_mgr->height;

		for (i = 0; i < 16; i++) {
			if (config[i].info.fb.fd && config[i].enable == true && !full_region_flush && area_width <= minimum_pixels_for_dma && area_height <= minimum_pixels_for_dma) {
				__debug("32bit gray layer(%d) \n",i);
				ret = copy_32bpp_to_8bpp_img(config, i, tmp_img, update_area, order, src_width, src_height, cfa_use, buffer_mgr->new_process_flag);
				if (ret) {
					__wrn("convert 32bpp to 8bpp fail, ret=%d\n", ret);
					ret = -2;
					cur_img->state = FREE;
					list_add_tail(&cur_img_node->node, &buffer_mgr->image_free_list);
					goto out;
				} else {
					use_dma_composite = false;
				}
			} else if (config[i].info.fb.y8_fd && config[i].enable == true) {
				__debug("8bit gray layer(%d) \n",i);
				ret = copy_8bpp_to_img(config, i, tmp_img, update_area, order, src_width, src_height);
				if (ret) {
					__wrn("convert 8bpp to 8bpp fail, ret=%d\n", ret);
					ret = -2;
					cur_img->state = FREE;
					list_add_tail(&cur_img_node->node, &buffer_mgr->image_free_list);
					goto out;
				} else {
					use_dma_composite = false;
					break;
				}
			}
		}
		
		if (use_dma_composite) {
			if ((gptHWCFG->m_val.bEPD_Flags & 0x08) && cfa_use) {
				ret = convert_32bpp_to_rgb888(buffer_mgr, config,layer_num, (unsigned long)tmp_img->cfa_paddr, src_width, src_height);
			}else
				ret = convert_32bpp_to_8bpp(buffer_mgr, config,layer_num, (unsigned long)tmp_img->paddr, src_width, src_height);
			if (ret) {
				__wrn("convert 32bpp to 8bpp fail, ret=%d\n", ret);
				ret = -2;
				cur_img->state = FREE;
				list_add_tail(&cur_img_node->node, &buffer_mgr->image_free_list);
				goto out;
			}
		}
		
		if ((gptHWCFG->m_val.bEPD_Flags & 0x08) && cfa_use && use_dma_composite) {
			disp_eink_cache_sync(tmp_img->cfa_vaddr, ((char *)tmp_img->cfa_vaddr + (buffer_mgr->width * buffer_mgr->height * 4)));
		}else {
			disp_eink_cache_sync(tmp_img->vaddr, ((char *)tmp_img->vaddr + (buffer_mgr->width * buffer_mgr->height)));
		}
		use_dma_composite = true;
	}
	
#ifdef TRACK_UPDATE_BUFFER
	trace_input_image_buffer(cur_img_node->update_order, tmp_img->vaddr, src_width, src_height, "convert");
    //trace_input_image_buffer(cur_img_node->update_order, tmp_img->cfa_vaddr, src_width, src_height, "convert");
#endif
    
	/* use g2d to do flip and rotate */
	if (EINK_INIT_MODE != GET_UPDATE_MODE(update_mode) && g_g2d_flag_type > G2D_BLT_NONE_H) {
		g2d_process(g_g2d_flag_type, tmp_img, cur_img, src_width, src_height, buffer_mgr->width, buffer_mgr->height, cfa_use);
        
#ifdef TRACK_UPDATE_BUFFER
		trace_input_image_buffer(cur_img_node->update_order, cur_img->vaddr, buffer_mgr->width, buffer_mgr->height, "g2d");
        //trace_input_image_buffer(cur_img_node->update_order, cur_img->cfa_vaddr, buffer_mgr->width, buffer_mgr->height, "g2d");
#endif
	}
	
	memcpy(&cur_img->update_area, &update_area, sizeof(struct area_info));
	cur_img->update_mode = update_mode;
	if ((gptHWCFG->m_val.bEPD_Flags & 0x08) && cfa_use)
		cur_img->cfa_use = 1;
	else
		cur_img->cfa_use = 0;
	
#ifdef TEST_QUEUE_BUFFER_TIME
	do_gettimeofday(&queue_image_etimer);
	printk("order=%d, queue image take %d us\n", order, get_delt_us_timer(queue_image_stimer, queue_image_etimer));
#endif
	
	if ( buffer_mgr->new_process_flag ) {
		memcpy(&tmp_update_area, &cur_img->update_area, sizeof(struct area_info));
		adjust_coordinates(g_g2d_flag_type, buffer_mgr->width, buffer_mgr->height, &tmp_update_area, &cur_img->update_area);
		if (GET_UPDATE_INFO(cur_img->update_mode) && GET_UPDATE_INFO(cur_img->update_mode) != EINK_NO_MERGE) {
			/* dithering */
			if (GET_UPDATE_INFO(cur_img->update_mode) & EINK_DITHERING_Y1)
				ret = dither_process(EINK_DITHER_Y8_Y1, cur_img);
			else if (GET_UPDATE_INFO(cur_img->update_mode) & EINK_DITHERING_Y4)
				ret = dither_process(EINK_DITHER_Y8_Y4, cur_img);
			else if (GET_UPDATE_INFO(cur_img->update_mode) & EINK_DITHERING_SIMPLE)
				ret = dither_process(EINK_SIMPLE_DITHER_Y8_Y1, cur_img);
			else if (GET_UPDATE_INFO(cur_img->update_mode) & EINK_DITHERING_NTX_Y1)
				ret = dither_process(EINK_NTX_DITHER_Y8_Y1, cur_img);
			
			if (ret)
				__wrn("dither fail, ret=%d\n", ret);
			
			if (GET_UPDATE_INFO(cur_img->update_mode) & EINK_GAMMA_CORRECT) {
				gamma_process(buffer_mgr->gamma_table, cur_img);
			}
			
			if (GET_UPDATE_INFO(cur_img->update_mode) & EINK_MONOCHROME) {
				ret = dither_process(EINK_MONOCHROME, cur_img);
				cur_img->update_mode &= ~(EINK_MONOCHROME);
			}
			
			disp_eink_cache_sync(cur_img->vaddr, ((char *)cur_img->vaddr + (buffer_mgr->width * buffer_mgr->height)));
#ifdef TRACK_UPDATE_BUFFER
			trace_input_image_buffer(cur_img_node->update_order, cur_img->vaddr, buffer_mgr->width, buffer_mgr->height, "process");
#endif
		}
		ret = eink_new_process(manager, cur_img_node);
	} else {
		list_add_tail(&cur_img_node->node, &buffer_mgr->image_process_list);
		image_proc_enable = true;
		wake_up_process(buffer_mgr->image_process_task);
		//ret = 0;
		ret = order;
	}
	
out:
	if ((NULL != cur_img) && (NULL != cur_img_node)) {
		if (USED == cur_img->state) {
			__debug("add to update list: order=%d, mode=0x%x, (%d,%d)~(%d,%d), empty=%d\n",
					order, cur_img->update_mode,
					cur_img->update_area.x_top, cur_img->update_area.y_top,
					cur_img->update_area.x_bottom, cur_img->update_area.y_bottom,
					cur_img->empty_frame_flag);
		}
	}else{	
		mutex_unlock(&buffer_mgr->mlock);
		return ret;
		
	}

	mutex_unlock(&buffer_mgr->mlock);

	return ret;
}


s32 __dequeue_image(struct eink_buffer_manager *buffer_mgr, struct image_buffer_node *img_node)
{
	bool queue_is_empty;
	int ret = 0;
	struct image_buffer_node *image_node = NULL, *tmp_image_node = NULL;
	struct eink_8bpp_image *img = NULL;
	unsigned long flags = 0;
	unsigned int order = 0;

	if (NULL == img_node) {
		__wrn("image buffer node is null\n");
		return 0;
	}

	mutex_lock(&buffer_mgr->mlock);
	list_for_each_entry_safe(image_node, tmp_image_node, &buffer_mgr->image_used_list, node) {
		if (image_node != img_node) {
			continue;
		}

		order = image_node->update_order;
		dequeue_order = order;
		if (NULL != image_node->img) {
			image_node->img->state = FREE;
			image_node->img->flash_mode = GLOBAL_MODE;
			image_node->img->update_mode = EINK_INIT_MODE;
			image_node->img->window_calc_enable = true;
		}
		list_move_tail(&image_node->node, &buffer_mgr->image_free_list);
	}

	mutex_unlock(&buffer_mgr->mlock);
	
	__debug("dequeue image buffer %d, order %d\n", img_node->buf_id, order);

	return ret;
}

static s32 __set_global_clean_counter(struct eink_buffer_manager *buffer_mgr, unsigned int cnt)
{
	if (NULL == buffer_mgr) {
		__wrn("input param is null\n");
		return -1;
	}

	if (cnt > 20) {
		__wrn("global clean counter is to large\n");
		return -2;
	}

	mutex_lock(&buffer_mgr->mlock);
	buffer_mgr->global_clean_cnt = cnt;
	mutex_unlock(&buffer_mgr->mlock);
	
	return 0;
}

static s32 __set_gamma_correction(struct eink_buffer_manager *buffer_mgr, u32 *gamma_tbl)
{
	if (NULL == buffer_mgr) {
		__wrn("input param is null\n");
		return -1;
	}
	
  
	//printk("__set_gamma_correction 200 =%d\n",*(gamma_tbl+200));

	mutex_lock(&buffer_mgr->mlock);
	if(gamma_tbl == NULL)
			printk("gamma tbl null---------------------------------------------\n");
//	memcpy(buffer_mgr->gamma_table,gamma_tbl,sizeof(buffer_mgr->gamma_table) );

	buffer_mgr->gamma_table = gamma_tbl;
	mutex_unlock(&buffer_mgr->mlock);
	
	return 0;
}

static s32 __set_cfa_cube_file(struct eink_buffer_manager *buffer_mgr, struct cfa_cube_lut *cube_tbl)
{
	if (NULL == buffer_mgr) {
		__wrn("input param is null\n");
		return -1;
	}

	if (cube_tbl->mode==IMAGE_MODE_LUT)
		printk("activate CFA lookup table mode\n");
	else if (cube_tbl->mode==IMAGE_MODE_EINK)
		printk("activate CFA eink mode\n");
	else if(cube_tbl->mode==IMAGE_MODE_NTX_HSL)
		printk("activate CFA ntx HSL mode\n");
	else if(cube_tbl->mode==IMAGE_MODE_NTX_MODE1)
		printk("activate CFA ntx mode1\n");
	else if(cube_tbl->mode==IMAGE_MODE_NTX_MODE2)
		printk("activate CFA ntx mode2\n");
	else if(cube_tbl->mode==IMAGE_MODE_NTX_MODE3)
		printk("activate CFA ntx mode3\n");
	else if(cube_tbl->mode==IMAGE_MODE_NTX_MODE4)
		printk("activate CFA ntx mode4\n");
	else if(cube_tbl->mode==IMAGE_MODE_NTX_MODE5)
		printk("activate CFA ntx mode5\n");
	else
		printk("activate CFA default mode\n");

	g_cube_data.mode = cube_tbl->mode;
	if (g_cube_data.mode!=IMAGE_MODE_LUT) {
		return 0;
	}

	mutex_lock(&buffer_mgr->mlock);

	if (g_cube_data.vaddr == NULL) {
		g_cube_data.vaddr = (unsigned char*)disp_eink_malloc(CUBE_DATA_SIZE, &g_cube_data.paddr);
	}

	memcpy(g_cube_data.vaddr, cube_tbl->lut, CUBE_DATA_SIZE);
	mutex_unlock(&buffer_mgr->mlock);
	return 0;
}

static s32 __set_bg_get_setting(struct eink_buffer_manager *buffer_mgr, struct y8_area_info *background_setting)
{
	int fd = -1, ret = 0;
	
	if (NULL == buffer_mgr) {
		__wrn("input param is null\n");
		return -1;
	}
	
	mutex_lock(&buffer_mgr->mlock);
	if (NULL == background_setting) {
		 __wrn("background_setting is null");
	}
	else {
		/* release old */
		if (g_bg_setting->y8_fd > 0) {
			if (g_bg_setting->vaddr) {
				__debug("old g_bg_setting addr unmap vaddr = 0x%08x", g_bg_setting->vaddr);
				dma_buf_kunmap(g_bg_setting->dmabuf, 0, g_bg_setting->vaddr);
			}
			
			if (!IS_ERR(g_bg_setting->dmabuf))
				dma_buf_put(g_bg_setting->dmabuf);
		}
		
		/* get new */
		memcpy(g_bg_setting, background_setting, sizeof(struct y8_area_info));
		
		fd = g_bg_setting->y8_fd;
		
		if ((0 != fd)) {
			g_bg_setting->dmabuf = dma_buf_get(fd);
			if (IS_ERR(g_bg_setting->dmabuf)) {
				__wrn("fail to get dma buffer, fd=%d\n", fd);
				ret = -2;
				goto exit;
			}
			
			g_bg_setting->vaddr = dma_buf_kmap(g_bg_setting->dmabuf, 0);
			if (NULL == g_bg_setting->vaddr) {
				__wrn("fail to get dma kernel addr, fd=%d\n",fd);
				ret = -3;
				goto exit;
			}
		}
		
		g_bg_setting->width = abs(g_bg_setting->get_region.x_bottom - g_bg_setting->get_region.x_top) + 1;
		g_bg_setting->height = abs(g_bg_setting->get_region.y_bottom - g_bg_setting->get_region.y_top) + 1;
		
		__debug("set bg setting : fd(%d), vaddr(0x%x)\n", g_bg_setting->y8_fd, g_bg_setting->vaddr);
		__debug("allocate width = %d, height = %d\n", g_bg_setting->width, g_bg_setting->height);
	}
	
	mutex_unlock(&buffer_mgr->mlock);

	return ret;
	
exit:
	if (g_bg_setting->vaddr) {
		__debug("g_bg_setting addr unmap vaddr = 0x%08x", g_bg_setting->vaddr);
		dma_buf_kunmap(g_bg_setting->dmabuf, 0, g_bg_setting->vaddr);
	}
	if (!IS_ERR(g_bg_setting->dmabuf))
		dma_buf_put(g_bg_setting->dmabuf);
	
	mutex_unlock(&buffer_mgr->mlock);

	return ret;
}

static s32 __set_bg_enable(struct eink_buffer_manager *buffer_mgr, bool enable, int cfa_use)
{
	struct eink_8bpp_image *cur_img = NULL;
	g2d_blt_flags_h g2d_flag_type = G2D_BLT_NONE_H;
	struct eink_8bpp_image *tmp_img = NULL;
	
	
	if (NULL == buffer_mgr) {
		__wrn("input param is null\n");
		return -1;
	}
	
	mutex_lock(&buffer_mgr->mlock);
	
	g_bg_setting->enable = enable;
	
	mutex_unlock(&buffer_mgr->mlock);
	
	if (enable == true) {
		if (gptHWCFG && 3 == gptHWCFG->m_val.bDisplayBusWidth) {
			
			g2d_flag_type = G2D_ROT_V;
		}
		
		tmp_img = process_img;
		cur_img = g_cur_image_node->img;
		
		eink_handwrite_background(g2d_flag_type, cur_img, tmp_img, buffer_mgr->width, buffer_mgr->height, cfa_use);
	}
	  
	return 0;
}
 

s32 ring_buffer_manager_init(struct disp_eink_manager* eink_manager)
{
	int i;
	int ret = 0;
	struct eink_buffer_manager* buffer_mgr = NULL;
	struct image_buffer_node *buf_node = NULL, *tmp_buf_node = NULL;
	unsigned int indexdata_buf_size = 0;

	buffer_mgr = (struct eink_buffer_manager*) disp_sys_malloc(sizeof(struct eink_buffer_manager));
	if (!buffer_mgr) {
		__wrn("malloc eink buffer manager memory fail!\n");
		ret = -ENOMEM;
		goto buffer_mgr_err;
	}

	/* init in_index is 1,ring buffer is empty. out_index is 0,point to last image. */
	memset((void*)(buffer_mgr), 0, sizeof(struct eink_buffer_manager));
	buffer_mgr->width = eink_manager->panel_info.width;
	buffer_mgr->height = eink_manager->panel_info.height;
	buffer_mgr->buf_size = buffer_mgr->width * buffer_mgr->height;
	buffer_mgr->update_order = 0;
	buffer_mgr->global_clean_cnt = DEFAULT_GC_COUNTER;
	mutex_init(&buffer_mgr->mlock);
	INIT_LIST_HEAD(&buffer_mgr->image_free_list);
	INIT_LIST_HEAD(&buffer_mgr->image_used_list);
	INIT_LIST_HEAD(&buffer_mgr->image_collision_list);
    INIT_LIST_HEAD(&buffer_mgr->image_process_list);
	buffer_mgr->max_image_num = IMAGE_BUF_NUM;
	buf_node = (struct image_buffer_node *)disp_sys_malloc(buffer_mgr->max_image_num * sizeof(struct image_buffer_node));
	if (NULL == buf_node) {
		__wrn("alloc buffer node fail\n");
		ret = -ENOMEM;
		goto buf_node_err;
	}
	
	/* create new eink 8bpp image buffer for image process use */
	process_img = (struct eink_8bpp_image *)disp_sys_malloc(sizeof(struct eink_8bpp_image));
	if (NULL == process_img) {
		__wrn("alloc process 8bpp img node fail\n");
		ret = -ENOMEM;
		goto image_buffer_err;
	}
	
	memset(process_img, 0, sizeof(struct eink_8bpp_image));
	
	process_img->vaddr = disp_eink_malloc(buffer_mgr->buf_size, &process_img->paddr);
	if (NULL == process_img->vaddr) {
		__wrn("malloc process image buffer memory fail!\n");
		ret = -ENOMEM;
		goto image_buffer_err;
	}
	__debug("process image (Y8) paddr=0x%08x, vaddr=0x%08x\n", process_img->paddr, process_img->vaddr);
	
	memset((void*)process_img->vaddr, 0xff, buffer_mgr->buf_size);
        
	if(gptHWCFG->m_val.bEPD_Flags & 0x08) //CFA 
	{
		process_img->cfa_vaddr = disp_eink_malloc(buffer_mgr->buf_size * 4, &process_img->cfa_paddr);
		if (NULL == process_img->cfa_vaddr) {
			__wrn("malloc process cfa image buffer memory fail!\n");
			ret = -ENOMEM;
			goto image_buffer_err;
		}
		__debug("process image (ABGR8888) cfa_paddr=0x%08x, cfa_vaddr=0x%08x\n", process_img->cfa_paddr, process_img->cfa_vaddr);
		
		memset((void*)process_img->cfa_vaddr, 0xff, buffer_mgr->buf_size * 4);
	}

	if (gptHWCFG->m_val.bEPD_Flags & 0x08)	//CFA
	{
		g_cube_data.vaddr = NULL;
		g_cube_data.paddr = NULL;
		g_cube_data.mode = IMAGE_MODE_DEFAULT;
		g_cube_data.vaddr = (unsigned char*)disp_eink_malloc(CUBE_DATA_SIZE, &g_cube_data.paddr);
	}
	
	first_node = buf_node;
	
	g_cur_image_node = buf_node;
	g_last_image_node = (buf_node + buffer_mgr->max_image_num - 1);
	g_last_2nd_image_node = (buf_node + buffer_mgr->max_image_num - 2);

	__debug("g_cur_image_node=0x%08x\n", g_cur_image_node);
	__debug("g_last_image_node=0x%08x\n", g_last_image_node);
	__debug("g_last_2nd_image_node=0x%08x\n", g_last_2nd_image_node);
	tmp_buf_node = buf_node;
	for (i = 0; i < buffer_mgr->max_image_num; i++) {
		tmp_buf_node->buf_id = i;
		tmp_buf_node->update_order = 0;
		tmp_buf_node->img = (struct eink_8bpp_image *)disp_sys_malloc(sizeof(struct eink_8bpp_image));
		if (NULL == tmp_buf_node->img) {
			__wrn("alloc 8bpp img node fail\n");
			ret = -ENOMEM;
			goto image_buffer_err;
		}

		memset(tmp_buf_node->img, 0, sizeof(struct eink_8bpp_image));
		tmp_buf_node->img->size.width = buffer_mgr->width;
		tmp_buf_node->img->size.height = buffer_mgr->height;

		/* fix it to match different drawer. */
		tmp_buf_node->img->size.align = 4;
		tmp_buf_node->img->update_mode = EINK_INIT_MODE;
		tmp_buf_node->img->vaddr = disp_eink_malloc(buffer_mgr->buf_size, &tmp_buf_node->img->paddr);
		if (NULL == tmp_buf_node->img->vaddr) {
			__wrn("malloc image buffer memory fail!\n");
			ret = -ENOMEM;
			goto image_buffer_err;
		}
		__debug("image (Y8) paddr%d=0x%08x, vaddr=0x%08x", i, tmp_buf_node->img->paddr, tmp_buf_node->img->vaddr);

		/* init mode need buf data 0xff */
		memset((void*)tmp_buf_node->img->vaddr, 0xff, buffer_mgr->buf_size);
        
		if(gptHWCFG->m_val.bEPD_Flags & 0x08) //CFA 
		{
			tmp_buf_node->img->cfa_vaddr = disp_eink_malloc(buffer_mgr->buf_size * 4, &tmp_buf_node->img->cfa_paddr);
			if (NULL == tmp_buf_node->img->cfa_vaddr) {
				__wrn("malloc image cfa buffer memory fail!\n");
				ret = -ENOMEM;
				goto image_buffer_err;
			}
			__debug("image (ABGR8888) cfa_paddr%d=0x%08x, cfa_vaddr=0x%08x", i, tmp_buf_node->img->cfa_paddr, tmp_buf_node->img->cfa_vaddr);

			/* init mode need buf data 0xff */
			memset((void*)tmp_buf_node->img->cfa_vaddr, 0xff, buffer_mgr->buf_size * 4);
		}
        
		list_add_tail(&tmp_buf_node->node, &buffer_mgr->image_free_list);

		tmp_buf_node++;
	}

	if (eink_manager->panel_info.bit_num > EINK_BIT_4) {
		indexdata_buf_size = buffer_mgr->buf_size << 1;
	} else {
		indexdata_buf_size = buffer_mgr->buf_size;
	}
	new_index_data_vaddr = disp_eink_malloc(indexdata_buf_size, (void *)&new_index_data_paddr);
	old_index_data_vaddr = disp_eink_malloc(indexdata_buf_size, (void *)&old_index_data_paddr);
	if ((NULL == new_index_data_vaddr) || (NULL == old_index_data_vaddr)) {
		__wrn("fail to alloc memory for calc update area\n");
		ret = -ENOMEM;
		goto index_buf_fail;
	}
	
	g_bg_setting = (struct y8_area_info *)disp_sys_malloc(sizeof(struct y8_area_info));
	if (NULL == g_bg_setting) {
			__wrn("malloc background setting memory fail!\n");
			ret = -ENOMEM;
			goto bg_setting_error;
	}
	
	memset(g_bg_setting, 0, sizeof(struct y8_area_info));
	g_bg_setting->y8_fd = -1;
	g_bg_setting->vaddr = NULL;
	g_bg_setting->dmabuf = NULL;

	buffer_mgr->is_full = __is_ring_queue_full;
	buffer_mgr->is_empty = __is_ring_queue_empty;
	buffer_mgr->is_collision_list_empty = __is_collision_list_empty;
	buffer_mgr->add_image_to_collision_list = __add_image_to_collision_list;
	buffer_mgr->remove_image_from_collision_list = __remove_image_from_collision_list;
	buffer_mgr->update_collision_state = __update_collision_state;
	buffer_mgr->update_soft_pipe_collision_state = __update_soft_pipe_collision_state;
	buffer_mgr->cancel_collision_update = __cancel_collision_update;
	buffer_mgr->get_current_image_from_collision_list = __get_current_image_from_collision_list;
	buffer_mgr->get_current_image = __get_current_image;
	buffer_mgr->queue_image = __queue_image;
	buffer_mgr->dequeue_image = __dequeue_image;
	buffer_mgr->set_global_clean_counter = __set_global_clean_counter;
	buffer_mgr->set_gamma_correction = __set_gamma_correction;
	buffer_mgr->set_cfa_cube_file = __set_cfa_cube_file;
	buffer_mgr->gamma_table = gamma22;
	buffer_mgr->set_bg_setting = __set_bg_get_setting;
	buffer_mgr->set_bg_enable = __set_bg_enable;
    
	buffer_mgr->reflush_working_buffer_data = reflush_working_buffer_data;
	
	eink_manager->buffer_mgr = buffer_mgr;
    
	buffer_mgr->image_process_task = kthread_create(eink_image_process_thread, (void *)buffer_mgr, "eink image proc");
	if (IS_ERR_OR_NULL(buffer_mgr->image_process_task)) {
		__wrn("create eink image process thread fail!\n");
		ret = PTR_ERR(buffer_mgr->image_process_task);
		goto bg_setting_error;
	}
	ret = wake_up_process(buffer_mgr->image_process_task);

	return ret;
	
bg_setting_error:
	if (g_bg_setting)
		disp_sys_free(g_bg_setting);
	
index_buf_fail:
	if (new_index_data_vaddr)
		disp_eink_free(&new_index_data_vaddr, &new_index_data_paddr, indexdata_buf_size);
	if (old_index_data_vaddr)
		disp_eink_free(&old_index_data_vaddr, &old_index_data_paddr, indexdata_buf_size);
image_buffer_err:
	if (NULL != process_img) {
		if (process_img->vaddr) {
			disp_eink_free(process_img->vaddr, (void*) process_img->paddr, buffer_mgr->buf_size * 4);
			process_img->vaddr = NULL;
		}
		if(gptHWCFG->m_val.bEPD_Flags & 0x08) //CFA 
		{
			if (process_img->cfa_vaddr) {
				disp_eink_free(process_img->cfa_vaddr, (void*) process_img->cfa_vaddr, buffer_mgr->buf_size * 4);
				process_img->cfa_vaddr = NULL;
			}
		}
		disp_sys_free(process_img);
		process_img = NULL;
	}
	
	tmp_buf_node = buf_node;
	for (i = 0; i < buffer_mgr->max_image_num; i++) {
		if (NULL == tmp_buf_node) {
			tmp_buf_node++;
			continue;
		}

		if (NULL != tmp_buf_node->img) {
			if (tmp_buf_node->img->vaddr) {
				disp_eink_free(tmp_buf_node->img->vaddr, (void*) tmp_buf_node->img->paddr,
					buffer_mgr->buf_size);
				tmp_buf_node->img->vaddr = NULL;
			}
			
			if(gptHWCFG->m_val.bEPD_Flags & 0x08) //CFA 
			{
				if (tmp_buf_node->img->cfa_vaddr) {
					disp_eink_free(tmp_buf_node->img->cfa_vaddr, (void*) tmp_buf_node->img->cfa_paddr,
						buffer_mgr->buf_size * 4);
					tmp_buf_node->img->cfa_vaddr = NULL;
				}
			}
			
			disp_sys_free(tmp_buf_node->img);
			tmp_buf_node->img = NULL;
		}
		tmp_buf_node++;
	}

buf_node_err:
	if (buf_node)
		disp_sys_free(buf_node);
buffer_mgr_err:
	if (buffer_mgr)
		disp_sys_free(buffer_mgr);

	return ret;
}
#endif
