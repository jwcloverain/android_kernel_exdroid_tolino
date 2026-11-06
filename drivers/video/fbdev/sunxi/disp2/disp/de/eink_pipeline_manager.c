/*
 * Copyright (C) 2015 Allwinnertech, z.q <zengqi@allwinnertech.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#include "disp_eink.h"
#include <asm/cacheflush.h>
#include <asm/neon.h>
#include <linux/encode.h>
#include <linux/random.h>
#include "../lcd/default_eink.h"


unsigned int g_decode_sframe_cnt = 0;
unsigned int g_decode_tframe_cnt = 0;
unsigned int g_sw_decode_tframe_cnt = 0;

#ifdef SUPPORT_EINK

//#define DEBUG_PIPELINE

#define PIPELINE_NUM 16
#define SOFT_PIPELINE_NUM 128

#define MAX_PIPELINE_MGR		1

#define PRE_DECODE_NUM		2
#define DECODE_TRIGER_BY_TCON	1

//must larger or equal to 2
#define PRE_DECODE_FRAMES	2

static unsigned long op_skip;
static struct mutex mlock;

#ifdef TEST_EDMA
static void *g_wavedata_vaddr = NULL;
static unsigned int g_wavedata_paddr = NULL;
#endif
extern unsigned int g_decode_eframe_cnt;
extern void print_eink_register(unsigned int start_addr, unsigned int end_addr);
extern s32 save_bin_file(u8 *buf,char* file_name,u32 lenght, loff_t pos);
extern s32 get_delt_us_timer(struct timeval start_timer, struct timeval end_timer);
extern s32 get_delt_ms_timer(struct timeval start_timer, struct timeval end_timer);

static struct pipeline_manager *g_pipeline_mgr[MAX_PIPELINE_MGR];

/*********************************************************************/
volatile static int g_enable_disp_proc = 0;
extern void dump_pixels(char * title, void *pixels, int x_offset, int y_offset, int x_length, int y_length, int stride, int byteperpixel); 
struct disp_eink_manager* g_disp_manager = 0;

struct workqueue_struct *reflush_ctrl_workqueue;
struct delayed_work reflush_work;

struct workqueue_struct *power_ctrl_workqueue;
struct delayed_work pipe_power_on_work;

static struct hrtimer before_lcd_int_hrtimer;
static ktime_t before_lcd_int_time;

static struct hrtimer before_write_edma_hrtimer;
static ktime_t before_write_edma_time;
static int g_write_edma_time = 9000000;

extern void EINK_pmic_force_onoff_ex(int enable);
extern bool EINK_get_powergood_status(void);

/*ssize_t get_edma_hrtimer(struct device *dev, struct device_attribute *attr, char *buf) 
{
	int hrtime;
	
	hrtime = g_write_edma_time/1000000;
	
	return sprintf(buf, "write_edma time delay %d\n", hrtime);
}

ssize_t set_edma_hrtimer(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	int err;
	int hrtime;

	err = kstrtoint(buf, 10, &hrtime);
	if (err) {
		pr_warn("Invalid size\n");
		return err;
	}
	
	g_write_edma_time = (hrtime * 1000000);

	if (0 != hrtime) {
		before_write_edma_time = ktime_set( 0, g_write_edma_time );
	}
	
	return count;
}*/

static struct pipeline_manager *get_pipeline_manager(unsigned int disp)
{
	return (disp < MAX_PIPELINE_MGR)?g_pipeline_mgr[disp]:NULL;
}

ssize_t get_frame_ctrl(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct pipeline_manager *mgr = NULL;
	
	mgr = get_pipeline_manager(0);
	
	return sprintf(buf, "%d\n", (mgr->update_time_ctrl == true)?1:0);
}

ssize_t set_frame_ctrl(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	int err;
	int enable;
	struct pipeline_manager *mgr = NULL;
	
	mgr = get_pipeline_manager(0);
	
	err = kstrtoint(buf, 10, &enable);
	if (err) {
		pr_warn("Invalid size\n");
		return err;
	}
	
	if (enable == 1) {
		mgr->update_time_ctrl = true;
	}
	else {
		mgr->update_time_ctrl = false;
	}
	
	return count;
}

static void save_one_wavedata_buffer(struct pipeline_manager *mgr)
{
	struct wavedata_queue *queue = NULL;
	u8 *buf = NULL;
	char file_name[256] = {0};
	u32 id = 0;
	u32 data_len = 0;
	u32 vsync = 0, hsync = 0;

	hsync = mgr->panel_info.lbl + mgr->panel_info.lsl + mgr->panel_info.ldl  + mgr->panel_info.lel;
	vsync = mgr->panel_info.fbl + mgr->panel_info.fsl + mgr->panel_info.fdl  + mgr->panel_info.fel;

	if (8 == mgr->panel_info.data_len) {
		data_len = hsync * vsync * 2;
	} else {
		data_len = hsync * vsync * 4;
	}

	queue = &mgr->wavedata_ring_buffer;
	memset(file_name, 0, sizeof(file_name));
	snprintf(file_name, sizeof(file_name), "/tmp/ctrl_data0.bin");
	buf = (void*)queue->wavedata_vaddr[0];
	save_bin_file(buf, file_name, data_len, 0);
}


static int __init_wavedata_buffer(struct wavedata_queue *queue, struct eink_panel_info info)
{
	int buf_id = 0, hsync = 0, vsync = 0;
	unsigned int wavedata_buf_size = 0;
	int ret = -1;
	
	/*init wavedata ring queue,it includes several wavedata buffer and queue infomation.*/
	memset(queue, 0, sizeof(struct wavedata_queue));

	spin_lock_init(&queue->slock);
	queue->head = 0;
	queue->tail = 0;
	queue->size.width = info.width;
	queue->size.height = info.height;
	/* align param need match with drawer's pitch */
	queue->size.align = 4;

	hsync = info.lsl + info.lbl + info.ldl + info.lel;
	vsync = info.fsl + info.fbl + info.fdl + info.fel;
	__debug("lsl=%d, lbl=%d, ldl=%d, lel=%d\n", info.lsl, info.lbl,  info.ldl, info.lel);
	__debug("fsl=%d, fbl=%d, fdl=%d, fel=%d\n", info.fsl, info.fbl,  info.fdl, info.fel);

	switch (info.data_len) {
	case 8:
		queue->wavedata_buf_size = 2 * hsync * vsync;	// 8 bit data + 8 bit control data = 16 bit
		break;
	case 16:
		queue->wavedata_buf_size = 4 * hsync * vsync;	// 16 bit data + 8 bit control data + 8 bit reserved = 32 bit
		break;
	default:
		queue->wavedata_buf_size = 4 * hsync * vsync;	// 16 bit data + 8 bit control data + 8 bit reserved = 32 bit
		__wrn("unkown eink data len(%d)\n", info.data_len);
		break;
	}

	/*init wave data, if need lager memory,open the LARGE_MEM_TEST.*/
	for (buf_id = 0; buf_id < WAVE_DATA_BUF_NUM; buf_id++) {
		queue->wavedata_vaddr[buf_id] = (void*)disp_eink_malloc(queue->wavedata_buf_size, &queue->wavedata_paddr[buf_id]);
		if(NULL == queue->wavedata_vaddr[buf_id] ) {
			__wrn("malloc eink wavedata memory fail, size=%d, id=%d\n", queue->wavedata_buf_size, buf_id);
			ret =  -ENOMEM;
			goto alloc_fail;
		}
		memset((void*)queue->wavedata_vaddr[buf_id], 0, queue->wavedata_buf_size);

		ret = disp_al_init_eink_ctrl_data(0, queue->wavedata_vaddr[buf_id], &info, buf_id);
		if (0 != ret) {
			__wrn("fail to init control data, id=%d\n", buf_id);
			//goto alloc_fail;
		}
		disp_eink_cache_sync(queue->wavedata_vaddr[buf_id], (((u8 *)queue->wavedata_vaddr[buf_id]) + queue->wavedata_buf_size));
		//__debug("wavedata id=%d, virt-addr=0x%x, phy-addr=0x%x\n", \
		//	 buf_id, ((u32)queue->wavedata_vaddr[buf_id]), queue->wavedata_paddr[buf_id]);
	}

#ifdef TEST_EDMA
	g_wavedata_vaddr = (void*)disp_eink_malloc(queue->wavedata_buf_size, (void *)&g_wavedata_paddr);
	if (NULL == g_wavedata_vaddr) {
		__wrn("malloc debug wavedata memory fail, size=%d\n", queue->wavedata_buf_size);
	}
#endif

	return 0;

alloc_fail:
	for (buf_id = 0; buf_id < WAVE_DATA_BUF_NUM; buf_id++) {
		if(NULL != queue->wavedata_vaddr[buf_id] ) {
			disp_eink_free(queue->wavedata_vaddr[buf_id], queue->wavedata_paddr[buf_id], queue->wavedata_buf_size);
		}
	}
	return ret;
}

static void dump_wavedata_buffer(struct wavedata_queue *queue)
{
	int i = 0;
	unsigned long flags = 0;
	unsigned int head = 0, tail = 0, tmp_tail = 0, tmp_head = 0;
	enum wv_buffer_state       buffer_state[WAVE_DATA_BUF_NUM];

	spin_lock_irqsave(&queue->slock, flags);
	head = queue->head;
	tail = queue->tail;
    	tmp_tail = queue->tmp_tail;
	tmp_head = queue->tmp_head;
		
	for (i = 0; i < WAVE_DATA_BUF_NUM; i++)
		buffer_state[i] = queue->buffer_state[i];
	spin_unlock_irqrestore(&queue->slock, flags);

	printk("head=%d, tail=%d, tmp_head=%d, tmp_tail=%d\n", head, tail, tmp_head, tmp_tail);
	for (i = 0; i < WAVE_DATA_BUF_NUM; i++) {
		printk("buffer state=%d\n", buffer_state[i]);
		if (buffer_state[i] != WV_INIT_STATE) {
			printk("miss buffer %d, state=%d\n", i, buffer_state[i]);
		}
	}
}

void reset_wavedata_buffer(char *buf, struct eink_panel_info pinfo)
{
	u16 *data = NULL;
	u16 tmp = 0;
	u32 x = 0, y = 0, start_y = 0, end_y = 0, start_x = 0, end_x = 0;
	u32 pitch = 0, offset = 0;
	
	if (NULL == buf) {
		__wrn("input param is null\n");
		return;
	}

	pitch = pinfo.lsl + pinfo.lbl + pinfo.ldl + pinfo.lel;
	
	start_x = pinfo.lsl + pinfo.lbl;
	end_x = pinfo.lsl + pinfo.lbl + pinfo.ldl;
	
	start_y = pinfo.fsl + pinfo.fbl;
	end_y = pinfo.fsl + pinfo.fbl + pinfo.fdl;
	
	for (y = start_y; y < end_y; y++) {
		offset = y * pitch + start_x;
		data = (u16 *)((u16 *)buf + offset);
		for (x = start_x; x < end_x; x++) {
			tmp = *data & 0x00ff;
			*(data+x) = tmp;
		}
	}
}

void __clear_wavedata_buffer(struct wavedata_queue *queue)
{
	int i = 0;
	unsigned long flags = 0;

	spin_lock_irqsave(&queue->slock, flags);

	queue->head = 0;
	queue->tail = 0;
    	queue->tmp_tail = 0;
	queue->tmp_head = 0;
	for (i = 0; i < WAVE_DATA_BUF_NUM; i++)
		queue->buffer_state[i] = WV_INIT_STATE;

	spin_unlock_irqrestore(&queue->slock, flags);
}


/* return a physic address for tcon
 * used to display wavedata,then
 * dequeue wavedata buffer.
*/
static void *__request_buffer_for_display(struct wavedata_queue *queue)
{
	void * ret;
	unsigned long flags = 0;
    	unsigned int head = 0, tail = 0, tmp_tail = 0;
    	enum wv_buffer_state state;
	bool is_wavedata_buf_empty;
	
	bool ntx_handwrite_solution = g_disp_manager->ntx_handwrite_solution;

	struct eink_buffer_manager *buf_mgr = NULL;
	buf_mgr = g_disp_manager->buffer_mgr;


	spin_lock_irqsave(&queue->slock, flags);

	head = queue->head;
	tail = queue->tail;
	tmp_tail = queue->tmp_tail;

	if (ntx_handwrite_solution == true && buf_mgr->new_process_flag == 1) {
		state = queue->buffer_state[WAVE_DATA_BUF_NUM - 1];
	} else {
		state = queue->buffer_state[tmp_tail];
		is_wavedata_buf_empty = (head == tmp_tail) ? true : false;
		if (is_wavedata_buf_empty) {
			ret = 1;
			goto out;
		}
	}

	if (WV_READY_STATE != state) {
		ret =  2;
		goto out;
		}

	if (ntx_handwrite_solution == true && buf_mgr->new_process_flag == 1) {
		ret = (void*)queue->wavedata_paddr[WAVE_DATA_BUF_NUM - 1];
		disp_eink_cache_sync(queue->wavedata_vaddr[WAVE_DATA_BUF_NUM - 1], (((u8 *)queue->wavedata_vaddr[WAVE_DATA_BUF_NUM - 1]) + queue->wavedata_buf_size));
		queue->buffer_state[WAVE_DATA_BUF_NUM - 1] = WV_DISPLAY_STATE;
	} else {
		ret = (void*)queue->wavedata_paddr[tmp_tail];
		disp_eink_cache_sync(queue->wavedata_vaddr[tmp_tail], (((u8 *)queue->wavedata_vaddr[tmp_tail]) + queue->wavedata_buf_size));
		queue->buffer_state[tmp_tail] = WV_DISPLAY_STATE;
	}
   
	if (ntx_handwrite_solution == true) {
		if (buf_mgr->new_process_flag != 1)
			queue->tmp_tail = (tmp_tail + 1) % NORMAL_UPDATE_WAVE_DATA_BUF_NUM;
	} else
		queue->tmp_tail = (tmp_tail + 1) % WAVE_DATA_BUF_NUM;

out:
	spin_unlock_irqrestore(&queue->slock, flags);

	return ret;
}

/* return a physic address for eink
*  engine used to decode one frame,
*  then queue wavedata buffer.
*/
static void *__request_buffer_for_decode(struct wavedata_queue *queue, u32 *vaddr)
{
	void * ret;
	unsigned long flags = 0;
	unsigned int head = 0, tail = 0, tmp_head = 0;
	bool is_wavedata_buf_full;
	enum wv_buffer_state state;
	
	bool ntx_handwrite_solution = g_disp_manager->ntx_handwrite_solution;
    
	struct eink_buffer_manager *buf_mgr = NULL;
	buf_mgr = g_disp_manager->buffer_mgr;

	spin_lock_irqsave(&queue->slock, flags);
	head = queue->head;
	tail = queue->tail;
	tmp_head = queue->tmp_head;

	if (ntx_handwrite_solution == true) {
		is_wavedata_buf_full = ((tmp_head + 1) % NORMAL_UPDATE_WAVE_DATA_BUF_NUM == tail) ? true : false;
    
		if ( buf_mgr->new_process_flag == 1 ) {
			state = queue->buffer_state[ WAVE_DATA_BUF_NUM - 1];
		} else {
			state = queue->buffer_state[tmp_head];
			if (is_wavedata_buf_full && (WV_INIT_STATE != state)) {
				ret =  NULL;
				goto out;
			}
		}
	} else {
		is_wavedata_buf_full = ((tmp_head + 1) % WAVE_DATA_BUF_NUM == tail) ? true : false;
		state = queue->buffer_state[tmp_head];
		if (is_wavedata_buf_full || (WV_INIT_STATE != state)) {
			ret =  NULL;
			goto out;
		}
	}

	if (ntx_handwrite_solution == true && buf_mgr->new_process_flag == 1) {
		__debug("use ntx handwrite wavedata_buffer\n");
		*vaddr = (unsigned int)queue->wavedata_vaddr[WAVE_DATA_BUF_NUM - 1];
		ret = (void*)queue->wavedata_paddr[WAVE_DATA_BUF_NUM - 1];
		queue->buffer_state[WAVE_DATA_BUF_NUM - 1] = WV_DECODE_STATE;
	} else {
		__debug("use %d wavedata_buffer\n",tmp_head);
		*vaddr = (unsigned int)queue->wavedata_vaddr[tmp_head];
		ret = (void*)queue->wavedata_paddr[tmp_head];
		queue->buffer_state[tmp_head] = WV_DECODE_STATE;
	}

	if (ntx_handwrite_solution == true) {
		if (buf_mgr->new_process_flag != 1)
			queue->tmp_head = (tmp_head + 1)%NORMAL_UPDATE_WAVE_DATA_BUF_NUM;
	} else
		queue->tmp_head = (tmp_head + 1)%WAVE_DATA_BUF_NUM;
	
	#ifdef TEST_EDMA
	vaddr = g_wavedata_vaddr;
	ret = g_wavedata_paddr;
	#endif

out:

	spin_unlock_irqrestore(&queue->slock, flags);

	if ((NULL == ret) && (false == is_wavedata_buf_full)) {
		__wrn("no wavedata buffer, full=%d, state=%d\n", is_wavedata_buf_full, state);
	}

	return ret;
}

static s32 __queue_wavedata_buffer(struct wavedata_queue *queue)
{
	int ret = 0;
	unsigned long flags = 0;
	unsigned int head = 0, tail = 0, tmp_head = 0;
	bool all_done_flag = false;

	bool ntx_handwrite_solution = g_disp_manager->ntx_handwrite_solution;
	struct eink_buffer_manager *buf_mgr = NULL;
	buf_mgr = g_disp_manager->buffer_mgr;


	spin_lock_irqsave(&queue->slock, flags);

	head = queue->head;
	tail = queue->tail;
	tmp_head = queue->tmp_head;
    
	__debug("head: %d queue->buffer_state[head]: %d", head, queue->buffer_state[head]);

	if (ntx_handwrite_solution == true) {
		if ( buf_mgr->new_process_flag == 1 ) {
			if (WV_DECODE_STATE == queue->buffer_state[WAVE_DATA_BUF_NUM - 1]) {
				queue->buffer_state[WAVE_DATA_BUF_NUM - 1] = WV_READY_STATE;
				//queue->head = (head + 1) % WAVE_DATA_BUF_NUM;
				disp_eink_cache_sync(queue->wavedata_vaddr[WAVE_DATA_BUF_NUM - 1], (((u8 *)queue->wavedata_vaddr[WAVE_DATA_BUF_NUM -1]) + queue->wavedata_buf_size));
			}
		} else {
			if (WV_DECODE_STATE == queue->buffer_state[head]) {
				queue->buffer_state[head] = WV_READY_STATE;
				queue->head = (head + 1) % NORMAL_UPDATE_WAVE_DATA_BUF_NUM;
				disp_eink_cache_sync(queue->wavedata_vaddr[head], (((u8 *)queue->wavedata_vaddr[head]) + queue->wavedata_buf_size));
			}
		}
	} else {
		if (WV_DECODE_STATE == queue->buffer_state[head]) {
			queue->buffer_state[head] = WV_READY_STATE;
			queue->head = (head + 1) % WAVE_DATA_BUF_NUM;
			disp_eink_cache_sync(queue->wavedata_vaddr[head], (((u8 *)queue->wavedata_vaddr[head]) + queue->wavedata_buf_size));
		}
	}

out:
	spin_unlock_irqrestore(&queue->slock, flags);

	return ret;
}

static s32 __clean_used_wavedata_buffer(struct wavedata_queue *queue)
{
	int ret = 0;
	unsigned int head = 0, tail = 0, tmp_tail = 0;
	enum wv_buffer_state state = 0;
	unsigned long flags = 0;

	bool ntx_handwrite_solution = g_disp_manager->ntx_handwrite_solution;
	struct eink_buffer_manager *buf_mgr = NULL;
	buf_mgr = g_disp_manager->buffer_mgr;



	spin_lock_irqsave(&queue->slock, flags);

	head = queue->head;
	tail = queue->tail;
	tmp_tail = queue->tmp_tail;
	
	if (ntx_handwrite_solution == true) {
		if ( buf_mgr->new_process_flag == 1 ) {
			state = queue->buffer_state[WAVE_DATA_BUF_NUM - 1];
			if (WV_DISPLAY_STATE == state) {
				queue->buffer_state[WAVE_DATA_BUF_NUM -1] = WV_INIT_STATE;
			}
		} else {
			state = queue->buffer_state[tail];
			if (WV_DISPLAY_STATE == state) {
				queue->buffer_state[tail] = WV_INIT_STATE;
				queue->tail = (tail + 1) % NORMAL_UPDATE_WAVE_DATA_BUF_NUM;
			}
		}
	} else {
		state = queue->buffer_state[tail];
		if (WV_DISPLAY_STATE == state) {
			queue->buffer_state[tail] = WV_INIT_STATE;
			queue->tail = (tail + 1) % WAVE_DATA_BUF_NUM;
		}
	}

	spin_unlock_irqrestore(&queue->slock, flags);

	return ret;
}

/*
* This function will be called in IRQ context, do print anything , because it will let CPU sleep
*/
static int g_frames = 0;
static int write_edma(struct pipeline_manager *mgr, bool wait)
{
    __debug("\n");
	int ret = 0, cur_line = 0, start_delay = 0;
	unsigned long wavedata_paddr = 0;
	unsigned int f_flags = 0;
	unsigned int addr = 0;

	g_frames++;
	g_frames = g_frames % MAX_TOTAL_FRAME;

	wavedata_paddr = (unsigned long)__request_buffer_for_display(&mgr->wavedata_ring_buffer);
	if (wavedata_paddr < 3) {
		__debug("frame no wavedata!\n");
		if (wait)
			mgr->list.miss_soft_wavedata_flag[g_frames] = (u8)wavedata_paddr;
		else
			mgr->list.miss_wavedata_flag[g_frames] = (u8)wavedata_paddr;
		mgr->list.all_miss_wavedata_cnt++;
		return -EBUSY;
	}

	start_delay = disp_al_lcd_get_start_delay(mgr->disp, NULL);
	
	cur_line = disp_al_lcd_get_cur_line(mgr->disp, NULL);

    if (wait) {
        if (cur_line < start_delay) {
            //__wrn("cfg edma too quicker.\n");
            while (cur_line < start_delay)
                cur_line = disp_al_lcd_get_cur_line(mgr->disp, 0);
            ret = disp_al_dbuf_rdy();
        } else {
            ret = disp_al_dbuf_rdy();
        }
    } else {
        while (cur_line < start_delay)
			cur_line = disp_al_lcd_get_cur_line(mgr->disp, 0);
        ret = disp_al_eink_edma_cfg_addr(mgr->disp, wavedata_paddr);
        ret = disp_al_dbuf_rdy();
    }
	
	/*
	for (addr = 0x00; addr <= 0x44; addr += 4) {
		mgr->list.edma_reg_val[g_frames][addr] = readl(addr | 0xf1400400);
	}
	*/
	
	return ret;
}

static int write_edma_first(struct pipeline_manager *mgr)
{
    __debug("");
	int ret = 0;
	unsigned long wavedata_paddr = 0;
	unsigned int addr = 0;

	g_frames = 0;
	wavedata_paddr = (unsigned long)__request_buffer_for_display(&mgr->wavedata_ring_buffer);
	if (wavedata_paddr < 3) {
		//__debug("first frame(%d) no wavedata!\n", current_frame);
		__debug("first frame no wavedata!\n");
		mgr->list.miss_wavedata_flag[g_frames] = (u8)wavedata_paddr;
		mgr->list.all_miss_wavedata_cnt++;
		return -EBUSY;
	}
	
	ret = disp_al_edma_config(mgr->disp, wavedata_paddr, &mgr->panel_info);
	ret = disp_al_edma_write(mgr->disp, 1);
	ret = disp_al_dbuf_rdy();

	/*
	for (addr = 0x00; addr <= 0x44; addr += 4) {
		mgr->list.edma_reg_val[g_frames][addr] = readl(addr | 0xf1400400);
	}
	*/
	
	return ret;
}

int write_edma_second(struct pipeline_manager *mgr)
{
    __debug("");

	int ret = 0, cur_line = 0, start_delay = 0;
	unsigned long wavedata_paddr = 0;
	unsigned int addr = 0;

	g_frames++;
	g_frames = g_frames % MAX_TOTAL_FRAME;
	
	wavedata_paddr = (unsigned long)__request_buffer_for_display(&mgr->wavedata_ring_buffer);
	if (wavedata_paddr < 3) {
		//__debug("second frame(%d) no wavedata!\n", current_frame);
		__debug("second frame no wavedata!\n");
		mgr->list.miss_wavedata_flag[g_frames] = (u8)wavedata_paddr;
		mgr->list.all_miss_wavedata_cnt++;
		return -EBUSY;
	}
	
	cur_line = disp_al_lcd_get_cur_line(mgr->disp, 0);
	start_delay = disp_al_lcd_get_start_delay(mgr->disp, 0);

	while (cur_line < start_delay)
			cur_line = disp_al_lcd_get_cur_line(mgr->disp, 0);
	ret = disp_al_eink_edma_cfg_addr(mgr->disp, wavedata_paddr);
	ret = disp_al_dbuf_rdy();

	/*
	for (addr = 0x00; addr <= 0x44; addr += 4) {
		mgr->list.edma_reg_val[g_frames][addr] = readl(addr | 0xf1400400);
	}
	*/
	
	return ret;
}

/*
* update index and wait for update to pipeline manager
*/
static int update_index_addr_sync(struct pipeline_manager *mgr, u32 id, unsigned long index_paddr, char *index_vaddr, bool handwrite)
{
	u32 timeout = 200;	// unit is ms
	int ret = 0;
	unsigned long flag = 0;
	
	if ((mgr->list.index_addr.new_paddr == mgr->list.index_addr.paddr) && 
	  (mgr->list.index_addr.new_vaddr == mgr->list.index_addr.vaddr) && handwrite == true) {
		return ret;
	}
	
	spin_lock_irqsave(&mgr->list.list_lock, flag);
	mgr->list.index_addr.new_paddr = index_paddr;
	mgr->list.index_addr.new_vaddr = index_vaddr;	
	init_completion(&mgr->list.index_addr.comp);
	mgr->list.index_addr.update_flag = true;
	spin_unlock_irqrestore(&mgr->list.list_lock, flag);

    __debug("\n");
	ret = wait_for_completion_timeout(&mgr->list.index_addr.comp, msecs_to_jiffies(timeout));
    __debug("\n");
	if (!ret) {
		spin_lock_irqsave(&mgr->list.list_lock, flag);
		mgr->list.index_addr.update_flag = false;
		spin_unlock_irqrestore(&mgr->list.list_lock, flag);
		
		//__wrn("wait for index address update timeout, t=%d ms\n", timeout);
		ret = -1;
	} else {
		ret = 0;
	}
	
	return ret;
}

/*
* directly update index addr to pipeline, this can only be called when E-ink engine is NOT started
*/
static void update_index_addr(struct pipeline_manager *mgr, unsigned long index_paddr, char *index_vaddr)
{
	unsigned long flag = 0;
	
	spin_lock_irqsave(&mgr->list.list_lock, flag);
	mgr->list.index_addr.paddr = index_paddr;
	mgr->list.index_addr.vaddr = index_vaddr;
	spin_unlock_irqrestore(&mgr->list.list_lock, flag);
}

