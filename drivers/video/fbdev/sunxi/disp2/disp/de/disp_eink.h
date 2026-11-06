/*
 * Copyright (C) 2015 Allwinnertech, z.q <zengqi@allwinnertech.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#ifndef __DISP_EINK__H__
#define __DISP_EINK__H__

#include "disp_private.h"
#include "include.h"
#include <video/sunxi_display2.h>
#include <linux/ion_sunxi.h>


//#define TEST_EDMA
#define SUPPORT_DEC_TIMER
#define SUPPORT_NEON_ALGORITHM	0
#define NEON_DATA_ALIGN			32

#define EINK_POWER_NUM 4
#define IMAGE_BUF_NUM 8
#define WAVE_DATA_BUF_NUM 25
#define NORMAL_UPDATE_WAVE_DATA_BUF_NUM (WAVE_DATA_BUF_NUM - 1)
#define EINK_PARTIAL_MODE 0x400
#define EDMA_MAX_ADDR	0x44
#define MAX_TOTAL_FRAME	256

typedef struct file ES_FILE;

struct st_argb {
	u8 blue;
	u8 green;
	u8 red;
	u8 alpha;
};

struct st_abgr {
	u8 red;
	u8 green;
	u8 blue;
	u8 alpha;
};

struct st_xyz {
    int x;
    int y;
    int z;
};

struct image_buffer_node {
	u32 buf_id;
	u32 update_order;
	u32 collision_flag;
	struct eink_8bpp_image *img;
	struct list_head node;
};

struct eink_buffer_manager {
	struct mutex mlock;
	//struct eink_8bpp_image image_slot[IMAGE_BUF_NUM];
	struct list_head image_free_list;
	struct list_head image_used_list;
	struct list_head image_collision_list;
	struct list_head image_process_list;    
	unsigned int max_image_num;
	unsigned int buf_size;
	unsigned int width;
	unsigned int height;
	unsigned int update_order;
	unsigned int global_clean_cnt;
	unsigned int new_process_flag;
	//u32 gamma_table[256];
	u32 *gamma_table;
    
	struct task_struct * 	   image_process_task;
	
	bool (*is_full)(struct eink_buffer_manager *buffer_mgr);
	bool (*is_empty)(struct eink_buffer_manager *buffer_mgr);
	bool (*is_collision_list_empty)(struct eink_buffer_manager *buffer_mgr);
	int (*add_image_to_collision_list)(struct eink_buffer_manager *buffer_mgr, struct image_buffer_node * img_node);
	int (*remove_image_from_collision_list)(struct eink_buffer_manager *buffer_mgr, struct image_buffer_node * img_node);
	bool (*update_collision_state)(struct eink_buffer_manager *buffer_mgr, u32 state);	
	
	bool (*update_soft_pipe_collision_state)(struct eink_buffer_manager *buffer_mgr, u32 state);	
	void (*cancel_collision_update)(struct eink_buffer_manager *buffer_mgr, u32 update_order, enum eink_update_mode mode, struct area_info update_area);
	struct image_buffer_node *(*get_current_image_from_collision_list)(struct eink_buffer_manager *buffer_mgr);
	struct image_buffer_node *(*get_current_image)(struct eink_buffer_manager *buffer_mgr);
	s32 (*queue_image)(struct eink_buffer_manager *buffer_mgr, struct disp_layer_config_inner *config,
				unsigned int layer_num, unsigned int mode,  struct area_info update_area, unsigned int g2d_flag, unsigned int cfa_use);
	s32 (*dequeue_image)(struct eink_buffer_manager *buffer_mgr, struct image_buffer_node *img_node);
	s32 (*set_global_clean_counter)(struct eink_buffer_manager *buffer_mgr, unsigned int cnt);
	s32 (*set_gamma_correction)(struct eink_buffer_manager *buffer_mgr, u32 *gamma_tbl);
	s32 (*set_cfa_cube_file)(struct eink_buffer_manager *buffer_mgr, struct cfa_cube_lut *cube_tbl);
	
	s32 (*set_bg_setting)(struct eink_buffer_manager *buffer_mgr, struct y8_area_info *background_setting);
	s32 (*set_bg_enable)(struct eink_buffer_manager *buffer_mgr, bool enable, int cfa_use);
    s32 (*reflush_working_buffer_data)(bool pmic_pg_status);

};

struct phy_virt_addr {
	bool update_flag;
	struct completion     comp;
	void *vaddr;
	unsigned int paddr;
	void *new_vaddr;
	unsigned int new_paddr;
};

enum wv_buffer_state {
	WV_INIT_STATE = 0,
	WV_DECODE_STATE = 1,
	WV_READY_STATE = 2,
	WV_DISPLAY_STATE = 3,
};

enum em_overlap_type {
	OVERLAP_NOT_COLLISION = 0,
	OVERLAP_WHOLE_COLLISION = 1,
	OVERLAP_PARTIAL_COLLISION = 2
};

struct wavedata_queue {
	unsigned int 				head;
	unsigned int  				tail;
	unsigned int				tmp_head;
    	unsigned int                		tmp_tail;	
	unsigned int			wavedata_buf_size;
	void                                   *wavedata_vaddr[WAVE_DATA_BUF_NUM];
	void                                   *wavedata_paddr[WAVE_DATA_BUF_NUM];
	enum wv_buffer_state                    buffer_state[WAVE_DATA_BUF_NUM];
	struct rect_size 			size;
	spinlock_t				slock;
};

struct pipeline_list {
	//spinlock_t 		list_lock;	//used in delete/add node from/to list
	bool				ee_enable;
	bool				ee_proccessing;
	bool				edma_enable;

	unsigned int		temperature;
	struct phy_virt_addr index_addr;
	struct phy_virt_addr wavedata_addr;

	unsigned long		all_total_frames;
	unsigned long		all_decode_frame_cnt;
	unsigned long		all_fresh_frame_cnt;
	spinlock_t			frame_slock;

	u8				update_state;
	spinlock_t			update_protect_slock;

	struct list_head   	free_list;
	struct list_head   	used_list;
	struct list_head   	soft_pipe_free_list;
	struct list_head   	soft_pipe_used_list;
	//struct mutex       	list_lock;
	spinlock_t              list_lock;

	//for debug	
	unsigned long		all_miss_wavedata_cnt;
	unsigned long		all_miss_edma_cnt;
	unsigned char		miss_wavedata_flag[MAX_TOTAL_FRAME];
	unsigned char		miss_soft_wavedata_flag[MAX_TOTAL_FRAME];
	unsigned char		miss_edma_flag[MAX_TOTAL_FRAME];
	unsigned int		edma_reg_val[MAX_TOTAL_FRAME][EDMA_MAX_ADDR + 1];
	struct timeval		tcon_stimer[MAX_TOTAL_FRAME];
	struct timeval		tcon_etimer[MAX_TOTAL_FRAME];
	
#ifdef SUPPORT_DEC_TIMER
	struct timeval		dec_stimer[MAX_TOTAL_FRAME];
	struct timeval		dec_ttimer[MAX_TOTAL_FRAME];
	struct timeval		sdec_timer[MAX_TOTAL_FRAME];
	struct timeval		dec_etimer[MAX_TOTAL_FRAME];
#endif
};

struct pipe_info {
	int          			pipe_id;	//0~15, -1 is invalid id
	unsigned int		update_order;
	enum eink_update_mode mode;
    	struct eink_8bpp_image *img;

	bool 				configure_flag;
	bool				active_flag;
	unsigned int		overlap_flag;
	unsigned int 		collision_flag;
	unsigned int        soft_pipe_cur_waveform_paddr;
	enum em_overlap_type	overlap_type;

	unsigned int          	waveform_paddr;
	unsigned int 		waveform_vaddr;
	struct area_info      	update_area;			//not overlap update area
	struct area_info	overlap_area;			//overlap update area, align in 32-bit
	struct area_info        org_update_area;
	char *			update_image_mask;	//must align in 32-bit, mask=1, not to display

	unsigned int          	total_frames;
	unsigned int          	dec_frame_cnt;
	unsigned int          	fresh_frame_cnt;

	//spinlock_t			slock;
	struct list_head      	node;	//when this node add to used list, it only modify by irq
	bool                   first_enable_pipe;
};

struct pipe_overlap_node {
	unsigned int          pipeline_no;
	struct area_info      area;
	enum eink_update_mode mode;
	unsigned int          frame_index;
	unsigned int          total_frames;
};

typedef enum {
	PIPE_MANAGER_FREE = 0,
	PIPE_MANAGER_BUSY,
	PIPE_MANAGER_MAX_STATUS
}PIPE_MANAGER_STATUS;

typedef enum {
	INDEX_BUFFER_FREE = 0,
	INDEX_BUFFER_CALC,
	INDEX_BUFFER_DECODING,
	INDEX_BUFFER_MAX_STATUS
}INDEX_BUFFER_STATUS;

enum e_power_state {
	EINK_POWER_OFF = 0,
	EINK_POWER_ON = 1,
	EINK_INVALID_STATE = 0xff
};

enum e_module_id {
	EINK_INDEX_MODULE = 0,
	EINK_EE_MODULE = 1,
	EINK_MAX_MODULE
};

enum wait_queue_type {
    BUF_IMG_QUQUE = 0,
    BUF_IMG_COLL_QUEUE = 1,
    PIPE_IMG_HW_PIPE_QUEUE = 2,
    PIPE_IMG_SW_PIPE_QUEUE = 3
};

struct frame_cnt {
	unsigned long		all_total_frames;
	unsigned long		all_decode_frame_cnt;
	unsigned long		all_fresh_frame_cnt;
};

struct pipeline_manager {
	unsigned int          	disp;
	unsigned int              max_pipe_num;		//default is 16
	struct eink_panel_info panel_info;
	struct wavedata_queue	wavedata_ring_buffer;
	struct workqueue_struct *hi_workqueue;
	//struct work_struct		decode_ctrl_work;
	struct tasklet_struct      decode_ctrl_work;
	struct work_struct      	frame_ctrl_work;
	struct tasklet_struct          fresh_frame_task;
	struct mutex			power_mlock;
	enum e_power_state	power_state;
	bool					power_off_wait_flag;
	struct completion		power_off_comp;
	bool					power_on_wait_flag;
	struct completion		power_on_comp;

	struct mutex			before_lcd_int_mlock;
	bool                    before_lcd_int_wait_flag;       
	struct completion       before_lcd_int_comp;
	
	bool			update_time_ctrl;
	
	u32			wait_frame_sync_frame;
	bool 			wait_frame_sync_flag;
	struct completion	wait_frame_sync_comp;
	u32			wait_frame_sync_queue;
	
	bool 		miss_wavedata_reflush;
	bool 		voltage_drop_reflush;

	struct work_struct 		power_on_work;
	struct work_struct 		power_down_work;
	struct pipeline_list  		list;
	u32 cur_mode;
	int (*enable_pipeline_mgr)(u32 disp);
	int (*disable_pipeline_mgr)(u32 disp);
	u32 (*query_free_pipe)(struct pipeline_manager *mgr);
	u32 (*query_used_pipe)(struct pipeline_manager *mgr);
	int(*request_pipe)(struct pipeline_manager *mgr);	//return pipe NO.
	void (*release_pipe)(struct pipeline_manager *mgr, int id);		//may auto release
	int (*image_update_start)(void);
	void (*image_update_end)(void);
	int (*init_pipe)(struct pipeline_manager *mgr, int id, struct pipe_info info);
	int (*update_and_active_pipe)(struct pipeline_manager *mgr, int id, unsigned long index_paddr, char *index_vaddr,struct pipe_info pinfo,  struct frame_cnt *frame);
	int (*active_all_valid_pipe)(struct pipeline_manager *mgr);
	int (*get_overlap_status)(struct pipeline_manager *mgr, int id);
	int (*get_collision_relationship)(struct pipeline_manager *mgr, struct pipe_info *pinfo);
	int (*get_collision_status)(struct pipeline_manager *mgr, int id);
	int (*update_index_addr_sync)(struct pipeline_manager *mgr, u32 id, unsigned long index_paddr, char *index_vaddr);
	void (*update_index_addr)(struct pipeline_manager *mgr, unsigned long index_paddr, char *index_vaddr);
	int (*fresh_frame_control)(struct pipeline_manager *mgr);	
	bool (*get_eink_engine_state)(struct pipeline_manager *mgr);
	int (*get_eink_power_state)(struct pipeline_manager *mgr, enum e_power_state *state);
	int (*wait_for_eink_power_off)(struct pipeline_manager *mgr);
	int (*wait_for_eink_power_on)(struct pipeline_manager *mgr);
	void (*decode_ctrl_func)(unsigned long param);
	int (*init_soft_pipe)(struct pipeline_manager *mgr, int id, struct pipe_info info);
	int (*update_and_active_soft_pipe)(struct pipeline_manager *mgr, int id, unsigned long index_paddr, char *index_vaddr,struct pipe_info pinfo,  struct frame_cnt *frame);
	int (*active_all_valid_soft_pipe)(struct pipeline_manager *mgr);
	int(*request_soft_pipe)(struct pipeline_manager *mgr);	//return pipe NO.
	void (*release_soft_pipe)(struct pipeline_manager *mgr, int id);		//may auto release
	u32 (*query_free_soft_pipe)(struct pipeline_manager *mgr);
	u32 (*query_used_soft_pipe)(struct pipeline_manager *mgr);
	int (*soft_pipe_get_collision_relationship)(struct pipeline_manager *mgr, struct pipe_info *pinfo);
	u32 (*wait_before_lcd_int_complete)(struct pipeline_manager *mgr);
	u32 (*update_frame_time_ctrl)(struct pipeline_manager *mgr, bool enable);
	u32 (*wait_frame_sync_complete)(struct pipeline_manager *mgr, u32 frame_ID);
};



struct eink_collision_node_info {
	enum eink_update_mode 	update_mode;
	struct area_info 			update_area;
	char 						*index_vaddr;
	struct list_head node;
};

s32 save_bin_file(u8 *buf,char* file_name,u32 lenght, loff_t pos);
s32 save_gray_image_as_bmp_24bit(u8 *buf,char* file_name,u32 scn_w, u32 scn_h);
s32 save_gray_image_as_bmp_8bit(u8 *buf,char* file_name,u32 scn_w, u32 scn_h);
bool rectangle_is_zero(struct area_info update_area);
void rectangle_print_coordinate(char *name, struct area_info area);

void align_area(struct area_info area, struct area_info *area_align, unsigned int align);
bool is_overlap(struct area_info a_area, struct area_info b_area);
int ring_buffer_manager_init(struct disp_eink_manager* eink_manager);
int pipeline_manager_init(struct disp_eink_manager* eink_manager);

void *disp_eink_malloc(u32 num_bytes, void *phy_addr);
void  disp_eink_free(void *virt_addr, void* phy_addr, u32 num_bytes);
void disp_eink_cache_sync(const void *start_addr, const void *end_addr);

int disp_init_eink(struct disp_bsp_init_para * para);
extern int disp_get_eink_resolution(unsigned int disp, unsigned int *xres, unsigned int *yres);
int eink_display_one_frame(struct disp_eink_manager* manager);
extern int eink_irq_query_index(void);
extern s32 get_delt_ms_timer(struct timeval start_timer, struct timeval end_timer);
extern s32 get_delt_us_timer(struct timeval start_timer, struct timeval end_timer);


#endif

