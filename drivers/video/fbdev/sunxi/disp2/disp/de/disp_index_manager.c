/*
 * Copyright (C) 2015 Allwinnertech, z.q <zengqi@allwinnertech.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#include "disp_index_manager.h"

#include "include.h"

#define INDEX_MANAGER_NUM 1

static struct index_manager fmt_mgr[INDEX_MANAGER_NUM];

extern s32 get_delt_ms_timer(struct timeval start_timer, struct timeval end_timer);

struct index_manager *disp_get_index_manager(unsigned int id)
{
	return &fmt_mgr[id];
}

#if defined(__LINUX_PLAT__)
s32 disp_index_calc_finish_proc(int irq, void *parg)
#else
s32 disp_index_calc_finish_proc(void *parg)
#endif
{
	struct index_manager *mgr = (struct index_manager *)parg;

	if (NULL == mgr)
		return DISP_IRQ_RETURN;

	if (0 == disp_al_get_eink_wb_status(mgr->disp)) {
		mgr->index_calc_finish = 1;
		wake_up_interruptible(&(mgr->index_calc_queue));
	} else
		__wrn("covert err!\n");
	disp_al_clear_eink_wb_interrupt(mgr->disp);

	return DISP_IRQ_RETURN;
}

static s32 disp_index_calc_enable(unsigned int id)
{
	struct index_manager *mgr = &fmt_mgr[id];
	s32 ret = -1;
	static int first = 1;

	if (NULL == mgr) {
		__wrn("input param is null\n");
		return -1;
	}

	//it's always enable

	return 0;
}

static s32 disp_index_calc_disable(unsigned int id)
{
	struct index_manager *mgr = &fmt_mgr[id];
	s32 ret = -1;

	if (NULL == mgr) {
		__wrn("%s: input param is null\n", __func__);
		return -1;
	}

	//it's always enable

	return 0;
}

/*
start to calc index or update area
1. index will be stored in new_index_data_paddr buffer.
2. if current_image->window_calc_enable == TRUE, area will update to current_image->update_area
*/
static struct timeval t_start, t_end;
static s32 disp_index_calc_start(u32 id, unsigned long old_index_data_paddr,
						unsigned long new_index_data_paddr,
						struct eink_8bpp_image *last_image,
						struct eink_8bpp_image *current_image)
{
	struct index_manager *mgr = &fmt_mgr[id];
	s32 ret = -1, k = 0;
	u32 lay_total_num = 0;
	bool calc_area_flag = false;

	long timerout = (50 * HZ)/1000;		/*50ms*/

	if ((NULL == last_image) || (NULL == current_image) || (0 == old_index_data_paddr) || (0 == new_index_data_paddr) || (NULL == mgr)) {
		__wrn("input param is null\n");
		return -1;
	}

	do_gettimeofday(&t_start);
	mutex_lock(&(mgr->lock));
	disp_al_eink_start_calculate_index(0, old_index_data_paddr, new_index_data_paddr, last_image, current_image);

    __debug("hw >>>>>> Y8->IB \n");
	
#ifdef __UBOOT_PLAT__
	//TODO
#else
	timerout = wait_event_interruptible_timeout(mgr->index_calc_queue, (1 == mgr->index_calc_finish),timerout);
	mgr->index_calc_finish = 0;
	if (0 == timerout) {
		__wrn("calc index timeout, t=%d ms\n", (timerout * 1000)/100);
		ret = -1;
		mutex_unlock(&(mgr->lock));
		return ret;
	}
#endif

    __debug("hw <<<<<< Y8->IB \n");
	calc_area_flag = current_image->window_calc_enable;
	if (current_image->window_calc_enable) {
		disp_al_get_update_area(0, &current_image->update_area);
		if (((current_image->update_area.x_top + 1) == current_image->size.width) && 
			((current_image->update_area.y_top + 1) == current_image->size.height) && 
			(current_image->update_area.x_bottom == 0) &&
			(current_image->update_area.y_bottom == 0)) {
			__inf("area coordinate maybe not right, auto fix it\n");
			current_image->update_area.x_top = 0;
			current_image->update_area.y_top = 0;
			current_image->update_area.x_bottom = 0;
			current_image->update_area.y_bottom = 0;
			//current_image->update_area.x_bottom = (current_image->size.width - 1);
			//current_image->update_area.y_bottom = (current_image->size.height - 1);
		}
		current_image->window_calc_enable = 0;
	}

	mutex_unlock(&(mgr->lock));
	do_gettimeofday(&t_end);

	__debug("calc %s take %d ms\n",calc_area_flag?"area":"index", get_delt_ms_timer(t_start, t_end));
	return 0;
}

#ifdef __UBOOT_PLAT__
int disp_index_irq_handler(unsigned int id)
{
	__inf("%s: TODO\n", __func__);
	return 0;
}
#else
int disp_index_irq_handler(unsigned int id)
{
	struct index_manager *mgr = &fmt_mgr[id];

	if (NULL == mgr)
		return DISP_IRQ_RETURN;

	mgr->index_calc_finish = 1;
	wake_up_interruptible(&(mgr->index_calc_queue));
	
	return 0;
}
#endif

s32 disp_init_index_calc_manager(struct disp_bsp_init_para * para)
{
	s32 ret = -1;
	unsigned int i = 0;
	struct index_manager *mgr;

	for (i = 0; i < INDEX_MANAGER_NUM; i++) {
		mgr = &fmt_mgr[i];
		memset(mgr, 0, sizeof(struct index_manager));
		mutex_init(&(mgr->lock));

		mgr->disp = i;
		init_waitqueue_head(&(mgr->index_calc_queue));
		mgr->index_calc_finish = 0;
		mgr->irq_num = para->irq_no[DISP_MOD_EINK];
		mgr->enable = disp_index_calc_enable;
		mgr->disable = disp_index_calc_disable;
		mgr->start_calc_index = disp_index_calc_start;
		//mgr->get_update_area = disp_get_update_area;
		mgr->index_irq_hdl = disp_index_irq_handler;
		mgr->clk = para->mclk[DISP_MOD_EINK];

		/* ee_base has been set by disp_eink_manager, index modules use the same address */
		//disp_al_set_eink_base(i, para->reg_base[DISP_MOD_EINK]);
	}

	return ret;
}