static bool get_eink_engine_state(struct pipeline_manager *mgr)
{
	bool state = 0;
	unsigned long flag = 0;

	spin_lock_irqsave(&mgr->list.list_lock, flag);
	state = mgr->list.ee_proccessing;
	spin_unlock_irqrestore(&mgr->list.list_lock, flag);

	return state;
}
/*
EINK_POWER_OFF = 0,
EINK_POWER_ON = 1,
*/
static void set_eink_power_state(struct pipeline_manager *mgr, unsigned int state)
{
	struct disp_device*  plcd = NULL;
	
	if (NULL == mgr) {
		__wrn("input param is null\n");
		return;
	}

	plcd = disp_device_find(mgr->disp, DISP_OUTPUT_TYPE_LCD);
	
	mutex_lock(&mgr->power_mlock);
	switch (state) {
	case EINK_POWER_ON:
		if (EINK_POWER_OFF == mgr->power_state) {
			__msg("PMIC power up\n");
			if ((g_disp_manager->last_update_mode & 0xff) == EINK_GLK16_MODE ||
				(g_disp_manager->last_update_mode & 0xff) == EINK_GCK16_MODE)
				EINK_pmic_force_onoff_ex(true);
			plcd->enable(plcd);
			mgr->power_state = EINK_POWER_ON;
		}
		if (mgr->power_on_wait_flag) {
			complete(&mgr->power_on_comp);
			mgr->power_on_wait_flag = false;
		}
		break;
	case EINK_POWER_OFF:
		if (EINK_POWER_ON == mgr->power_state) {
			__msg("PMIC power down\n");
			plcd->disable(plcd);
            if ((g_disp_manager->last_update_mode & 0xff) == EINK_GLK16_MODE ||
                (g_disp_manager->last_update_mode & 0xff) == EINK_GCK16_MODE)
                EINK_pmic_force_onoff_ex(false);
			mgr->power_state = EINK_POWER_OFF;
		}

		if (mgr->power_off_wait_flag) {
			complete(&mgr->power_off_comp);
			mgr->power_off_wait_flag = false;
		}
		break;
	default:
		__wrn("unkown power state(%d)\n", state);
		break;
	}
	mutex_unlock(&mgr->power_mlock);
}


static int wait_for_eink_power_off(struct pipeline_manager *mgr)
{
	int ret = 0;
	int t = 1000;
	
	if (NULL == mgr) {
		__wrn("input param is null\n");
		return -1;
	}

	mutex_lock(&mgr->power_mlock);
	if ((EINK_POWER_OFF == mgr->power_state)) {
		__debug("eink is power off already\n");
		mutex_unlock(&mgr->power_mlock);
		return 0;
	}

	init_completion(&mgr->power_off_comp);
	mgr->power_off_wait_flag = true;
	mutex_unlock(&mgr->power_mlock);
	
	ret = wait_for_completion_timeout(&mgr->power_off_comp, msecs_to_jiffies(t));
	if (!ret) {
		__wrn("wait for eink power off timeout, t=%d ms\n", t);
		ret = -1;
	} else {
		__wrn("wait for eink power off ok\n");
		ret = 0;
	}
	
	return ret;
}

static int wait_for_eink_power_on(struct pipeline_manager *mgr)
{
	int ret = 0;
	int t = 1000;
	
	if (NULL == mgr) {
		__wrn("input param is null\n");
		return -1;
	}

	mutex_lock(&mgr->power_mlock);
	if ((EINK_POWER_ON == mgr->power_state)) {
		mutex_unlock(&mgr->power_mlock);
		return 0;
	}

	init_completion(&mgr->power_on_comp);
	mgr->power_on_wait_flag = true;
	mutex_unlock(&mgr->power_mlock);
	
	ret = wait_for_completion_timeout(&mgr->power_on_comp, msecs_to_jiffies(t));
	if (!ret) {
		__wrn("wait for eink power on timeout, t=%d ms\n", t);
		ret = -1;
	} else {
		//__debug("wait for eink power on ok\n");
		ret = 0;
	}
	
	return ret;
}



#ifdef DEBUG_PIPELINE
static void print_free_pipe_list(struct pipeline_manager *mgr)
{
	struct pipe_info *pipe, *tpipe;
	unsigned long flag = 0;
	int i = 0;

	spin_lock_irqsave(&mgr->list.list_lock, flag);
	if (list_empty(&mgr->list.free_list)) {		
		printk(KERN_ALERT "FREE_LIST is empty\n");
		spin_unlock_irqrestore(&mgr->list.list_lock, flag);
		return 0;
	}
	
	list_for_each_entry_safe(pipe, tpipe, &mgr->list.free_list, node) {
		__debug("FREE_LIST: pipe %d is free\n", pipe->pipe_id);
	}
	spin_unlock_irqrestore(&mgr->list.list_lock, flag);
}

static void print_used_pipe_list(struct pipeline_manager *mgr)
{
	struct pipe_info *pipe, *tpipe;
	unsigned long flag = 0;

	spin_lock_irqsave(&mgr->list.list_lock, flag);
	if (list_empty(&mgr->list.used_list)) {		
		printk(KERN_ALERT "USED_LIST is empty\n");
		spin_unlock_irqrestore(&mgr->list.list_lock, flag);
		return 0;
	}

	list_for_each_entry_safe(pipe, tpipe, &mgr->list.used_list, node) {
		__debug("USED_LIST: pipe %d is used\n", pipe->pipe_id);
	}
	spin_unlock_irqrestore(&mgr->list.list_lock, flag);
}

static void print_free_soft_pipe_list(struct pipeline_manager *mgr)
{
	struct pipe_info *pipe, *tpipe;
	unsigned long flag = 0;
	int i = 0;

	spin_lock_irqsave(&mgr->list.list_lock, flag);
	if (list_empty(&mgr->list.free_list)) {		
		printk(KERN_ALERT "FREE_SOFT_PIPE_LIST is empty\n");
		spin_unlock_irqrestore(&mgr->list.list_lock, flag);
		return 0;
	}
	
	list_for_each_entry_safe(pipe, tpipe, &mgr->list.soft_pipe_free_list, node) {
		__debug("FREE_SOFT_PIPE_LIST: soft pipe %d is free\n", pipe->pipe_id);
	}
	spin_unlock_irqrestore(&mgr->list.list_lock, flag);
}

static void print_used_soft_pipe_list(struct pipeline_manager *mgr)
{
	struct pipe_info *pipe, *tpipe;
	unsigned long flag = 0;

	spin_lock_irqsave(&mgr->list.list_lock, flag);
	if (list_empty(&mgr->list.used_list)) {		
		printk(KERN_ALERT "USED_SOFT_PIPE_LIST is empty\n");
		spin_unlock_irqrestore(&mgr->list.list_lock, flag);
		return 0;
	}

	list_for_each_entry_safe(pipe, tpipe, &mgr->list.soft_pipe_used_list, node) {
		__debug("USED_SOFT_PIPE_LIST: soft pipe %d is used\n", pipe->pipe_id);
	}
	spin_unlock_irqrestore(&mgr->list.list_lock, flag);
}


#endif


inline bool is_invalid_pipe_id(struct pipeline_manager *mgr, int id)
{
	return ((id <0) || (id >= mgr->max_pipe_num));
}

inline bool is_invalid_soft_pipe_id(struct pipeline_manager *mgr, int id)
{
	return ((id <0) || (id >= SOFT_PIPELINE_NUM));
}


static u32 query_free_pipe(struct pipeline_manager *mgr)
{
	struct pipe_info *pinfo = NULL, *tmp_pinfo = NULL;
	u32 pipe_free_state = 0;
	unsigned long flag = 0;
	
	if (NULL == mgr) {
		__wrn("input para is null\n");
		return 0;
	}

	pipe_free_state = 0;
	spin_lock_irqsave(&mgr->list.list_lock, flag);

	list_for_each_entry_safe(pinfo, tmp_pinfo, &mgr->list.free_list, node) {
		pipe_free_state = pipe_free_state | (1 << pinfo->pipe_id);
	}
	
	spin_unlock_irqrestore(&mgr->list.list_lock, flag);
	return pipe_free_state;
}

static u32 query_used_pipe(struct pipeline_manager *mgr)
{
	struct pipe_info *pinfo = NULL, *tmp_pinfo = NULL;
	u32 pipe_used_state = 0;
	unsigned long flag = 0;
	
	if (NULL == mgr) {
		__wrn("input para is null\n");
		return 0;
	}

	pipe_used_state = 0;
	spin_lock_irqsave(&mgr->list.list_lock, flag);
	if (list_empty(&mgr->list.used_list)) {
		spin_unlock_irqrestore(&mgr->list.list_lock, flag);
		return 0;
	}

	list_for_each_entry_safe(pinfo, tmp_pinfo, &mgr->list.used_list, node) {
		pipe_used_state = pipe_used_state | (1 << pinfo->pipe_id);
	}
	
	spin_unlock_irqrestore(&mgr->list.list_lock, flag);
	return pipe_used_state;
}

static int request_pipe(struct pipeline_manager *mgr)
{
	unsigned long flags = 0;
	struct pipe_info* pipe, *tmp_pipe;
	unsigned long flag = 0;
	int id = -1;
	
	if (NULL == mgr) {
		__wrn("input param is null\n");
		return -1;
	}

#ifdef DEBUG_PIPELINE
	__wrn("before request pipe\n");
	print_free_pipe_list(mgr);
	print_used_pipe_list(mgr);
#endif

	spin_lock_irqsave(&mgr->list.list_lock, flag);

	if (list_empty(&mgr->list.free_list)) {
		spin_unlock_irqrestore(&mgr->list.list_lock, flag);
		__wrn("free pipe list is empty\n");		
		return -2;
	}

	list_for_each_entry_safe(pipe, tmp_pipe, &mgr->list.free_list ,node) {
		if (!(pipe->configure_flag || pipe->active_flag)) {
			id = pipe->pipe_id;

			//remove from free_list and add to used_list
			list_move_tail(&pipe->node, &mgr->list.used_list);
			break;
		} else {
			__wrn("something is wrong in free list\n");
		}
	}
	spin_unlock_irqrestore(&mgr->list.list_lock, flag);

#ifdef DEBUG_PIPELINE
	__wrn("after request pipe, id=%d\n", id);
	print_free_pipe_list(mgr);
	print_used_pipe_list(mgr);
#endif

	//__debug("request pipe id=%d\n", pipe->pipe_id);

	return id;
}

/*
* must protect by caller
*/
static void __release_pipe(struct pipeline_manager *mgr, struct pipe_info *pipe)
{
	unsigned int id = 0;
	
	if (pipe->update_image_mask) {
		vfree(pipe->update_image_mask);	//alloc by disp_sys_malloc, free by disp_sys_free
		pipe->update_image_mask = NULL;
	}

	//disable this pipe in HW
	disp_al_eink_pipe_disable(mgr->disp, pipe->pipe_id);

	//initial and add to free list
	pipe->active_flag = false;
	pipe->collision_flag = 0;
	pipe->configure_flag = false;
	pipe->overlap_flag = 0;
	pipe->dec_frame_cnt = 0;
	pipe->fresh_frame_cnt = 0;
	pipe->total_frames = 0;
	//pipe->update_order = 0;

	__debug("release pipe id=%d\n", pipe->pipe_id);
	
	wake_up_process(g_disp_manager->pixel_process_task);
	
	return;
}

static void release_pipe(struct pipeline_manager *mgr, int id)
{
	unsigned long flags = 0, flag = 0;
	struct pipe_info *pipe = NULL, *tmp_pipe = NULL;
	
	if (NULL == mgr) {
		__wrn("input param is null\n");
		return;
	}

	if (is_invalid_pipe_id(mgr, id)) {
		__wrn("pipe id(=%d) is invalid\n", id);
		return;
	}
	
#ifdef DEBUG_PIPELINE
	__wrn("before release pipe, id=%d\n", id);
	print_free_pipe_list(mgr);
	print_used_pipe_list(mgr);
#endif

	spin_lock_irqsave(&mgr->list.list_lock, flag);
	list_for_each_entry_safe(pipe, tmp_pipe, &mgr->list.used_list, node) {
		if (id != pipe->pipe_id) {
			//clear overlap and collision flag with other pipe
			//pipe->collision_flag = pipe->collision_flag & (~(1<<id));
			//pipe->overlap_flag = pipe->overlap_flag & (~(1<<id));
			continue;
		}
		
		if ((mgr->wait_frame_sync_flag == true) && 
				(pipe->update_order == mgr->wait_frame_sync_frame)) {
				mgr->wait_frame_sync_queue &= (~(1 << PIPE_IMG_HW_PIPE_QUEUE));
				if (mgr->wait_frame_sync_queue == 0)
					complete(&mgr->wait_frame_sync_comp);
		}

		 __release_pipe(mgr, pipe);

		 //remove from used_list and add to free_list
		 list_move_tail(&pipe->node, &mgr->list.free_list);
		break;
	}
	spin_unlock_irqrestore(&mgr->list.list_lock, flag);

#ifdef DEBUG_PIPELINE
	__wrn("after release pipe\n");
	print_free_pipe_list(mgr);
	print_used_pipe_list(mgr);
#endif

}


static void __release_soft_pipe(struct pipeline_manager *mgr, struct pipe_info *pipe)
{
	unsigned int id = 0;
	
	if (pipe->update_image_mask) {
		vfree(pipe->update_image_mask);	//alloc by disp_sys_malloc, free by disp_sys_free
		pipe->update_image_mask = NULL;
	}

	//initial and add to free list
	pipe->active_flag = false;
	pipe->collision_flag = 0;
	pipe->configure_flag = false;
	pipe->overlap_flag = 0;
	pipe->dec_frame_cnt = 0;
	pipe->fresh_frame_cnt = 0;
	pipe->total_frames = 0;
	pipe->update_order = 0;

	__debug("release soft pipe id=%d\n", pipe->pipe_id);
	
	return;
}


static void release_soft_pipe(struct pipeline_manager *mgr, int id)
{
	unsigned long flags = 0, flag = 0;
	struct pipe_info *pipe = NULL, *tmp_pipe = NULL;
	
	if (NULL == mgr) {
		__wrn("input param is null\n");
		return;
	}

	if (is_invalid_pipe_id(mgr, id)) {
		__wrn("pipe id(=%d) is invalid\n", id);
		return;
	}
	
#ifdef DEBUG_PIPELINE
	__wrn("before release soft pipe, id=%d\n", id);
	print_free_soft_pipe_list(mgr);
	print_used_soft_pipe_list(mgr);
#endif

	spin_lock_irqsave(&mgr->list.list_lock, flag);
	list_for_each_entry_safe(pipe, tmp_pipe, &mgr->list.soft_pipe_used_list, node) {
		if (id != pipe->pipe_id) {
			//clear overlap and collision flag with other pipe
			//pipe->collision_flag = pipe->collision_flag & (~(1<<id));
			//pipe->overlap_flag = pipe->overlap_flag & (~(1<<id));
			continue;
		}
	
		 __release_soft_pipe(mgr, pipe);

		 //remove from used_list and add to free_list
		 list_move_tail(&pipe->node, &mgr->list.soft_pipe_free_list);
		break;
	}
	spin_unlock_irqrestore(&mgr->list.list_lock, flag);

#ifdef DEBUG_PIPELINE
	__wrn("after release soft pipe\n");
	print_free_soft_pipe_list(mgr);
	print_used_soft_pipe_list(mgr);
#endif

}




static void release_all_pipe(struct pipeline_manager *mgr)
{
	unsigned long flags = 0, flag = 0;
	struct pipe_info *pipe = NULL, *tmp_pipe = NULL;
	
	if (NULL == mgr) {
		__wrn("input param is null\n");
		return;
	}
	
	
	spin_lock_irqsave(&mgr->list.list_lock, flag);
	list_for_each_entry_safe(pipe, tmp_pipe, &mgr->list.used_list, node) {
		if (pipe->active_flag) {
			if ((mgr->wait_frame_sync_flag == true) && 
				(pipe->update_order == mgr->wait_frame_sync_frame)) {
				mgr->wait_frame_sync_queue &= (~(1 << PIPE_IMG_HW_PIPE_QUEUE));
				if (mgr->wait_frame_sync_queue == 0)
					complete(&mgr->wait_frame_sync_comp);
			}
			
			 __release_pipe(mgr, pipe);
			 
			 //remove from used_list and add to free_list
			list_move_tail(&pipe->node, &mgr->list.free_list);
		}
	}
	spin_unlock_irqrestore(&mgr->list.list_lock, flag);
	
	
}

static void release_all_soft_pipe(struct pipeline_manager *mgr)
{
	unsigned long flags = 0, flag = 0;
	struct pipe_info *pipe = NULL, *tmp_pipe = NULL;
	
	if (NULL == mgr) {
		__wrn("input param is null\n");
		return;
	}

	spin_lock_irqsave(&mgr->list.list_lock, flag);
	list_for_each_entry_safe(pipe, tmp_pipe, &mgr->list.soft_pipe_used_list, node) {
		if (pipe->active_flag) {
			if ((mgr->wait_frame_sync_flag == true) && 
				(pipe->update_order == mgr->wait_frame_sync_frame)) {
				mgr->wait_frame_sync_queue &= (~(1 << PIPE_IMG_SW_PIPE_QUEUE));
				if (mgr->wait_frame_sync_queue == 0)
					complete(&mgr->wait_frame_sync_comp);
			}
			
			 __release_soft_pipe(mgr, pipe);
			 
			 //remove from used_list and add to free_list
			list_move_tail(&pipe->node, &mgr->list.soft_pipe_free_list);
		}
	}
	spin_unlock_irqrestore(&mgr->list.list_lock, flag);
}



static bool query_has_config_pipe(struct pipeline_manager *mgr)
{
	struct pipe_info *pipe = NULL, *tmp_pipe = NULL;
	unsigned long flag = 0;
	bool ret = false;
	
	if (NULL == mgr) {
		__wrn("input param is null\n");
		return;
	}

	spin_lock_irqsave(&mgr->list.list_lock, flag);
	list_for_each_entry_safe(pipe, tmp_pipe, &mgr->list.used_list, node) {
		if (pipe->configure_flag) {
			ret = true;
			break;
		}
	}
	spin_unlock_irqrestore(&mgr->list.list_lock, flag);
	return ret;
}


static u32 query_free_soft_pipe(struct pipeline_manager *mgr)
{
	struct pipe_info *pinfo = NULL, *tmp_pinfo = NULL;
	u32 pipe_free_state = 0;
	unsigned long flag = 0;
	
	if (NULL == mgr) {
		__wrn("input para is null\n");
		return 0;
	}

	pipe_free_state = 0;
	spin_lock_irqsave(&mgr->list.list_lock, flag);

	list_for_each_entry_safe(pinfo, tmp_pinfo, &mgr->list.soft_pipe_free_list, node) {
		pipe_free_state = pipe_free_state | (1 << pinfo->pipe_id);
	}
	
	spin_unlock_irqrestore(&mgr->list.list_lock, flag);
	return pipe_free_state;
}


static u32 query_used_soft_pipe(struct pipeline_manager *mgr)
{
	struct pipe_info *pinfo = NULL, *tmp_pinfo = NULL;
	u32 pipe_used_state = 0;
	unsigned long flag = 0;
	
	if (NULL == mgr) {
		__wrn("input para is null\n");
		return 0;
	}

	pipe_used_state = 0;
	spin_lock_irqsave(&mgr->list.list_lock, flag);
	if (list_empty(&mgr->list.soft_pipe_used_list)) {
		spin_unlock_irqrestore(&mgr->list.list_lock, flag);
		return 0;
	}

	list_for_each_entry_safe(pinfo, tmp_pinfo, &mgr->list.soft_pipe_used_list, node) {
		pipe_used_state = pipe_used_state | (1 << pinfo->pipe_id);
	}
	
	spin_unlock_irqrestore(&mgr->list.list_lock, flag);
	return pipe_used_state;
}


static int request_soft_pipe(struct pipeline_manager *mgr)
{
	unsigned long flags = 0;
	struct pipe_info* pipe, *tmp_pipe;
	unsigned long flag = 0;
	int id = -1;
	
	if (NULL == mgr) {
		__wrn("input param is null\n");
		return -1;
	}

#ifdef DEBUG_PIPELINE
	__wrn("before request soft pipe\n");
	print_free_soft_pipe_list(mgr);
	print_used_soft_pipe_list(mgr);
#endif

	spin_lock_irqsave(&mgr->list.list_lock, flag);

	if (list_empty(&mgr->list.soft_pipe_free_list)) {
		spin_unlock_irqrestore(&mgr->list.list_lock, flag);
		__wrn("free soft pipe list is empty\n");		
		return -2;
	}

	list_for_each_entry_safe(pipe, tmp_pipe, &mgr->list.soft_pipe_free_list ,node) {
		if (!(pipe->configure_flag || pipe->active_flag)) {
			id = pipe->pipe_id;
			//remove from free_list and add to used_list
			list_move_tail(&pipe->node, &mgr->list.soft_pipe_used_list);
			break;
		} else {
			__wrn("something is wrong in soft pipe free list\n");
		}
	}
	spin_unlock_irqrestore(&mgr->list.list_lock, flag);

#ifdef DEBUG_PIPELINE
	__wrn("after request soft  pipe, id=%d\n", id);
	print_free_soft_pipe_list(mgr);
	print_used_soft_pipe_list(mgr);
#endif

	__debug("request pipe id=%d\n", pipe->pipe_id);

	return id;
}


static int init_soft_pipe(struct pipeline_manager *mgr, int id, struct pipe_info info)
{
	struct pipe_info *pipe, *tmp_pipe;
	unsigned long flags = 0, flag = 0;
	u32 order = 0;
	
	if (NULL == mgr) {
		__wrn("input param is null\n");
		return -1;
	}

	if (is_invalid_soft_pipe_id(mgr, id)) {
		__wrn("pipe id(=%d) is invalid\n", id);
		return -2;
	}

	spin_lock_irqsave(&mgr->list.list_lock, flag);
	list_for_each_entry_safe(pipe, tmp_pipe, &mgr->list.soft_pipe_used_list, node) {
		if (pipe->pipe_id != id) {
			continue;
		}
		mgr->cur_mode = info.mode;
		pipe->active_flag = false;
		pipe->update_order = info.update_order;
		pipe->mode = info.mode;
		
		memcpy(&pipe->update_area, &info.update_area, sizeof(info.update_area));
		memcpy(&pipe->overlap_area, &info.overlap_area, sizeof(info.overlap_area));
		memcpy(&pipe->org_update_area, &info.org_update_area, sizeof(info.org_update_area));

		//the follow info must be processed by index thread, so that it can reduce time
		pipe->overlap_flag = info.overlap_flag;
		pipe->collision_flag = info.collision_flag;		
		pipe->update_image_mask = info.update_image_mask;
		pipe->total_frames = info.total_frames;
		pipe->waveform_paddr = info.waveform_paddr;		//waveform first phy address
		pipe->waveform_vaddr = info.waveform_vaddr;		//waveform first virtual address
		pipe->dec_frame_cnt = 0;
		pipe->fresh_frame_cnt = 0;

		pipe->soft_pipe_cur_waveform_paddr = pipe->waveform_paddr;

		//pipe has been configed now, and active it at the next VSYNC blanking period
		pipe->configure_flag = true;
		#if 1
		__debug("init soft pipe, order =%d, id=%d,UPD:(%d,%d)~(%d,%d), OVP:(%d,%d)~(%d,%d)\n", 
				pipe->update_order, pipe->pipe_id,	                		
	            		pipe->update_area.x_top,pipe->update_area.y_top,
	            		pipe->update_area.x_bottom,pipe->update_area.y_bottom,
	            		pipe->overlap_area.x_top,pipe->overlap_area.y_top,
	            		pipe->overlap_area.x_bottom,pipe->overlap_area.y_bottom);
	
		#endif
		break;
	}
	spin_unlock_irqrestore(&mgr->list.list_lock, flag);

	return 0;
}

static int __active_soft_pipe(struct pipeline_manager *mgr, struct pipe_info *pipe)
{
	if ((true != pipe->configure_flag) || (true == pipe->active_flag)) {
		return -1;
	}
	
	//this pipe is active now
	pipe->active_flag = true;
	__debug("active soft pipe id=%d\n", pipe->pipe_id);
	
	return 0;
}

static int __inactive_soft_pipe(struct pipeline_manager *mgr, struct pipe_info *pipe)
{
	if (true != pipe->configure_flag) {
		return -1;
	}

	//this pipe is inactive now
	pipe->active_flag = false;
	__debug("active soft pipe id=%d\n", pipe->pipe_id);
	
	return 0;
}

static int active_soft_pipe(struct pipeline_manager *mgr, int id, struct frame_cnt *frame)
{
	struct pipe_info *pipe, *tmp_pipe;
	void *wvdata_paddr = NULL;
	unsigned int wvdata_vaddr = NULL;
	unsigned long dec_left_frames = 0, tframes = 0;
	unsigned long flags = 0, flag = 0;
	unsigned int wave_buf_fail_cnt = 0;
	unsigned int tmp_total = 0, tmp_dec_cnt = 0, tmp_fresh_cnt = 0, tmp_cur_total = 0;
	int ret = -1;
	
	bool ntx_handwrite_solution = g_disp_manager->ntx_handwrite_solution;
	
	if (NULL == mgr) {
		__wrn("input param is null\n");
		return -1;
	}

	if (is_invalid_soft_pipe_id(mgr, id)) {
		__wrn("pipe id(=%d) is invalid\n", id);
		return -2;
	}

	spin_lock_irqsave(&mgr->list.frame_slock, flags);
	tmp_total = mgr->list.all_total_frames;
	tmp_fresh_cnt= mgr->list.all_fresh_frame_cnt;
	tmp_dec_cnt = mgr->list.all_decode_frame_cnt;
	frame->all_total_frames = tmp_total;
	frame->all_fresh_frame_cnt = tmp_fresh_cnt;
	frame->all_decode_frame_cnt = tmp_dec_cnt;
	if (((tmp_fresh_cnt + 5) < tmp_total) && (0 != tmp_fresh_cnt)) {
		//normal
	} else {
		if ((0 == tmp_fresh_cnt) || (tmp_fresh_cnt >= tmp_total)) {
			//normal
		} else {
			spin_unlock_irqrestore(&mgr->list.frame_slock, flags);
			return -3;
		}
	}
	spin_unlock_irqrestore(&mgr->list.frame_slock, flags);

	spin_lock_irqsave(&mgr->list.list_lock, flag);
	list_for_each_entry_safe(pipe, tmp_pipe, &mgr->list.soft_pipe_used_list, node) {
		if (pipe->pipe_id != id) {
			continue;
		}
		
		ret = __active_soft_pipe(mgr, pipe);
		if (0 == ret) {
			tframes =  pipe->total_frames;
		}
		break;
	}
    __debug("tframes: %d", tframes);

	if (0 == tframes) {
		__wrn("[ERROR]total frame of pipe %d is zero or not found this pipe\n", id);
		ret = -1;
		spin_unlock_irqrestore(&mgr->list.list_lock, flag);
		return ret;
	}

	if (false == mgr->list.ee_proccessing) {
		if ((0 == mgr->list.index_addr.vaddr) || (0 == mgr->list.index_addr.paddr)) {
			__wrn("please set index addr and wavedata addr first\n");
		}

		#if DECODE_TRIGER_BY_TCON
			wvdata_paddr = (void *)__request_buffer_for_decode(&mgr->wavedata_ring_buffer, &wvdata_vaddr);
			if (NULL == wvdata_paddr) {
				//request buffer fail, set this pipe to inactive, active it next time
				__wrn("no valid decode buffer, set pipe(%d) to inactive\n", pipe->pipe_id);
				dump_wavedata_buffer(&mgr->wavedata_ring_buffer);
				__inactive_soft_pipe(mgr,pipe);
				ret = -1;
				spin_unlock_irqrestore(&mgr->list.list_lock, flag);
				return ret;
			}
		#else
			spin_unlock_irqrestore(&mgr->list.list_lock, flag);
			wave_buf_fail_cnt = 0;
			do {
				wvdata_paddr = (void *)__request_buffer_for_decode(&mgr->wavedata_ring_buffer, &wvdata_vaddr);
				if (NULL == wvdata_paddr) {
					wave_buf_fail_cnt++;
					if (wave_buf_fail_cnt >= 200) {
						wave_buf_fail_cnt = 0;
						__debug("no wavedata buffer\n");
					}
					msleep(5);
				}
			} while(NULL == wvdata_paddr);
			spin_lock_irqsave(&mgr->list.list_lock, flag);
		#endif

	}
	spin_unlock_irqrestore(&mgr->list.list_lock, flag);

	//active one pipe successfully, update total frames
	spin_lock_irqsave(&mgr->list.frame_slock, flags);
	tmp_total = mgr->list.all_total_frames;
	tmp_fresh_cnt= mgr->list.all_fresh_frame_cnt;
	tmp_dec_cnt = mgr->list.all_decode_frame_cnt;
	if (((tmp_fresh_cnt + 2) < tmp_total) && (0 != tmp_fresh_cnt)) {
		mgr->list.all_total_frames +=  tframes;
		ret = 0;
	} else {
		if ((0 == tmp_fresh_cnt) || (tmp_fresh_cnt >= tmp_total)) {
			mgr->list.all_total_frames = tframes;
			mgr->list.all_fresh_frame_cnt = 0;
			mgr->list.all_decode_frame_cnt = 0;
			ret = 0;
		} else {
			//TODO: too late to insert, pls wait power off
			//mgr->list.all_total_frames = tframes;
			//mgr->list.all_fresh_frame_cnt = 0;
			//mgr->list.all_decode_frame_cnt = 0;
			ret = -4;
			__wrn("inserting pipe(%d) is not fast, total=%d, fresh=%d, decode=%d\n",
				id, tmp_total, tmp_fresh_cnt, tmp_dec_cnt);
		}
	}
	tmp_cur_total = mgr->list.all_total_frames;
	spin_unlock_irqrestore(&mgr->list.frame_slock, flags);
	
	__debug("active one soft pipe(%d), last_total=%d, dec=%d,fresh=%d, cur_total=%d\n", 
		id, tmp_total, tmp_dec_cnt, tmp_fresh_cnt, tmp_cur_total);

	if (0 == ret) {
		spin_lock_irqsave(&mgr->list.list_lock, flag);
		if (false == mgr->list.ee_proccessing) {
			//reset_wavedata_buffer(wvdata_vaddr, mgr->panel_info);
			mgr->list.wavedata_addr.paddr = wvdata_paddr;
			mgr->list.wavedata_addr.vaddr = wvdata_vaddr;

			if (ntx_handwrite_solution == true) {
				mgr->list.ee_proccessing = true;
				__debug("disp_al_eink_prepare_decode\n");
				disp_al_eink_prepare_decode(mgr->disp, mgr->list.index_addr.paddr, wvdata_paddr, &mgr->panel_info);
				__debug("hw >>>>>> IB->WD\n");
				disp_al_eink_triger_decode(mgr->disp);
			} else {
				__debug("disp_al_eink_prepare_decode\n");
				disp_al_eink_prepare_decode(mgr->disp, mgr->list.index_addr.paddr, wvdata_paddr, &mgr->panel_info);
				__debug("hw >>>>>> IB->WD\n");
				disp_al_eink_triger_decode(mgr->disp);
				mgr->list.ee_proccessing = true;
			}

			#ifdef SUPPORT_DEC_TIMER
			g_decode_sframe_cnt = 0;
			do_gettimeofday(&mgr->list.dec_stimer[g_decode_sframe_cnt]);
			g_decode_sframe_cnt++;
			#endif
			
		}
        spin_unlock_irqrestore(&mgr->list.list_lock, flag);
    }

	return ret;
}

static void enable_screen_power(void) {
	struct pipeline_manager *mgr = NULL;
	mgr = get_pipeline_manager(0);
	bool ntx_handwrite_solution = g_disp_manager->ntx_handwrite_solution;
      
	if (ntx_handwrite_solution == true) {
		//schedule_work(&mgr->power_on_work);
		//queue_delayed_work(power_ctrl_workqueue, &(pipe_power_on_work), 0);
		queue_work_on(0, system_highpri_wq, &pipe_power_on_work.work);
	}
}
EXPORT_SYMBOL(enable_screen_power);

static int update_and_active_soft_pipe(struct pipeline_manager *mgr, int id, unsigned long index_paddr, char *index_vaddr, struct pipe_info pinfo,  struct frame_cnt *frame)
{
	int ret = 0;
	
	if (false == get_eink_engine_state(mgr)) {
		//eink engine is not enable
        __debug("eink engine does not enable");
		update_index_addr(mgr, index_paddr, index_vaddr);
		ret = active_soft_pipe(mgr, id, frame);
	} else {
		//eink engine is enable, update index and wait		
        __debug("eink engine has enabled");
		ret = update_index_addr_sync(mgr, id, index_paddr, index_vaddr, true);
        __debug("\n");
		if (ret) {
			//update index buffer timeout
			if (false == get_eink_engine_state(mgr)) {
				update_index_addr(mgr, index_paddr, index_vaddr);
				 ret = active_soft_pipe(mgr, id, frame);
				__wrn("wait for update index timeout, re-active soft pipe(%d)\n",id);
			} else {
				__wrn("engine is on, but wait for update index timeout,soft pipe=%d\n",id);
			}
		} else {
			__debug("update index addr and wait ok, soft pipe=%d\n",id);
		}
	}

	return ret;
}



static int init_pipe(struct pipeline_manager *mgr, int id, struct pipe_info info)
{
	struct pipe_info *pipe, *tmp_pipe;
	unsigned long flags = 0, flag = 0;
	u32 order = 0;
	
	if (NULL == mgr) {
		__wrn("input param is null\n");
		return -1;
	}

	if (is_invalid_pipe_id(mgr, id)) {
		__wrn("pipe id(=%d) is invalid\n", id);
		return -2;
	}

	spin_lock_irqsave(&mgr->list.list_lock, flag);
	list_for_each_entry_safe(pipe, tmp_pipe, &mgr->list.used_list, node) {
		if (pipe->pipe_id != id) {
			continue;
		}
		mgr->cur_mode = info.mode;
		pipe->active_flag = false;
		pipe->update_order = info.update_order;
		pipe->mode = info.mode;
		memcpy(&pipe->update_area, &info.update_area, sizeof(info.update_area));
		memcpy(&pipe->overlap_area, &info.overlap_area, sizeof(info.overlap_area));
		memcpy(&pipe->org_update_area, &info.org_update_area, sizeof(info.org_update_area));

		//the follow info must be processed by index thread, so that it can reduce time
		pipe->overlap_flag = info.overlap_flag;
		pipe->collision_flag = info.collision_flag;		
		pipe->update_image_mask = info.update_image_mask;
		pipe->total_frames = info.total_frames;
		pipe->waveform_paddr = info.waveform_paddr;		//waveform first phy address
		pipe->waveform_vaddr = info.waveform_vaddr;		//waveform first virtual address
		pipe->dec_frame_cnt = 0;
		pipe->fresh_frame_cnt = 0;

		//config this pipe in HW, but DO NOT active it
		disp_al_eink_pipe_config(mgr->disp,  pipe->pipe_id, pipe->update_area);
		disp_al_eink_pipe_config_wavefile(mgr->disp, pipe->waveform_paddr, pipe->pipe_id);

		//pipe has been configed now, and active it at the next VSYNC blanking period
		pipe->configure_flag = true;
		#if 1
		__debug("init pipe, order =%d, id=%d,UPD:(%d,%d)~(%d,%d), OVP:(%d,%d)~(%d,%d)\n", 
				pipe->update_order, pipe->pipe_id,	                		
	            		pipe->update_area.x_top,pipe->update_area.y_top,
	            		pipe->update_area.x_bottom,pipe->update_area.y_bottom,
	            		pipe->overlap_area.x_top,pipe->overlap_area.y_top,
	            		pipe->overlap_area.x_bottom,pipe->overlap_area.y_bottom);
		#endif
		break;
	}
	spin_unlock_irqrestore(&mgr->list.list_lock, flag);

	return 0;
}

static int __active_pipe(struct pipeline_manager *mgr, struct pipe_info *pipe)
{
	if ((true != pipe->configure_flag) || (true == pipe->active_flag)) {
		return -1;
	}
	
	//active this pipe in HW, even this pipe will calculate by SW
	//disp_al_eink_pipe_config_wavefile(mgr->disp, pipe->waveform_addr, pipe->pipe_id);
	disp_al_eink_pipe_enable(mgr->disp, pipe->pipe_id);

	//this pipe is active now
	pipe->active_flag = true;
	__debug("active pipe id=%d\n", pipe->pipe_id);
	
	return 0;
}

static int __inactive_pipe(struct pipeline_manager *mgr, struct pipe_info *pipe)
{
	if (true != pipe->configure_flag) {
		return -1;
	}
	
	//active this pipe in HW, even this pipe will calculate by SW
	//disp_al_eink_pipe_config_wavefile(mgr->disp, pipe->waveform_addr, pipe->pipe_id);
	disp_al_eink_pipe_disable(mgr->disp, pipe->pipe_id);

	//this pipe is inactive now
	pipe->active_flag = false;
	__debug("inactive pipe id=%d\n", pipe->pipe_id);
	
	return 0;
}


/*
* active the pipe that has already configured, 
* this function MUST be called before Eink engine is started
* if this function return fail, wait for moment and call again
*/
static int active_pipe(struct pipeline_manager *mgr, int id, struct frame_cnt *frame)
{
	struct pipe_info *pipe, *tmp_pipe;
	void *wvdata_paddr = NULL;
	unsigned int wvdata_vaddr = NULL;
	unsigned long dec_left_frames = 0, tframes = 0;
	unsigned long flags = 0, flag = 0;
	unsigned int wave_buf_fail_cnt = 0;
	unsigned int tmp_total = 0, tmp_dec_cnt = 0, tmp_fresh_cnt = 0, tmp_cur_total = 0;
	int ret = -1;
	
	if (NULL == mgr) {
		__wrn("input param is null\n");
		return -1;
	}

	if (is_invalid_pipe_id(mgr, id)) {
		__wrn("pipe id(=%d) is invalid\n", id);
		return -2;
	}

	spin_lock_irqsave(&mgr->list.frame_slock, flags);
	tmp_total = mgr->list.all_total_frames;
	tmp_fresh_cnt= mgr->list.all_fresh_frame_cnt;
	tmp_dec_cnt = mgr->list.all_decode_frame_cnt;
	frame->all_total_frames = tmp_total;
	frame->all_fresh_frame_cnt = tmp_fresh_cnt;
	frame->all_decode_frame_cnt = tmp_dec_cnt;
	if (((tmp_fresh_cnt + 5) < tmp_total) && (0 != tmp_fresh_cnt)) {
		//normal
	} else {
		if ((0 == tmp_fresh_cnt) || (tmp_fresh_cnt >= tmp_total)) {
			//normal
		} else {
			spin_unlock_irqrestore(&mgr->list.frame_slock, flags);
			return -3;
		}
	}
	spin_unlock_irqrestore(&mgr->list.frame_slock, flags);

	spin_lock_irqsave(&mgr->list.list_lock, flag);
	list_for_each_entry_safe(pipe, tmp_pipe, &mgr->list.used_list, node) {
		if (pipe->pipe_id != id) {
			continue;
		}

		ret = __active_pipe(mgr, pipe);
		if (0 == ret) {
			tframes =  pipe->total_frames;
		}
		break;
	}

    __debug("tframes: %d", tframes);

	if (0 == tframes) {
		__wrn("[ERROR]total frame of pipe %d is zero or not found this pipe\n", id);
		ret = -1;
		spin_unlock_irqrestore(&mgr->list.list_lock, flag);
		return ret;
	}

	if (false == mgr->list.ee_proccessing) {
		if ((0 == mgr->list.index_addr.vaddr) || (0 == mgr->list.index_addr.paddr)) {
			__wrn("please set index addr and wavedata addr first\n");
		}

		#if DECODE_TRIGER_BY_TCON
			wvdata_paddr = (void *)__request_buffer_for_decode(&mgr->wavedata_ring_buffer, &wvdata_vaddr);
			if (NULL == wvdata_paddr) {
				//request buffer fail, set this pipe to inactive, active it next time
				__wrn("no valid decode buffer, set pipe(%d) to inactive\n", pipe->pipe_id);
				dump_wavedata_buffer(&mgr->wavedata_ring_buffer);
				__inactive_pipe(mgr,pipe);
				ret = -1;
				spin_unlock_irqrestore(&mgr->list.list_lock, flag);
				return ret;
			}
		#else
			spin_unlock_irqrestore(&mgr->list.list_lock, flag);
			wave_buf_fail_cnt = 0;
			do {
				wvdata_paddr = (void *)__request_buffer_for_decode(&mgr->wavedata_ring_buffer, &wvdata_vaddr);
				if (NULL == wvdata_paddr) {
					wave_buf_fail_cnt++;
					if (wave_buf_fail_cnt >= 200) {
						wave_buf_fail_cnt = 0;
						__debug("no wavedata buffer\n");
					}
					msleep(5);
				}
			} while(NULL == wvdata_paddr);
			spin_lock_irqsave(&mgr->list.list_lock, flag);
		#endif

		#if 0
		reset_wavedata_buffer(wvdata_vaddr, mgr->panel_info);
		mgr->list.wavedata_addr.paddr = wvdata_paddr;
		mgr->list.wavedata_addr.vaddr = wvdata_vaddr;
		//disp_al_eink_start_decode(mgr->disp, mgr->list.index_addr.paddr, wvdata_paddr, &mgr->panel_info);
		disp_al_eink_prepare_decode(mgr->disp, mgr->list.index_addr.paddr, wvdata_paddr, &mgr->panel_info);
		disp_al_eink_triger_decode(mgr->disp);
		mgr->list.ee_proccessing = true;
		#endif
	}
	spin_unlock_irqrestore(&mgr->list.list_lock, flag);

	//active one pipe successfully, update total frames
	spin_lock_irqsave(&mgr->list.frame_slock, flags);
	tmp_total = mgr->list.all_total_frames;
	tmp_fresh_cnt= mgr->list.all_fresh_frame_cnt;
	tmp_dec_cnt = mgr->list.all_decode_frame_cnt;
	if (((tmp_fresh_cnt + 2) < tmp_total) && (0 != tmp_fresh_cnt)) {
		mgr->list.all_total_frames +=  tframes;
		ret = 0;
	} else {
		if ((0 == tmp_fresh_cnt) || (tmp_fresh_cnt >= tmp_total)) {
			mgr->list.all_total_frames = tframes;
			mgr->list.all_fresh_frame_cnt = 0;
			mgr->list.all_decode_frame_cnt = 0;
			ret = 0;
		} else {
			//TODO: too late to insert, pls wait power off
			//mgr->list.all_total_frames = tframes;
			//mgr->list.all_fresh_frame_cnt = 0;
			//mgr->list.all_decode_frame_cnt = 0;
			ret = -4;
			__wrn("inserting pipe(%d) is not fast, total=%d, fresh=%d, decode=%d\n",
				id, tmp_total, tmp_fresh_cnt, tmp_dec_cnt);
		}
	}
	tmp_cur_total = mgr->list.all_total_frames;
	spin_unlock_irqrestore(&mgr->list.frame_slock, flags);
	
	__debug("active one pipe(%d), last_total=%d, dec=%d,fresh=%d, cur_total=%d\n", 
		id, tmp_total, tmp_dec_cnt, tmp_fresh_cnt, tmp_cur_total);

	if (0 == ret) {
		spin_lock_irqsave(&mgr->list.list_lock, flag);
		if (false == mgr->list.ee_proccessing) {
			//reset_wavedata_buffer(wvdata_vaddr, mgr->panel_info);
			mgr->list.wavedata_addr.paddr = wvdata_paddr;
			mgr->list.wavedata_addr.vaddr = wvdata_vaddr;
			disp_al_eink_prepare_decode(mgr->disp, mgr->list.index_addr.paddr, wvdata_paddr, &mgr->panel_info);
            __debug("hw >>>>>> IB->WD\n");
			disp_al_eink_triger_decode(mgr->disp);

			#ifdef SUPPORT_DEC_TIMER
			g_decode_sframe_cnt = 0;
			do_gettimeofday(&mgr->list.dec_stimer[g_decode_sframe_cnt]);
			g_decode_sframe_cnt++;
			#endif
			
			mgr->list.ee_proccessing = true;
		}
		spin_unlock_irqrestore(&mgr->list.list_lock, flag);
	}

	return ret;
}

static int update_and_active_pipe(struct pipeline_manager *mgr, int id, unsigned long index_paddr, char *index_vaddr, struct pipe_info pinfo,  struct frame_cnt *frame)
{
	int ret = 0;
	
	if (false == get_eink_engine_state(mgr)) {
		//eink engine is not enable
		update_index_addr(mgr, index_paddr, index_vaddr);
		ret = active_pipe(mgr, id, frame);
	} else {
		//eink engine is enable, update index and wait		
		ret = update_index_addr_sync(mgr, id, index_paddr, index_vaddr, false);
		if (ret) {
			//update index buffer timeout
			if (false == get_eink_engine_state(mgr)) {
				update_index_addr(mgr, index_paddr, index_vaddr);
				 ret = active_pipe(mgr, id, frame);
				__wrn("wait for update index timeout, re-active pipe(%d)\n",id);
			} else {
                mgr->list.ee_proccessing = false;
				__wrn("engine is on, but wait for update index timeout, pipe=%d\n",id);
			}
		} else {
			__debug("update index addr and wait ok, pipe=%d\n",id);
		}
	}

	return ret;
}

static void  __change_wavedata_to_local_mode(char *dest, char *src, char bits_num)
{
	unsigned int gray_level = 0;
	unsigned int old_gray = 0, new_gray = 0;
	unsigned int offset = 0;
	char buf[31] = {0};
	
	if ((NULL == src) || (NULL == dest)) {
		__wrn("input param is null\n");
		return;
	}

	if (5 == bits_num) {
		gray_level = 32;
	} else if (4 == bits_num){
		gray_level = 16;
	} else {
		__wrn("this bit num(%d) is no support\n", bits_num);
		return ;
	}

	memcpy(dest, src, (gray_level * gray_level));

	/*for (new_gray = 0; new_gray < gray_level; new_gray++) {
		offset = (new_gray << bits_num) + new_gray;
		*(dest + offset) = 0;
	}*/
}

static void  __change_wavedata_to_du_mode(char *dest, char *src, char bits_num)
{
	unsigned int gray_level = 0;
	unsigned int old_gray = 0, new_gray = 0;
	unsigned int offset = 0;
	u8 wf_b_b = 0, wf_b_w = 0, wf_w_b = 0, wf_w_w = 0;
	
	if ((NULL == src) || (NULL == dest)) {
		__wrn("input param is null\n");
		return;
	}
	
	if (5 == bits_num) {
		gray_level = 32;
	} else if (4 == bits_num){
		gray_level = 16;
	} else {
		__wrn("this bit num(%d) is no support\n", bits_num);
		return ;
	}
	
	memcpy(dest, src, (gray_level * gray_level));
	
	if (5 == bits_num) {
		wf_b_b = *(src + 0);//G0G0
		wf_b_w = *(src + 0x1f);//G0G31
		wf_w_b = *(src + 0x3e0);//G31G0
		wf_w_w = *(src + 0x3ff);//G31G31
	} else if (4 == bits_num){
		wf_b_b = *(src + 0);
		wf_b_w = *(src + 0xf);
		wf_w_b = *(src + 0xf0);
		wf_w_w = *(src + 0xff);
	} else {
		__wrn("this bit num(%d) is no support\n", bits_num);
		return ;
	}

	//*(dest + 0) = wf_b_b;//00
	*(dest + 0) = wf_w_b;//00
	*(dest + 1) = wf_b_w;//02
	*(dest + 2) = wf_w_b;//01
	*(dest + 3) = wf_b_w;//00
}


static void __calc_wavedata_4bit_8data(struct pipeline_manager *mgr, struct pipe_info *pipe, unsigned int wavefile_offset)
{
	register u8 *wavedata_addr = NULL;
	register u32 *indexdata_addr = NULL;
	register u8 *waveform_addr = NULL;
	register u32 *update_image_mask = NULL, mask_data = 0;
	register u8 t_wavedata1 = 0, t_wavedata2 = 0;
	register u8 tmp_wavedata = 0, wavedata = 0;
	register u8 image_mask1 = 0, image_mask2 = 0, image_mask3 = 0, image_mask4 = 0;
	register u8 index_data1 = 0, index_data2 = 0, index_data3 = 0, index_data4 = 0;
	register u8 index_data5 = 0, index_data6 = 0, index_data7 = 0, index_data8 = 0;
	register u8 pixel1 = 0, pixel2 = 0, pixel3 = 0, pixel4 = 0;
	register u8 pixel5 = 0, pixel6 = 0, pixel7 = 0, pixel8 = 0;
	register u32  mask_offset = 0, index_offset = 0;
	register u32 wv_width = 0, wv_offset = 0;
	register u32 x_start_align = 0, x_end_align = 0, w_align = 0;;
	register u32 y_start = 0, y_end = 0, w = 0, h = 0;
	register u32 screen_width = 0;	
	register u8 mask_flag = 0;
	struct timeval stimer, etimer;
	struct area_info overlap_area_align;
	u8 waveform_array[256];
	
	if ((NULL == mgr) || (NULL == pipe)) {
		return;
	}

	if (NULL == pipe->update_image_mask) {
		__wrn("mask buffer is null\n");
		return;
	}

	do_gettimeofday(&stimer);
	
	align_area(pipe->overlap_area, &overlap_area_align, 8);
	screen_width = mgr->panel_info.width;	
	wv_width = mgr->panel_info.lbl + mgr->panel_info.lsl + mgr->panel_info.ldl + mgr->panel_info.lel;

	x_start_align = overlap_area_align.x_top;
	x_end_align = overlap_area_align.x_bottom;
	y_start = overlap_area_align.y_top;
	y_end = overlap_area_align.y_bottom;

	w_align = (x_end_align - x_start_align + 1);

	waveform_addr = (u8 *)pipe->waveform_vaddr + wavefile_offset;

	__change_wavedata_to_local_mode(waveform_array, waveform_addr , 4);
	asm ("PLD [%0, #64]"::"r" (pipe->update_image_mask));
	for (h = y_start; h <= y_end; h++) {
		mask_offset = (h - y_start) * w_align;
		update_image_mask = (u32 *)((u8 *)pipe->update_image_mask + mask_offset);

		index_offset = h * screen_width + x_start_align;
		indexdata_addr = (u32 *)((u8 *)mgr->list.index_addr.vaddr + index_offset);

		wv_offset = (h + mgr->panel_info.fsl + mgr->panel_info.fbl) * wv_width +
			(mgr->panel_info.lbl + mgr->panel_info.lsl) + (x_start_align >> 2);	// 4 byte in one wavedata

		wavedata_addr = (u8 *)((u8 *)mgr->list.wavedata_addr.vaddr + (wv_offset << 1));
		//memcpy(waveform_array, waveform_addr, 256); //refresh cache

		for (w = x_start_align; w <= x_end_align; w += 4) {
			mask_data = *update_image_mask++;
			if (0x03030303 == mask_data) {			// 4 pixel is mask, skip this
				indexdata_addr++;				// 4 bytes per cycle
				wavedata_addr += 2;
				continue;
			} else if (0x0 == mask_data) {
				mask_flag = 0;
			} else {
				image_mask1 = mask_data;
				image_mask2 = mask_data >> 6;
				image_mask3 = mask_data >> 12;
				image_mask4 = mask_data >> 18;
				mask_flag = (image_mask4) | (image_mask3) | (image_mask2) | (image_mask1);
			}

			index_data1 = (*indexdata_addr);
			index_data2 = (*indexdata_addr) >> 8;
			index_data3 = (*indexdata_addr) >> 16;
			index_data4 = (*indexdata_addr) >> 24;
			indexdata_addr++;				// 4 bytes per cycle

			pixel1 = waveform_array[index_data1];
			pixel2 = waveform_array[index_data2];
			pixel3 = waveform_array[index_data3];
			pixel4 = waveform_array[index_data4];
			t_wavedata1 = (pixel1 |(pixel2 << 2) |(pixel3 << 4) |(pixel4 << 6)) & (~mask_flag);

			wavedata_addr++;
			wavedata = *wavedata_addr & mask_flag | t_wavedata1;
			*wavedata_addr = wavedata;

			wavedata_addr++;
		}
	}
	do_gettimeofday(&etimer);
	//__debug("pipe(%d) dec_cnt=%d, time=%d us\n", pipe->pipe_id, decode_frame_cnt, 
	//	get_delt_us_timer(stimer, etimer));

	return;
}

static void __calc_wavedata_4bit_8data_du(struct pipeline_manager *mgr, struct pipe_info *pipe, unsigned int wavefile_offset)
{
	register u8 *wavedata_addr = NULL;
	register u32 *indexdata_addr = NULL;
	register u8 *waveform_addr = NULL;
	register u32 *update_image_mask = NULL, mask_data = 0;
	register u8 t_wavedata1 = 0, t_wavedata2 = 0;
	register u8 tmp_wavedata = 0, wavedata = 0;
	register u8 image_mask1 = 0, image_mask2 = 0, image_mask3 = 0, image_mask4 = 0;
	register u8 index_data1 = 0, index_data2 = 0, index_data3 = 0, index_data4 = 0;
	register u8 index_data5 = 0, index_data6 = 0, index_data7 = 0, index_data8 = 0;
	register u8 pixel1 = 0, pixel2 = 0, pixel3 = 0, pixel4 = 0;
	register u8 pixel5 = 0, pixel6 = 0, pixel7 = 0, pixel8 = 0;
	register u32  mask_offset = 0, index_offset = 0;
	register u32 wv_width = 0, wv_offset = 0;
	register u32 x_start_align = 0, x_end_align = 0, w_align = 0;;
	register u32 y_start = 0, y_end = 0, w = 0, h = 0;
	register u32 screen_width = 0;	
	register u8 mask_flag = 0;
	struct timeval stimer, etimer;
	struct area_info overlap_area_align;
	u8 waveform_array[256];
	
	if ((NULL == mgr) || (NULL == pipe)) {
		return;
	}

	if (NULL == pipe->update_image_mask) {
		__wrn("mask buffer is null\n");
		return;
	}

	do_gettimeofday(&stimer);
	
	align_area(pipe->overlap_area, &overlap_area_align, 8);
	screen_width = mgr->panel_info.width;	
	wv_width = mgr->panel_info.lbl + mgr->panel_info.lsl + mgr->panel_info.ldl + mgr->panel_info.lel;

	x_start_align = overlap_area_align.x_top;
	x_end_align = overlap_area_align.x_bottom;
	y_start = overlap_area_align.y_top;
	y_end = overlap_area_align.y_bottom;

	w_align = (x_end_align - x_start_align + 1);

	waveform_addr = (u8 *)pipe->waveform_vaddr + wavefile_offset;

	__change_wavedata_to_du_mode(waveform_array, waveform_addr , 4);
	asm ("PLD [%0, #64]"::"r" (pipe->update_image_mask));
	for (h = y_start; h <= y_end; h++) {
		mask_offset = (h - y_start) * w_align;
		update_image_mask = (u32 *)((u8 *)pipe->update_image_mask + mask_offset);

		index_offset = h * screen_width + x_start_align;
		indexdata_addr = (u32 *)((u8 *)mgr->list.index_addr.vaddr + index_offset);

		wv_offset = (h + mgr->panel_info.fsl + mgr->panel_info.fbl) * wv_width +
			(mgr->panel_info.lbl + mgr->panel_info.lsl) + (x_start_align >> 2);	// 4 byte in one wavedata

		wavedata_addr = (u8 *)((u8 *)mgr->list.wavedata_addr.vaddr + (wv_offset << 1));
		//memcpy(waveform_array, waveform_addr, 256); //refresh cache

		for (w = x_start_align; w <= x_end_align; w += 4) {
			mask_data = *update_image_mask++;
			if (0x03030303 == mask_data) {			// 4 pixel is mask, skip this
				indexdata_addr++;				// 4 bytes per cycle
				wavedata_addr += 2;
				continue;
			} else if (0x0 == mask_data) {
				mask_flag = 0;
			} else {
				image_mask1 = mask_data;
				image_mask2 = mask_data >> 6;
				image_mask3 = mask_data >> 12;
				image_mask4 = mask_data >> 18;
				mask_flag = (image_mask4) | (image_mask3) | (image_mask2) | (image_mask1);
			}

			index_data1 = (*indexdata_addr);
			index_data2 = (*indexdata_addr) >> 8;
			index_data3 = (*indexdata_addr) >> 16;
			index_data4 = (*indexdata_addr) >> 24;
			indexdata_addr++;				// 4 bytes per cycle
			
			index_data1 = ((index_data1  >> 7) << 1) | ((index_data1 & 0x8) >> 3);
			index_data2 = ((index_data2  >> 7) << 1) | ((index_data2 & 0x8) >> 3);
			index_data3 = ((index_data3  >> 7) << 1) | ((index_data3 & 0x8) >> 3);
			index_data4 = ((index_data4  >> 7) << 1) | ((index_data4 & 0x8) >> 3);

			pixel1 = waveform_array[index_data1];
			pixel2 = waveform_array[index_data2];
			pixel3 = waveform_array[index_data3];
			pixel4 = waveform_array[index_data4];
			t_wavedata1 = (pixel1 |(pixel2 << 2) |(pixel3 << 4) |(pixel4 << 6)) & (~mask_flag);

			wavedata_addr++;
			wavedata = *wavedata_addr & mask_flag | t_wavedata1;
			*wavedata_addr = wavedata;

			wavedata_addr++;
		}
	}
	do_gettimeofday(&etimer);
	//__debug("pipe(%d) dec_cnt=%d, time=%d us\n", pipe->pipe_id, decode_frame_cnt, 
	//	get_delt_us_timer(stimer, etimer));

	return;
}

static void __calc_wavedata_4bit_16data(struct pipeline_manager *mgr, struct pipe_info *pipe, unsigned int wavefile_offset)
{
	register u32 *wavedata_addr = NULL;
	register u32 *indexdata_addr = NULL;
	register u8 *waveform_addr = NULL;
	register u32 *update_image_mask = NULL, mask_data1 = 0, mask_data2 = 0;
	register u32 t_wavedata1 = 0, t_wavedata2 = 0;
	register u32 tmp_wavedata = 0, wavedata = 0;
	register u32 image_mask1 = 0, image_mask2 = 0, image_mask3 = 0, image_mask4 = 0;
	register u32 image_mask5 = 0, image_mask6 = 0, image_mask7 = 0, image_mask8 = 0;
	register u8 index_data1 = 0, index_data2 = 0, index_data3 = 0, index_data4 = 0;
	register u8 index_data5 = 0, index_data6 = 0, index_data7 = 0, index_data8 = 0;
	register u8 pixel1 = 0, pixel2 = 0, pixel3 = 0, pixel4 = 0;
	register u8 pixel5 = 0, pixel6 = 0, pixel7 = 0, pixel8 = 0;
	register u32  mask_offset = 0, index_offset = 0;
	register u32 wv_width = 0, wv_offset = 0;
	register u32 x_start_align = 0, x_end_align = 0, w_align = 0;
	register u32 y_start = 0, y_end = 0, w = 0, h = 0;
	register u32 screen_width = 0;
	register u8 mask_flag = 0;
	struct area_info overlap_area_align;
	u8 waveform_array[256];

	if ((mgr == NULL) || (pipe == NULL)) {
		return;
	}

	if (pipe->update_image_mask == NULL) {
		__wrn("mask buffer is null\n");
		return;
	}

	align_area(pipe->overlap_area, &overlap_area_align, 8);
	screen_width = mgr->panel_info.width;
	wv_width = mgr->panel_info.lbl + mgr->panel_info.lsl + mgr->panel_info.ldl + mgr->panel_info.lel;

	x_start_align = overlap_area_align.x_top;
	x_end_align = overlap_area_align.x_bottom;
	y_start = overlap_area_align.y_top;
	y_end = overlap_area_align.y_bottom;

	w_align = (x_end_align - x_start_align + 1);

	waveform_addr = (u8 *)pipe->waveform_vaddr + wavefile_offset;

	__change_wavedata_to_local_mode(waveform_array, waveform_addr, 4);
	asm ("PLD [%0, #64]"::"r" (pipe->update_image_mask));
	for (h = y_start; h <= y_end; h++) {
		mask_offset = (h - y_start) * w_align;
		update_image_mask = (u32 *)((u8 *)pipe->update_image_mask + mask_offset);

		index_offset = h * screen_width + x_start_align;
		indexdata_addr = (u32 *)((u8 *)mgr->list.index_addr.vaddr + index_offset);

		wv_offset = (h + mgr->panel_info.fsl + mgr->panel_info.fbl) * wv_width +
			(mgr->panel_info.lbl + mgr->panel_info.lsl) + (x_start_align >> 3);	// 8 byte in one wavedata

		wavedata_addr = (u32 *)((u8 *)mgr->list.wavedata_addr.vaddr + (wv_offset << 2));
		//memcpy(waveform_array, waveform_addr, 256); //refresh cache

		for (w = x_start_align; w <= x_end_align; w += 8) {
			mask_data1 = *update_image_mask++;
			mask_data2 = *update_image_mask++;

			if ((mask_data1 == 0x03030303) && (mask_data2 == 0x03030303)) {		// 8 pixel is mask, skip this
				indexdata_addr += 2;				// 4 bytes per cycle
				wavedata_addr += 1;
				continue;
			}

			image_mask1 = (mask_data1 & 0xff) << 8;
			image_mask2 = (mask_data1 & 0xff00) << 2;
			image_mask3 = (mask_data1 & 0xff0000) >> 4;
			image_mask4 = (mask_data1 & 0xff000000) >> 10;
			image_mask5 = (mask_data2 & 0xff) << 16;
			image_mask6 = (mask_data2 & 0xff00) << 10;
			image_mask7 = (mask_data2 & 0xff0000) << 4;
			image_mask8 = (mask_data2 & 0xff000000) >> 2;
			mask_flag = 0xff0000ff | (image_mask4) | (image_mask3) | (image_mask2) | (image_mask1) | \
				(image_mask8) | (image_mask7) | (image_mask6) | (image_mask5);
			index_data1 = (*indexdata_addr);
			index_data2 = (*indexdata_addr) >> 8;
			index_data3 = (*indexdata_addr) >> 16;
			index_data4 = (*indexdata_addr) >> 24;
			indexdata_addr++;				// 4 bytes per cycle
			index_data5 = (*indexdata_addr);
			index_data6 = (*indexdata_addr) >> 8;
			index_data7 = (*indexdata_addr) >> 16;
			index_data8 = (*indexdata_addr) >> 24;
			indexdata_addr++;

			pixel1 = waveform_array[index_data1];
			pixel2 = waveform_array[index_data2];
			pixel3 = waveform_array[index_data3];
			pixel4 = waveform_array[index_data4];
			pixel5 = waveform_array[index_data5];
			pixel6 = waveform_array[index_data6];
			pixel7 = waveform_array[index_data7];
			pixel8 = waveform_array[index_data8];

			t_wavedata1 = ((pixel1 << 8) | (pixel2 << 10) | (pixel3 << 12) | (pixel4 << 14) | \
				(pixel5 << 16) | (pixel6 << 18) | (pixel7 << 20) | (pixel8 << 22)) & (~mask_flag);

			wavedata = *wavedata_addr & mask_flag | t_wavedata1;
			*wavedata_addr = wavedata;

			wavedata_addr++;
		}
	}

	return;
}

static void __calc_wavedata_4bit_16data_du(struct pipeline_manager *mgr, struct pipe_info *pipe, unsigned int wavefile_offset)
{
	register u32 *wavedata_addr = NULL;
	register u32 *indexdata_addr = NULL;
	register u8 *waveform_addr = NULL;
	register u32 *update_image_mask = NULL, mask_data1 = 0, mask_data2 = 0;
	register u32 t_wavedata1 = 0, t_wavedata2 = 0;
	register u32 tmp_wavedata = 0, wavedata = 0;
	register u32 image_mask1 = 0, image_mask2 = 0, image_mask3 = 0, image_mask4 = 0;
	register u32 image_mask5 = 0, image_mask6 = 0, image_mask7 = 0, image_mask8 = 0;
	register u8 index_data1 = 0, index_data2 = 0, index_data3 = 0, index_data4 = 0;
	register u8 index_data5 = 0, index_data6 = 0, index_data7 = 0, index_data8 = 0;
	register u8 pixel1 = 0, pixel2 = 0, pixel3 = 0, pixel4 = 0;
	register u8 pixel5 = 0, pixel6 = 0, pixel7 = 0, pixel8 = 0;
	register u32  mask_offset = 0, index_offset = 0;
	register u32 wv_width = 0, wv_offset = 0;
	register u32 x_start_align = 0, x_end_align = 0, w_align = 0;
	register u32 y_start = 0, y_end = 0, w = 0, h = 0;
	register u32 screen_width = 0;
	register u8 mask_flag = 0;
	struct area_info overlap_area_align;
	u8 waveform_array[256];

	if ((mgr == NULL) || (pipe == NULL)) {
		return;
	}

	if (pipe->update_image_mask == NULL) {
		__wrn("mask buffer is null\n");
		return;
	}

	align_area(pipe->overlap_area, &overlap_area_align, 8);
	screen_width = mgr->panel_info.width;
	wv_width = mgr->panel_info.lbl + mgr->panel_info.lsl + mgr->panel_info.ldl + mgr->panel_info.lel;

	x_start_align = overlap_area_align.x_top;
	x_end_align = overlap_area_align.x_bottom;
	y_start = overlap_area_align.y_top;
	y_end = overlap_area_align.y_bottom;

	w_align = (x_end_align - x_start_align + 1);

	waveform_addr = (u8 *)pipe->waveform_vaddr + wavefile_offset;

	__change_wavedata_to_du_mode(waveform_array, waveform_addr, 4);
	asm ("PLD [%0, #64]"::"r" (pipe->update_image_mask));
	for (h = y_start; h <= y_end; h++) {
		mask_offset = (h - y_start) * w_align;
		update_image_mask = (u32 *)((u8 *)pipe->update_image_mask + mask_offset);

		index_offset = h * screen_width + x_start_align;
		indexdata_addr = (u32 *)((u8 *)mgr->list.index_addr.vaddr + index_offset);

		wv_offset = (h + mgr->panel_info.fsl + mgr->panel_info.fbl) * wv_width +
			(mgr->panel_info.lbl + mgr->panel_info.lsl) + (x_start_align >> 3);	// 8 byte in one wavedata

		wavedata_addr = (u32 *)((u8 *)mgr->list.wavedata_addr.vaddr + (wv_offset << 2));
		//memcpy(waveform_array, waveform_addr, 256); //refresh cache

		for (w = x_start_align; w <= x_end_align; w += 8) {
			mask_data1 = *update_image_mask++;
			mask_data2 = *update_image_mask++;

			if ((mask_data1 == 0x03030303) && (mask_data2 == 0x03030303)) {		// 8 pixel is mask, skip this
				indexdata_addr += 2;				// 4 bytes per cycle
				wavedata_addr += 1;
				continue;
			}

			image_mask1 = (mask_data1 & 0xff) << 8;
			image_mask2 = (mask_data1 & 0xff00) << 2;
			image_mask3 = (mask_data1 & 0xff0000) >> 4;
			image_mask4 = (mask_data1 & 0xff000000) >> 10;
			image_mask5 = (mask_data2 & 0xff) << 16;
			image_mask6 = (mask_data2 & 0xff00) << 10;
			image_mask7 = (mask_data2 & 0xff0000) << 4;
			image_mask8 = (mask_data2 & 0xff000000) >> 2;
			mask_flag = 0xff0000ff | (image_mask4) | (image_mask3) | (image_mask2) | (image_mask1) | \
				(image_mask8) | (image_mask7) | (image_mask6) | (image_mask5);
			index_data1 = (*indexdata_addr);
			index_data2 = (*indexdata_addr) >> 8;
			index_data3 = (*indexdata_addr) >> 16;
			index_data4 = (*indexdata_addr) >> 24;
			indexdata_addr++;				// 4 bytes per cycle
			index_data5 = (*indexdata_addr);
			index_data6 = (*indexdata_addr) >> 8;
			index_data7 = (*indexdata_addr) >> 16;
			index_data8 = (*indexdata_addr) >> 24;
			indexdata_addr++;

			index_data1 = ((index_data1  >> 7) << 1) | ((index_data1 & 0x8) >> 3);
			index_data2 = ((index_data2  >> 7) << 1) | ((index_data2 & 0x8) >> 3);
			index_data3 = ((index_data3  >> 7) << 1) | ((index_data3 & 0x8) >> 3);
			index_data4 = ((index_data4  >> 7) << 1) | ((index_data4 & 0x8) >> 3);
			index_data5 = ((index_data5  >> 7) << 1) | ((index_data5 & 0x8) >> 3);
			index_data6 = ((index_data6  >> 7) << 1) | ((index_data6 & 0x8) >> 3);
			index_data7 = ((index_data7  >> 7) << 1) | ((index_data7 & 0x8) >> 3);
			index_data8 = ((index_data8  >> 7) << 1) | ((index_data8 & 0x8) >> 3);

			pixel1 = waveform_array[index_data1];
			pixel2 = waveform_array[index_data2];
			pixel3 = waveform_array[index_data3];
			pixel4 = waveform_array[index_data4];
			pixel5 = waveform_array[index_data5];
			pixel6 = waveform_array[index_data6];
			pixel7 = waveform_array[index_data7];
			pixel8 = waveform_array[index_data8];

			t_wavedata1 = ((pixel1 << 8) | (pixel2 << 10) | (pixel3 << 12) | (pixel4 << 14) | \
				(pixel5 << 16) | (pixel6 << 18) | (pixel7 << 20) | (pixel8 << 22)) & (~mask_flag);

			wavedata = *wavedata_addr & mask_flag | t_wavedata1;
			*wavedata_addr = wavedata;

			wavedata_addr++;
		}
	}

	return;
}

static void __calc_wavedata_5bit_8data(struct pipeline_manager *mgr, struct pipe_info *pipe, unsigned int wavefile_offset)
{
	register u8 *wavedata_addr = NULL;
	register u32 *indexdata_addr = NULL;
	register u8 *waveform_addr = NULL;
	register u32 *update_image_mask = NULL, mask_data = 0;
	register u8 t_wavedata1 = 0, t_wavedata2 = 0;
	register u8 tmp_wavedata = 0, wavedata = 0;
	register u8 image_mask1 = 0, image_mask2 = 0, image_mask3 = 0, image_mask4 = 0;
	register u16 index_data1 = 0, index_data2 = 0, index_data3 = 0, index_data4 = 0;
	register u16 index_data5 = 0, index_data6 = 0, index_data7 = 0, index_data8 = 0;
	register u8 pixel1 = 0, pixel2 = 0, pixel3 = 0, pixel4 = 0;
	register u8 pixel5 = 0, pixel6 = 0, pixel7 = 0, pixel8 = 0;
	register u32  mask_offset = 0, index_offset = 0;
	register u32 wv_width = 0, wv_offset = 0;
	register u32 x_start_align = 0, x_end_align = 0, w_align = 0;;
	register u32 y_start = 0, y_end = 0, w = 0, h = 0;
	register u32 screen_width = 0;	
	register u8 mask_flag = 0;
	struct timeval stimer, etimer;
	struct area_info overlap_area_align;
	u8 waveform_array[1024];
	
	if ((NULL == mgr) || (NULL == pipe)) {
		return;
	}

	if (NULL == pipe->update_image_mask) {
		__wrn("mask buffer is null\n");
		return;
	}

	do_gettimeofday(&stimer);
	
	align_area(pipe->overlap_area, &overlap_area_align, 8);
	screen_width = mgr->panel_info.width;
	wv_width = mgr->panel_info.lbl + mgr->panel_info.lsl + mgr->panel_info.ldl + mgr->panel_info.lel;

	x_start_align = overlap_area_align.x_top;
	x_end_align = overlap_area_align.x_bottom;
	y_start = overlap_area_align.y_top;
	y_end = overlap_area_align.y_bottom;

	w_align = (x_end_align - x_start_align + 1);

	waveform_addr = (u8 *)pipe->waveform_vaddr + wavefile_offset;

	__change_wavedata_to_local_mode(waveform_array, waveform_addr , 5);
	asm ("PLD [%0, #64]"::"r" (pipe->update_image_mask));
	for (h = y_start; h <= y_end; h++) {
		mask_offset = (h - y_start) * w_align;
		update_image_mask = (u32 *)((u8 *)pipe->update_image_mask + mask_offset);

		index_offset = h * screen_width + x_start_align;
		indexdata_addr = (u32 *)((u8 *)mgr->list.index_addr.vaddr + (index_offset << 1));

		wv_offset = (h + mgr->panel_info.fsl + mgr->panel_info.fbl) * wv_width +
			(mgr->panel_info.lbl + mgr->panel_info.lsl) + (x_start_align >> 2);	// 4 byte in one wavedata

		wavedata_addr = (u8 *)((u8 *)mgr->list.wavedata_addr.vaddr + (wv_offset << 1));
		//memcpy(waveform_array, waveform_addr, 1024); //refresh cache

		for (w = x_start_align; w <= x_end_align; w += 4) {
			mask_data = *update_image_mask++;
			if (0x03030303 == mask_data) {			// 4 pixel is mask, skip this
				indexdata_addr += 2;			// 4 bytes per cycle
				wavedata_addr += 2;
				continue;
			} else if (0x0 == mask_data) {
				mask_flag = 0;
			} else {
				image_mask1 = mask_data;
				image_mask2 = mask_data >> 6;
				image_mask3 = mask_data >> 12;
				image_mask4 = mask_data >> 18;
				mask_flag = (image_mask4) | (image_mask3) | (image_mask2) | (image_mask1);
			}

			// 4 pixel per cycle
			index_data1 = (*indexdata_addr);
			index_data2 = (*indexdata_addr) >> 16;
			indexdata_addr++;
			index_data3 = (*indexdata_addr);
			index_data4 = (*indexdata_addr) >> 16;
			indexdata_addr++;

			pixel1 = waveform_array[index_data1];
			pixel2 = waveform_array[index_data2];
			pixel3 = waveform_array[index_data3];
			pixel4 = waveform_array[index_data4];
			t_wavedata1 = (pixel1 |(pixel2 << 2) |(pixel3 << 4) |(pixel4 << 6)) & (~mask_flag);

			wavedata_addr++;
			wavedata = *wavedata_addr & mask_flag | t_wavedata1;
			*wavedata_addr = wavedata;

			wavedata_addr++;
		}
	}
	do_gettimeofday(&etimer);
	__debug("pipe(%d) decode(%d,%d)~(%d,%d), time=%d us\n", 
		pipe->pipe_id, 
		x_start_align, y_start,
		x_end_align,y_end,
		get_delt_us_timer(stimer, etimer));

	return;
}

static void __calc_wavedata_5bit_8data_du(struct pipeline_manager *mgr, struct pipe_info *pipe, unsigned int wavefile_offset)
{
	register u8 *wavedata_addr = NULL;
	register u32 *indexdata_addr = NULL;
	register u8 *waveform_addr = NULL;
	register u32 *update_image_mask = NULL, mask_data = 0;
	register u8 t_wavedata1 = 0, t_wavedata2 = 0;
	register u8 tmp_wavedata = 0, wavedata = 0;
	register u8 image_mask1 = 0, image_mask2 = 0, image_mask3 = 0, image_mask4 = 0;
	register u16 index_data1 = 0, index_data2 = 0, index_data3 = 0, index_data4 = 0;
	register u16 index_data5 = 0, index_data6 = 0, index_data7 = 0, index_data8 = 0;
	register u8 pixel1 = 0, pixel2 = 0, pixel3 = 0, pixel4 = 0;
	register u8 pixel5 = 0, pixel6 = 0, pixel7 = 0, pixel8 = 0;
	register u32  mask_offset = 0, index_offset = 0;
	register u32 wv_width = 0, wv_offset = 0;
	register u32 x_start_align = 0, x_end_align = 0, w_align = 0;;
	register u32 y_start = 0, y_end = 0, w = 0, h = 0;
	register u32 screen_width = 0;	
	register u8 mask_flag = 0;
	struct timeval stimer, etimer;
	struct area_info overlap_area_align;
	u8 waveform_array[1024];
	
	if ((NULL == mgr) || (NULL == pipe)) {
		return;
	}

	if (NULL == pipe->update_image_mask) {
		__wrn("mask buffer is null\n");
		return;
	}

	do_gettimeofday(&stimer);
	
	align_area(pipe->overlap_area, &overlap_area_align, 8);
	screen_width = mgr->panel_info.width;
	wv_width = mgr->panel_info.lbl + mgr->panel_info.lsl + mgr->panel_info.ldl + mgr->panel_info.lel;

	x_start_align = overlap_area_align.x_top;
	x_end_align = overlap_area_align.x_bottom;
	y_start = overlap_area_align.y_top;
	y_end = overlap_area_align.y_bottom;

	w_align = (x_end_align - x_start_align + 1);

	waveform_addr = (u8 *)pipe->waveform_vaddr + wavefile_offset;

	__change_wavedata_to_du_mode(waveform_array, waveform_addr , 5);
	asm ("PLD [%0, #64]"::"r" (pipe->update_image_mask));
	for (h = y_start; h <= y_end; h++) {
		mask_offset = (h - y_start) * w_align;
		update_image_mask = (u32 *)((u8 *)pipe->update_image_mask + mask_offset);

		index_offset = h * screen_width + x_start_align;
		indexdata_addr = (u32 *)((u8 *)mgr->list.index_addr.vaddr + (index_offset << 1));

		wv_offset = (h + mgr->panel_info.fsl + mgr->panel_info.fbl) * wv_width +
			(mgr->panel_info.lbl + mgr->panel_info.lsl) + (x_start_align >> 2);	// 4 byte in one wavedata

		wavedata_addr = (u8 *)((u8 *)mgr->list.wavedata_addr.vaddr + (wv_offset << 1));
		//memcpy(waveform_array, waveform_addr, 1024); //refresh cache

		for (w = x_start_align; w <= x_end_align; w += 4) {
			mask_data = *update_image_mask++;
			if (0x03030303 == mask_data) {			// 4 pixel is mask, skip this
				indexdata_addr += 2;			// 4 bytes per cycle
				wavedata_addr += 2;
				continue;
			} else if (0x0 == mask_data) {
				mask_flag = 0;
			} else {
				image_mask1 = mask_data;
				image_mask2 = mask_data >> 6;
				image_mask3 = mask_data >> 12;
				image_mask4 = mask_data >> 18;
				mask_flag = (image_mask4) | (image_mask3) | (image_mask2) | (image_mask1);
			}

			// 4 pixel per cycle
			index_data1 = (*indexdata_addr);
			index_data2 = (*indexdata_addr) >> 16;
			indexdata_addr++;
			index_data3 = (*indexdata_addr);
			index_data4 = (*indexdata_addr) >> 16;
			indexdata_addr++;
            
//printk("before index data:(I0-I3) %02x%02x%02x%02x\n",index_data1, index_data2, index_data3, index_data4);
			index_data1 = (index_data1  >> 8) | ((index_data1 & 0x10) >> 4);
			index_data2 = (index_data2  >> 8) | ((index_data2 & 0x10) >> 4);
			index_data3 = (index_data3  >> 8) | ((index_data3 & 0x10) >> 4);
			index_data4 = (index_data4  >> 8) | ((index_data4 & 0x10) >> 4);

			pixel1 = waveform_array[index_data1];
			pixel2 = waveform_array[index_data2];
			pixel3 = waveform_array[index_data3];
			pixel4 = waveform_array[index_data4];
			t_wavedata1 = (pixel1 |(pixel2 << 2) |(pixel3 << 4) |(pixel4 << 6)) & (~mask_flag);

			wavedata_addr++;
			wavedata = *wavedata_addr & mask_flag | t_wavedata1;
			*wavedata_addr = wavedata;

			wavedata_addr++;
		}
	}
	do_gettimeofday(&etimer);
	__debug("pipe(%d) decode(%d,%d)~(%d,%d), time=%d us\n", 
		pipe->pipe_id, 
		x_start_align, y_start,
		x_end_align,y_end,
		get_delt_us_timer(stimer, etimer));

	return;
}

static void __calc_wavedata_5bit_16data(struct pipeline_manager *mgr, struct pipe_info *pipe, unsigned int wavefile_offset)
{
	register u32 *wavedata_addr = NULL;
	register u32 *indexdata_addr = NULL;
	register u8 *waveform_addr = NULL;
	register u32 *update_image_mask = NULL, mask_data1 = 0, mask_data2 = 0;
	register u32 t_wavedata1 = 0, t_wavedata2 = 0;
	register u32 tmp_wavedata = 0, wavedata = 0;
	register u32 image_mask1 = 0, image_mask2 = 0, image_mask3 = 0, image_mask4 = 0;
	register u32 image_mask5 = 0, image_mask6 = 0, image_mask7 = 0, image_mask8 = 0;
	register u16 index_data1 = 0, index_data2 = 0, index_data3 = 0, index_data4 = 0;
	register u16 index_data5 = 0, index_data6 = 0, index_data7 = 0, index_data8 = 0;
	register u32 pixel1 = 0, pixel2 = 0, pixel3 = 0, pixel4 = 0;
	register u32 pixel5 = 0, pixel6 = 0, pixel7 = 0, pixel8 = 0;
	register u32  mask_offset = 0, index_offset = 0;
	register u32 wv_width = 0, wv_offset = 0;
	register u32 x_start_align = 0, x_end_align = 0, w_align = 0;;
	register u32 y_start = 0, y_end = 0, w = 0, h = 0;
	register u32 screen_width = 0;	
	register u32 mask_flag = 0;
	struct timeval stimer, etimer;
	struct area_info overlap_area_align;
	u8 waveform_array[1024];
	unsigned int total_pixel = 0, no_change_pixel = 0, change_pixel = 0, change_band_pixel = 0;

	if ((NULL == mgr) || (NULL == pipe)) {
		return;
	}

	if (NULL == pipe->update_image_mask) {
		__wrn("mask buffer is null\n");
		return;
	}
	do_gettimeofday(&stimer);
	
	align_area(pipe->overlap_area, &overlap_area_align, 8);
	screen_width = mgr->panel_info.width;
	wv_width = mgr->panel_info.lbl + mgr->panel_info.lsl + mgr->panel_info.ldl + mgr->panel_info.lel;

	x_start_align = overlap_area_align.x_top;
	x_end_align = overlap_area_align.x_bottom;
	y_start = overlap_area_align.y_top;
	y_end = overlap_area_align.y_bottom;

	w_align = (x_end_align - x_start_align + 1);

	waveform_addr = (u8 *)pipe->waveform_vaddr + wavefile_offset;

	__change_wavedata_to_local_mode(waveform_array, waveform_addr , 5);
	asm ("PLD [%0, #64]"::"r" (pipe->update_image_mask));
	for (h = y_start; h <= y_end; h++) {
		mask_offset = (h - y_start) * w_align;
		update_image_mask = (u32 *)((u8 *)pipe->update_image_mask + mask_offset);

		index_offset = h * screen_width + x_start_align;
		indexdata_addr = (u32 *)((u8 *)mgr->list.index_addr.vaddr + (index_offset << 1));

		wv_offset = (h + mgr->panel_info.fsl + mgr->panel_info.fbl) * wv_width +
			(mgr->panel_info.lbl + mgr->panel_info.lsl) + (x_start_align >> 3);	// 8 byte in one wavedata

		wavedata_addr = (u32 *)((u8 *)mgr->list.wavedata_addr.vaddr + (wv_offset << 2));
		//memcpy(waveform_array, waveform_addr, 1024); //refresh cache

		for (w = x_start_align; w <= x_end_align; w += 8) {
			mask_data1 = *update_image_mask++;
			mask_data2 = *update_image_mask++;
			
			total_pixel++;
			
			if ((0x03030303 == mask_data1) && (0x03030303 == mask_data2)) {		// 8 pixel is mask, skip this
				indexdata_addr += 4;				// 8 bytes per cycle
				wavedata_addr += 1;
				
				no_change_pixel++;				
				continue;
			}

			change_pixel++;
			image_mask1 = (mask_data1 & 0xff) << 8;
			image_mask2 = (mask_data1 & 0xff00) << 2;
			image_mask3 = (mask_data1 & 0xff0000) >> 4;
			image_mask4 = (mask_data1 & 0xff000000) >> 10;
			image_mask5 = (mask_data2 & 0xff) << 16;
			image_mask6 = (mask_data2 & 0xff00) << 10;
			image_mask7 = (mask_data2 & 0xff0000) << 4;
			image_mask8 = (mask_data2 & 0xff000000) >> 2;
			mask_flag = 0xff0000ff | (image_mask4) | (image_mask3) | (image_mask2) | (image_mask1) | \
				(image_mask8) | (image_mask7) | (image_mask6) | (image_mask5);

			// 8 pixel per cycle
			index_data1 = (*indexdata_addr);
			index_data2 = (*indexdata_addr) >> 16;
			indexdata_addr++;
			index_data3 = (*indexdata_addr);
			index_data4 = (*indexdata_addr) >> 16;
			indexdata_addr++;
			index_data5 = (*indexdata_addr);
			index_data6 = (*indexdata_addr) >> 16;
			indexdata_addr++;
			index_data7 = (*indexdata_addr);
			index_data8 = (*indexdata_addr) >> 16;
			indexdata_addr++;

			pixel1 = waveform_array[index_data1];
			pixel2 = waveform_array[index_data2];
			pixel3 = waveform_array[index_data3];
			pixel4 = waveform_array[index_data4];
			pixel5 = waveform_array[index_data5];
			pixel6 = waveform_array[index_data6];
			pixel7 = waveform_array[index_data7];
			pixel8 = waveform_array[index_data8];

			t_wavedata1 = ((pixel1 << 8) |(pixel2 << 10) |(pixel3 << 12) |(pixel4 << 14) | \
				(pixel5 << 16) | (pixel6 << 18) |(pixel7 << 20) |(pixel8 << 22)) & (~mask_flag);

			wavedata = *wavedata_addr & mask_flag | t_wavedata1;
			*wavedata_addr = wavedata;
			wavedata_addr++;
		}
	}
	do_gettimeofday(&etimer);

/*	
	__debug("pipe(%d) decode(%d,%d)~(%d,%d), time=%d us\n", 
		pipe->pipe_id, 
		x_start_align, y_start,
		x_end_align,y_end,
		get_delt_us_timer(stimer, etimer));

	__debug("total_pixel=%d, no_chg_pixel=%d, chg_pixel=%d\n",
		total_pixel, no_change_pixel, change_pixel);
	
*/
	return;
}


static void __calc_wavedata_5bit_16data_du(struct pipeline_manager *mgr, struct pipe_info *pipe, unsigned int wavefile_offset)
{
	register u32 *wavedata_addr = NULL;
	register u32 *indexdata_addr = NULL;
	register u8 *waveform_addr = NULL;
	register u32 *update_image_mask = NULL, mask_data1 = 0, mask_data2 = 0;
	register u32 t_wavedata1 = 0, t_wavedata2 = 0;
	register u32 tmp_wavedata = 0, wavedata = 0;
	register u32 image_mask1 = 0, image_mask2 = 0, image_mask3 = 0, image_mask4 = 0;
	register u32 image_mask5 = 0, image_mask6 = 0, image_mask7 = 0, image_mask8 = 0;
	register u16 index_data1 = 0, index_data2 = 0, index_data3 = 0, index_data4 = 0;
	register u16 index_data5 = 0, index_data6 = 0, index_data7 = 0, index_data8 = 0;
	register u32 pixel1 = 0, pixel2 = 0, pixel3 = 0, pixel4 = 0;
	register u32 pixel5 = 0, pixel6 = 0, pixel7 = 0, pixel8 = 0;
	register u32  mask_offset = 0, index_offset = 0;
	register u32 wv_width = 0, wv_offset = 0;
	register u32 x_start_align = 0, x_end_align = 0, w_align = 0;
	register u32 x_start_org = 0, x_end_org = 0;
	register u32 y_start = 0, y_end = 0, w = 0, h = 0;
	register u32 screen_width = 0;	
	register u32 mask_flag = 0;
	struct timeval stimer, etimer;
	struct area_info overlap_area_align;
	u8 waveform_array[1024];
	unsigned int total_pixel = 0, no_change_pixel = 0, change_pixel = 0, change_band_pixel = 0;

	if ((NULL == mgr) || (NULL == pipe)) {
		return;
	}

	if (NULL == pipe->update_image_mask) {
		__wrn("mask buffer is null\n");
		return;
	}
	do_gettimeofday(&stimer);
	struct area_info *overlap_org_area = &pipe->overlap_area;

	align_area(pipe->overlap_area, &overlap_area_align, 8);
	screen_width = mgr->panel_info.width;
	wv_width = mgr->panel_info.lbl + mgr->panel_info.lsl + mgr->panel_info.ldl + mgr->panel_info.lel;

	x_start_align = overlap_area_align.x_top;
	x_end_align = overlap_area_align.x_bottom;
	y_start = overlap_area_align.y_top;
	y_end = overlap_area_align.y_bottom;
	x_start_org = overlap_org_area->x_top;
	x_end_org = overlap_org_area->x_bottom;
	u32 mid = (y_start + y_end) >> 1;

	w_align = (x_end_align - x_start_align + 1);

	waveform_addr = (u8 *)pipe->waveform_vaddr + wavefile_offset;

	__change_wavedata_to_du_mode(waveform_array, waveform_addr , 5);


	asm ("PLD [%0, #64]"::"r" (pipe->update_image_mask));
	for (h = y_start; h <= y_end; h++) {
		mask_offset = (h - y_start) * w_align;
		update_image_mask = (u32 *)((u8 *)pipe->update_image_mask + mask_offset);

		index_offset = h * screen_width + x_start_align;
		indexdata_addr = (u32 *)((u8 *)mgr->list.index_addr.vaddr + (index_offset << 1));

		wv_offset = (h + mgr->panel_info.fsl + mgr->panel_info.fbl) * wv_width +
			(mgr->panel_info.lbl + mgr->panel_info.lsl) + (x_start_align >> 3);	// 8 byte in one wavedata

		wavedata_addr = (u32 *)((u8 *)mgr->list.wavedata_addr.vaddr + (wv_offset << 2));
		//memcpy(waveform_array, waveform_addr, 1024); //refresh cache
        //__debug("h:%d\n",h);
		for (w = x_start_align; w <= x_end_align; w += 8) {
			mask_data1 = *update_image_mask++;
			mask_data2 = *update_image_mask++;
            //__debug("mask_data: %08x %08x\n",mask_data1 ,mask_data2);
			
			total_pixel++;
			
			if ((0x03030303 == mask_data1) && (0x03030303 == mask_data2)) {		// 8 pixel is mask, skip this
                //__debug("mask data in");
				indexdata_addr += 4;				// 8 bytes per cycle
				wavedata_addr += 1;
				
				no_change_pixel++;				
				continue;
			}

			change_pixel++;
			image_mask1 = (mask_data1 & 0xff) << 8;
			image_mask2 = (mask_data1 & 0xff00) << 2;
			image_mask3 = (mask_data1 & 0xff0000) >> 4;
			image_mask4 = (mask_data1 & 0xff000000) >> 10;
			image_mask5 = (mask_data2 & 0xff) << 16;
			image_mask6 = (mask_data2 & 0xff00) << 10;
			image_mask7 = (mask_data2 & 0xff0000) << 4;
			image_mask8 = (mask_data2 & 0xff000000) >> 2;
			mask_flag = 0xff0000ff | (image_mask4) | (image_mask3) | (image_mask2) | (image_mask1) | \
				(image_mask8) | (image_mask7) | (image_mask6) | (image_mask5);

			// 8 pixel per cycle

			index_data1 = (*indexdata_addr);
			index_data2 = (*indexdata_addr) >> 16;
			indexdata_addr++;
			index_data3 = (*indexdata_addr);
			index_data4 = (*indexdata_addr) >> 16;
			indexdata_addr++;
			index_data5 = (*indexdata_addr);
			index_data6 = (*indexdata_addr) >> 16;
			indexdata_addr++;
			index_data7 = (*indexdata_addr);
			index_data8 = (*indexdata_addr) >> 16;
			indexdata_addr++;

//printk("before index data:(I0-I7) %02x%02x%02x%02x%02x%02x%02x%02x\n",index_data1, index_data2, index_data3, index_data4, index_data5, index_data6, index_data7,index_data8);


			index_data1 = (index_data1  >> 8) | ((index_data1 & 0x10) >> 4);
			index_data2 = (index_data2  >> 8) | ((index_data2 & 0x10) >> 4);
			index_data3 = (index_data3  >> 8) | ((index_data3 & 0x10) >> 4);
			index_data4 = (index_data4  >> 8) | ((index_data4 & 0x10) >> 4);
			index_data5 = (index_data5  >> 8) | ((index_data5 & 0x10) >> 4);
			index_data6 = (index_data6  >> 8) | ((index_data6 & 0x10) >> 4);
			index_data7 = (index_data7  >> 8) | ((index_data7 & 0x10) >> 4);
			index_data8 = (index_data8  >> 8) | ((index_data8 & 0x10) >> 4);

            //printk("index data:(I0-I7) %02x%02x%02x%02x%02x%02x%02x%02x\n",index_data1, index_data2, index_data3, index_data4, index_data5, index_data6, index_data7,index_data8);
			pixel1 = waveform_array[index_data1];
			pixel2 = waveform_array[index_data2];
			pixel3 = waveform_array[index_data3];
			pixel4 = waveform_array[index_data4];
			pixel5 = waveform_array[index_data5];
			pixel6 = waveform_array[index_data6];
			pixel7 = waveform_array[index_data7];
			pixel8 = waveform_array[index_data8];
			

			t_wavedata1 = ((pixel1 << 8) |(pixel2 << 10) |(pixel3 << 12) |(pixel4 << 14) | \
				(pixel5 << 16) | (pixel6 << 18) |(pixel7 << 20) |(pixel8 << 22)) & (~mask_flag);

			wavedata = *wavedata_addr & mask_flag | t_wavedata1;
			*wavedata_addr = wavedata;
			wavedata_addr++;
		}
	}
	do_gettimeofday(&etimer);

        
#ifdef TRACE_WAVEDATA_BUFFER_HANDWRITE
        u32 vsync = 0, hsync = 0;
        int x = (mgr->panel_info.lbl + mgr->panel_info.lsl) + (x_start_align >> 3);
        int y = (y_start + mgr->panel_info.fsl + mgr->panel_info.fbl);
        int width = ((x_end_align - x_start_align) >> 3 ) + 1;
        int height = y_end - y_start;

        hsync = mgr->panel_info.lbl + mgr->panel_info.lsl + mgr->panel_info.ldl  + mgr->panel_info.lel;
        vsync = mgr->panel_info.fbl + mgr->panel_info.fsl + mgr->panel_info.fdl  + mgr->panel_info.fel;
        

        dump_pixels("wavedata",mgr->list.wavedata_addr.vaddr, x,y,width,height,hsync*4,4);
#endif


	__debug("pipe(%d) decode(%d,%d)~(%d,%d), time=%d us\n", 
		pipe->pipe_id, 
		x_start_align, y_start,
		x_end_align,y_end,
		get_delt_us_timer(stimer, etimer));

	__debug("total_pixel=%d, no_chg_pixel=%d, chg_pixel=%d\n",
		total_pixel, no_change_pixel, change_pixel);
	

	return;
}





static void __calc_wavedata_5bit_16data_neon(struct pipeline_manager *mgr, struct pipe_info *pipe, unsigned int wavefile_offset)
{
	register u32 *wavedata_addr = NULL;
	register u32 *indexdata_addr = NULL;
	register u8 *waveform_addr = NULL;
	register u32 *update_image_mask = NULL, mask_data1 = 0, mask_data2 = 0;
	register u32 t_wavedata1 = 0, t_wavedata2 = 0;
	register u32 tmp_wavedata = 0, wavedata = 0;
	register u32 image_mask1 = 0, image_mask2 = 0, image_mask3 = 0, image_mask4 = 0;
	register u32 image_mask5 = 0, image_mask6 = 0, image_mask7 = 0, image_mask8 = 0;
	register u16 index_data1 = 0, index_data2 = 0, index_data3 = 0, index_data4 = 0;
	register u16 index_data5 = 0, index_data6 = 0, index_data7 = 0, index_data8 = 0;
	register u32 pixel1 = 0, pixel2 = 0, pixel3 = 0, pixel4 = 0;
	register u32 pixel5 = 0, pixel6 = 0, pixel7 = 0, pixel8 = 0;
	register u32  mask_offset = 0, index_offset = 0;
	register u32 wv_width = 0, wv_offset = 0;
	register u32 x_start_align = 0, x_end_align = 0, w_align = 0;;
	register u32 y_start = 0, y_end = 0, w = 0, h = 0;
	register u32 screen_width = 0;	
	register u32 mask_flag = 0;
	struct timeval stimer, etimer;
	struct area_info overlap_area_align;
	u8 waveform_array[1024];
	unsigned int total_pixel = 0, no_change_pixel = 0, change_pixel = 0, change_band_pixel = 0;
	u32 mask_data[8] = {0};
	
	if ((NULL == mgr) || (NULL == pipe)) {
		return;
	}

	if (NULL == pipe->update_image_mask) {
		__wrn("mask buffer is null\n");
		return;
	}
	do_gettimeofday(&stimer);
	
	align_area(pipe->overlap_area, &overlap_area_align, NEON_DATA_ALIGN);
	screen_width = mgr->panel_info.width;
	wv_width = mgr->panel_info.lbl + mgr->panel_info.lsl + mgr->panel_info.ldl + mgr->panel_info.lel;

	x_start_align = overlap_area_align.x_top;
	x_end_align = overlap_area_align.x_bottom;
	y_start = overlap_area_align.y_top;
	y_end = overlap_area_align.y_bottom;

	w_align = (x_end_align - x_start_align + 1);

	waveform_addr = (u8 *)pipe->waveform_vaddr + wavefile_offset;

	__change_wavedata_to_du_mode(waveform_array, waveform_addr , 5);
	asm ("PLD [%0, #64]"::"r" (pipe->update_image_mask));
	
//	printk("y_s(%d) y_e(%d) x_s(%d),x_e(%d) \n",y_start,y_end,x_start_align,x_end_align);
	for (h = y_start; h <= y_end; h++) {
		mask_offset = (h - y_start) * w_align;
		update_image_mask = (u32 *)((u8 *)pipe->update_image_mask + mask_offset);

		index_offset = h * screen_width + x_start_align;
		indexdata_addr = (u32 *)((u8 *)mgr->list.index_addr.vaddr + (index_offset << 1));

		wv_offset = (h + mgr->panel_info.fsl + mgr->panel_info.fbl) * wv_width +
			(mgr->panel_info.lbl + mgr->panel_info.lsl) + (x_start_align >> 3);	// 8 byte in one wavedata

		wavedata_addr = (u32 *)((u8 *)mgr->list.wavedata_addr.vaddr + (wv_offset << 2));
		//memcpy(waveform_array, waveform_addr, 1024); //refresh cache

		kernel_neon_begin();
		for (w = x_start_align; w <= x_end_align; w += 32) {
			neon_encode_core_function((u8 *)update_image_mask, (u16 *)indexdata_addr, waveform_array, wavedata_addr);
			update_image_mask += 8;
			indexdata_addr += 16;				// 8 bytes per cycle
			wavedata_addr += 4;
		}
		kernel_neon_end();
	}
	do_gettimeofday(&etimer);
	
	__debug("pipe(%d) decode(%d,%d)~(%d,%d), neon time=%d us\n", 
		pipe->pipe_id, 
		x_start_align, y_start,
		x_end_align,y_end,
		get_delt_us_timer(stimer, etimer));
	
	
	return;
}



static void calc_wavedata(struct pipeline_manager *mgr, struct pipe_info *pipe, unsigned int wavefile_offset)
{
	unsigned int bit_num = mgr->panel_info.bit_num;
	unsigned int data_len = mgr->panel_info.data_len;

	if ((8 != data_len) && (16 != data_len)) {
		__wrn("unkown data len=%d\n", data_len);
		return;
	}
	if (bit_num > EINK_BIT_4) {
		// 5-bit
		if (8 == data_len) {
			if ((pipe->mode & 0xff) == EINK_DU_MODE || (pipe->mode & 0xff) == EINK_A2_MODE) {
				__calc_wavedata_5bit_8data_du(mgr, pipe, wavefile_offset);
			} else {
				__calc_wavedata_5bit_8data(mgr, pipe, wavefile_offset);
			}
		} else {
			#if SUPPORT_NEON_ALGORITHM
			if ((pipe->mode & 0xff) == EINK_DU_MODE) {
				__calc_wavedata_5bit_16data_neon(mgr, pipe, wavefile_offset);
			} else {
				__calc_wavedata_5bit_16data(mgr, pipe, wavefile_offset);
			}
			#else
			if ((pipe->mode & 0xff) == EINK_DU_MODE || (pipe->mode & 0xff) == EINK_A2_MODE) {
				__calc_wavedata_5bit_16data_du(mgr, pipe, wavefile_offset);
			} else {
				__calc_wavedata_5bit_16data(mgr, pipe, wavefile_offset);
			}
			#endif
		}
	} else {
		// 4-bit
		if (8 == data_len) {
			if ((pipe->mode & 0xff) == EINK_DU_MODE || (pipe->mode & 0xff) == EINK_A2_MODE) {
				__calc_wavedata_4bit_8data_du(mgr, pipe, wavefile_offset);
			} else {
				__calc_wavedata_4bit_8data(mgr, pipe, wavefile_offset);
			}
		} else {
			if ((pipe->mode & 0xff) == EINK_DU_MODE || (pipe->mode & 0xff) == EINK_A2_MODE) {
				__calc_wavedata_4bit_16data_du(mgr, pipe, wavefile_offset);
			} else {
				__calc_wavedata_4bit_16data(mgr, pipe, wavefile_offset);
			}
		}
	}
		
	return;
}



/*
* active all pipe that has already configured but not active yet, 
* this function MUST be called in within DECODE interrupt and VSYNC interrupt
*/
static int active_all_valid_pipe(struct pipeline_manager *mgr)
{
	struct pipe_info *pipe, *tmp_pipe;
	unsigned long flags = 0, flag = 0;
	unsigned int id = 0;
	unsigned long dec_left_frames = 0;
	unsigned long tframes = 0, tmp_frames = 0;
	void *wvdata_paddr = NULL;
	unsigned int cur_wave_paddr = 0;
	unsigned int bit_num = 0, step_pow = 0;
	int ret = -1;
	u32 test_width = 0, test_height = 0, test_time = 0;
	struct timeval dec_stimer, dec_etimer;
	struct area_info overlap_area_align;
	u32 wavefile_offset = 0;
	u32 order = 0;

	if (NULL == mgr) {
		__wrn("input param is null\n");
		return -1;
	}


	bit_num = mgr->panel_info.bit_num;
	step_pow = bit_num << 1;

	spin_lock_irqsave(&mgr->list.list_lock, flag);
	list_for_each_entry_safe(pipe, tmp_pipe, &mgr->list.used_list, node) {
		BUG_ON(NULL == pipe);
		if (true != pipe->configure_flag) {
			continue;
		}
		if(pipe->mode == 0x402){
//			pipe->total_frames = 3;
		}	
		
		if (false == pipe->active_flag) {
			//this pipe has been configed in init_pipe, here we just enable it
			ret = __active_pipe(mgr, pipe);
			if (0 == ret) {
				tmp_frames = max(tframes, pipe->total_frames);
				tframes = tmp_frames;
				order = pipe->update_order;
			}
		} else {
			pipe->dec_frame_cnt++;
			if (pipe->dec_frame_cnt == pipe->total_frames) {
				//calc overlap area part for the last one frame
				if (false == rectangle_is_zero(pipe->overlap_area)) {
					wavefile_offset = disp_al_eink_pipe_get_config_wavefile(pipe->pipe_id) - pipe->waveform_paddr;
					calc_wavedata(mgr, pipe, wavefile_offset);
				}
				
				//all frame has been decoded, disable this pipe in HW
				disp_al_eink_pipe_disable(mgr->disp, pipe->pipe_id);
			} else if (pipe->dec_frame_cnt < pipe->total_frames){
				//calc overlap area part for current frame
				if (false == rectangle_is_zero(pipe->overlap_area)) {
					wavefile_offset = disp_al_eink_pipe_get_config_wavefile(pipe->pipe_id) - pipe->waveform_paddr;
                    __debug("calc_wavedata");
					calc_wavedata(mgr, pipe, wavefile_offset);
				}
				
				//config waveform of next frame
				cur_wave_paddr = pipe->waveform_paddr + (pipe->dec_frame_cnt << step_pow);
				disp_al_eink_pipe_config_wavefile(mgr->disp, cur_wave_paddr, pipe->pipe_id);
			} else {
				//there are some frame has not fresh out, wait for release this pipe
				//__debug("pipe(%d),dec_frame(%d) > total_frame(%d), ignore it\n", pipe->pipe_id, pipe->dec_frame_cnt, pipe->total_frames);
			}
		}
	}
	spin_unlock_irqrestore(&mgr->list.list_lock, flag);

	//insert one or more pipe
	if (tframes) {
		spin_lock_irqsave(&mgr->list.frame_slock, flags);
		dec_left_frames = mgr->list.all_total_frames - mgr->list.all_decode_frame_cnt;
		if (dec_left_frames <= tframes) {
			mgr->list.all_total_frames +=  (tframes - dec_left_frames + 1);
		}
		spin_unlock_irqrestore(&mgr->list.frame_slock, flags);
		//printk("new insert: order=%d, left frames=%d, t=%d, all total frames=%d\n", order, dec_left_frames, tframes, mgr->list.all_total_frames);
	}

	return 0;
}


static int active_all_valid_soft_pipe(struct pipeline_manager *mgr)
{
	struct pipe_info *pipe, *tmp_pipe;
	unsigned long flags = 0, flag = 0;
	unsigned int id = 0;
	unsigned long dec_left_frames = 0;
	unsigned long tframes = 0, tmp_frames = 0;
	void *wvdata_paddr = NULL;
	unsigned int cur_wave_paddr = 0;
	unsigned int bit_num = 0, step_pow = 0;
	int ret = -1;
	u32 test_width = 0, test_height = 0, test_time = 0;
	struct timeval dec_stimer, dec_etimer;
	struct area_info overlap_area_align;
	u32 wavefile_offset = 0;
	u32 order = 0;

	if (NULL == mgr) {
		__wrn("input param is null\n");
		return -1;
	}
	bit_num = mgr->panel_info.bit_num;
	step_pow = bit_num << 1;

	spin_lock_irqsave(&mgr->list.list_lock, flag);
	list_for_each_entry_safe(pipe, tmp_pipe, &mgr->list.soft_pipe_used_list, node) {
		BUG_ON(NULL == pipe);
		if (true != pipe->configure_flag) {
			continue;
		}
		if(pipe->mode = 0x402){
//			pipe->total_frames = 3;
		}

		if (false == pipe->active_flag) {
			//this pipe has been configed in init_pipe, here we just enable it
			ret = __active_soft_pipe(mgr, pipe);
			if (0 == ret) {
				tmp_frames = max(tframes, pipe->total_frames);
				tframes = tmp_frames;
				order = pipe->update_order;
			}
		} 
		pipe->dec_frame_cnt++;
		if (pipe->dec_frame_cnt == pipe->total_frames) {
			//calc overlap area part for the last one frame
			if (false == rectangle_is_zero(pipe->overlap_area)) {
				wavefile_offset = pipe->soft_pipe_cur_waveform_paddr - pipe->waveform_paddr;
				calc_wavedata(mgr, pipe, wavefile_offset);
			}
		}else if (pipe->dec_frame_cnt < pipe->total_frames){
			//calc overlap area part for current frame
			if (false == rectangle_is_zero(pipe->overlap_area)) {
				wavefile_offset = pipe->soft_pipe_cur_waveform_paddr - pipe->waveform_paddr;
				__debug("calc_wavedata %x\n",wavefile_offset);
				calc_wavedata(mgr, pipe, wavefile_offset);
			}
			//__debug("pipe->dec_frame_cnt:%d\n",pipe->dec_frame_cnt);
			//config waveform of next frame
			pipe->soft_pipe_cur_waveform_paddr = pipe->waveform_paddr + (pipe->dec_frame_cnt << step_pow);
		} else {			
			pipe->soft_pipe_cur_waveform_paddr = pipe->waveform_paddr;
		}
	}
	spin_unlock_irqrestore(&mgr->list.list_lock, flag);

    //__debug("[%s %d] \n",__func__, __LINE__);
	//insert one or more pipe
	if (tframes) {
		spin_lock_irqsave(&mgr->list.frame_slock, flags);
		dec_left_frames = mgr->list.all_total_frames - mgr->list.all_decode_frame_cnt;
		if (dec_left_frames <= tframes) {
			mgr->list.all_total_frames +=  (tframes - dec_left_frames + 1);
		}
		spin_unlock_irqrestore(&mgr->list.frame_slock, flags);
		//__debug("new insert: order=%d, left frames=%d, t=%d, all total frames=%d\n", order, dec_left_frames, tframes, mgr->list.all_total_frames);
	}

	return 0;
}




/*
* get the overlap info of this pipe with other pipe
*/
static int get_overlap_status(struct pipeline_manager *mgr, int id)
{
	struct pipe_info *pipe, *tmp_pipe;
	unsigned long flags = 0, flag = 0;
	unsigned int overlap_flag = 0;
	
	if (NULL == mgr) {
		__wrn("input param is null\n");
		return 0;
	}

	if (is_invalid_pipe_id(mgr, id)) {
		__wrn("pipe id(=%d) is invalid\n", id);
		return 0;
	}

	spin_lock_irqsave(&mgr->list.list_lock, flag);
	list_for_each_entry_safe(pipe, tmp_pipe, &mgr->list.used_list, node) {
		if (pipe->pipe_id != id) {
			continue;
		}

		overlap_flag = pipe->overlap_flag;
		break;
	}
	spin_unlock_irqrestore(&mgr->list.list_lock, flag);

	return overlap_flag;
}

bool is_overlap(struct area_info a_area, struct area_info b_area)
{
	bool overlap_flag = true;

	if (rectangle_is_zero(a_area) || rectangle_is_zero(b_area)) {
		return false;
	}
	
	if ((max(a_area.x_top, b_area.x_top) <= min(a_area.x_bottom, b_area.x_bottom))
		&& (max(a_area.y_top, b_area.y_top) <= min(a_area.y_bottom, b_area.y_bottom))) {
		overlap_flag = true;
		/*__debug("[OVERLAP]A: (%d,%d)~(%d,%d); B: (%d,%d)~(%d,%d)\n",
			a_area.x_top, a_area.y_top, a_area.x_bottom, a_area.y_bottom,
			b_area.x_top, b_area.y_top, b_area.x_bottom, b_area.y_bottom);*/
	} else {
		overlap_flag = false;
	}

	return overlap_flag;
}

/*
* A area and B area MUST overlap
* output is overlap area
*/
void get_overlap_area(struct area_info a_area, struct area_info b_area, struct area_info *overlap_area)
{
	overlap_area->x_top = max(a_area.x_top, b_area.x_top);
	overlap_area->x_bottom= min(a_area.x_bottom, b_area.x_bottom);

	overlap_area->y_top = max(a_area.y_top, b_area.y_top);
	overlap_area->y_bottom= min(a_area.y_bottom, b_area.y_bottom);
	/*
	__debug("overlap_area(%d,%d)~(%d,%d)\n",
	          overlap_area->x_top, overlap_area->y_top,
	          overlap_area->x_bottom, overlap_area->y_bottom);
	*/
}

/*
* merge two area and write back area to merge_area
*/
void merge_area(struct area_info *merge_area, struct area_info b_area)
{
	struct area_info a_area;

	//backup original area
	memcpy(&a_area, merge_area, sizeof(*merge_area));
	
	merge_area->x_top = min(a_area.x_top, b_area.x_top);
	merge_area->x_bottom = max(a_area.x_bottom, b_area.x_bottom);

	merge_area->y_top = min(a_area.y_top, b_area.y_top);
	merge_area->y_bottom = max(a_area.y_bottom, b_area.y_bottom);
}

/*
* align x_top and x_bottom  to x_top_align and x_bottom_align, 
* x_top_align <= x_top && x_bottom_align >= x_bottom
* so this function will expand the original area, which include the original area 
*/
void align_area(struct area_info area, struct area_info *area_align, unsigned int align)
{
	unsigned int x_top_align = 0, x_bottom_align = 0;
	if ((0 == area.x_top) && (0 == area.y_top) && (0 == area.x_bottom) && (0 == area.y_bottom)) {
		area_align->x_top = 0;
		area_align->x_bottom = 0;
		area_align->y_top = 0;
		area_align->y_bottom = 0;
	} else {
		x_top_align = ALIGN(area.x_top, align);
		x_bottom_align = ALIGN(area.x_bottom, align);

		area_align->x_top = (x_top_align < align)?0:(x_top_align - align);
		area_align->x_bottom = (x_bottom_align <= area.x_bottom)?(x_bottom_align + align - 1):(x_bottom_align -1);
		area_align->y_top = area.y_top;
		area_align->y_bottom = area.y_bottom;
	}

	/*
	__debug("area:(%d,%d)~(%d,%d), area_align:(%d,%d)~(%d,%d), align=%d\n",
		area.x_top,area.y_top, area.x_bottom, area.y_bottom,
		area_align->x_top, area_align->y_top, area_align->x_bottom, area_align->y_bottom,
		align);
	*/
}
EXPORT_SYMBOL(align_area);

/*
rectangle A include rectangle B, then return true;otherwise, return false
*/
bool rect_include_judge(struct area_info a_rect, struct area_info b_rect)
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

/*
two area MUST macth one of the follow condition:
1. A.x_top = B.x_top and A.x_bottom = B.x_bottom
2. A.y_top = B.y_top and A.y_bottom = B.y_bottom
*/
bool rect_split_into_two_part(struct area_info org_area, struct area_info max_overlap_area, 
	struct area_info *update_area)
{
	bool ret = false;
	if ((org_area.x_top == max_overlap_area.x_top) && (org_area.x_bottom== max_overlap_area.x_bottom)) {
		if (org_area.y_top == max_overlap_area.y_top) {
			ret = true;
			update_area->x_top = org_area.x_top;
			update_area->x_bottom = org_area.x_bottom;
			update_area->y_top = max_overlap_area.y_bottom + 1;
			update_area->y_bottom = org_area.y_bottom;
		} else {
			if (org_area.y_bottom == max_overlap_area.y_bottom) {
				ret = true;
				update_area->x_top = org_area.x_top;
				update_area->x_bottom = org_area.x_bottom;
				update_area->y_top = org_area.y_top;
				update_area->y_bottom = max_overlap_area.y_top - 1;
			} else {
				ret = false;
			}
		}

		return ret;
	}

	
	if ((org_area.y_top == max_overlap_area.y_top) && (org_area.y_bottom== max_overlap_area.y_bottom)) {
		if (org_area.x_top == max_overlap_area.x_top) {
			ret = true;
			update_area->x_top = max_overlap_area.x_bottom + 1;
			update_area->x_bottom = org_area.x_bottom;
			update_area->y_top = org_area.y_top;
			update_area->y_bottom = org_area.y_bottom;
		} else {
			if (org_area.x_bottom == max_overlap_area.x_bottom) {
				ret = true;
				update_area->x_top = org_area.x_top;
				update_area->x_bottom = max_overlap_area.x_top - 1;
				update_area->y_top = org_area.y_top;
				update_area->y_bottom = org_area.y_bottom;
			} else {
				ret = false;
			}
		}

		return ret;
	}

	return ret;
}

/*
* two area Must overlap, this function will split the org_area into big part and small part,
* according to max_overlap_area.
*/
bool rect_split_overlap_to_two_part(struct area_info org_area, struct area_info max_overlap_area,
	struct area_info *not_overlap_part_area, struct area_info *overlap_part_area)
{
	bool ret = false;
	struct area_info not_overlap_part, overlap_part;

	if ((NULL == not_overlap_part_area) || (NULL == overlap_part_area)) {
		__wrn("input param is null\n");
		return false;
	}
	
	overlap_part.x_top = org_area.x_top;	
	overlap_part.x_bottom = org_area.x_bottom;
	not_overlap_part.x_top = org_area.x_top;
	not_overlap_part.x_bottom = org_area.x_bottom;

	if (((org_area.x_top == max_overlap_area.x_top) && (org_area.y_top == max_overlap_area.y_top))
	|| ((org_area.x_bottom == max_overlap_area.x_bottom) && (org_area.y_top == max_overlap_area.y_top))) {
		overlap_part.y_top = org_area.y_top;
		overlap_part.y_bottom = max_overlap_area.y_bottom;

		not_overlap_part.y_top = max_overlap_area.y_bottom + 1;
		not_overlap_part.y_bottom = org_area.y_bottom;
		
		ret = true;
	} else if (((org_area.x_top == max_overlap_area.x_top) && (org_area.y_bottom == max_overlap_area.y_bottom)) 
	||((org_area.x_bottom == max_overlap_area.x_bottom) && (org_area.y_bottom == max_overlap_area.y_bottom))) {
		overlap_part.y_top = max_overlap_area.y_top;
		overlap_part.y_bottom = org_area.y_bottom;

		not_overlap_part.y_top = org_area.y_top;
		not_overlap_part.y_bottom = max_overlap_area.y_top - 1;
		
		ret = true;
	} else {
		ret = false;
	}

	if (true == ret) {
		memcpy(not_overlap_part_area, &not_overlap_part, sizeof(not_overlap_part));
		memcpy(overlap_part_area, &overlap_part, sizeof(overlap_part));
	}

	return ret;
}

bool rectangle_is_too_large(struct area_info area, enum eink_update_mode mode)
{
	bool ret = true;	
	u32 max_area = 0, rect_area = 0;
	
	return false;
	
	rect_area = (area.x_bottom - area.x_top + 1) * (area.y_bottom - area.y_top + 1);
	if ((mode & 0xff) == EINK_DU_MODE || (mode & 0xff) == EINK_A2_MODE) {
		max_area = 600000;
	} else {
		return false;
		//max_area = 100000;
	}

	if (rect_area > max_area) {
		ret = true;
	} else {
		ret = false;
	}
		
	return ret;
}

bool rectangle_is_same(struct area_info a,struct area_info b)
{
    if ((a.x_top == b.x_top) && (a.x_bottom == b.x_bottom) 
        && (a.y_top == b.y_top) && (a.y_bottom == b.y_bottom)) {
        return true;
    }

    return false;
}

/*
* ONLY split the y_top or y_bottom of overlap_area and org_area is the same
*/
void split_overlap_area(struct area_info org_area, struct area_info overlap_area, struct area_info *out_area)
{
    if (rectangle_is_same(org_area,overlap_area)) {
        out_area->x_top = 0;
        out_area->y_top = 0;
        out_area->x_bottom = 0;
        out_area->y_bottom = 0;
        return;
    }
	if ((org_area.y_top == overlap_area.y_top) || (org_area.y_bottom == overlap_area.y_bottom)) {
		out_area->x_top = org_area.x_top;
		out_area->x_bottom = org_area.x_bottom;
		out_area->y_top = max(org_area.y_top, overlap_area.y_top);
		out_area->y_bottom = min(org_area.y_bottom, overlap_area.y_bottom);
	} else {
		//overlap_area is not part of org_area, can NOT split
		out_area->x_top = org_area.x_top;
		out_area->y_top = org_area.y_top;
		out_area->x_bottom = org_area.x_bottom;
		out_area->y_bottom = org_area.y_bottom;
	}
}


//#define TEST_SW_CALC_WAVEDATA
/*
* input update area info, compare with area in other pipe.
* if update area is overlap with other pipe, then split update area into update area + overlap_area
* overlap_area MUST align with 32-bit
*/
static int get_collision_relationship(struct pipeline_manager *mgr, struct pipe_info *pinfo)
{
	struct pipe_info *pipe, *tmp_pipe;
	struct area_info tmp_area_array[16], max_overlap_area, overlap_area_align;
	struct area_info overlap_part_area, not_overlap_part_area;
	unsigned int overlap_flag = 0, collision_flag = 0;
	unsigned int align = 0;
	unsigned int i = 0;
	unsigned int w = 0, h = 0, width = 0, height = 0, start = 0, end = 0;
	char *tmp_mask = NULL;
	unsigned int offset = 0, x_top = 0, y_top = 0, x_bottom = 0, y_bottom = 0;
	unsigned int valid_len = 0;
	unsigned char data = 0;
	unsigned int max_area = 0;
	bool split_flag = false;
	u32 data_len = 0;
	enum eink_update_mode mode = 0;
	unsigned long flag = 0;
    bool is_overlap_area = false;

	if ((NULL == mgr) || (NULL == pinfo)) {
		__wrn("input param is null\n");
		return -1;
	}

	memset(tmp_area_array, 0, sizeof(tmp_area_array));
	memset(&overlap_area_align, 0, sizeof(overlap_area_align));
	max_overlap_area.x_top =(unsigned int)(-1);
	max_overlap_area.y_top = (unsigned int)(-1);
	max_overlap_area.x_bottom = 0;
	max_overlap_area.y_bottom = 0;
	data_len = mgr->panel_info.data_len;

	pinfo->overlap_type = OVERLAP_NOT_COLLISION;

	if (pinfo->img->empty_frame_flag) {
		__debug("empty frame\n");
		memset(&pinfo->overlap_area, 0, sizeof(pinfo->overlap_area));
		memset(&pinfo->update_area, 0, sizeof(pinfo->update_area));
		pinfo->overlap_flag = 0;
		pinfo->collision_flag = 0;

		return 0;
	}
	
	spin_lock_irqsave(&mgr->list.list_lock, flag);
	
	if (list_empty(&mgr->list.used_list)) {
		spin_unlock_irqrestore(&mgr->list.list_lock, flag);

		memset(&pinfo->overlap_area, 0, sizeof(pinfo->overlap_area));
		memcpy(&pinfo->update_area, &pinfo->org_update_area, sizeof(pinfo->org_update_area));
		pinfo->overlap_flag = 0;
		pinfo->collision_flag = 0;
        
		__debug("used list is empty, no overlap\n");
		return 0;
	}

	list_for_each_entry_safe(pipe, tmp_pipe, &mgr->list.used_list, node) {
		if (is_overlap(pipe->org_update_area, pinfo->org_update_area) && pipe->update_order != pinfo->update_order) {
			overlap_flag = overlap_flag | (1 << pipe->pipe_id);
			get_overlap_area(pipe->org_update_area, pinfo->org_update_area, &tmp_area_array[pipe->pipe_id]);
			/*if ((false == disp_al_is_local_update_mode(pipe->mode)) ||
				(false == disp_al_is_local_update_mode(pinfo->img->update_mode))) {
				collision_flag = collision_flag | (1 << pipe->pipe_id);
			}*/

			max_overlap_area.x_top = min(max_overlap_area.x_top, tmp_area_array[pipe->pipe_id].x_top);
			max_overlap_area.x_bottom = max(max_overlap_area.x_bottom, tmp_area_array[pipe->pipe_id].x_bottom);
			max_overlap_area.y_top = min(max_overlap_area.y_top, tmp_area_array[pipe->pipe_id].y_top);
			max_overlap_area.y_bottom = max(max_overlap_area.y_bottom, tmp_area_array[pipe->pipe_id].y_bottom);
            is_overlap_area = true;
		}
	}

	spin_unlock_irqrestore(&mgr->list.list_lock, flag);

	mode = pinfo->img->update_mode;
    if (is_overlap_area)
        memcpy(&pinfo->overlap_area, &max_overlap_area, sizeof(struct area_info));
	
	#if SUPPORT_NEON_ALGORITHM
	if ((pinfo->mode & 0xff) == EINK_DU_MODE) {
		align = NEON_DATA_ALIGN;	//align 32 according to NEON algorithm
	} else {
		align = 8;	// pixel per cycle
	}
	#else
	align = 8;	// pixel per cycle
	#endif
	if (overlap_flag) {
		if ((pinfo->mode & 0xff) == EINK_CLEAR_MODE) {
			memset(&pinfo->update_area, 0, sizeof(pinfo->update_area));
			memcpy(&pinfo->overlap_area, &pipe->org_update_area, sizeof(pipe->org_update_area));
		}
		pinfo->overlap_flag = overlap_flag;

		align_area(pinfo->overlap_area, &overlap_area_align, align);

		/*
		rectangle_print_coordinate("org_area", pinfo->org_update_area);
		rectangle_print_coordinate("overlap_area", pinfo->overlap_area);
		rectangle_print_coordinate("update area", pinfo->update_area);
		*/

		if (rectangle_is_zero(pinfo->overlap_area)) {
			//__debug("whole area collision(%d) or overlap area is zero\n", pinfo->overlap_type);
			return 0;
		}
		width = overlap_area_align.x_bottom - overlap_area_align.x_top + 1;
		height = overlap_area_align.y_bottom - overlap_area_align.y_top + 1;

		//alloc mask buffer
		pinfo->update_image_mask = vmalloc(width * height);
		if (NULL == pinfo->update_image_mask) {
			__wrn("fail to alloc mask buffer, size=%d\n", width * height);
			return -2;
		}
		memset(pinfo->update_image_mask, 0x03, (width * height));

		//set default value to 0x0(update), outside the update area set to 0x03(mask)
		start = pinfo->overlap_area.x_top - overlap_area_align.x_top;
		end = pinfo->overlap_area.x_bottom - overlap_area_align.x_top;
		valid_len = end - start + 1;
		//__debug("overlap: start=%d, end=%d, width=%d, height=%d\n", start, end, width, height);

		for (h = 0; h < height; h++) {
			tmp_mask = (char *)pinfo->update_image_mask + h * width + start;
			memset(tmp_mask, 0x00, valid_len); 
		}

		//mask the overlap area
		for (i = 0; i < ARRAY_SIZE(tmp_area_array); i++) {
			if (overlap_flag & (1 << i)) {
				//set tmp_area_array[i] area to 0x3(mask)
				x_top = tmp_area_array[i].x_top - overlap_area_align.x_top;
				y_top = tmp_area_array[i].y_top - overlap_area_align.y_top;
				x_bottom = tmp_area_array[i].x_bottom - overlap_area_align.x_top;
				y_bottom = tmp_area_array[i].y_bottom -overlap_area_align.y_top;

				/*__debug("overlap area: (%d,%d)~(%d,%d),(%d,%d)~(%d,%d)\n",
					tmp_area_array[i].x_top,tmp_area_array[i].y_top,
					tmp_area_array[i].x_bottom, tmp_area_array[i].y_bottom,
					x_top,y_top,x_bottom,y_bottom);*/
				for (h = y_top; h <= y_bottom; h++) {
					offset = h * width + x_top;
					tmp_mask = (char *)pinfo->update_image_mask + offset;
					memset(tmp_mask, (i+1), (x_bottom - x_top + 1));
				}
			}
		}
	} else {
		memset(&pinfo->overlap_area, 0, sizeof(pinfo->overlap_area));
		memcpy(&pinfo->update_area, &pinfo->org_update_area, sizeof(pinfo->org_update_area));
		pinfo->overlap_flag = 0;
	}
	// IB need to be update no matter what overlap or collision
	memcpy(&pinfo->update_area, &pinfo->org_update_area, sizeof(pinfo->org_update_area));

	return 0;
}


static int soft_pipe_get_collision_relationship(struct pipeline_manager *mgr, struct pipe_info *pinfo)
{
	struct pipe_info *pipe, *tmp_pipe;
	//struct area_info tmp_area_array[32], max_overlap_area, overlap_area_align;
	struct area_info tmp_area_array[SOFT_PIPELINE_NUM], max_overlap_area, overlap_area_align;
	struct area_info overlap_part_area, not_overlap_part_area;
	unsigned int overlap_flag = 0, collision_flag = 0;
	unsigned int align = 0;
	unsigned int i = 0;
	unsigned int w = 0, h = 0, width = 0, height = 0, start = 0, end = 0;
	char *tmp_mask = NULL;
	unsigned int offset = 0, x_top = 0, y_top = 0, x_bottom = 0, y_bottom = 0;
	unsigned int valid_len = 0;
	unsigned char data = 0;
	unsigned int max_area = 0;
	bool split_flag = false;
	u32 data_len = 0;
	enum eink_update_mode mode = 0;
	unsigned long flag = 0;

	if ((NULL == mgr) || (NULL == pinfo)) {
		__wrn("input param is null\n");
		return -1;
	}

	memset(tmp_area_array, 0, sizeof(tmp_area_array));
	memset(&overlap_area_align, 0, sizeof(overlap_area_align));
	max_overlap_area.x_top =(unsigned int)(-1);
	max_overlap_area.y_top = (unsigned int)(-1);
	max_overlap_area.x_bottom = 0;
	max_overlap_area.y_bottom = 0;
	data_len = mgr->panel_info.data_len;

	pinfo->overlap_type = OVERLAP_NOT_COLLISION;

	if (pinfo->img->empty_frame_flag) {
		__debug("empty frame\n");
		memset(&pinfo->overlap_area, 0, sizeof(pinfo->overlap_area));
		memset(&pinfo->update_area, 0, sizeof(pinfo->update_area));
		pinfo->overlap_flag = 0;
		pinfo->collision_flag = 0;

		return 0;
	}
	
	spin_lock_irqsave(&mgr->list.list_lock, flag);
	
	if (list_empty(&mgr->list.soft_pipe_used_list)) {
		spin_unlock_irqrestore(&mgr->list.list_lock, flag);

#if 1		
		memcpy(&pinfo->overlap_area, &pinfo->org_update_area, sizeof(pinfo->overlap_area));
		memset(&pinfo->update_area, 0, sizeof(pinfo->org_update_area));	
		pinfo->overlap_flag = 0;
		pinfo->collision_flag = 0;	
		align = 8;	
		align_area(pinfo->overlap_area, &overlap_area_align, align);
		width = overlap_area_align.x_bottom - overlap_area_align.x_top + 1;
		height = overlap_area_align.y_bottom - overlap_area_align.y_top + 1;
		//alloc mask buffer
		pinfo->update_image_mask = vmalloc(width * height);
		if (NULL == pinfo->update_image_mask) {
			__wrn("fail to alloc mask buffer, size=%d\n", width * height);
			return -2;
		}
		memset(pinfo->update_image_mask, 0x3, (width * height));
#endif
		__debug("soft pipe list is empty, no overlap\n");
		return 0;
	}

	list_for_each_entry_safe(pipe, tmp_pipe, &mgr->list.soft_pipe_used_list, node) {
		if (is_overlap(pipe->org_update_area, pinfo->org_update_area)) {
			overlap_flag = overlap_flag | (1 << pipe->pipe_id);
			get_overlap_area(pipe->org_update_area, pinfo->org_update_area, &tmp_area_array[pipe->pipe_id]);
			/*if ((false == disp_al_is_local_update_mode(pipe->mode)) ||
				(false == disp_al_is_local_update_mode(pinfo->img->update_mode))) {
				collision_flag = collision_flag | (1 << pipe->pipe_id);
			}*/

			max_overlap_area.x_top = min(max_overlap_area.x_top, tmp_area_array[pipe->pipe_id].x_top);
			max_overlap_area.x_bottom = max(max_overlap_area.x_bottom, tmp_area_array[pipe->pipe_id].x_bottom);
			max_overlap_area.y_top = min(max_overlap_area.y_top, tmp_area_array[pipe->pipe_id].y_top);
			max_overlap_area.y_bottom = max(max_overlap_area.y_bottom, tmp_area_array[pipe->pipe_id].y_bottom);
		}
	}

	spin_unlock_irqrestore(&mgr->list.list_lock, flag);

	mode = pinfo->img->update_mode;

	#if SUPPORT_NEON_ALGORITHM
	if ((pinfo->mode & 0xff) == EINK_DU_MODE) {
		align = NEON_DATA_ALIGN;	//align 32 according to NEON algorithm
	} else {
		align = 8;	// pixel per cycle
	}
	#else
	align = 8;	// pixel per cycle
	#endif


	if ((pinfo->mode & 0xff) == EINK_DU_MODE || (pinfo->mode & 0xff) == EINK_CLEAR_MODE || (pinfo->mode & 0xff) == EINK_A2_MODE) {
		memset(&pinfo->update_area, 0, sizeof(pinfo->update_area));
		memcpy(&pinfo->overlap_area, &pinfo->org_update_area, sizeof(pinfo->org_update_area));
	}
	pinfo->overlap_flag = overlap_flag;
	pinfo->collision_flag = collision_flag;

	align_area(pinfo->overlap_area, &overlap_area_align, align);

	/*
	rectangle_print_coordinate("org_area", pinfo->org_update_area);
	rectangle_print_coordinate("overlap_area", pinfo->overlap_area);
	rectangle_print_coordinate("update area", pinfo->update_area);
	*/

	if (rectangle_is_zero(pinfo->overlap_area)) {
		//__debug("whole area collision(%d) or overlap area is zero\n", pinfo->overlap_type);
		return 0;
	}
	width = overlap_area_align.x_bottom - overlap_area_align.x_top + 1;
	height = overlap_area_align.y_bottom - overlap_area_align.y_top + 1;

	//alloc mask buffer
	pinfo->update_image_mask = vmalloc(width * height);
	if (NULL == pinfo->update_image_mask) {
		__wrn("fail to alloc mask buffer, size=%d\n", width * height);
		return -2;
	}
	memset(pinfo->update_image_mask, 0x03, (width * height));

	//set default value to 0x0(update), outside the update area set to 0x03(mask)
	start = pinfo->overlap_area.x_top - overlap_area_align.x_top;
	end = pinfo->overlap_area.x_bottom - overlap_area_align.x_top;
	valid_len = end - start + 1;
	//__debug("overlap: start=%d, end=%d, width=%d, height=%d\n", start, end, width, height);

	for (h = 0; h < height; h++) {
		tmp_mask = (char *)pinfo->update_image_mask + h * width + start;
		memset(tmp_mask, 0x00, valid_len); 
	}

	//mask the overlap area
	for (i = 0; i < ARRAY_SIZE(tmp_area_array); i++) {
		if (overlap_flag & (1 << i)) {
			//set tmp_area_array[i] area to 0x3(mask)
			x_top = tmp_area_array[i].x_top - overlap_area_align.x_top;
			y_top = tmp_area_array[i].y_top - overlap_area_align.y_top;
			x_bottom = tmp_area_array[i].x_bottom - overlap_area_align.x_top;
			y_bottom = tmp_area_array[i].y_bottom -overlap_area_align.y_top;

			/*__debug("overlap area: (%d,%d)~(%d,%d),(%d,%d)~(%d,%d)\n",
				tmp_area_array[i].x_top,tmp_area_array[i].y_top,
				tmp_area_array[i].x_bottom, tmp_area_array[i].y_bottom,
				x_top,y_top,x_bottom,y_bottom);*/
			if ((pinfo->mode & 0xff) == EINK_DU_MODE || (pinfo->mode & 0xff) == EINK_A2_MODE) {					
				for (h = y_top; h <= y_bottom; h++) {
					offset = h * width + x_top;
					tmp_mask = (char *)pinfo->update_image_mask + offset;
					memset(tmp_mask, 0x03, (x_bottom - x_top + 1));
				}
			} else {
				for (h = y_top; h <= y_bottom; h++) {
					offset = h * width + x_top;
					tmp_mask = (char *)pinfo->update_image_mask + offset;
					memset(tmp_mask, (i+1), (x_bottom - x_top + 1));
				}
			}
		}
	}

	return 0;
}



static int get_collision_status(struct pipeline_manager *mgr, int id)
{
	struct pipe_info *pipe, *tmp_pipe;
	unsigned long flags = 0, flag = 0;
	unsigned int collision_flag = 0;
	
	if (NULL == mgr) {
		__wrn("input param is null\n");
		return 0;
	}

	if (is_invalid_pipe_id(mgr, id)) {
		__wrn("pipe id(=%d) is invalid\n", id);
		return 0;
	}

	spin_lock_irqsave(&mgr->list.list_lock, flag);
	list_for_each_entry_safe(pipe, tmp_pipe, &mgr->list.used_list, node) {
		if (pipe->pipe_id != id) {
			continue;
		}

		collision_flag = pipe->collision_flag;
		break;
	}
	spin_unlock_irqrestore(&mgr->list.list_lock, flag);

	return collision_flag;
}


static int get_eink_power_state(struct pipeline_manager *mgr, enum e_power_state *state)
{
	if ((NULL == mgr) || (NULL == state)) {
		__wrn("input param is null\n");
		return -1;
	}

	mutex_lock(&mgr->power_mlock);
	*state = mgr->power_state;
	mutex_unlock(&mgr->power_mlock);

	return 0;
}

static void save_all_wavedata_buffer(struct pipeline_manager *mgr, u32 all_total_frames)
{
	struct wavedata_queue *queue = NULL;
	u8 *buf = NULL;
	char file_name[256] = {0};
	u32 id = 0;
	u32 data_len = 0;
	u32 vsync = 0, hsync = 0;

	if (all_total_frames > WAVE_DATA_BUF_NUM) {
		__wrn("too many frames(%d), not to save\n ", all_total_frames);
		return;
	}

	hsync = mgr->panel_info.lbl + mgr->panel_info.lsl + mgr->panel_info.ldl  + mgr->panel_info.lel;
	vsync = mgr->panel_info.fbl + mgr->panel_info.fsl + mgr->panel_info.fdl  + mgr->panel_info.fel;

	if (8 == mgr->panel_info.data_len) {
		data_len = hsync * vsync * 2;
	} else {
		data_len = hsync * vsync * 4;
	}

	__debug("save wavedata, total_frames=%d\n", all_total_frames);
	queue = &mgr->wavedata_ring_buffer;
	for (id = 0; id < all_total_frames; id++) {
		memset(file_name, 0, sizeof(file_name));
		snprintf(file_name, sizeof(file_name), "/tmp/wvdata_%d.bin", id);
		buf = (void*)queue->wavedata_vaddr[id];
		save_bin_file(buf, file_name, data_len, 0);
	}
	
}


static int enable_pipeline_mgr(u32 disp)
{
	struct pipeline_manager *mgr = NULL;
	unsigned long flag = 0;
	int ret = -1;

	mgr=get_pipeline_manager(disp);
	if (NULL == mgr) {
		__wrn("input param is null\n");
		return -1;
	}
	spin_lock_irqsave(&mgr->list.list_lock, flag);
	if (true == mgr->list.ee_enable) {
		spin_unlock_irqrestore(&mgr->list.list_lock, flag);
		return 0;
	}

	/* init eink and edma*/
	ret = disp_al_eink_config(mgr->disp, &mgr->panel_info);
	ret = disp_al_edma_init(mgr->disp, &mgr->panel_info);

	disp_al_eink_irq_enable(mgr->disp);

	mgr->list.ee_enable = true;
	spin_unlock_irqrestore(&mgr->list.list_lock, flag);

	return ret;
}

static int disable_pipeline_mgr(u32 disp)
{
	struct pipeline_manager *mgr = NULL;
	unsigned long flag = 0;
	int ret = 0;

	mgr=get_pipeline_manager(disp);
	if (NULL == mgr) {
		__wrn("input param is null\n");
		return -1;
	}

	spin_lock_irqsave(&mgr->list.list_lock, flag);
	if (false == mgr->list.ee_enable) {
		spin_unlock_irqrestore(&mgr->list.list_lock, flag);
		return 0;
	}

	//TODO: manager clk for ee and sperate index and decoder irq
	//ret = disp_al_eink_irq_disable(mgr->disp);

	/* disable eink engine. */
	ret = disp_al_eink_disable(mgr->disp);

	mgr->list.ee_enable = false;
	spin_unlock_irqrestore(&mgr->list.list_lock, flag);
	return ret;
}

static int image_update_start(void)
{
	unsigned long flags = 0;
	struct pipeline_manager *mgr = NULL;
	int ret = -1;

	mgr=get_pipeline_manager(0);
	if (NULL == mgr) {
		__wrn("input param is null\n");
		return -1;
	}

	spin_lock_irqsave(&mgr->list.update_protect_slock, flags);
	if (0 == mgr->list.update_state) {
		mgr->list.update_state = 2;
		ret = 0;
	}
	spin_unlock_irqrestore(&mgr->list.update_protect_slock, flags);

	return ret;
}

static void image_update_end(void)
{
	unsigned long flags = 0;
	struct pipeline_manager *mgr = NULL;

	mgr=get_pipeline_manager(0);
	if (NULL == mgr) {
		__wrn("input param is null\n");
		return;
	}

	spin_lock_irqsave(&mgr->list.update_protect_slock, flags);
	if (2 == mgr->list.update_state) {
		mgr->list.update_state = 0;
	}
	spin_unlock_irqrestore(&mgr->list.update_protect_slock, flags);

	return;
}

static void epd_reflush_work(struct work_struct *work)
{
	struct eink_buffer_manager *buf_mgr = NULL;
	struct pipeline_manager *pipe_mgr = NULL;
	bool pmic_pg_status = false;
	
	buf_mgr = g_disp_manager->buffer_mgr;
	pipe_mgr = get_pipeline_manager(0);
    
	if (pipe_mgr->voltage_drop_reflush == true) {
		if ((false == buf_mgr->is_collision_list_empty(buf_mgr)) || 
			(false == buf_mgr->is_empty(buf_mgr)) ||
			(pipe_mgr->query_used_pipe(pipe_mgr)) || (pipe_mgr->query_used_soft_pipe(pipe_mgr))) {
			queue_delayed_work(reflush_ctrl_workqueue, &(reflush_work), 0);
			return;
		}
		
		__wrn("detect voltage drop, reflush start\n");
		buf_mgr->reflush_working_buffer_data(false);
	} else if (pipe_mgr->miss_wavedata_reflush == true) {
		__wrn("miss wavedata frame, reflush start\n");
		buf_mgr->reflush_working_buffer_data(true);
	} 
	
	pipe_mgr->miss_wavedata_reflush = false;
	pipe_mgr->voltage_drop_reflush = false;
	
	return;
}

static void power_on_work(struct work_struct *work)
{
	struct pipeline_manager *mgr = NULL;

	mgr = get_pipeline_manager(0);
	if (NULL == mgr) {
		__wrn("pipeline manager not init yet!\n");
		return;
	}
	
	set_eink_power_state(mgr, EINK_POWER_ON);
	
	if (mgr->update_time_ctrl == true) {
		before_lcd_int_time = ktime_set( 0, g_disp_manager->lcd_int_wait_time );//8ms
		hrtimer_start( &before_lcd_int_hrtimer,  before_lcd_int_time, HRTIMER_MODE_REL );
		//launch no.3 frame
		//mgr->fresh_frame_control(mgr);
	}
	
	//hrtimer_start( &before_write_edma_hrtimer, before_write_edma_time, HRTIMER_MODE_REL );
}

static void power_down_work(struct work_struct *work)
{
	struct pipeline_manager *mgr = NULL;
	unsigned long	flags = 0, fflags = 0, flag = 0;
	unsigned long	all_total_frames;
	unsigned long	all_decode_frame_cnt;
	unsigned long	all_fresh_frame_cnt;
	unsigned long	all_miss_wavedata_cnt;
	unsigned long	all_miss_edma_cnt;
	unsigned long	miss_wavedata_flag;
	unsigned long	frames = 0;
	unsigned int addr = 0, frame_id = 0;
	int hw_decode_time = 0, sw_decode_time = 0, schedule_time = 0;
	int t = 0;
    struct eink_buffer_manager *buf_mgr = NULL;
    int pmic_pg_status = -1;

	mgr = get_pipeline_manager(0);
	if (NULL == mgr) {
		__wrn("pipeline manager not init yet!\n");
		return;
	}
	
	pmic_pg_status = EINK_get_powergood_status();
	if (!pmic_pg_status) {
		__wrn("PMIC powergood is fail, get voltage drop!\n", __func__);
		mgr->voltage_drop_reflush = true;
	}

	set_eink_power_state(mgr, EINK_POWER_OFF);

	//disp_al_lcd_disable(mgr->disp, NULL);
	spin_lock_irqsave(&mgr->list.frame_slock, flags);
	all_total_frames = mgr->list.all_total_frames;
	all_decode_frame_cnt = mgr->list.all_decode_frame_cnt;
	all_fresh_frame_cnt = mgr->list.all_fresh_frame_cnt;
	all_miss_wavedata_cnt = mgr->list.all_miss_wavedata_cnt;
	all_miss_edma_cnt = mgr->list.all_miss_edma_cnt;
	spin_unlock_irqrestore(&mgr->list.frame_slock, flags);

	__debug("total=%d, dec_frame=%d, fresh_frame=%d, miss_wv=%d, miss_edma=%d\n",
		all_total_frames, all_decode_frame_cnt, all_fresh_frame_cnt,
		all_miss_wavedata_cnt, all_miss_edma_cnt);

	for(frames = 0; frames < all_total_frames; frames++) {
		if (mgr->list.miss_wavedata_flag[frames]) {
#ifdef SUPPORT_DEC_TIMER
			hw_decode_time =	get_delt_us_timer(mgr->list.dec_stimer[frames], mgr->list.dec_etimer[frames]);
			schedule_time = get_delt_us_timer(mgr->list.dec_etimer[frames], mgr->list.dec_ttimer[frames]);
			sw_decode_time =	get_delt_us_timer(mgr->list.dec_ttimer[frames], mgr->list.sdec_timer[frames]);
			__wrn("miss %d frame wavedata(%d), hw %d us, sw %d us, sch %d us\n",
			frames, mgr->list.miss_wavedata_flag[frames], hw_decode_time, sw_decode_time, schedule_time);
#else
			__wrn("miss %d frame wavedata\n", frames);
#endif
			mgr->list.miss_wavedata_flag[frames] = 0;
            mgr->miss_wavedata_reflush = true;
		}
		
		if (mgr->list.miss_soft_wavedata_flag[frames]) {
			__wrn("miss %d frame soft wavedata\n", frames);
			mgr->list.miss_soft_wavedata_flag[frames] = 0;
		}
		
		if (mgr->list.miss_edma_flag[frames]) {
			__wrn("miss %d frame edma, cur_line=%d\n", frames, mgr->list.miss_edma_flag[frames]);
			mgr->list.miss_edma_flag[frames] = 0;
		}
	}
	
	if (mgr->miss_wavedata_reflush == true || mgr->voltage_drop_reflush == true) {
		/* frame flush not current, it could happend at cpu busy(lcd interrupt not on time),
		 * or voltage drop, check image queue list or using working buffer data to reflush 
		 */
		cancel_delayed_work_sync(&(reflush_work));
		queue_delayed_work(reflush_ctrl_workqueue, &(reflush_work), 0);
	}
	
#ifdef SUPPORT_DEC_TIMER
	memset(mgr->list.dec_stimer, 0, sizeof(struct timeval)*MAX_TOTAL_FRAME);
	memset(mgr->list.dec_etimer, 0, sizeof(struct timeval)*MAX_TOTAL_FRAME);
	memset(mgr->list.dec_ttimer, 0, sizeof(struct timeval)*MAX_TOTAL_FRAME);
	memset(mgr->list.sdec_timer, 0, sizeof(struct timeval)*MAX_TOTAL_FRAME);
	g_decode_sframe_cnt = 0;
	g_decode_tframe_cnt = 0;
	g_decode_eframe_cnt = 0;
	g_sw_decode_tframe_cnt = 0;
#endif

	spin_lock_irqsave(&mgr->list.frame_slock, flags);
	mgr->list.all_miss_wavedata_cnt = 0;
	mgr->list.all_miss_edma_cnt = 0;
	spin_unlock_irqrestore(&mgr->list.frame_slock, flags);

	flush_work(&mgr->frame_ctrl_work);
	if (all_miss_wavedata_cnt) {
		spin_lock_irqsave(&mgr->list.frame_slock, flags);
		mgr->list.all_decode_frame_cnt = 0;
		spin_unlock_irqrestore(&mgr->list.frame_slock, flags);

		disable_pipeline_mgr(mgr->disp);

		spin_lock_irqsave(&mgr->list.list_lock, flag);
		mgr->list.ee_proccessing = false;
		spin_unlock_irqrestore(&mgr->list.list_lock, flag);
	}

	release_all_soft_pipe(mgr);
	release_all_pipe(mgr);

	//dump_wavedata_buffer(&(mgr->wavedata_ring_buffer));
	__clear_wavedata_buffer(&(mgr->wavedata_ring_buffer));
	
	spin_lock_irqsave(&mgr->list.frame_slock, flags);
	mgr->list.all_fresh_frame_cnt = 0;
	spin_unlock_irqrestore(&mgr->list.frame_slock, flags);
	
	spin_lock_irqsave(&mgr->list.update_protect_slock, fflags);
	if (1 == mgr->list.update_state) {
		mgr->list.update_state = 0;
	}
	spin_unlock_irqrestore(&mgr->list.update_protect_slock, fflags);

#if 0
	char tmp_buf[50] = {0};
	char buf[256] = {0};
	
	msleep(20);
	for (frame_id = 0; frame_id < all_total_frames; frame_id++) {
		printk(KERN_ALERT "frame %d edma config: \n", frame_id);
		for (addr = 0x00; addr <= 0x44; addr += 4) {
			if (0 == (addr & 0xf)) {
				memset(tmp_buf, 0, sizeof(tmp_buf));
				memset(buf, 0, sizeof(buf));
				snprintf(tmp_buf, sizeof(tmp_buf), "0x%08lx: ", addr);
				strncat(buf, tmp_buf, sizeof(tmp_buf));
			}

			memset(tmp_buf, 0, sizeof(tmp_buf));
			snprintf(tmp_buf, sizeof(tmp_buf), "0x%08lx ", mgr->list.edma_reg_val[frame_id][addr]);
			strncat(buf, tmp_buf, sizeof(tmp_buf));
			if (0 == ((addr + 4) & 0xf)) {
				printk(KERN_ALERT "%s\n", buf);
				msleep(5);
			}
		}
		if (0 != ((addr + 4) & 0xf)) {
			printk(KERN_ALERT "%s\n", buf);
			msleep(5);
		}
	}
#endif
	//save_all_wavedata_buffer(mgr, all_total_frames);
}


/*
* This function MUST be called within decode IRQ and VSYNC IRQ
*/
static void decode_frame_control(struct pipeline_manager *mgr)
{
	struct pipe_info *pipe, *tmp_pipe;
	void *wvdata_paddr = NULL;
	unsigned long dec_frames = 0, total_frame = 0, fresh_frames = 0, all_dec_frame = 0;
	unsigned long flags = 0, flag = 0;
	unsigned long index_buf_paddr = 0;
	void *wavedata_paddr = NULL;
	u32 wavedata_vaddr = NULL;
	unsigned int bit_num = 0, step_pow = 0;
	unsigned int cur_wave_paddr = 0;
	unsigned int wave_buf_fail_cnt = 0;

	bool ntx_handwrite_solution = g_disp_manager->ntx_handwrite_solution;
	
	struct eink_buffer_manager *buf_mgr = NULL;
	buf_mgr = g_disp_manager->buffer_mgr;


	if (NULL == mgr) {
		__wrn("pipe mgr is null\n");
		return;
	}
	
	spin_lock_irqsave(&mgr->list.frame_slock, flags);
	mgr->list.all_decode_frame_cnt++;
	dec_frames = mgr->list.all_decode_frame_cnt;
	fresh_frames = mgr->list.all_fresh_frame_cnt;
	total_frame = mgr->list.all_total_frames;
	spin_unlock_irqrestore(&mgr->list.frame_slock, flags);

	/* all frame has been decoded completely */
	if (dec_frames >= total_frame) {
		//disable E-ink engine
		spin_lock_irqsave(&mgr->list.frame_slock, flags);
		all_dec_frame = mgr->list.all_decode_frame_cnt;
		//mgr->list.all_decode_frame_cnt = 0;
		spin_unlock_irqrestore(&mgr->list.frame_slock, flags);
		
		disable_pipeline_mgr(mgr->disp);
		
		spin_lock_irqsave(&mgr->list.list_lock, flag);
        if (mgr->list.index_addr.update_flag) {
            mgr->list.index_addr.update_flag = false;
            complete(&mgr->list.index_addr.comp);
        }
		mgr->list.ee_proccessing = false;
		spin_unlock_irqrestore(&mgr->list.list_lock, flag);
		
		__debug("frame(%d) has decode, total=%d, all_dec_frame=%d, all decode done\n", (dec_frames -1), total_frame, all_dec_frame);
		if (ntx_handwrite_solution == true) {
			if (buf_mgr->new_process_flag != 1)
				return;
		} else
			return;
	} else {
		__debug("frame %d has decode,fresh_frames=%d total=%d\n", (dec_frames -1), fresh_frames,total_frame);
	}
	
	if (ntx_handwrite_solution == true && buf_mgr->new_process_flag == 1) {
		if ( dec_frames == 1 ){
			queue_work_on(0, system_highpri_wq, &pipe_power_on_work.work);
			__debug("fresh preframe %d",dec_frames);
			write_edma_first(mgr);
		} else if ( dec_frames == 2 ) {
			__debug("fresh preframe %d",dec_frames);
			__debug("start delay=%d\n", disp_al_lcd_get_start_delay(mgr->disp, NULL));
			write_edma_second(mgr);
		}
	} else {
		/* pre-decode PRE_DECODE_NUM frames for EDMA */
		if (PRE_DECODE_NUM == dec_frames) {
			__debug("fresh preframe");
			write_edma_first(mgr);
			//queue_work_on(0, system_highpri_wq, &pipe_power_on_work.work);
            queue_delayed_work(power_ctrl_workqueue, &(pipe_power_on_work), 0);
			__debug("start delay=%d\n", disp_al_lcd_get_start_delay(mgr->disp, NULL));
			//it will wait for VSYNC
			write_edma_second(mgr);
		}
	}

	/* has other frame need to decode, go on...... */
	#if DECODE_TRIGER_BY_TCON
	wavedata_paddr = (void *)__request_buffer_for_decode(&mgr->wavedata_ring_buffer, &wavedata_vaddr);
	if (NULL == wavedata_paddr) {
	        //it's impossible for alloc buffer failure.
		__wrn("fail to alloc buffer for decode, are you kidding me?\n");
		dump_wavedata_buffer(&mgr->wavedata_ring_buffer);
		return;
	}
	#else
	wave_buf_fail_cnt = 0;
	do {
		wavedata_paddr = (void *)__request_buffer_for_decode(&mgr->wavedata_ring_buffer, &wavedata_vaddr);
		if (NULL == wavedata_paddr) {
			wave_buf_fail_cnt++;
			if (wave_buf_fail_cnt >= 200) {
				wave_buf_fail_cnt = 0;
				__debug("no wavedata buffer\n");
			}
			msleep(5);
		}
	} while(NULL == wavedata_paddr);
	#endif

	//reset_wavedata_buffer(wavedata_vaddr, mgr->panel_info);

	spin_lock_irqsave(&mgr->list.list_lock, flag);
	if (mgr->list.index_addr.update_flag) {
		mgr->list.index_addr.paddr = mgr->list.index_addr.new_paddr;
		mgr->list.index_addr.vaddr = mgr->list.index_addr.new_vaddr;
	}

	index_buf_paddr = mgr->list.index_addr.paddr;
	mgr->list.wavedata_addr.paddr = wavedata_paddr;
	mgr->list.wavedata_addr.vaddr = wavedata_vaddr;
	
	__debug("[%s %d] prepare decode",__func__, __LINE__);
	disp_al_eink_prepare_decode(mgr->disp, index_buf_paddr, wavedata_paddr, &mgr->panel_info);
	#if DECODE_TRIGER_BY_TCON
	if ((dec_frames <= PRE_DECODE_NUM) || (dec_frames < (fresh_frames + 1))) {
		//TCON has not ready yet, triger decode after configure
        //__debug("trigger decode");

		if (ntx_handwrite_solution == true && buf_mgr->new_process_flag == 1) {
			if (dec_frames < PRE_DECODE_NUM) { //do not decode PRE_DECODE_NUM + 1 frame, let lcd isr decode it 
				spin_unlock_irqrestore(&mgr->list.list_lock, flag);
				//__debug("sw >>>>>> IB->WD\n");
				mgr->decode_ctrl_func(0);
				spin_lock_irqsave(&mgr->list.list_lock, flag);
			} else {
				__debug("enable disp_proc");
				g_enable_disp_proc = 1;
			}
		} else {
			__debug("hw >>>>>> IB->WD\n");
			disp_al_eink_triger_decode(mgr->disp);
		}

		#ifdef SUPPORT_DEC_TIMER
		if (g_decode_sframe_cnt < MAX_TOTAL_FRAME) {
			do_gettimeofday(&mgr->list.dec_stimer[g_decode_sframe_cnt]);
			g_decode_sframe_cnt++;
		}
		#endif
	} else {
		__debug("enable disp_proc");
		g_enable_disp_proc = 1; 
	}
	#else
	disp_al_eink_triger_decode(mgr->disp);
	#endif


	if (mgr->list.index_addr.update_flag) {
		mgr->list.index_addr.update_flag = false;
		complete(&mgr->list.index_addr.comp);
	}
	spin_unlock_irqrestore(&mgr->list.list_lock, flag);

	return;
}

static void decode_frame_tasklet(unsigned long param)
//static void decode_frame_workqueue(struct work_struct *work)
{
	struct pipeline_manager *mgr = NULL;
	unsigned long flags = 0;
	
	bool ntx_handwrite_solution = g_disp_manager->ntx_handwrite_solution;
	struct eink_buffer_manager *buf_mgr = NULL;
	buf_mgr = g_disp_manager->buffer_mgr;

	mgr = get_pipeline_manager(0);
	if (NULL == mgr) {
		__wrn("cannot found pipeline mgr\n");
		return;
	}
#ifdef SUPPORT_DEC_TIMER
	if (g_decode_tframe_cnt < MAX_TOTAL_FRAME) {
		do_gettimeofday(&mgr->list.dec_ttimer[g_decode_tframe_cnt]);
		g_decode_tframe_cnt++;
		//do_gettimeofday(&mgr->list.dec_ttimer[g_decode_eframe_cnt]);
		//g_decode_eframe_cnt++;
	}
#endif

    __debug("g_decode_tframe_cnt:%d\n", g_decode_tframe_cnt);

    __debug("sw >>>>>> IB->WD \n");
	active_all_valid_soft_pipe(mgr);
    __debug("sw <<<<<< IB->WD \n");
	active_all_valid_pipe(mgr);

	//one frame has been decode, can get to display
	__queue_wavedata_buffer(&mgr->wavedata_ring_buffer);

#ifdef SUPPORT_DEC_TIMER
	if (g_sw_decode_tframe_cnt < MAX_TOTAL_FRAME) {
		do_gettimeofday(&mgr->list.sdec_timer[g_sw_decode_tframe_cnt]);
		g_sw_decode_tframe_cnt++;
	}
#endif

	decode_frame_control(mgr);

	if (ntx_handwrite_solution == true) {
		//due to single wavedata buffer, state need to set to ready
		if ( buf_mgr->new_process_flag == 1 ) {
			__queue_wavedata_buffer(&mgr->wavedata_ring_buffer);
		}
	}
}

void set_index_buffer(struct pipeline_manager *mgr, struct area_info *update_area) 
{

	unsigned long new_index_vaddr = mgr->list.index_addr.vaddr;
	unsigned int w = 0, h = 0, offset = 0;
	u16 *tmp_ib = NULL;
	unsigned int pitch = mgr->panel_info.width;
	unsigned int width = update_area->x_bottom - update_area->x_top + 1;
	unsigned int ib_width = width * 2;
	
	__debug("\n");
	__debug("p:%p %x\n", mgr->list.index_addr.vaddr, new_index_vaddr);
	
	for (h = update_area->y_top; h <= update_area->y_bottom; h++) {
		offset = h * pitch + update_area->x_top;
		tmp_ib = (u16 *)new_index_vaddr + offset;
		memset(tmp_ib, 0 , ALIGN(ib_width, 8));
	}
	
	disp_eink_cache_sync(mgr->list.index_addr.vaddr, (mgr->list.index_addr.vaddr + g_disp_manager->index_buffer.index_buf_size));
	flush_cache_all();
	outer_flush_all();
}

static bool fresh_frame_counter(struct pipeline_manager *mgr)
{
	struct pipe_info *pipe, *tmp_pipe;
	struct disp_device*  plcd = NULL;
	unsigned long flags = 0, flag = 0;
	unsigned int total_frame = 0, decode_cnt = 0, fresh_cnt = 0;
	bool used_list_empty_flag = false;

	spin_lock_irqsave(&mgr->list.list_lock, flag);
	list_for_each_entry_safe(pipe, tmp_pipe, &mgr->list.used_list, node) {		
		BUG_ON(NULL == pipe);
		if (false == pipe->active_flag) {
			continue;
		}
		
		pipe->fresh_frame_cnt++;
		
		//__debug("pipe=%d, total=%d, fresh_frame_cnt=%d\n", pipe->pipe_id, pipe->total_frames, pipe->fresh_frame_cnt);
		if (pipe->fresh_frame_cnt == pipe->total_frames) {
			if ((mgr->wait_frame_sync_flag == true) && 
				(pipe->update_order == mgr->wait_frame_sync_frame)) {
				mgr->wait_frame_sync_queue &= (~(1 << PIPE_IMG_HW_PIPE_QUEUE));
				if (mgr->wait_frame_sync_queue == 0)
					complete(&mgr->wait_frame_sync_comp);
			}
			
			//all frame for this pipe has completely fresh out, free this pipe
			 __release_pipe(mgr, pipe);
			list_move_tail(&pipe->node, &mgr->list.free_list);
			__debug("all frame has fresh out, free pipe(%d)\n", pipe->pipe_id);
		} else if (pipe->fresh_frame_cnt < pipe->total_frames) {
			//normal case
		} else {
			//invalid case: fresh_frame_cnt > total_frames
		}
	}

	if (list_empty(&mgr->list.used_list)) {
		used_list_empty_flag = true;
	}
	spin_unlock_irqrestore(&mgr->list.list_lock, flag);

	return used_list_empty_flag;
}


static bool fresh_frame_soft_pipe_counter(struct pipeline_manager *mgr)
{
	struct pipe_info *pipe, *tmp_pipe;
	struct disp_device*  plcd = NULL;
	unsigned long flags = 0, flag = 0;
	unsigned int total_frame = 0, decode_cnt = 0, fresh_cnt = 0;
	bool used_list_empty_flag = false;

	spin_lock_irqsave(&mgr->list.list_lock, flag);
	list_for_each_entry_safe(pipe, tmp_pipe, &mgr->list.soft_pipe_used_list, node) {		
		BUG_ON(NULL == pipe);
		if (false == pipe->active_flag) {
			continue;
		}

		pipe->fresh_frame_cnt++;
		
		//__debug("pipe=%d, total=%d, fresh_frame_cnt=%d\n", pipe->pipe_id, pipe->total_frames, pipe->fresh_frame_cnt);
		if (pipe->fresh_frame_cnt > pipe->total_frames) {
			if ((mgr->wait_frame_sync_flag == true) && 
				(pipe->update_order == mgr->wait_frame_sync_frame)) {
				mgr->wait_frame_sync_queue &= (~(1 << PIPE_IMG_SW_PIPE_QUEUE));
				if (mgr->wait_frame_sync_queue == 0)
					complete(&mgr->wait_frame_sync_comp);
			}
			//all frame for this pipe has completely fresh out, free this pipe
			 __release_soft_pipe(mgr, pipe);
			list_move_tail(&pipe->node, &mgr->list.soft_pipe_free_list);
			__debug("all frame has fresh out, free soft pipe(%d)\n", pipe->pipe_id);
		} else if (pipe->fresh_frame_cnt <= pipe->total_frames) {
			//normal case
		} /*else {
			 __release_soft_pipe(mgr, pipe);
			list_move_tail(&pipe->node, &mgr->list.soft_pipe_free_list);
			__debug("fresh_frame_cnt > total_frames , free soft pipe(%d)\n", pipe->pipe_id);
			//invalid case: fresh_frame_cnt > total_frames
		}*/
	}

	if (list_empty(&mgr->list.used_list)) {
		used_list_empty_flag = true;
	}
	spin_unlock_irqrestore(&mgr->list.list_lock, flag);

	return used_list_empty_flag;
}


#if 0
    static void fresh_frame_workqueue(struct work_struct *work)
#else
    static void fresh_frame_workqueue( void )
#endif
{
	struct pipeline_manager *mgr = NULL;	
	unsigned long flags = 0;
	unsigned int fresh_cnt = 0, decode_cnt = 0, total_frame = 0;
	bool used_list_empty_flag = false;
	u32 lcd_debug_reg = 0;

	mgr = get_pipeline_manager(0);
	if (NULL == mgr) {
		__wrn("cannot found pipeline mgr\n");
		return;
	}

	used_list_empty_flag = fresh_frame_soft_pipe_counter(mgr);

	used_list_empty_flag = fresh_frame_counter(mgr);

	spin_lock_irqsave(&mgr->list.frame_slock, flags);
	fresh_cnt = mgr->list.all_fresh_frame_cnt;
	spin_unlock_irqrestore(&mgr->list.frame_slock, flags);
	//__debug("total=%d, decode=%d, fresh=%d\n", total_frame, decode_cnt, fresh_cnt);
	
	//FIXME: for debug
	//read tcon status
	lcd_debug_reg = disp_al_tcon_get_debug_reg(0);
	if (lcd_debug_reg & 0x80000000) {
		printk("frame %d under flow, lcd_debug_reg=0x%08x\n", fresh_cnt, lcd_debug_reg);
	}
}

static void fresh_frame_task(unsigned long param)
{
    __debug(""); 
	struct pipeline_manager *mgr = param;
	int start_delay = 0;
	int cur_line = 0;
    unsigned int fresh_cnt = 0, decode_cnt = 0, total_frame = 0;
	unsigned long flags;
    int ret = 0;
    
    struct timeval stimer, etimer;
    
    bool ntx_handwrite_solution = g_disp_manager->ntx_handwrite_solution;
	
	struct eink_buffer_manager *buf_mgr = NULL;
	buf_mgr = g_disp_manager->buffer_mgr;

	if (NULL == mgr) {
		return;
	}
	
    if (ntx_handwrite_solution == true && buf_mgr->new_process_flag == 1) {
        start_delay = disp_al_lcd_get_start_delay(mgr->disp, NULL); 
        cur_line = disp_al_lcd_get_cur_line(mgr->disp, NULL);

        //do_gettimeofday(&stimer);
        while (cur_line < start_delay) {
            cur_line = disp_al_lcd_get_cur_line(mgr->disp, NULL);
            //do_gettimeofday(&etimer);
            if (get_delt_ms_timer(stimer, etimer) > 12) {
                pr_err("cur_line too late!\n");
                break;
            }
            asm("NOP");
        }
        
        ret = write_edma(mgr, true);
        if (ret) {
            pr_err("soft write edma failed ...%d\n", ret);
        }
    } else {
        spin_lock_irqsave(&mgr->list.frame_slock, flags);
        total_frame = mgr->list.all_total_frames;
        decode_cnt = mgr->list.all_decode_frame_cnt;
        fresh_cnt = mgr->list.all_fresh_frame_cnt;
        spin_unlock_irqrestore(&mgr->list.frame_slock, flags);
#if DECODE_TRIGER_BY_TCON
        //triger to decode next frame
        if ((decode_cnt < total_frame) && ((decode_cnt + 1) >= PRE_DECODE_NUM)) {
            disp_al_eink_triger_decode(mgr->disp);

#ifdef SUPPORT_DEC_TIMER
            if (g_decode_sframe_cnt < MAX_TOTAL_FRAME) {
                do_gettimeofday(&mgr->list.dec_stimer[g_decode_sframe_cnt]);
                g_decode_sframe_cnt++;
            }
#endif
        }
#endif
        ret = write_edma(mgr, false);
        if (ret) {
            pr_err("hard write edma failed ...%d\n", ret);
        }

#if 0
        schedule_work(&mgr->frame_ctrl_work);
#else
        fresh_frame_workqueue();
#endif

        __clean_used_wavedata_buffer(&mgr->wavedata_ring_buffer);
    }
}


/*
* in VSYNC IRQ context
*/
static int g_fresh_cnt = 0;
int fresh_frame_control(struct pipeline_manager *mgr)
{
	unsigned long flags = 0, fflags = 0, flag = 0;
	int ret =0;
	//unsigned int i = 0;
	//unsigned int index_buffer_valid = 0;
	//struct disp_device*  plcd = NULL;
	unsigned int fresh_cnt = 0, decode_cnt = 0, total_frame = 0;
	//int cur_line = 0;
	//static int start_delay = 0;
	//unsigned int addr = 0;
	struct pipe_info *pipe, *tmp_pipe;
    
	bool ntx_handwrite_solution = g_disp_manager->ntx_handwrite_solution;
	
	struct eink_buffer_manager *buf_mgr = NULL;
	buf_mgr = g_disp_manager->buffer_mgr;


	if ( g_enable_disp_proc == 0 ) {
		__debug("skip fresh due to frame not ready");
		return ret;
	}

	spin_lock_irqsave(&mgr->list.frame_slock, flags);
	total_frame = mgr->list.all_total_frames;
	decode_cnt = mgr->list.all_decode_frame_cnt;
	mgr->list.all_fresh_frame_cnt++;
	fresh_cnt = mgr->list.all_fresh_frame_cnt;

	if (0 == total_frame) {
		//invalid interrupt
		mgr->list.all_fresh_frame_cnt = 0;
		spin_unlock_irqrestore(&mgr->list.frame_slock, flags);
		return ret;
	}
	spin_unlock_irqrestore(&mgr->list.frame_slock, flags);
	
	if ((fresh_cnt == total_frame)) {
		spin_lock_irqsave(&mgr->list.update_protect_slock, fflags);

#if 0
    schedule_work(&mgr->frame_ctrl_work);
#else
    fresh_frame_workqueue();
#endif

		__clean_used_wavedata_buffer(&mgr->wavedata_ring_buffer);

		//Here we need to disable TCON immediately
		if (0 == mgr->list.update_state) {
            //__debug("start to turn off epd power\n");
            //printk(KERN_ALERT "start to turn off epd power\n");
			if ((g_disp_manager->last_update_mode & 0xff) == EINK_GLK16_MODE ||
				(g_disp_manager->last_update_mode & 0xff) == EINK_GCK16_MODE) {
				disp_al_tcon_disable_for_eink(mgr->disp, 1);
	                } else
				disp_al_tcon_disable_for_eink(mgr->disp, 0);
			mgr->list.update_state = 1;
			schedule_work(&mgr->power_down_work);
			g_frames = 0;
            g_enable_disp_proc = 0;
		}
		spin_unlock_irqrestore(&mgr->list.update_protect_slock, fflags);
	} else if (fresh_cnt < total_frame) {
		if (mgr->update_time_ctrl == true) {
			before_lcd_int_time = ktime_set( 0, g_disp_manager->lcd_int_wait_time );//8ms
			hrtimer_start( &before_lcd_int_hrtimer,  before_lcd_int_time, HRTIMER_MODE_REL );
			//hrtimer_start( &before_write_edma_hrtimer, before_write_edma_time, HRTIMER_MODE_REL );
		}
	
		#if DECODE_TRIGER_BY_TCON
		//triger to decode next frame
		__debug("decode_cnt:%d",decode_cnt);
		if ((decode_cnt < total_frame) && ((decode_cnt + 1) >= PRE_DECODE_NUM)) {
			if (ntx_handwrite_solution == true && buf_mgr->new_process_flag == 1) {
				__debug("sw >>>>>> IB->WD\n");
				mgr->decode_ctrl_func(0);
			} /*else {
				__debug("hw >>>>>> IB->WD\n");
				disp_al_eink_triger_decode(mgr->disp);
			}*/

			#ifdef SUPPORT_DEC_TIMER
			if (g_decode_sframe_cnt < MAX_TOTAL_FRAME) {
				do_gettimeofday(&mgr->list.dec_stimer[g_decode_sframe_cnt]);
				g_decode_sframe_cnt++;
			}
			#endif
		}
		#endif
		
		//we have pre-setting 2 frames for edma
		if ((fresh_cnt + 1) < total_frame) {
            __debug("fresh_cnt:%d total_frame:%d", fresh_cnt, total_frame );
			tasklet_schedule(&mgr->fresh_frame_task);
		}
		
		if (ntx_handwrite_solution == true && buf_mgr->new_process_flag == 1) {
			
#if 0
    schedule_work(&mgr->frame_ctrl_work);
#else
    fresh_frame_workqueue();
#endif

			__clean_used_wavedata_buffer(&mgr->wavedata_ring_buffer);
		}
	} else {
		//fresh too much frames
	}
		
	return ret;
}

enum hrtimer_restart before_lcd_int_callback( struct hrtimer *timer )
{ 
    struct pipeline_manager *mgr = NULL;
    mgr = get_pipeline_manager(0);
  
    //mutex_lock(&mgr->before_lcd_int_mlock);
    if (mgr->before_lcd_int_wait_flag) {
        __debug(">>> before lcd int complete\n");
        complete(&mgr->before_lcd_int_comp);
        mgr->before_lcd_int_wait_flag = false;
    }
    //mutex_unlock(&mgr->before_lcd_int_mlock);

    return HRTIMER_NORESTART;
}

enum hrtimer_restart before_int_write_edma_callback( struct hrtimer *timer )
{ 
    struct pipeline_manager *mgr = NULL;
    mgr = get_pipeline_manager(0);

    mgr->fresh_frame_control(mgr);

    return HRTIMER_NORESTART;
}

u32 wait_before_lcd_int_complete (struct pipeline_manager *mgr) {
    u32 ret = -1;
    int t = 1000;

    //mutex_lock(&mgr->before_lcd_int_mlock);
    init_completion(&mgr->before_lcd_int_comp);
    mgr->before_lcd_int_wait_flag = true;
    //mutex_unlock(&mgr->before_lcd_int_mlock);
    
    ret = wait_for_completion_timeout(&mgr->before_lcd_int_comp, msecs_to_jiffies(t));
	if (!ret) {
		__wrn("wait for before lcd int timeout, t=%d ms\n", t);
		ret = -1;
	} else {
		ret = 0;
	}
 
    return ret;
}

u32 wait_frame_sync_complete (struct pipeline_manager *mgr, u32 frame_ID) {
	u32 ret = -1;
	int t = 3000;
	
	//bool need_wait = true;
	int need_wait = 0;
	struct pipe_info* pipe, *tmp_pipe;
	unsigned long flag = 0;
    
    struct image_buffer_node *image_node = NULL, *tmp_image_node = NULL;
    struct eink_buffer_manager *buf_mgr = NULL;
	buf_mgr = g_disp_manager->buffer_mgr;
    
    mutex_lock(&buf_mgr->mlock);
    list_for_each_entry_safe(image_node, tmp_image_node, &buf_mgr->image_used_list, node) {
		if (frame_ID == image_node->update_order) {
            __debug("order %d exist at image_used_list\n", frame_ID);
            need_wait |= (1 << BUF_IMG_QUQUE);
            break;
		}
	}
	
	list_for_each_entry_safe(image_node, tmp_image_node, &buf_mgr->image_collision_list, node) {
        if (frame_ID == image_node->update_order) {
            __debug("order %d exist at image_collision_list\n", frame_ID);
            need_wait |= (1 << BUF_IMG_COLL_QUEUE);
            break;
        }
    }
    mutex_unlock(&buf_mgr->mlock);
    
	spin_lock_irqsave(&mgr->list.list_lock, flag);
	list_for_each_entry_safe(pipe, tmp_pipe, &mgr->list.used_list, node) {
		if (frame_ID == pipe->update_order) {
            __debug("order %d exist at used_list\n", frame_ID);
			need_wait |= (1 << PIPE_IMG_HW_PIPE_QUEUE);
			break;
		}
	}
	
	list_for_each_entry_safe(pipe, tmp_pipe, &mgr->list.soft_pipe_used_list, node) {
		if (frame_ID == pipe->update_order) {
            __debug("order %d exist at soft_pipe_used_list\n", frame_ID);
			need_wait |= (1 << PIPE_IMG_SW_PIPE_QUEUE);
			break;
		}
	}
	spin_unlock_irqrestore(&mgr->list.list_lock, flag);
	
	if (!need_wait) {
		__debug("order: %d wait frame sync complete\n", frame_ID);
		return 0;
	}
	
	mgr->wait_frame_sync_queue = need_wait;
	mgr->wait_frame_sync_frame = frame_ID;
	init_completion(&mgr->wait_frame_sync_comp);
	mgr->wait_frame_sync_flag = true;
	
	ret = wait_for_completion_timeout(&mgr->wait_frame_sync_comp, msecs_to_jiffies(t));
	if (!ret) {
		__wrn("wait for frame %d sync timeout, t=%d ms\n", frame_ID, t);
		ret = -1;
	} else
		ret = 0;
    
	return ret;
}

u32 update_frame_time_ctrl(struct pipeline_manager *mgr, bool enable) {
	mgr->update_time_ctrl = enable;
	return 0;
}

void neon_function_test(void)
{
	u8 src_randon_data[32] = {0};
	u8 dst[32] = {0};
	u32 len = 0, i = 0;
	u8 src_mask_data[32] = {0}, tmp_data = 0;
	u16 dst_mask_data[4] = {0};
	u16 src_index_data[32] = {0};
	u8 dst_index_data[32] = {0};
	u8 tmp_old_data = 0, tmp_new_data = 0;
	u8 waveform[8] = {0};
	u16 wvdata[4] = {0};
	u32 test_cnt = 0, error = 0;
	bool ret = false;

	for (test_cnt = 0; test_cnt < 100; test_cnt++) {
		kernel_neon_begin();
		
		memset(src_randon_data, 0, sizeof(src_randon_data));
		memset(dst, 0, sizeof(dst));
		//printk("memory copy test:\n");
		for (i = 0; i < ARRAY_SIZE(src_randon_data); i++) {
			get_random_bytes(&src_randon_data[i], sizeof(src_randon_data[0]));
			//printk("0x%x ", src_randon_data[i]);
		}
		//printk("\n");
		
		len = neon_memcpy(dst, src_randon_data, sizeof(src_randon_data));
		ret = neon_memcpy_check(dst, src_randon_data, 32);
		if (false == ret) {			
			printk("memory copy test fail\n");
			error = 1;
			goto err;
		} else {
			//printk("memory copy test ok\n");
		}

		//printk("mask combine test:\n");
		memset(src_mask_data, 0, sizeof(src_mask_data));
		memset(dst_mask_data, 0, sizeof(dst_mask_data));
		for (i = 0; i < ARRAY_SIZE(src_mask_data); i++) {
			get_random_bytes(&tmp_data, 1);
			src_mask_data[i] = (tmp_data%2)?0:0x3;
			//printk("0x%x ", src_mask_data[i]);
		}
		//printk("\n");
		neon_mask_combine(dst_mask_data, src_mask_data);
		
		ret = neon_mask_combine_check(dst_mask_data, src_mask_data, 32);
		if (false == ret) {
			printk("mask combine test fail\n");
			error = 2;
			goto err;
		} else {
			//printk("mask combine test ok\n");
		}

		//printk("index data reduce bit test:\n");
		memset(src_index_data, 0, sizeof(src_index_data));
		memset(dst_index_data, 0, sizeof(dst_index_data));
		for (i = 0; i < ARRAY_SIZE(src_index_data); i++) {
			get_random_bytes(&tmp_old_data, 1);
			get_random_bytes(&tmp_new_data, 1);
			src_index_data[i] = (tmp_old_data & 0x1f) << 5 | (tmp_new_data & 0x1f);
			//printk("0x%x ", src_index_data[i]);
		}
		//printk("\n");
		
		neon_index_bit_reduce(dst_index_data, src_index_data);
		ret = neon_index_bit_reduce_check(dst_index_data, src_index_data);
		if (false == ret) {
			//printk("index data reduce bit test fail\n");
			error = 3;
			goto err;
		} else {
			//printk("index data reduce bit test ok\n");
		}

		//printk("waveform lookup test:\n");
		memset(waveform, 0, sizeof(waveform));
		memset(wvdata, 0, sizeof(wvdata));
		for (i = 0; i < ARRAY_SIZE(waveform); i++) {
			get_random_bytes(&tmp_data, 1);
			tmp_data = tmp_data & 0x3;
			waveform[i] = (tmp_data > 2)?2:tmp_data;
			//printk("0x%x ", waveform[i]);
		}
		//printk("\n");
		
		neon_lookup_waveform(wvdata, dst_index_data, waveform);
		ret = neon_lookup_waveform_check(wvdata, dst_index_data, waveform);
		if (false == ret) {
			printk("waveform lookup test fail\n");
			error = 4;
			goto err;
		} else {
			//printk("waveform lookup test ok\n");
		}

		kernel_neon_end();

		printk("round %d neon test ok\n", test_cnt);
	}

	return 0;

err:
	kernel_neon_end();
}


int pipeline_manager_init(struct disp_eink_manager* eink_manager)
{
	int ret = 0;
	int i;
	struct pipeline_manager* mgr;
	struct pipe_info* pipe[PIPELINE_NUM];	
	struct pipe_info* soft_pipe[SOFT_PIPELINE_NUM];
	unsigned long flags = 0;

	mgr = (struct pipeline_manager*) disp_sys_malloc(sizeof(*mgr));
	if (!mgr) {
		__wrn("malloc eink pipeline manager memory fail!\n");
		ret = -ENOMEM;
		goto pipeline_mgr_err;
	}
	memset((void*)mgr, 0, sizeof(struct pipeline_manager));

	mgr->disp = eink_manager->disp;
	mgr->max_pipe_num = PIPELINE_NUM;
	memcpy(&mgr->panel_info, &eink_manager->panel_info, sizeof(eink_manager->panel_info));
	if ((0 == mgr->panel_info.width) || (0 == mgr->panel_info.height)) {
		__wrn("please init panel info first\n");
		ret = -EINVAL;
		goto pipeline_mgr_err;
	}

	spin_lock_init(&mgr->list.frame_slock);
	spin_lock_init(&mgr->list.update_protect_slock);
	//mutex_init(&mgr->list.list_lock);
	spin_lock_init(&mgr->list.list_lock);
	mutex_init(&mgr->power_mlock);
	mutex_init(&mgr->before_lcd_int_mlock);

	INIT_LIST_HEAD(&mgr->list.free_list);
	INIT_LIST_HEAD(&mgr->list.used_list);
	INIT_LIST_HEAD(&mgr->list.soft_pipe_free_list);
	INIT_LIST_HEAD(&mgr->list.soft_pipe_used_list);

	//mgr->hi_workqueue = alloc_workqueue("EINK_DECODE_WORK",
	//			WQ_HIGHPRI | WQ_CPU_INTENSIVE | WQ_MEM_RECLAIM | WQ_POWER_EFFICIENT, 1);
	
	//INIT_WORK(&mgr->decode_ctrl_work, decode_frame_workqueue);
	//tasklet_init(&mgr->decode_ctrl_work, decode_frame_tasklet, (unsigned long)mgr);
#if 0
	INIT_WORK(&mgr->frame_ctrl_work, fresh_frame_workqueue);
#else
	//INIT_WORK(&mgr->frame_ctrl_work, fresh_frame_workqueue);
#endif
	//INIT_WORK(&mgr->power_on_work, power_on_work);
	power_ctrl_workqueue = create_workqueue("power on/off ctrl");
	INIT_DELAYED_WORK(&(pipe_power_on_work), power_on_work);
	INIT_WORK(&mgr->power_down_work, power_down_work);
	tasklet_init(&mgr->fresh_frame_task, fresh_frame_task, (unsigned long)mgr);
    
	reflush_ctrl_workqueue = create_workqueue("EPD reflush ctrl");
	INIT_DELAYED_WORK(&(reflush_work), epd_reflush_work);

	mgr->list.update_state = 0;
	mgr->power_state = EINK_POWER_OFF;

	for (i = 0; i < PIPELINE_NUM; i++) {
		pipe[i] = (struct pipe_info *)disp_sys_malloc(sizeof(struct pipe_info));
		if (NULL == pipe[i]) {
			__wrn("malloc eink pipeline memory fail!\n");
			ret = -ENOMEM;
			goto pipeline_err;
		}
		memset((void*)pipe[i], 0, sizeof(struct pipe_info));
		pipe[i]->pipe_id= i;
		
		list_add_tail(&pipe[i]->node, &mgr->list.free_list);
	}


	for (i = 0; i < SOFT_PIPELINE_NUM; i++) {
		soft_pipe[i] = (struct pipe_info *)disp_sys_malloc(sizeof(struct pipe_info));
		if (NULL == soft_pipe[i]) {
			__wrn("malloc eink pipeline memory fail!\n");
			ret = -ENOMEM;
			goto soft_pipeline_err;
		}
		memset((void*)soft_pipe[i], 0, sizeof(struct pipe_info));
		soft_pipe[i]->pipe_id = i;
		list_add_tail(&soft_pipe[i]->node, &mgr->list.soft_pipe_free_list);
	}
	
	ret = __init_wavedata_buffer(&mgr->wavedata_ring_buffer, mgr->panel_info);
	if (0 != ret) {
		goto pipeline_err;
	}

	//save_all_wavedata_buffer(mgr, 40);

	mgr->enable_pipeline_mgr = enable_pipeline_mgr;
	mgr->disable_pipeline_mgr = disable_pipeline_mgr;
	mgr->query_free_pipe = query_free_pipe;
	mgr->query_used_pipe = query_used_pipe;
	mgr->request_pipe = request_pipe;
	mgr->release_pipe = release_pipe;
	mgr->image_update_start = image_update_start;
	mgr->image_update_end = image_update_end;
	mgr->init_pipe = init_pipe;
	//mgr->active_pipe = active_pipe;
	mgr->update_and_active_pipe = update_and_active_pipe;
	//mgr->active_all_valid_pipe = active_all_valid_pipe;
	//mgr->get_overlap_status = get_overlap_status;
	mgr->get_collision_relationship = get_collision_relationship;
	//mgr->get_collision_status = get_collision_status;
	//mgr->update_index_addr_sync = update_index_addr_sync;
	mgr->update_index_addr = update_index_addr;
	mgr->fresh_frame_control = fresh_frame_control;
	mgr->get_eink_engine_state = get_eink_engine_state;
	mgr->get_eink_power_state = get_eink_power_state;
	mgr->wait_for_eink_power_off = wait_for_eink_power_off;
	mgr->wait_for_eink_power_on = wait_for_eink_power_on;
	mgr->decode_ctrl_func = decode_frame_tasklet;

	
	mgr->query_used_soft_pipe = query_used_soft_pipe;
	mgr->query_free_soft_pipe = query_free_soft_pipe;

	mgr->request_soft_pipe = request_soft_pipe;
	mgr->release_soft_pipe = release_soft_pipe;
	mgr->init_soft_pipe = init_soft_pipe;
	mgr->update_and_active_soft_pipe = update_and_active_soft_pipe;
	mgr->soft_pipe_get_collision_relationship = soft_pipe_get_collision_relationship;
	mgr->wait_before_lcd_int_complete = wait_before_lcd_int_complete;
	mgr->wait_frame_sync_complete = wait_frame_sync_complete;
	
	/* control before_lcd_int and write_edma hrtimer on/off */
	mgr->update_time_ctrl = false;
	mgr->update_frame_time_ctrl = update_frame_time_ctrl;

	mgr->miss_wavedata_reflush = false;
	mgr->voltage_drop_reflush = false;

	eink_manager->pipeline_mgr = mgr;

	g_pipeline_mgr[eink_manager->disp] = mgr;

    g_disp_manager = eink_manager;
	//neon_function_test();

    hrtimer_init( &before_lcd_int_hrtimer, CLOCK_MONOTONIC, HRTIMER_MODE_REL );
    before_lcd_int_hrtimer.function = &before_lcd_int_callback;
    before_lcd_int_time = ktime_set( 0, 7000000 );//8ms
    
    hrtimer_init( &before_write_edma_hrtimer, CLOCK_MONOTONIC, HRTIMER_MODE_REL );
    before_write_edma_hrtimer.function = &before_int_write_edma_callback;
    before_write_edma_time = ktime_set( 0, g_write_edma_time );//10ms


	
	return 0;

pipeline_err:
	for (i = 0; i < PIPELINE_NUM; i++) {
		if (pipe[i])
			disp_sys_free(pipe[i]);
	}

soft_pipeline_err:
	for (i = 0; i < PIPELINE_NUM; i++) {
		if (soft_pipe[i])
			disp_sys_free(soft_pipe[i]);
	}



pipeline_mgr_err:
	if (mgr)
		disp_sys_free(mgr);

	return ret;
}

#endif
