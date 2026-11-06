/*
 * Copyright (C) 2015 Allwinnertech, z.q <zengqi@allwinnertech.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */
#include "disp_eink.h"
#include <linux/prefetch.h>
#include <asm/neon.h>
#include <asm/cacheflush.h>
#include <linux/memory.h>
#include <linux/string.h>
#include <linux/sunxi-sid.h>
#include <linux/freezer.h>
#include "libeink.h"

#include "../../../../../../../arch/arm/mach-sunxi/ntx_hwconfig.h"
extern volatile NTX_HWCONFIG *gptHWCFG;


#ifdef SUPPORT_EINK
#include <linux/sched.h>

#if defined(CONFIG_TPS65185_VCOM) || defined(CONFIG_TPS65185_VCOM_MODULE) || defined(CONFIG_SY7636_VCOM) || \
    defined(CONFIG_JD9930_VCOM)
#include <linux/ebc.h>
extern int tps65185_vcom_set(int vcom_mv);
extern int tps65185_vcom_get(int *vcom);
//extern int sy7636_power_on(void);

#if defined(CONFIG_JD9930_VCOM)
extern int jd9930_vcom_set(int vcom_mv);
extern int jd9930_vcom_get(int *vcom);
#endif

#endif

//#define LOAD_VCOM_FORM_FILE    /* load vcom voltage form file */

#define MAX_EINK_ENGINE 1
#define START_LOCK_FAIL_MAX_CNT	200
#define REQUST_PIPE_FAIL_MAX_CNT	200
#define INIT_PIPE_FAIL_MAX_CNT		20
#define LCD_INTERRUPT_WAIT_DEFAULT	7000000

extern void *disp_eink_malloc(u32 num_bytes, void *phy_addr);
extern void  disp_eink_free(void *virt_addr, void *phy_addr, u32 num_bytes);
extern void disp_eink_cache_sync(const void *start_addr, const void *end_addr);
extern struct index_manager *disp_get_index_manager(unsigned int id);
extern struct format_manager *disp_get_format_manager(unsigned int id);

/* load libeink ko used */
extern char *get_waveform_paddr(void);
extern int regal_waveform_init(void);
extern int regal_waveform_deinit(void);

struct disp_eink_manager *eink_manager;
static u32 image_order = 0;

long glVCOM_uV;

static struct hrtimer before_write_edma_hrtimer;
static ktime_t before_write_edma_time;
static int g_write_edma_time = 9000000;

static struct __eink_libeink *eink_libeink_array;

size_t get_edma_hrtimer(struct device *dev, struct device_attribute *attr, char *buf) 
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
}

enum hrtimer_restart before_write_edma_callback( struct hrtimer *timer )
{ 
	struct disp_eink_manager *eink_manager = NULL;
	struct pipeline_manager *pmgr = NULL;
    
	eink_manager = disp_get_eink_manager(0);
	if (eink_manager == NULL)
		return HRTIMER_NORESTART;
	
	pmgr = eink_manager->pipeline_mgr;
	if (NULL == pmgr) {
		//__wrn("pipeline manager is not initial\n");
		return HRTIMER_NORESTART;
	}

	if (pmgr->fresh_frame_control)
		pmgr->fresh_frame_control(pmgr);
	else
		__wrn("fresh_frame_control is null\n");
 
	return HRTIMER_NORESTART;
}

//BMP文件：文件头(地址0x0000~0x000D，共14字节)
#pragma pack(1)
typedef struct
{  
	u8  bfType[2];			         //表明位图文件的类型，必须为BM
	u32  bfSize;					   //表明bmp 文件的大小，以字节为单位
	u16 bfReserved1;		                //属于保留字，必须为本0
	u16 bfReserved2;		                //也是保留字，必须为本0
	u32  bfOffBits;			                //位图阵列的起始位置，以字节为单位
}BMP_FILE_HEADER;

#define BMP_FILE_HEADER_SIZE			(sizeof(BMP_FILE_HEADER))

//BMP文件：信息头(地址0x000E~0x0035，共40字节)
typedef struct {
	u32  biSize;			                 //指出本数据结构所需要的字节数
	u32  biWidth;		                        //以象素为单位，给出BMP图象的宽度
	u32  biHeight;		                        //以象素为单位，给出BMP图象的高度
	u16 biPlanes;		                        //输出设备的位平面数，必须置为1
	u16 biBitCount;				    //给出每个象素的位数
	u32  biCompress;				    //给出位图的压缩类型
	u32  biSizeImage;			           //给出图象字节数的多少
	u32  biXPelsPerMeter;	                  //图像的水平分辨率
	u32  biYPelsPerMeter;	                  //图象的垂直分辨率
	u32  biClrUsed;				     //调色板中图象实际使用的颜色素数, 为0 ，表示默认大小等于(2 ^ biBitCount)  Bytes
	u32  biClrImportant;		            //给出重要颜色的索引值
}BMP_INFO_HEADER;
#pragma pack()

#define BMP_INFO_HEADER_SIZE			(sizeof(BMP_INFO_HEADER))
#define BMP_MIN_SIZE					(BMP_FILE_HEADER_SIZE + BMP_INFO_HEADER_SIZE)

//BMP文件：彩色表
typedef struct {
	u8 r;				        	//red
	u8 g;				 	//green
	u8 b;				 	//blue
	u8 reserved;	              	//alpha透明度，一般不用
}ST_ARGB;

typedef struct {
	u32  width;		                   //以象素为单位，给出BMP图象的宽度
	u32  height;		                  //以象素为单位，给出BMP图象的高度
	u16  bit_count;			//给出每个象素的位数
	u32  image_size;			 //给出图象字节数的多少
	u32  color_tbl_size;		//调色板中图象实际使用的颜色素数, 为0 ，表示默认大小等于(2 ^ biBitCount)  Bytes
}BMP_INFO;

#pragma pack(1)
typedef struct {
	u8 blue;
	u8 green;
	u8 red;
}ST_RGB;
#pragma pack()

#define BMP_COLOR_SIZE                          (sizeof(ST_ARGB) * 256)
#define BMP_IMAGE_DATA_OFFSET              (BMP_FILE_HEADER_SIZE + BMP_INFO_HEADER_SIZE + BMP_COLOR_SIZE)

void dump_pixels(char * title, void *pixels, int x_offset, int y_offset, int x_length, int y_length, int stride, int byteperpixel);

static  int convert_8bit_buf_to_24bit_bmp(ST_RGB *dest_buf, u8 *src_buf, u32 scn_w, u32 scn_h)
{
	int ret = -1;
	u32 w = 0, h = 0;
	ST_RGB *pst_dest_buf = NULL;
	u8 *p_src_buf = NULL;
	
	if ((NULL == dest_buf) || (NULL == src_buf)) {
		__wrn("%s: input param is null\n", __func__);
		return -1;
	}

	p_src_buf = src_buf;
	h = scn_h;
	while(h > 0) {
		pst_dest_buf = (ST_RGB *)dest_buf + (h - 1) * scn_w;
/*
		for (w = 0; w < scn_w; w++) {
			pst_dest_buf->red = *p_src_buf;
			pst_dest_buf->green = *p_src_buf;
			pst_dest_buf->blue = *p_src_buf;
			
			p_src_buf++;
			pst_dest_buf++;
		}
*/
		for (w = 0; w < scn_w; w++) {
			pst_dest_buf->red = *p_src_buf;		//Damon modify RGBA to RGB
			p_src_buf++;
			pst_dest_buf->green = *p_src_buf;
			p_src_buf++;
			pst_dest_buf->blue = *p_src_buf;
			p_src_buf++;		//bypass alpha

			p_src_buf++;
			pst_dest_buf++;
		}

		h--;
	}

	return 0;
}


s32 save_gray_image_as_bmp_24bit(u8 *buf,char* file_name,u32 scn_w, u32 scn_h)
{
	int ret = -1;
	ES_FILE *fd = NULL;
	mm_segment_t old_fs;
	loff_t pos = 0;
	BMP_FILE_HEADER st_file_header;
	BMP_INFO_HEADER st_info_header;
	char *src_buf = buf;
	char * path = file_name;
	BMP_INFO dest_info;
	u32 width = 0, height = 0;
	u32 bit_count = 0;
	ST_RGB *dest_buf = NULL;
	
	if ((NULL == path) || (NULL == src_buf)) {
		__wrn("%s: input param is null\n", __func__);
		return -1;
	}

	fd = filp_open(path,O_RDWR | O_CREAT, 0644);
	if (IS_ERR(fd)) {
		__wrn("%s: create bmp file(%s) error\n", __func__, path);
		return -2;
	}

	bit_count = 8;
	memset(&dest_info, 0, sizeof(BMP_INFO));
	dest_info.width = scn_w;
	dest_info.height = scn_h;
	dest_info.bit_count = 24;
	dest_info.color_tbl_size = 0;
	dest_info.image_size = (dest_info.width * dest_info.height * dest_info.bit_count)/8;

	st_file_header.bfType[0] = 'B';
	st_file_header.bfType[1] = 'M';
	st_file_header.bfOffBits = 0x36;
	st_file_header.bfSize = st_file_header.bfOffBits + dest_info.image_size;
	st_file_header.bfReserved1 = 0;
	st_file_header.bfReserved2 = 0;

	st_info_header.biSize = sizeof(BMP_INFO_HEADER);
	st_info_header.biWidth = dest_info.width;
	st_info_header.biHeight = dest_info.height;
	st_info_header.biPlanes = 1;
	st_info_header.biBitCount = dest_info.bit_count;
	st_info_header.biCompress = 0;
	st_info_header.biSizeImage = dest_info.image_size;
	st_info_header.biXPelsPerMeter = 0;
	st_info_header.biYPelsPerMeter = 0;
	st_info_header.biClrUsed = 0;
	st_info_header.biClrImportant = 0;

//	dest_buf = (ST_RGB *)disp_sys_malloc(dest_info.image_size);
	dest_buf = (ST_RGB *)vmalloc(dest_info.image_size);
	if (NULL == dest_buf) {
		__wrn("%s: alloc memory fail, size=%d\n", __func__, dest_info.image_size);
		ret = -4;
		goto close_file;
	}
	
	ret = convert_8bit_buf_to_24bit_bmp(dest_buf, src_buf, scn_w, scn_h);
	if (0 != ret) {
		__wrn("%s: convert 8-bit buffer to 24-bit bmp buffer fail, ret = %d\n", __func__, ret);
		ret = -5;
		goto free_mem;
	}
	
	old_fs = get_fs();
	set_fs(KERNEL_DS);

 	ret= vfs_write(fd, (u8 *)(&st_file_header), sizeof(BMP_FILE_HEADER), &pos);
	__debug("save file header, len=%d, pos=%d, ret=%d\n", sizeof(BMP_FILE_HEADER), pos, ret);

	ret= vfs_write(fd, (u8 *)(&st_info_header), sizeof(BMP_INFO_HEADER), &pos);
	__debug("save file header, len=%d, pos=%d, ret=%d\n", sizeof(BMP_INFO_HEADER), pos, ret);

	ret= vfs_write(fd, (u8 *)(dest_buf), dest_info.image_size, &pos);
	__debug("save file header, len=%d, pos=%d, ret=%d\n", dest_info.image_size, pos, ret);
 
	set_fs(old_fs);

	ret = 0;
	
free_mem:
	if (NULL != dest_buf) {
//		disp_sys_free(dest_buf);
		vfree(dest_buf);
		dest_buf = NULL;
	}

close_file:
	if (!IS_ERR(fd)) {
		filp_close(fd, NULL);
	}

	return ret;
}
EXPORT_SYMBOL(save_gray_image_as_bmp_24bit);

s32 save_gray_image_as_bmp_8bit(u8 *buf,char* file_name,u32 scn_w, u32 scn_h)
{
	int ret = -1;
	ES_FILE *fd = NULL;
	mm_segment_t old_fs;
	loff_t pos = 0;
	BMP_FILE_HEADER st_file_header;
	BMP_INFO_HEADER st_info_header;
	char *src_buf = buf;
	char * path = file_name;
	BMP_INFO dest_info;
	u32 width = 0, height = 0;
	u32 bit_count = 0;
	ST_RGB *dest_buf = NULL;
	ST_ARGB color_table[256];
	u32 i  = 0;
	
	if ((NULL == path) || (NULL == src_buf)) {
		__wrn("%s: input param is null\n", __func__);
		return -1;
	}

	fd = filp_open(path,O_RDWR | O_CREAT, 0644);
	if (IS_ERR(fd)) {
		__wrn("%s: create bmp file(%s) error\n", __func__, path);
		return -2;
	}

	bit_count = 8;
	memset(&dest_info, 0, sizeof(BMP_INFO));
	dest_info.width = scn_w;
	dest_info.height = scn_h;
	dest_info.bit_count = 8;
	dest_info.color_tbl_size = 0;
	dest_info.image_size = (dest_info.width * dest_info.height * dest_info.bit_count)/8;

	st_file_header.bfType[0] = 'B';
	st_file_header.bfType[1] = 'M';
	st_file_header.bfOffBits = BMP_IMAGE_DATA_OFFSET;
	st_file_header.bfSize = st_file_header.bfOffBits + dest_info.image_size;
	st_file_header.bfReserved1 = 0;
	st_file_header.bfReserved2 = 0;

	st_info_header.biSize = sizeof(BMP_INFO_HEADER);
	st_info_header.biWidth = dest_info.width;
	st_info_header.biHeight = dest_info.height;
	st_info_header.biPlanes = 1;
	st_info_header.biBitCount = dest_info.bit_count;
	st_info_header.biCompress = 0;
	st_info_header.biSizeImage = dest_info.image_size;
	st_info_header.biXPelsPerMeter = 0;
	st_info_header.biYPelsPerMeter = 0;
	st_info_header.biClrUsed = 0;
	st_info_header.biClrImportant = 0;

	for (i = 0; i < 256; i++) {
		color_table[i].reserved = 0;
		color_table[i].r = i;
		color_table[i].g = i;
		color_table[i].b = i;
	}

	dest_buf = (ST_RGB *)src_buf;
	
	old_fs = get_fs();
	set_fs(KERNEL_DS);

 	ret= vfs_write(fd, (u8 *)(&st_file_header), BMP_FILE_HEADER_SIZE, &pos);
	__debug("save file header, len=%d, pos=%d, ret=%d\n", BMP_FILE_HEADER_SIZE, pos, ret);

	ret= vfs_write(fd, (u8 *)(&st_info_header), BMP_INFO_HEADER_SIZE, &pos);
	__debug("save file header, len=%d, pos=%d, ret=%d\n", BMP_INFO_HEADER_SIZE, pos, ret);

	ret= vfs_write(fd, (u8 *)(color_table), BMP_COLOR_SIZE, &pos);
	__debug("save file header, len=%d, pos=%d, ret=%d\n", BMP_COLOR_SIZE, pos, ret);

	ret= vfs_write(fd, (u8 *)(dest_buf), dest_info.image_size, &pos);
	__debug("save file header, len=%d, pos=%d, ret=%d\n", dest_info.image_size, pos, ret);
 
	set_fs(old_fs);

	ret = 0;

close_file:
	if (!IS_ERR(fd)) {
		filp_close(fd, NULL);
	}

	return ret;
}
EXPORT_SYMBOL(save_gray_image_as_bmp_8bit);


s32 save_bin_file(u8 *buf,char* file_name,u32 lenght, loff_t pos)
{
	ES_FILE *fp = NULL;
	mm_segment_t old_fs;
	ssize_t ret = 0;

	if ((NULL == buf) || (NULL == file_name)) {
		__wrn("buf or file_name is null\n");
		return -1;
	}

	fp = filp_open(file_name,O_RDWR | O_CREAT, 0644);
	if(IS_ERR(fp)) {
		 __wrn("fail to open file(%s), ret=%d\n",file_name, (u32)fp);
		return -2;
	}
	
	old_fs = get_fs();
	set_fs(KERNEL_DS);

 	ret= vfs_write(fp, buf, lenght, &pos);
	__debug("save %s done, len=%d, pos=%d, ret=%d\n",file_name, lenght, pos, ret);
 
	set_fs(old_fs);
	filp_close(fp, NULL);

	return ret;
}	
EXPORT_SYMBOL(save_bin_file);


s32 eink_get_working_buffer_data(struct disp_eink_manager *manager)
{
    s32 ret = -1;
	char file_name[256] = {0};
    char *working_buffer_vaddr = manager->work_buffer.workbuffer_vaddr;

	if (NULL == working_buffer_vaddr) {
		__wrn("input param is null\n");
		return ret;
	}
	
	memset(file_name, 0, sizeof(file_name));
	snprintf(file_name, sizeof(file_name), "/mnt/flash/workingbuffer.bmp");	
	ret = save_gray_image_as_bmp_8bit((u8 *)working_buffer_vaddr, file_name, manager->panel_info.width, manager->panel_info.height);
    
    return ret;
}

static int atoi(char* buf) {
	int nIntegerRet = 0; 
	int nIntegerSign = 1;
	if(buf == NULL) {
		printk(KERN_ERR "buf == NULL \n");
		return -1;
	}

	while(*buf == ' ') {
			buf++;
	}

	if(*buf == '-') {
		nIntegerSign = -1;
	}
	if(*buf == '-' || *buf == '+') {
		buf++;
	}

	while(*buf >= '0' && *buf <= '9') {
		nIntegerRet = nIntegerRet*10 + *buf - '0';
		buf++;
	}
	nIntegerRet = nIntegerRet*nIntegerSign;
	return nIntegerRet;
}

static int atoi_float(char* buf) 
{
	int num = 0;
	int i = 0;
	int point_pos = -1;
	int pow = 0;

	while(buf[i] != '\0') { 
		
		if( buf[i] >= '0' && buf[i] <= '9')	{
			if((point_pos < 0) || ((point_pos >= 0) && ((point_pos + 4) > i))) {
				num = num * 10 + (buf[i] - '0');
			} else {
				//drop the left char
				break;
			}
		}
		
		if(buf[i]=='.') {
			point_pos = i;
		}
		i++;
	}

	//auto *1000
	if ((point_pos >= 0) && ((point_pos + 4) > i)) {
		pow = (point_pos + 4 - i);
		while(pow > 0) {
			num = num * 10;
			pow--;
		}
	}
	
	if( buf[0] == '-' )
		num = num * (-1);

	__debug("input str=%s, num=%d\n", buf, num);
	
	return num;
}


void print_eink_register(unsigned int start_addr, unsigned int end_addr)
{
	unsigned int start_addr_align = ALIGN(start_addr, 4);
	unsigned int end_addr_align = ALIGN(end_addr, 4);
	unsigned int addr = 0;
	unsigned char tmp_buf[50] = {0};
	unsigned char buf[256] = {0};

	printk(KERN_ALERT "print reg: 0x%08x ~ 0x%08x", start_addr_align, end_addr_align);
	for (addr = start_addr_align; addr <= end_addr_align; addr += 4) {
		if (0 == (addr & 0xf)) {
			memset(tmp_buf, 0, sizeof(tmp_buf));
			memset(buf, 0, sizeof(buf));
			snprintf(tmp_buf, sizeof(tmp_buf), "0x%08lx: ", addr);
			strncat(buf, tmp_buf, sizeof(tmp_buf));
		}

		memset(tmp_buf, 0, sizeof(tmp_buf));
		snprintf(tmp_buf, sizeof(tmp_buf), "0x%08lx ", readl(addr | 0xf0000000));
		strncat(buf, tmp_buf, sizeof(tmp_buf));
		if (0 == ((addr + 4) & 0xf)) {
			printk(KERN_ALERT "%s\n", buf);
			msleep(5);
		}
	}
}
EXPORT_SYMBOL(print_eink_register);

bool rectangle_is_zero(struct area_info update_area)
{
	if ((update_area.x_bottom== 0) && (update_area.x_top == 0) && (update_area.y_bottom== 0) && (update_area.y_top == 0))
		return true;
	else
		return false;
}
EXPORT_SYMBOL(rectangle_is_zero);

bool is_whole_screen_update(struct eink_panel_info panel_info, struct area_info update_area)
{
	bool ret = false;

	ret = (0 == update_area.x_top) && 
		(0 == update_area.y_top) &&
		(panel_info.width <= (update_area.x_bottom + 1)) &&
		(panel_info.height <= (update_area.y_bottom + 1));
	
	return ret;
}

void rectangle_print_coordinate(char *name, struct area_info area)
{
	__debug("area %s: (%d, %d)~(%d,%d)\n", name, area.x_top, area.y_top, area.x_bottom, area.y_bottom);
}
EXPORT_SYMBOL(rectangle_print_coordinate);


struct disp_eink_manager *disp_get_eink_manager(unsigned int disp)
{
	return &eink_manager[disp];
}

unsigned int get_temperature(struct disp_eink_manager *manager)
{
	unsigned int temp = 28;

	if (manager) {
		temp = manager->get_temperature(manager);
	}else {
		__debug("eink manager is null\n");
	}

	return temp;
}

int __eink_clk_init(struct disp_eink_manager *manager)
{
	return 0;
}

int __eink_clk_disable(struct disp_eink_manager *manager)
{
	int ret = 0;

	if (manager->eink_clk)
		clk_disable(manager->eink_clk);

	if (manager->edma_clk)
		clk_disable(manager->edma_clk);

	return ret;
}

int __eink_clk_enable(struct disp_eink_manager *manager)
{
	int ret = 0;

	if (manager->eink_clk) {
		ret = clk_prepare_enable(manager->eink_clk);
	}

	if (manager->edma_clk) {
		ret = clk_prepare_enable(manager->edma_clk);
	}

	return ret;
}

static int __eink_get_sys_config(u32 disp, struct eink_panel_info *panel_info)
{
	int  ret;

	struct disp_device *lcd = disp_device_find(disp, DISP_OUTPUT_TYPE_LCD);
	struct disp_panel_para info;
	if (NULL == panel_info) {
		__wrn("input param is null\n");
		return -1;
	}

	memset(&info, 0, sizeof(info));
	ret = lcd->get_panel_info(lcd, &info);
	if (0 == ret) {
		memcpy(panel_info, &info.eink_timing, sizeof(*panel_info));
	}

	return ret;
}


static void __print_panel_info(u32 disp, struct eink_panel_info info)
{
	printk(KERN_ALERT "print %d panel info\n", disp);
	printk(KERN_ALERT "width=%d, height=%d, fresh_hz=%d\n", 
		info.width, info.height, info.fresh_hz);
	printk(KERN_ALERT "sdck=%d, bit_num=%d, data_len=%d\n", 
		info.sdck, info.bit_num, info.data_len);	
	printk(KERN_ALERT "lsl=%d, lbl=%d, ldl=%d, lel=%d, gdck_sta=%d, lgonl=%d\n", 
		info.lsl, info.lbl, info.ldl, info.lel, info.gdck_sta, info.lgonl);
	printk(KERN_ALERT "fsl=%d, fbl=%d, fdl=%d, fel=%d\n", 
		info.fsl, info.fbl, info.fdl, info.fel);
}

static unsigned int __get_lcd_int_wait_time(u32 disp)
{
	int value = 0;
	char primary_key[20];
	int ret;
	
	sprintf(primary_key, "lcd%d", disp);

	ret = disp_sys_script_get_item(primary_key, "hw_lcd_int_wait", &value, 1);
	if(ret != 1) {
		value = LCD_INTERRUPT_WAIT_DEFAULT;        
	}
	
	printk("lcd interrupt wait time = %d\n", value);	
	__debug("lcd interrupt wait time = %d\n", value);
	
	return (unsigned int)value;
}

static void __eink_get_waveform_path(u32 disp, char *path)
{
	int  value = 1;
	char primary_key[20];
	int  ret;

	sprintf(primary_key, "lcd%d", disp);

	ret = disp_sys_script_get_item(primary_key, "eink_path", (int*)path, 2);
	if(ret != 2) {
		memcpy(path, DEFAULT_WAVEFORM_PATH, strnlen(DEFAULT_WAVEFORM_PATH, MAX_PATH_LEN));
		__wrn("get eink path fail, set default path=%s\n", path);
	}
	printk("waveform path=%s\n", path);	
	__debug("waveform path=%s\n", path);
}

static void __eink_get_vcom_path(u32 disp, char *path)
{
	int  value = 1;
	char primary_key[20];
	int  ret;

	sprintf(primary_key, "lcd%d", disp);

	ret = disp_sys_script_get_item(primary_key, "vcom_path", (int*)path, 2);
	if(ret != 2) {
		memcpy(path, DEFAULT_VCOM_FILE_PATH, strnlen(DEFAULT_VCOM_FILE_PATH, MAX_PATH_LEN));
		__wrn("get vcom path fail, set default path=%s\n", path);
	}
	printk("vcom path=%s\n", path);
	__debug("vcom path=%s\n", path);
}

static int __get_vcom_from_file(char *path, int *vcom)
{
	struct file *fp = NULL;
	char vcombuf[64] = {0};
	__s32 file_len = 0;
	__s32 read_len = 0;
	mm_segment_t fs;
	loff_t pos;
	int nVcomVoltage = 0;
	int ret = 0;

	if ((NULL == vcom) || (NULL == path)) {
		__wrn("input param is null\n");
		return -1;
	}
	
	fp = filp_open(path, O_RDONLY, 0);
	if(IS_ERR(fp)) {
		__wrn("VCOM: fail to open %s\n", path);
		return -2;
	}
	
	//get vcom voltage
	fs = get_fs();
	set_fs(KERNEL_DS);
	pos = 0;
	file_len = fp->f_path.dentry->d_inode->i_size;

	__debug("file-len=%d\n", file_len);

	read_len = vfs_read(fp, (char *)vcombuf, file_len, &pos);
	if(read_len != file_len) {
		__wrn("read file(%s) error(read=%d byte, file=%d byte)\n", path, read_len, file_len);
		ret = -3;
		goto out;
	}

	nVcomVoltage = atoi_float(vcombuf);
	*vcom = nVcomVoltage;
	__debug("get nVcomVoltage = %d \n", nVcomVoltage);

	ret = 0;

out:
	if(fp) {
		filp_close(fp, NULL);
		set_fs(fs);
	}
	return ret;
}

static int eink_set_vcom_voltage(int vcom)
{
	int ret = -1;
#if defined(CONFIG_TPS65185_VCOM) || defined(CONFIG_TPS65185_VCOM_MODULE) || defined(CONFIG_SY7636_VCOM) || \
	defined(CONFIG_JD9930_VCOM)
	ret = tps65185_vcom_set(vcom);		//unit is mv
	if (0 != ret) {
#if defined(CONFIG_JD9930_VCOM)
		ret = jd9930_vcom_set(vcom);
		if (0 != ret) {
			__wrn("%s: set vcom fail, ret=%d\n", __func__, ret);
		}
#else
		__wrn("%s: set vcom fail, ret=%d\n", __func__, ret);
#endif
	}
#else
	//other method
#endif

	return ret;
}

ssize_t get_waveform_vcom(struct device *dev, struct device_attribute *attr, char *buf)
{
	int ret, vcom;
#if defined(CONFIG_TPS65185_VCOM) || defined(CONFIG_TPS65185_VCOM_MODULE) || defined(CONFIG_SY7636_VCOM) || \
	defined(CONFIG_JD9930_VCOM)
	ret = tps65185_vcom_get(&vcom);
#if defined(CONFIG_JD9930_VCOM)
	if (0 != ret) {
		ret = jd9930_vcom_get(&vcom);
		if (0 != ret) {
			__wrn("%s: get vcom fail, ret=%d\n", __func__, ret);
		}
	}
#endif
#endif
	
	return sprintf(buf, "%hdmV\n", vcom);
}

ssize_t set_waveform_vcom(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	int err;
	int vcom;

	err = kstrtoint(buf, 10, &vcom);
	if (err) {
		pr_warn("Invalid size\n");
		return err;
	}

	if (0!=vcom) {
		eink_set_vcom_voltage(vcom);
	}
	
	return count;
}

static int __get_vcom_config_value(struct disp_eink_manager *manager,char *path)
{
	int vcom = 0;
	int ret  = -1;
	
	unsigned short wVCOM_10mV = 0 ;
	short sVCOM_10mV;
	
#ifdef LOAD_VCOM_FORM_FILE   // read vcom voltage from file
	ret = __get_vcom_from_file(path, &vcom);
	manager->vcom_init_flag = true;
	if (0 != ret) {
		__wrn("fail to get vcom from file, use default value\n");
		vcom = DEFAULT_VCOM_VOLTAGE;
	}
	
	eink_set_vcom_voltage(vcom);
	printk("%s: vcom value is %d mV\n", __func__, vcom);
	
#else  // read vcom voltage from hw_config
	manager->vcom_init_flag = true;
	wVCOM_10mV = gptHWCFG->m_val.bVCOM_10mV_HiByte<<8|gptHWCFG->m_val.bVCOM_10mV_LoByte;
	sVCOM_10mV = (short) wVCOM_10mV;
	glVCOM_uV = (long)sVCOM_10mV*10000;
	
	if(0!=glVCOM_uV) {
		printk("%s:VCOM from HWCONFIG 0x%x=>%hd 10mV,%ld uV\n",
			__FUNCTION__,wVCOM_10mV,sVCOM_10mV,glVCOM_uV);
		
		vcom = (int)glVCOM_uV/1000;
	
		eink_set_vcom_voltage(vcom);
		return 0;
	}
#endif

	return ret;
}
static int eink_get_vcom_voltage(int disp)
{
	struct disp_eink_manager* manager;
	manager = disp_get_eink_manager(disp);
	if (NULL == manager) {
		return 0;
	}

	return manager->vcom_voltage;
}

unsigned int g_decode_eframe_cnt = 0;

static int __eink_interrupt_proc(int irq, void *parg)
{
	struct disp_eink_manager* manager = NULL;
	struct index_manager *idx_mgr = NULL;
	struct pipeline_manager *pmgr = NULL;
	unsigned int disp;
	int irq_type = 0;
	int ret = -1;
	unsigned long flags = 0;
    __debug("eink interrupt trigger %d name:%s\n", current->pid, current->comm);

	manager = (struct disp_eink_manager*)parg;
	if(NULL == manager)
		return DISP_IRQ_RETURN;

	idx_mgr = manager->idx_mgr;
	pmgr = manager->pipeline_mgr;
	if((NULL == idx_mgr) || (NULL == pmgr))
		return DISP_IRQ_RETURN;

	disp = manager->disp;
	irq_type = disp_al_eink_irq_query(manager->disp);

	switch (irq_type) {
	case 1:	//index calculate finish irq
        __debug("index calculate finish irq\n");
		if (idx_mgr->index_irq_hdl)
			idx_mgr->index_irq_hdl(idx_mgr->disp);
		break;
	case 0:	//decode one frame finish irq
	
        __debug("decode one frame finish irq\n");
		#ifdef SUPPORT_DEC_TIMER
		if (g_decode_eframe_cnt < MAX_TOTAL_FRAME) {
			do_gettimeofday(&pmgr->list.dec_etimer[g_decode_eframe_cnt]);
			g_decode_eframe_cnt++;
		}
		#endif

		//queue_work(pmgr->hi_workqueue, &pmgr->decode_ctrl_work);
		//tasklet_schedule(&pmgr->decode_ctrl_work);

        __debug("hw <<<<<< IB->WD \n");
		if (pmgr->decode_ctrl_func)
			pmgr->decode_ctrl_func(0);
		else
			__wrn("decode_ctrl_func is null\n");
		
		break;
	default:
		__wrn("unkown irq type(%d)\n", irq_type);
		break;
	}

out:
	return DISP_IRQ_RETURN;
}

int eink_display_one_frame(struct disp_eink_manager *manager)
{
	struct pipeline_manager *pmgr = NULL;
    struct eink_buffer_manager *buf_mgr = NULL;
	int ret = -1;
	
	pmgr = manager->pipeline_mgr;
	if (NULL == pmgr) {
		//__wrn("pipeline manager is not initial\n");
		return -1;
	}
	
	buf_mgr = manager->buffer_mgr;
	if (NULL == buf_mgr) {
		//__wrn("buffer manager is not initial\n");
		return -1;
	}

#if 1
       if (pmgr->update_time_ctrl == true && buf_mgr->width < 1920 && buf_mgr->height < 1440) {
               hrtimer_start( &before_write_edma_hrtimer, before_write_edma_time, HRTIMER_MODE_REL );
               return 0;
       }
       else {
               if (pmgr->fresh_frame_control)
                       ret = pmgr->fresh_frame_control(pmgr);
               else
                       __wrn("fresh_frame_control is null\n");
               
       }
       return ret;
#else
	hrtimer_start( &before_write_edma_hrtimer, before_write_edma_time, HRTIMER_MODE_REL );
	return 0;
#endif
}

static int copy_area_buffer(unsigned char *dest_index_vaddr, unsigned char *src_index_vaddr, 
			unsigned int pitch, struct area_info copy_area, unsigned char unit_size)
{
	unsigned char *tmp_src_index_vaddr = NULL;
	unsigned char *tmp_dest_index_vaddr = NULL;
	unsigned int h = 0, w = 0;
	unsigned int size = 0;
	
	if ((NULL == src_index_vaddr) || (NULL == dest_index_vaddr)) {
		__wrn("input param is null\n");
		return -1;
	}

	size = (copy_area.x_bottom - copy_area.x_top + 1) * unit_size;
	for (h = copy_area.y_top; h <= copy_area.y_bottom; h++) {
		tmp_src_index_vaddr = src_index_vaddr + (h * pitch + copy_area.x_top) * unit_size;
		tmp_dest_index_vaddr = dest_index_vaddr + (h * pitch + copy_area.x_top) * unit_size;
		memcpy(tmp_dest_index_vaddr, tmp_src_index_vaddr, size);
	}

	return 0;
}

static void save_update_mask_buffer(u32 order, u8 *buf, u32 len)
{
	char file_name[256] = {0};

	if (NULL == buf) {
		__wrn("input param is null\n");
		return;
	}

	memset(file_name, 0, sizeof(file_name));
	snprintf(file_name, sizeof(file_name), "/tmp/mask_buf%d.bin", order);
	save_bin_file(buf, file_name, len, 0);
}

static void trace_last_buffer(u32 order, struct disp_eink_manager *manager,struct pipe_info *pinfo)
{
	char file_name[256] = {0};
	unsigned int index_buf_len = 0, work_buf_size = 0;
	unsigned char new_index_id = manager->index_buffer.cur_index;
	unsigned char old_index_id = 0;
	unsigned long old_index_paddr = 0;
	unsigned long new_index_paddr = 0;
	unsigned long old_index_vaddr = 0;
	unsigned long new_index_vaddr = 0;	
	unsigned int working_buffer_paddr = manager->work_buffer.workbuffer_paddr;
	char *working_buffer_vaddr = manager->work_buffer.workbuffer_vaddr;
	char *update_buffer_vaddr = pinfo->img->vaddr;

	old_index_id = (new_index_id)?0:1;
	
	old_index_paddr = manager->index_buffer.index_paddr[old_index_id];
	new_index_paddr = manager->index_buffer.index_paddr[new_index_id];
	old_index_vaddr = manager->index_buffer.index_vaddr[old_index_id];
	new_index_vaddr = manager->index_buffer.index_vaddr[new_index_id];

	work_buf_size = manager->panel_info.width * manager->panel_info.height;
	if (manager->panel_info.bit_num > EINK_BIT_4) {
		index_buf_len = manager->panel_info.width * manager->panel_info.height * 2;
	} else {
		index_buf_len = manager->panel_info.width * manager->panel_info.height;
	}

    __debug("save last index buffer");
	memset(file_name, 0, sizeof(file_name));
	snprintf(file_name, sizeof(file_name), "/data/local/tmp/last_index%d.bin", order);
	save_bin_file(old_index_vaddr, file_name, index_buf_len, 0);
	
	memset(file_name, 0, sizeof(file_name));
	snprintf(file_name, sizeof(file_name), "/tmp/work_buffer%d.bin", order);
	//save_gray_image_as_bmp_24bit(working_buffer_vaddr, file_name, manager->panel_info.width, manager->panel_info.height);
	//save_gray_image_as_bmp_8bit(working_buffer_vaddr, file_name, manager->panel_info.width, manager->panel_info.height);
	
	save_bin_file(working_buffer_vaddr,file_name,work_buf_size, 0);

	memset(file_name, 0, sizeof(file_name));
	snprintf(file_name, sizeof(file_name), "/tmp/update_buffer%d.bin", order);
	//save_gray_image_as_bmp_24bit(working_buffer_vaddr, file_name, manager->panel_info.width, manager->panel_info.height);
	//save_gray_image_as_bmp_8bit(working_buffer_vaddr, file_name, manager->panel_info.width, manager->panel_info.height);
	
	save_bin_file(update_buffer_vaddr,file_name,work_buf_size, 0);
}


static void trace_new_buffer(u32 order, struct disp_eink_manager *manager)
{
	char file_name[256] = {0};
	unsigned int index_buf_len = 0, work_buf_size = 0;
	unsigned char new_index_id = manager->index_buffer.cur_index;
	unsigned char old_index_id = 0;
	unsigned long old_index_paddr = 0;
	unsigned long new_index_paddr = 0;
	unsigned long old_index_vaddr = 0;
	unsigned long new_index_vaddr = 0;	
	unsigned int working_buffer_paddr = manager->work_buffer.workbuffer_paddr;
	char *working_buffer_vaddr = manager->work_buffer.workbuffer_vaddr;

	old_index_id = (new_index_id)?0:1;
	
	old_index_paddr = manager->index_buffer.index_paddr[old_index_id];
	new_index_paddr = manager->index_buffer.index_paddr[new_index_id];
	old_index_vaddr = manager->index_buffer.index_vaddr[old_index_id];
	new_index_vaddr = manager->index_buffer.index_vaddr[new_index_id];

	work_buf_size = manager->panel_info.width * manager->panel_info.height;
	if (manager->panel_info.bit_num > EINK_BIT_4) {
		index_buf_len = manager->panel_info.width * manager->panel_info.height * 2;
	} else {
		index_buf_len = manager->panel_info.width * manager->panel_info.height;
	}
	
    __debug("save new index buffer");
	memset(file_name, 0, sizeof(file_name));
	snprintf(file_name, sizeof(file_name), "/data/local/tmp/new_index%d.bin", order);
	save_bin_file(new_index_vaddr, file_name, index_buf_len, 0);
	
	memset(file_name, 0, sizeof(file_name));
	snprintf(file_name, sizeof(file_name), "/tmp/work_buffer_after%d.bin", order);
	//save_gray_image_as_bmp_24bit(working_buffer_vaddr, file_name, manager->panel_info.width, manager->panel_info.height);
	//save_gray_image_as_bmp_8bit(working_buffer_vaddr, file_name, manager->panel_info.width, manager->panel_info.height);
	
	save_bin_file(working_buffer_vaddr,file_name,work_buf_size, 0);
}

static void save_waveform(u32 order, u8 *buf, u32 frames)
{
	char file_name[256] = {0};
	u32 len = 0;

	if (NULL == buf) {
		__wrn("input param is null\n");
		return;
	}

	len = frames * 256;
	memset(file_name, 0, sizeof(file_name));
	snprintf(file_name, sizeof(file_name), "/tmp/waveform%d.bin", order);
	save_bin_file(buf, file_name, len, 0);
}

#define I_TOP_WAVEFORM_INDEX		31 //31
#define I_FULL_WAVEFORM_INDEX	29
enum CARDINAL {
	NORTH = 0,
	SOUTH = 1,
	EAST = 2,
	WEST = 3,
	MAX_CARDINAL_NUM
};

#define ITOP_DETECT_VERSION	3
#define IFULL_DETECT_VERSION	ITOP_DETECT_VERSION
#define SIT					1

static inline bool is_cardinal_neighbors_match_itop_v1(u16 *cardinal_cur_data, u16 *cardinal_next_data)
{
	bool ret = false;
	return ret;
}

static inline bool is_cardinal_neighbors_match_itop_v2(u16 *cardinal_cur_data, u16 *cardinal_next_data)
{
	bool ret = false;
	return ret;
}

static inline bool is_cardinal_neighbors_match_itop_v3(u16 *cardinal_cur_data, u16 *cardinal_next_data)
{
	bool ret = false;
	u16 north_cur_data = *(cardinal_cur_data + NORTH);
	u16 south_cur_data = *(cardinal_cur_data + SOUTH);
	u16 east_cur_data = *(cardinal_cur_data + EAST);
	u16 west_cur_data = *(cardinal_cur_data + WEST);
	
	u16 north_next_data = *(cardinal_next_data + NORTH);
	u16 south_next_data = *(cardinal_next_data + SOUTH);
	u16 east_next_data = *(cardinal_next_data + EAST);
	u16 west_next_data = *(cardinal_next_data + WEST);

	ret = ((15 == north_next_data) && (15 == south_next_data) && (15 == east_next_data) && (15 == west_next_data)) && 
		((15 != north_cur_data) || (15 != south_cur_data) || (15 != east_cur_data) || (15 != west_cur_data));

	return ret;
}

static inline bool is_cardinal_neighbors_match_itop_v4(u16 *cardinal_cur_data, u16 *cardinal_next_data)
{
	bool ret = false;
	return ret;
}

static inline bool is_cardinal_neighbors_match_itop(u16 *cardinal_cur_data, u16 *cardinal_next_data)
{
	bool ret = false;
	
#if (ITOP_DETECT_VERSION == 1)
	ret = is_cardinal_neighbors_match_itop_v1(cardinal_cur_data, cardinal_next_data);
#elif (ITOP_DETECT_VERSION == 2)
	ret = is_cardinal_neighbors_match_itop_v2(cardinal_cur_data, cardinal_next_data);
#elif (ITOP_DETECT_VERSION == 3)
	ret = is_cardinal_neighbors_match_itop_v3(cardinal_cur_data, cardinal_next_data);
#elif (ITOP_DETECT_VERSION == 4)
	ret = is_cardinal_neighbors_match_itop_v4(cardinal_cur_data, cardinal_next_data);
#else
#error "ITOP_DETECT_VERSION is not right"
#endif
	
	return ret;
}

static inline bool is_cardinal_neighbors_match_ifull_v1(u16 *cardinal_cur_data, u16 *cardinal_next_data)
{
	bool ret = false;
	return ret;
}

static inline bool is_cardinal_neighbors_match_ifull_v2(u16 *cardinal_cur_data, u16 *cardinal_next_data)
{
	bool ret = false;
	return ret;
}

static inline bool is_cardinal_neighbors_match_ifull_v3(u16 *cardinal_cur_data, u16 *cardinal_next_data)
{
	bool ret = false;
	u16 cur_data = 0, next_data = 0;
	u8 i = 0, cnt = 0;

	for (i = 0; i < MAX_CARDINAL_NUM; i++) {
		cur_data = *(cardinal_cur_data + i);
		next_data = *(cardinal_next_data + i);
		if ((15 != cur_data) || (15 != next_data)) {
			cnt++;
			if (cnt >= SIT) {
				ret = true;
				break;
			}
		}
	}
	return ret;
}

static inline bool is_cardinal_neighbors_match_ifull_v4(u16 *cardinal_cur_data, u16 *cardinal_next_data)
{
	bool ret = false;
	return ret;
}


static inline bool is_cardinal_neighbors_match_ifull(u16 *cardinal_cur_data, u16 *cardinal_next_data)
{
	bool ret = false;
	
#if (IFULL_DETECT_VERSION == 1)
	ret = is_cardinal_neighbors_match_ifull_v1(cardinal_cur_data, cardinal_next_data);
#elif (IFULL_DETECT_VERSION == 2)
	ret = is_cardinal_neighbors_match_ifull_v2(cardinal_cur_data, cardinal_next_data);
#elif (IFULL_DETECT_VERSION == 3)
	ret = is_cardinal_neighbors_match_ifull_v3(cardinal_cur_data, cardinal_next_data);
#elif (IFULL_DETECT_VERSION == 4)
	ret = is_cardinal_neighbors_match_ifull_v4(cardinal_cur_data, cardinal_next_data);
#else
#error "IFULL_DETECT_VERSION is not right"
#endif
	
	return ret;
}

static inline bool is_need_special_waveform(u16 *cardinal_index_data, u16 *spec_index)
{
	u16 cardinal_cur_data[MAX_CARDINAL_NUM] = {0}, cardinal_next_data[MAX_CARDINAL_NUM] = {0};
	register u16 east_index_data = 0, west_index_data = 0, north_index_data = 0, south_index_data = 0;
	bool ret = false;

	east_index_data = *(cardinal_index_data + EAST);
	cardinal_cur_data[EAST] = (east_index_data & 0x1f) >> 1;
	cardinal_next_data[EAST] =  ((east_index_data & 0x3e0) >> 6);

	west_index_data = *(cardinal_index_data + WEST);
	cardinal_cur_data[WEST] = (west_index_data & 0x1f) >> 1;
	cardinal_next_data[WEST] = ((west_index_data & 0x3e0) >> 6);

	north_index_data = *(cardinal_index_data + NORTH);
	cardinal_cur_data[NORTH] = (north_index_data & 0x1f) >> 1;
	cardinal_next_data[NORTH] = ((north_index_data & 0x3e0) >> 6);

	south_index_data = *(cardinal_index_data + SOUTH);
	cardinal_cur_data[SOUTH] = (south_index_data & 0x1f) >> 1;
	cardinal_next_data[SOUTH] = ((south_index_data & 0x3e0) >> 6);

	if (is_cardinal_neighbors_match_itop(cardinal_cur_data, cardinal_next_data)) {
		//use iTop waveform		
		*spec_index = I_TOP_WAVEFORM_INDEX;	//update index buffer
		ret = true;
	} else if (is_cardinal_neighbors_match_ifull(cardinal_cur_data, cardinal_next_data)) {
		//use iFull waveform		
		*spec_index = I_FULL_WAVEFORM_INDEX;	//update index buffer
		ret = true;
	} else {
		//G30->G30 normal waveform, do nothing
	}

	return ret;
}

/*
This function MUST call after cacl index by HW, and it will take less time to process
*/
static int fix_index_for_special_waveform(struct disp_eink_manager *manager, struct pipe_info *pinfo)
{
	struct index_manager *idx_mgr = manager->idx_mgr;
	unsigned char new_index_id = manager->index_buffer.cur_index;
	unsigned char old_index_id = 0;
	unsigned long new_index_vaddr = 0;
	char *working_buffer_vaddr = manager->work_buffer.workbuffer_vaddr;
	char *update_buffer_vaddr = pinfo->img->vaddr;
	char *tmp_wb = NULL, *tmp_ub = NULL;
	u16 *tmp_mid_ib = NULL;
	u16 *tmp_north_ib = NULL;
	u16 *tmp_south_ib = NULL;
	u16 cur_data = 0, next_data = 0;
	u16 cardinal_cur_data[MAX_CARDINAL_NUM] = {0}, cardinal_next_data[MAX_CARDINAL_NUM] = {0};
	u16 cardinal_index_data[MAX_CARDINAL_NUM] = {0};
	u16 north_index_data = 0, south_index_data = 0, east_index_data = 0, west_index_data = 0, mid_index_data = 0;
	u16 spec_index = 0;
	struct area_info *update_area = &pinfo->update_area;
	unsigned int panel_width = manager->panel_info.width;
	unsigned int panel_height = manager->panel_info.height;
	unsigned int i = 0, j = 0, offset = 0;
	unsigned char pipe_id = 0;
	enum eink_update_mode mode = pinfo->mode & 0xff;
	unsigned int index_buffer_size = 0;

	//printk("GL mode\n");
	old_index_id = (new_index_id) ? 0: 1;
	new_index_vaddr = manager->index_buffer.index_vaddr[new_index_id];

	for (i = (update_area->y_top + 1); i < (update_area->y_bottom -1); i++) {
		offset = i * panel_width + update_area->x_top + 1;
		tmp_mid_ib = (u16 *)new_index_vaddr + offset;
		tmp_north_ib = tmp_mid_ib - panel_width;
		tmp_south_ib = tmp_mid_ib + panel_width;
		for (j = (update_area->x_top + 1); j< (update_area->x_bottom -1); j++) {
			mid_index_data = *tmp_mid_ib;
			
			//check special waveform
			if (0x3de == mid_index_data) {
				cardinal_index_data[EAST] = *(tmp_mid_ib + 1);
				cardinal_index_data[WEST] = *(tmp_mid_ib - 1);
				cardinal_index_data[SOUTH] = *tmp_south_ib;
				cardinal_index_data[NORTH] = *tmp_north_ib;
				
				if (is_need_special_waveform(cardinal_index_data,&spec_index)) {
					*tmp_mid_ib = (mid_index_data & 0x3e0) | (spec_index);	//update index buffer
				}
			} else {
				//normal waveform, do nothing
			}
			
			tmp_mid_ib++;
			tmp_south_ib++;
			tmp_north_ib++;
		}
	}

	
	index_buffer_size = manager->index_buffer.index_buf_size;		
	disp_eink_cache_sync(new_index_vaddr, (new_index_vaddr + index_buffer_size));
	return 0;
}


int calc_index_for_non_overlap_part(struct disp_eink_manager *manager, struct pipe_info *pinfo)
{
	struct eink_8bpp_image last_image;
	struct eink_8bpp_image cur_image;
	struct area_info update_area;
	u8 new_data = 0, old_data = 0, index_data = 0;
	u32 offset = 0;
	struct index_manager *idx_mgr = manager->idx_mgr;
	unsigned char new_index_id = manager->index_buffer.cur_index;
	unsigned char old_index_id = 0;
	unsigned long old_index_paddr = 0;
	unsigned long new_index_paddr = 0;
	unsigned long old_index_vaddr = 0;
	unsigned long new_index_vaddr = 0;	
	unsigned int working_buffer_paddr = manager->work_buffer.workbuffer_paddr;
	char *working_buffer_vaddr = manager->work_buffer.workbuffer_vaddr;
	unsigned char unit_size = 0;
	unsigned int index_buffer_size = 0, working_buffer_size = 0;
	int ret = -1;
    int align = 0, width = 0, height = 0;
	
	bool ntx_handwrite_solution = manager->ntx_handwrite_solution;
    bool ntx_force_region_update = manager->force_region_update;
	
	struct eink_buffer_manager *buf_mgr = NULL;
	buf_mgr = manager->buffer_mgr;

	old_index_id = (new_index_id)?0:1;

    __debug("new_index_id:%d\n",new_index_id);

	old_index_paddr = manager->index_buffer.index_paddr[old_index_id];
	new_index_paddr = manager->index_buffer.index_paddr[new_index_id];
	old_index_vaddr = manager->index_buffer.index_vaddr[old_index_id];
	new_index_vaddr = manager->index_buffer.index_vaddr[new_index_id];

	
	// current image
	memset(&cur_image, 0, sizeof(cur_image));
	memcpy(&cur_image, pinfo->img, sizeof(cur_image));
    if ((pinfo->mode & 0xFF) == EINK_GC16_MODE || (pinfo->mode & 0xFF) == EINK_GCK16_MODE || ntx_force_region_update) {
        memcpy(&cur_image.update_area, &pinfo->org_update_area, sizeof(pinfo->update_area));	// non-overlap update area
        memcpy(&pinfo->update_area, &pinfo->org_update_area, sizeof(pinfo->update_area));
    }
    
    //  __debug("[%s %d]pinfo->update_area:(%d %d %d %d)\n",__func__,__LINE__,pinfo->update_area.x_top, pinfo->update_area.y_top, pinfo->update_area.x_bottom, pinfo->update_area.y_bottom);
    
	// last image
	memset(&last_image, 0, sizeof(last_image));
	last_image.vaddr = working_buffer_vaddr;
	last_image.paddr = working_buffer_paddr;
	memcpy(&last_image.size, &cur_image.size, sizeof(cur_image.size));
    memcpy(&last_image.update_area, &pinfo->update_area, sizeof(pinfo->update_area));
    
#ifdef TRACE_INDEX_BUFFER_HW
    if((pinfo->mode & 0xFF) == EINK_DU_MODE) {
	    int x = pinfo->org_update_area.x_top, y = pinfo->org_update_area.y_top;
	    int width = pinfo->org_update_area.x_bottom - x + 1;
	    int height = pinfo->org_update_area.y_bottom - y + 1;

        dump_pixels("cur_image",cur_image.vaddr, x,y,width,height,manager->panel_info.width,1);
        dump_pixels("last_image",last_image.vaddr, x,y,width,height,manager->panel_info.width,1);
    }
#endif

	if (ntx_handwrite_solution == true && buf_mgr->new_process_flag == 1)
		ret = 0;
	else {
        if ((pinfo->mode & 0xFF) != EINK_GC16_MODE && (pinfo->mode & 0xFF) != EINK_GCK16_MODE)
            cur_image.window_calc_enable = true;
        else
            cur_image.window_calc_enable = false;
        
        if (ntx_force_region_update)
            cur_image.window_calc_enable = false;

        mutex_lock(&buf_mgr->mlock); //avoid DE and EE launch at the same time
		ret = idx_mgr->start_calc_index(manager->disp, old_index_paddr, new_index_paddr, &last_image, &cur_image);
        mutex_unlock(&buf_mgr->mlock);

        cur_image.window_calc_enable = false;

        if (pinfo->mode != EINK_INIT_MODE && !rectangle_is_zero(cur_image.update_area)) {
            if ((pinfo->mode & 0xFF) != EINK_GC16_MODE && (pinfo->mode & 0xFF) != EINK_GCK16_MODE) {
                if (!ntx_force_region_update) {
                    memcpy(&pinfo->org_update_area, &cur_image.update_area, sizeof(pinfo->update_area));
                    memcpy(&pinfo->update_area, &cur_image.update_area, sizeof(pinfo->update_area));
                }
            }
        }
    }
	working_buffer_size = manager->work_buffer.workbuffer_size;
	disp_eink_cache_sync(working_buffer_vaddr, (working_buffer_vaddr + working_buffer_size));
	
	if (ret) {
		__wrn("calculate index buffer fail, ret=%d\n", ret);
	} else {
		//update working buffer
		//__debug("calculate index buffer successfully\n");
        unit_size = 1; 		// bpp = 1;
        if (rectangle_is_zero(cur_image.update_area) || 
            (pinfo->mode & 0xFF) == EINK_GC16_MODE || 
            (pinfo->mode & 0xFF) == EINK_GCK16_MODE || ntx_force_region_update) {
            copy_area_buffer(working_buffer_vaddr, cur_image.vaddr, 
                        cur_image.size.width, pinfo->update_area, unit_size);
        } else {
            copy_area_buffer(working_buffer_vaddr, cur_image.vaddr, 
                        cur_image.size.width, cur_image.update_area, unit_size);
        }
	}
	
	index_buffer_size = manager->index_buffer.index_buf_size;		
	disp_eink_cache_sync(new_index_vaddr, (new_index_vaddr + index_buffer_size));
	
	working_buffer_size = manager->work_buffer.workbuffer_size;
	disp_eink_cache_sync(working_buffer_vaddr, (working_buffer_vaddr + working_buffer_size));
	
#if TRACE_INDEX_BUFFER_HW
    if((pinfo->mode & 0xFF) == EINK_DU_MODE) {
		int x = pinfo->org_update_area.x_top, y = pinfo->org_update_area.y_top;
	    int width = pinfo->org_update_area.x_bottom - x + 1;
	    int height = pinfo->org_update_area.y_bottom - y + 1;
        dump_pixels("old_index_vaddr",old_index_vaddr , x,y,width,height,manager->panel_info.width*2,2);
        dump_pixels("new_index_vaddr",new_index_vaddr , x,y,width,height,manager->panel_info.width*2,2);
    }
#endif

	return ret;
}

/*
calculate index for 4 pixel in one loop
*/
#define PIXEL_NUM_PER_LOOP	4
static int calc_index_for_non_overlap_part_sw(struct disp_eink_manager *manager, struct pipe_info *pinfo)
{
	struct area_info *update_area = &pinfo->update_area;
	struct index_manager *idx_mgr = manager->idx_mgr;
	unsigned char new_index_id = manager->index_buffer.cur_index;	
	unsigned char old_index_id = 0;
	char *working_buffer_vaddr = manager->work_buffer.workbuffer_vaddr;
	char *update_buffer_vaddr = pinfo->img->vaddr;
	unsigned long old_index_vaddr = 0;
	unsigned long new_index_vaddr = 0;
	unsigned int index_buffer_size = 0, working_buffer_size = 0;
	
	register u32 x_top = 0, y_top = 0, x_bottom = 0, y_bottom = 0;
	register u32 x_top_align = 0, x_bottom_align = 0, x_top_left = 0, x_bottom_left = 0;
	register u32 h = 0, w = 0, j = 0, panel_width = 0, panel_height = 0,offset = 0;
	register u32 tmp_new_data = 0, tmp_old_data = 0;
	register u16 tmp_new_data_u16 = 0, tmp_old_data_u16 = 0;

	u32 *new_data = NULL, *old_data = NULL;
	u8 *new_data_u8 = NULL, *old_data_u8 = NULL;
	u32 *new_index = NULL;
	u16 *new_index_u16 = NULL;
	u32 index_data[4] = {0};
	
	int ret = -1;

	//printk("%s: sw index\n", __func__);
	old_index_id = (new_index_id) ? 0: 1;	
	old_index_vaddr = manager->index_buffer.index_vaddr[old_index_id];
	new_index_vaddr = manager->index_buffer.index_vaddr[new_index_id];

	if (false == is_whole_screen_update(manager->panel_info, *update_area)) {
		memcpy(new_index_vaddr, old_index_vaddr, manager->index_buffer.index_buf_size);
	}

	panel_width = manager->panel_info.width;
	panel_height = manager->panel_info.height;
	if ((update_area->x_bottom - update_area->x_top) < (3 * PIXEL_NUM_PER_LOOP)) {
		x_top = update_area->x_top;
		y_top = update_area->y_top;
		x_bottom = update_area->x_bottom;
		y_bottom = update_area->y_bottom;

		__debug("area too small, calc directly\n");
		for (h = y_top; h <= y_bottom; h++) {
			offset = (panel_width * h + x_top);
			new_index_u16 = (u16 *)new_index_vaddr + offset;
			new_data_u8 = (u8 *)update_buffer_vaddr + offset;
			old_data_u8 = (u8 *)working_buffer_vaddr + offset;		
			
			/*calc 1 pixel per loop*/
			for (w = x_top; w <= x_bottom; w += 1) {
				tmp_new_data_u16 = *new_data_u8;
				tmp_old_data_u16 = *old_data_u8;
				*old_data_u8 = *new_data_u8;
				*new_index_u16 = (((tmp_old_data_u16) & 0xf0) << 2) | (((tmp_new_data_u16) & 0xf0) >> 3);
				new_index_u16++;
				new_data_u8++;
				old_data_u8++;
			}
		}
	} else {		
		/* x_top <= x_top_align */
		x_top_left = update_area->x_top % PIXEL_NUM_PER_LOOP;		
		if (x_top_left) {
			x_top = update_area->x_top;
			x_top_align = ALIGN(update_area->x_top, PIXEL_NUM_PER_LOOP);
		} else {
			x_top = update_area->x_top;
			x_top_align = update_area->x_top;
		}

		/* x_bottom_align <= x_bottom */
		x_bottom_left = (update_area->x_bottom + 1) % PIXEL_NUM_PER_LOOP;
		if (x_bottom_left) {
			x_bottom = update_area->x_bottom;
			x_bottom_align = update_area->x_bottom - x_bottom_left;
		} else {
			x_bottom = update_area->x_bottom;
			x_bottom_align = update_area->x_bottom;
		}
		
		y_top = update_area->y_top;
		y_bottom = update_area->y_bottom;

		__debug("x_top=%d,x_top_a=%d,x_bottom_a=%d, x_bottom=%d\n",
			x_top, x_top_align, x_bottom_align,x_bottom);
		for (h = y_top; h <= y_bottom; h++) {
			/* x_top <= x_top_align < x_bottom_align <= x_bottom*/
			offset = (panel_width * h + x_top);
			new_index_u16 = (u16 *)new_index_vaddr + offset;
			new_data_u8 = (u8 *)update_buffer_vaddr + offset;
			old_data_u8 = (u8 *)working_buffer_vaddr + offset;

			/*calc 1 pixel per loop*/
			for (w = x_top; w < x_top_align; w += 1) {
				tmp_new_data_u16 = *new_data_u8;
				tmp_old_data_u16 = *old_data_u8;
				*old_data_u8 = *new_data_u8;
				*new_index_u16 = (((tmp_old_data_u16) & 0xf0) << 2) | (((tmp_new_data_u16) & 0xf0) >> 3);
				new_index_u16++;
				new_data_u8++;
				old_data_u8++;
			}

			/*calc 4 pixel per loop*/
			new_index = (u32 *)new_index_u16;
			new_data = (u32 *)new_data_u8;
			old_data = (u32 *)old_data_u8;
			/*
			asm ("PLD [%0, #128]"::"rw" (new_index));
			asm ("PLD [%0, #128]"::"r" (new_data));
			asm ("PLD [%0, #128]"::"r" (old_data));
			*/
			for (w = x_top_align; w < x_bottom_align; w += PIXEL_NUM_PER_LOOP) {
				tmp_new_data = *new_data;
				tmp_old_data = *old_data;
				*old_data = *new_data;
				new_data++;
				old_data++;

				index_data[0] = (((tmp_old_data) & 0xf0) << 2) | (((tmp_new_data) & 0xf0) >> 3);
				index_data[1] = (((tmp_old_data >> 8) & 0xf0) << 2) | (((tmp_new_data >> 8) & 0xf0) >> 3);
				index_data[2] = (((tmp_old_data >> 16) & 0xf0) << 2) | (((tmp_new_data >> 16) & 0xf0) >> 3);
				index_data[3] = (((tmp_old_data >> 24) & 0xf0) << 2) | (((tmp_new_data >> 24) & 0xf0) >> 3);

				*new_index = (index_data[1] << 16) | index_data[0];
				new_index++;
				*new_index = (index_data[3] << 16) | index_data[2];
				new_index++;
			}

			/*calc 1 pixel per loop*/
			if (x_bottom_align < x_bottom) {
				new_index_u16 = (u16 *)new_index;
				new_data_u8 = (u8 *)new_data;
				old_data_u8 = (u8 *)old_data;
				for (w = (x_bottom_align + 1); w <= x_bottom; w += 1) {
					tmp_new_data_u16 = *new_data_u8;
					tmp_old_data_u16 = *old_data_u8;
					*old_data_u8 = *new_data_u8;
					*new_index_u16 = (((tmp_old_data_u16) & 0xf0) << 2) | (((tmp_new_data_u16) & 0xf0) >> 3);
					new_index_u16++;
					new_data_u8++;
					old_data_u8++;
				}
			}
		}
	}

	index_buffer_size = manager->index_buffer.index_buf_size;		
	disp_eink_cache_sync(new_index_vaddr, (new_index_vaddr + index_buffer_size));
	
	working_buffer_size = manager->work_buffer.workbuffer_size;
	disp_eink_cache_sync(working_buffer_vaddr, (working_buffer_vaddr + working_buffer_size));
	return 0;
}

int calc_index_for_overlap_part_for_4bit(struct disp_eink_manager *manager, struct pipe_info *pinfo, struct area_info *collision_area)
{
	struct index_manager *idx_mgr = manager->idx_mgr;
	unsigned char new_index_id = manager->index_buffer.cur_index;
	unsigned char old_index_id = 0;
	unsigned long old_index_vaddr = 0;
	unsigned long new_index_vaddr = 0;
	char *working_buffer_vaddr = manager->work_buffer.workbuffer_vaddr;
	char *update_buffer_vaddr = pinfo->img->vaddr;
	char *tmp_wb = NULL, *tmp_ub = NULL, *tmp_ib = NULL, *tmp_mask = NULL;
	char upd_data = 0, begin_data = 0, end_data = 0;
	char index_new_data = 0, index_old_data = 0;
	struct area_info overlap_area_align;
	struct area_info *overlap_area = NULL;
	struct area_info *overlap_org_area = &pinfo->overlap_area;
	unsigned int pitch = manager->panel_info.width;
	unsigned int w = 0, h = 0, offset = 0;
	unsigned int mask_offset = 0, mask_pitch = 0, x = 0, y = 0;
	unsigned char pipe_id = 0;
	enum eink_update_mode mode = pinfo->mode & 0xff;
	unsigned int index_buffer_size = 0, working_buffer_size = 0;

	collision_area->x_top = (unsigned int)(-1);
	collision_area->y_top = (unsigned int)(-1);
	collision_area->x_bottom = (unsigned int)(0);	
	collision_area->y_bottom = (unsigned int)(0);

	overlap_area = &overlap_area_align;
	align_area(pinfo->overlap_area, &overlap_area_align, 8);
	
	old_index_id = (new_index_id) ? 0: 1;
	old_index_vaddr = manager->index_buffer.index_vaddr[old_index_id];
	new_index_vaddr = manager->index_buffer.index_vaddr[new_index_id];

	mask_pitch = overlap_area->x_bottom - overlap_area->x_top + 1;
	for (h = overlap_area->y_top; h <= overlap_area->y_bottom; h++) {
		offset = h * pitch + overlap_org_area->x_top;
		tmp_wb = working_buffer_vaddr + offset;
		tmp_ub = update_buffer_vaddr + offset;
		tmp_ib = new_index_vaddr + offset;

		mask_offset = (h - overlap_area->y_top) * mask_pitch + (overlap_org_area->x_top - overlap_area->x_top);
		tmp_mask = (char *)pinfo->update_image_mask + mask_offset;
		for (w = overlap_org_area->x_top; w <= overlap_org_area->x_bottom; w++) {
			upd_data = *tmp_ub >> 4;
			end_data = *tmp_wb >> 4;

			if (*tmp_mask == 0) {
				*tmp_ib = (end_data << 4) | upd_data;	//update index buffer
				*tmp_wb = *tmp_ub;				//update working buffer
			} else {
				index_new_data = *tmp_ib & 0x0f;
				index_old_data = *tmp_ib >> 4;
				begin_data = index_new_data;
				//begin_data = index_old_data;

				if (index_new_data != end_data) {	//for debug
					__debug("index_new_data(%d) not equal to end_data(%d),pos(%d,%d)\n",
					      index_new_data, end_data, w, h);
				}
				//this pixel may not overlap with other pipe
				if (upd_data == end_data) {
					//working buffer no need to update, mask this pixel
					*tmp_mask = 0x3;
				} else if (begin_data == end_data) {
					//update working buffer with mask buffer
					*tmp_mask = 0;					//update mask buffer
					*tmp_ib = (end_data << 4) | upd_data;	//update index buffer
					*tmp_wb = *tmp_ub;				//update working buffer
				} else {
					//do NOT update working buffer with this pixel
					if (mode != EINK_DU_MODE) {
						pipe_id = (*tmp_mask - 1);
						*tmp_mask = 0x3;
						pinfo->collision_flag |= (1 << pipe_id);

						collision_area->x_top = min(collision_area->x_top, w);
						collision_area->y_top = min(collision_area->y_top, h);
						collision_area->x_bottom = max(collision_area->x_bottom, w);
						collision_area->y_bottom = max(collision_area->y_bottom, h);
					} else {
						*tmp_mask = 0x0;
						if (upd_data <= 0x8) {
							*tmp_ib = 0xf0;	//update index buffer
							*tmp_wb = 0x00;	//update working buffer
						} else {
							*tmp_ib = 0x0f;	//update index buffer
							*tmp_wb = 0xff;	//update working buffer
						}
						//__debug("collision: n=%d,e=%d,in=%d, b=%d,pos(%d,%d)\n",
						//	new_data, end_data, index_new_data, begin_data, w, h);
					}
				}
			}

			tmp_wb++;
			tmp_ub++;
			tmp_ib++;
			tmp_mask++;
		}
	}

	if ((collision_area->x_top == ((u32)(-1))) && (collision_area->y_top == ((u32)(-1))) &&
		(collision_area->x_bottom == 0) && (collision_area->y_bottom == 0)) {
		pinfo->collision_flag = 0;
	}
	
	index_buffer_size = manager->index_buffer.index_buf_size;		
	disp_eink_cache_sync(new_index_vaddr, (new_index_vaddr + index_buffer_size));
	
	working_buffer_size = manager->work_buffer.workbuffer_size;
	disp_eink_cache_sync(working_buffer_vaddr, (working_buffer_vaddr + working_buffer_size));
	return 0;
}

int calc_index_for_overlap_part_for_du_4bit(struct disp_eink_manager *manager,
	struct pipe_info *pinfo, struct area_info *collision_area)
{
    struct index_manager *idx_mgr = manager->idx_mgr;
    unsigned char new_index_id = manager->index_buffer.cur_index;
    unsigned char old_index_id = 0;
    unsigned long old_index_vaddr = 0;
    unsigned long new_index_vaddr = 0;
    char *working_buffer_vaddr = manager->work_buffer.workbuffer_vaddr;
    char *update_buffer_vaddr = pinfo->img->vaddr;
    char *tmp_wb = NULL, *tmp_ub = NULL, *tmp_ib = NULL, *tmp_mask = NULL;
    char upd_data = 0, begin_data = 0, end_data = 0;
    char index_new_data = 0, index_old_data = 0;
    struct area_info overlap_area_align;
    struct area_info *overlap_area = NULL;
    struct area_info *overlap_org_area = &pinfo->overlap_area;
    unsigned int pitch = manager->panel_info.width;
    unsigned int w = 0, h = 0, offset = 0;
    unsigned int mask_offset = 0, mask_pitch = 0, x = 0, y = 0;
    unsigned char pipe_id = 0;
    enum eink_update_mode mode = pinfo->mode & 0xff;
    unsigned int index_buffer_size = 0, working_buffer_size = 0;

    collision_area->x_top = (unsigned int)(-1);
    collision_area->y_top = (unsigned int)(-1);
    collision_area->x_bottom = (unsigned int)(0);
    collision_area->y_bottom = (unsigned int)(0);

    overlap_area = &overlap_area_align;
    align_area(pinfo->overlap_area, &overlap_area_align, 8);

    old_index_id = (new_index_id) ? 0 : 1;
    old_index_vaddr = manager->index_buffer.index_vaddr[old_index_id];
    new_index_vaddr = manager->index_buffer.index_vaddr[new_index_id];

    mask_pitch = overlap_area->x_bottom - overlap_area->x_top + 1;
    for (h = overlap_area->y_top; h <= overlap_area->y_bottom; h++) {
	offset = h * pitch + overlap_org_area->x_top;
	tmp_wb = working_buffer_vaddr + offset;
	tmp_ub = update_buffer_vaddr + offset;
	tmp_ib = new_index_vaddr + offset;

	mask_offset = (h - overlap_area->y_top) * mask_pitch + (overlap_org_area->x_top - overlap_area->x_top);
	tmp_mask = (char *)pinfo->update_image_mask + mask_offset;
	for (w = overlap_org_area->x_top; w <= overlap_org_area->x_bottom; w++) {
	    upd_data = *tmp_ub >> 4;
	    end_data = *tmp_wb >> 4;

	if (upd_data != end_data) {
		//update working buffer with mask buffer
		*tmp_mask = 0;					//update mask buffer
		*tmp_ib = (end_data << 4) | upd_data;	//update index buffer
		*tmp_wb = *tmp_ub;				//update working buffer
	} else {
		*tmp_mask = 0x3;
	}
	    tmp_wb++;
	    tmp_ub++;
	    tmp_ib++;
	    tmp_mask++;
	}

	if ((collision_area->x_top == ((u32)(-1))) && (collision_area->y_top == ((u32)(-1))) &&
		(collision_area->x_bottom == 0) && (collision_area->y_bottom == 0)) {
	    pinfo->collision_flag = 0;
	}

	index_buffer_size = manager->index_buffer.index_buf_size;
	disp_eink_cache_sync(new_index_vaddr, (new_index_vaddr + index_buffer_size));

	working_buffer_size = manager->work_buffer.workbuffer_size;
	disp_eink_cache_sync(working_buffer_vaddr, (working_buffer_vaddr + working_buffer_size));
	return 0;
    }
}

int new_process_calc_index_for_overlap_part_for_du_4bit(struct disp_eink_manager *manager,
	struct pipe_info *pinfo, struct area_info *collision_area)
{
    struct index_manager *idx_mgr = manager->idx_mgr;
    unsigned char new_index_id = manager->index_buffer.cur_index;
    unsigned char old_index_id = 0;
    unsigned long old_index_vaddr = 0;
    unsigned long new_index_vaddr = 0;
    char *working_buffer_vaddr = manager->work_buffer.workbuffer_vaddr;
    char *update_buffer_vaddr = pinfo->img->vaddr;
    char *tmp_wb = NULL, *tmp_ub = NULL, *tmp_ib = NULL, *tmp_mask = NULL;
    char upd_data = 0, begin_data = 0, end_data = 0;
    char index_new_data = 0, index_old_data = 0;
    struct area_info overlap_area_align;
    struct area_info *overlap_area = NULL;
    struct area_info *overlap_org_area = &pinfo->overlap_area;
    unsigned int pitch = manager->panel_info.width;
    unsigned int w = 0, h = 0, offset = 0;
    unsigned int mask_offset = 0, mask_pitch = 0, x = 0, y = 0;
    unsigned char pipe_id = 0;
    enum eink_update_mode mode = pinfo->mode & 0xff;
    unsigned int index_buffer_size = 0, working_buffer_size = 0;

    collision_area->x_top = (unsigned int)(-1);
    collision_area->y_top = (unsigned int)(-1);
    collision_area->x_bottom = (unsigned int)(0);
    collision_area->y_bottom = (unsigned int)(0);

    overlap_area = &overlap_area_align;
    align_area(pinfo->overlap_area, &overlap_area_align, 8);

    old_index_id = (new_index_id) ? 0 : 1;
    old_index_vaddr = manager->index_buffer.index_vaddr[old_index_id];
    new_index_vaddr = manager->index_buffer.index_vaddr[new_index_id];

    mask_pitch = overlap_area->x_bottom - overlap_area->x_top + 1;
    for (h = overlap_area->y_top; h <= overlap_area->y_bottom; h++) {
	offset = h * pitch + overlap_org_area->x_top;
	tmp_wb = working_buffer_vaddr + offset;
	tmp_ub = update_buffer_vaddr + offset;
	tmp_ib = (u8 *)new_index_vaddr + offset;

	mask_offset = (h - overlap_area->y_top) * mask_pitch + (overlap_org_area->x_top - overlap_area->x_top);
	tmp_mask = (char *)pinfo->update_image_mask + mask_offset;
	for (w = overlap_org_area->x_top; w <= overlap_org_area->x_bottom; w++) {
	    upd_data = *tmp_ub;
	    end_data = *tmp_wb;

	if ((upd_data - end_data) > 3 || (end_data - upd_data) > 3) {
		upd_data = upd_data >> 7;
		end_data = end_data >> 7;
		*tmp_mask = 0x0;
		*tmp_ib = (end_data << 7) | (upd_data << 3);
		*tmp_wb = *tmp_ub;
	} else {
		*tmp_mask = 0x3;
	}

	    tmp_wb++;
	    tmp_ub++;
	    tmp_ib++;
	    tmp_mask++;
	}
    }

    if ((collision_area->x_top == ((u32)(-1))) && (collision_area->y_top == ((u32)(-1))) &&
	    (collision_area->x_bottom == 0) && (collision_area->y_bottom == 0)) {
	pinfo->collision_flag = 0;
    }

    index_buffer_size = manager->index_buffer.index_buf_size;
    //disp_eink_cache_sync(new_index_vaddr, (new_index_vaddr + index_buffer_size));

    working_buffer_size = manager->work_buffer.workbuffer_size;
    //disp_eink_cache_sync(working_buffer_vaddr, (working_buffer_vaddr + working_buffer_size));
    return 0;
}

int calc_index_for_overlap_part_for_5bit(struct disp_eink_manager *manager,
		struct pipe_info *pinfo, struct area_info *collision_area)
{
	struct index_manager *idx_mgr = manager->idx_mgr;
	unsigned char new_index_id = manager->index_buffer.cur_index;
	unsigned char old_index_id = 0;
	unsigned long old_index_vaddr = 0;
	unsigned long new_index_vaddr = 0;
	unsigned char *working_buffer_vaddr = manager->work_buffer.workbuffer_vaddr;
	unsigned char *update_buffer_vaddr = pinfo->img->vaddr;
	unsigned char *tmp_wb = NULL, *tmp_ub = NULL, *tmp_mask = NULL;
    //u16 *tmp_ib = NULL;
    u16 *tmp_ib = NULL, *tmp_previous_ib = NULL;
    u16 new_data = 0, begin_data = 0, end_data = 0;
	u16 index_new_data = 0, index_old_data = 0;
	u16 previous_index_new_data = 0, previous_index_old_data = 0;
	struct area_info overlap_area_align;
	struct area_info *overlap_area = NULL;
	struct area_info *overlap_org_area = &pinfo->overlap_area;
	unsigned int pitch = manager->panel_info.width;
	unsigned int w = 0, h = 0, offset = 0;
	unsigned int mask_offset = 0, mask_pitch = 0, x = 0, y = 0;
	unsigned char pipe_id = 0;
	enum eink_update_mode mode = pinfo->mode & 0xff;
	unsigned int index_buffer_size = 0, working_buffer_size = 0;

	collision_area->x_top = (unsigned int)(-1);
	collision_area->y_top = (unsigned int)(-1);
	collision_area->x_bottom = (unsigned int)(0);	
	collision_area->y_bottom = (unsigned int)(0);

	

	overlap_area = &overlap_area_align;
	align_area(pinfo->overlap_area, &overlap_area_align, 8);
	
	old_index_id = (new_index_id) ? 0: 1;
	old_index_vaddr = manager->index_buffer.index_vaddr[old_index_id];
	new_index_vaddr = manager->index_buffer.index_vaddr[new_index_id];

	mask_pitch = overlap_area->x_bottom - overlap_area->x_top + 1;
	
	for (h = overlap_area->y_top; h <= overlap_area->y_bottom; h++) {
		offset = h * pitch + overlap_org_area->x_top;
		tmp_wb = working_buffer_vaddr + offset;
		tmp_ub = update_buffer_vaddr + offset;
		tmp_ib = (u16 *)new_index_vaddr + offset;
		tmp_previous_ib = (u16 *)old_index_vaddr + offset;

		mask_offset = (h - overlap_area->y_top) * mask_pitch + (overlap_org_area->x_top - overlap_area->x_top);
		tmp_mask = (unsigned char *)pinfo->update_image_mask + mask_offset;
        
		for (w = overlap_org_area->x_top; w <= overlap_org_area->x_bottom; w++) {
			// new_data and end_data are the same, due to calc_index_for_non_overlap_part
			new_data = (*tmp_ub >> 3) & 0x1f;
			end_data = (*tmp_wb >> 3) & 0x1f;
			
			if (new_data != end_data) {
				*tmp_mask = 0x0;
				*tmp_ib = ((new_data) << 5) | (new_data << 0);
				*tmp_wb = *tmp_ub;
			} else {
				previous_index_new_data = *tmp_previous_ib & 0x1f;
				previous_index_old_data = *tmp_previous_ib >> 5;
				if (previous_index_old_data != previous_index_new_data) {
					*tmp_ib = *tmp_previous_ib;
					*tmp_wb = previous_index_new_data << 3; // update working buffer
					
					pipe_id = (*tmp_mask - 1);
					*tmp_mask = 0x3;
					pinfo->collision_flag |= (1 << pipe_id);
					
					collision_area->x_top = min(collision_area->x_top, w);
					collision_area->y_top = min(collision_area->y_top, h);
					collision_area->x_bottom = max(collision_area->x_bottom, w);
					collision_area->y_bottom = max(collision_area->y_bottom, h);
				} else {
					*tmp_mask = 0x3;
				} 
			} 

			tmp_wb++;
			tmp_ub++;
			tmp_ib++;
			tmp_previous_ib++;
			tmp_mask++;
		}
	}

	if ((collision_area->x_top == ((u32)(-1))) && (collision_area->y_top == ((u32)(-1))) &&
		(collision_area->x_bottom == 0) && (collision_area->y_bottom == 0)) {
		pinfo->collision_flag = 0;
	}
		
	index_buffer_size = manager->index_buffer.index_buf_size;		
	disp_eink_cache_sync(new_index_vaddr, (new_index_vaddr + index_buffer_size));
	
	working_buffer_size = manager->work_buffer.workbuffer_size;
	disp_eink_cache_sync(working_buffer_vaddr, (working_buffer_vaddr + working_buffer_size));
	return 0;
}


int new_process_calc_index_for_overlap_part_for_du_5bit(struct disp_eink_manager *manager, struct pipe_info *pinfo, struct area_info *collision_area)
{
	struct index_manager *idx_mgr = manager->idx_mgr;
	unsigned char new_index_id = manager->index_buffer.cur_index;
	unsigned char old_index_id = 0;
	unsigned long old_index_vaddr = 0;
	unsigned long new_index_vaddr = 0;
	char *working_buffer_vaddr = manager->work_buffer.workbuffer_vaddr;
	char *update_buffer_vaddr = pinfo->img->vaddr;
	char *tmp_wb = NULL, *tmp_ub = NULL, *tmp_mask = NULL;
	u16 *tmp_ib = NULL;
	u16 new_data = 0, begin_data = 0, end_data = 0;
	u16 index_new_data = 0, index_old_data = 0;
	struct area_info overlap_area_align;
	struct area_info *overlap_area = NULL;
	struct area_info *overlap_org_area = &pinfo->overlap_area;
	unsigned int pitch = manager->panel_info.width;
	unsigned int w = 0, h = 0, offset = 0;
	unsigned int mask_offset = 0, mask_pitch = 0, x = 0, y = 0;
	unsigned char pipe_id = 0;
	enum eink_update_mode mode = pinfo->mode & 0xff;
	unsigned int index_buffer_size = 0, working_buffer_size = 0;
	u32 total_pixel = 0, skip_pixel = 0, show_pixel = 0;

	overlap_area = &overlap_area_align;
	#if SUPPORT_NEON_ALGORITHM
	align_area(pinfo->overlap_area, &overlap_area_align, NEON_DATA_ALIGN);
	#else
	align_area(pinfo->overlap_area, &overlap_area_align, 8);
	#endif
		
	old_index_id = (new_index_id) ? 0: 1;
	old_index_vaddr = manager->index_buffer.index_vaddr[old_index_id];
	new_index_vaddr = manager->index_buffer.index_vaddr[new_index_id];

#ifdef TRACE_INDEX_BUFFER_HW
    if((pinfo->mode & 0xFF) == EINK_DU_MODE) {
	    int x = overlap_area->x_top, y = overlap_area->y_top;
	    int width = overlap_area->x_bottom - x + 1;
	    int height = overlap_area->y_bottom - y + 1;

        dump_pixels("working_buffer",working_buffer_vaddr , x,y,width,height,manager->panel_info.width,1);
        dump_pixels("update_buffer_vaddr",update_buffer_vaddr , x,y,width,height,manager->panel_info.width,1);
    }
#endif

	mask_pitch = overlap_area->x_bottom - overlap_area->x_top + 1;
	for (h = overlap_area->y_top; h <= overlap_area->y_bottom; h++) {
		offset = h * pitch + overlap_org_area->x_top;
		tmp_wb = working_buffer_vaddr + offset;
		tmp_ub = update_buffer_vaddr + offset;
		tmp_ib = (u16 *)new_index_vaddr + offset;

		mask_offset = (h - overlap_area->y_top) * mask_pitch + (overlap_org_area->x_top - overlap_area->x_top);
		tmp_mask = (char *)pinfo->update_image_mask + mask_offset;
		for (w = overlap_org_area->x_top; w <= overlap_org_area->x_bottom; w++) {

			new_data = *tmp_ub;
			end_data = *tmp_wb;
			if ((new_data - end_data) > 3 || (end_data - new_data) > 3) {
				new_data = new_data >> 7;
				end_data = end_data >> 7;
				*tmp_mask = 0x0;
				*tmp_ib = (end_data << 9)| (new_data << 4);
				*tmp_wb = *tmp_ub;
			} else {
				*tmp_mask = 0x3;
			}


			tmp_wb++;
			tmp_ub++;
			tmp_ib++;
			tmp_mask++;
		}
	}
	
	index_buffer_size = manager->index_buffer.index_buf_size;		
	disp_eink_cache_sync(new_index_vaddr, (new_index_vaddr + index_buffer_size));
	
	working_buffer_size = manager->work_buffer.workbuffer_size;
	disp_eink_cache_sync(working_buffer_vaddr, (working_buffer_vaddr + working_buffer_size));

	return 0;
}





int calc_index_for_overlap_part_for_du_5bit(struct disp_eink_manager *manager, struct pipe_info *pinfo, struct area_info *collision_area)
{
	struct index_manager *idx_mgr = manager->idx_mgr;
	unsigned char new_index_id = manager->index_buffer.cur_index;
	unsigned char old_index_id = 0;
	unsigned long old_index_vaddr = 0;
	unsigned long new_index_vaddr = 0;
	char *working_buffer_vaddr = manager->work_buffer.workbuffer_vaddr;
	char *update_buffer_vaddr = pinfo->img->vaddr;
	char *tmp_wb = NULL, *tmp_ub = NULL, *tmp_mask = NULL;
	//u16 *tmp_ib = NULL;
	u16 *tmp_ib = NULL, *tmp_previous_ib = NULL;
	u16 new_data = 0, begin_data = 0, end_data = 0;
	u16 index_new_data = 0, index_old_data = 0;
	u16 previous_index_new_data = 0, previous_index_old_data = 0;
	struct area_info overlap_area_align;
	struct area_info *overlap_area = NULL;
	struct area_info *overlap_org_area = &pinfo->overlap_area;
	unsigned int pitch = manager->panel_info.width;
	unsigned int w = 0, h = 0, offset = 0;
	unsigned int mask_offset = 0, mask_pitch = 0, x = 0, y = 0;
	unsigned char pipe_id = 0;
	enum eink_update_mode mode = pinfo->mode & 0xff;
	unsigned int index_buffer_size = 0, working_buffer_size = 0;
	u32 total_pixel = 0, skip_pixel = 0, show_pixel = 0;

	collision_area->x_top = (unsigned int)(-1);
	collision_area->y_top = (unsigned int)(-1);
	collision_area->x_bottom = (unsigned int)(0);	
	collision_area->y_bottom = (unsigned int)(0);

	overlap_area = &overlap_area_align;
	#if SUPPORT_NEON_ALGORITHM
	align_area(pinfo->overlap_area, &overlap_area_align, NEON_DATA_ALIGN);
	#else
	align_area(pinfo->overlap_area, &overlap_area_align, 8);
	#endif
		
	old_index_id = (new_index_id) ? 0: 1;
	old_index_vaddr = manager->index_buffer.index_vaddr[old_index_id];
	new_index_vaddr = manager->index_buffer.index_vaddr[new_index_id];

	mask_pitch = overlap_area->x_bottom - overlap_area->x_top + 1;
	for (h = overlap_area->y_top; h <= overlap_area->y_bottom; h++) {
		offset = h * pitch + overlap_org_area->x_top;
		tmp_wb = working_buffer_vaddr + offset;
		tmp_ub = update_buffer_vaddr + offset;
		tmp_ib = (u16 *)new_index_vaddr + offset;
		tmp_previous_ib = (u16 *)old_index_vaddr + offset;

		mask_offset = (h - overlap_area->y_top) * mask_pitch + (overlap_org_area->x_top - overlap_area->x_top);
		tmp_mask = (char *)pinfo->update_image_mask + mask_offset;
		for (w = overlap_org_area->x_top; w <= overlap_org_area->x_bottom; w++) {
			new_data = (*tmp_ub >> 3) & 0xf0;
			end_data = (*tmp_wb >> 3) & 0xf0;
			previous_index_new_data = *tmp_previous_ib & 0x1f;
			previous_index_old_data = *tmp_previous_ib >> 5;

			if (new_data != end_data) {
				*tmp_mask = 0x0;
				*tmp_ib = ((end_data) << 5) | (new_data << 0);
				*tmp_wb = *tmp_ub;
			} else {
				*tmp_mask = 0x3;
				if (previous_index_old_data != previous_index_new_data) {
					*tmp_ib = *tmp_previous_ib;
					*tmp_wb = previous_index_new_data << 3; // update working buffer
					
					pipe_id = (*tmp_mask - 1);
					pinfo->collision_flag |= (1 << pipe_id);
					
					collision_area->x_top = min(collision_area->x_top, w);
					collision_area->y_top = min(collision_area->y_top, h);
					collision_area->x_bottom = max(collision_area->x_bottom, w);
					collision_area->y_bottom = max(collision_area->y_bottom, h);
				}
			}

			tmp_wb++;
			tmp_ub++;
			tmp_ib++;
			tmp_previous_ib++;
			tmp_mask++;
		}
	}

	if ((collision_area->x_top == ((u32)(-1))) && (collision_area->y_top == ((u32)(-1))) &&
		(collision_area->x_bottom == 0) && (collision_area->y_bottom == 0)) {
		pinfo->collision_flag = 0;
	}
	
	index_buffer_size = manager->index_buffer.index_buf_size;		
	disp_eink_cache_sync(new_index_vaddr, (new_index_vaddr + index_buffer_size));
	
	working_buffer_size = manager->work_buffer.workbuffer_size;
	disp_eink_cache_sync(working_buffer_vaddr, (working_buffer_vaddr + working_buffer_size));

	return 0;
}



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

static void __disp_get_special_waveform(char *wf, u32 total_frame, u8 old_gray, u8 new_gray)
{
	u8 data[100] = {0};
	u32 f = 0, offset = 0;
	char buf[100] = {0}, tmp_buf[20] = {0};
	char *p = NULL;

	p = buf;
	memset(buf, 0, sizeof(buf));
	memset(tmp_buf, 0, sizeof(tmp_buf));
	
	sprintf(tmp_buf, "G%d->G%d:",old_gray, new_gray);
	strncat(p, tmp_buf, strlen(tmp_buf));
	p = p + strlen(tmp_buf);
	
	for (f = 0; f < total_frame; f++) {
		offset = (f << 10) + ((old_gray << 5) | new_gray);
		data[f] = *((u8 *)wf + offset);
		
		memset(tmp_buf, 0, sizeof(tmp_buf));
		sprintf(tmp_buf, "%d",data[f]);
		strncat(p, tmp_buf, strlen(tmp_buf));
		p = p + strlen(tmp_buf);
	}

	printk("%s\n", buf);
}

static void __disp_set_special_waveform(char *wf, u32 total_frame, u8 old_gray, u8 new_gray, u8 *data)
{
	u32 f = 0, offset = 0;
	
	for (f = 0; f < total_frame; f++) {
		offset = (f << 10) + ((old_gray << 5) | new_gray);
		*((u8 *)wf + offset) = *(data + f);
	}
}

static void regal_process(struct disp_eink_manager *manager, struct pipe_info *pinfo)
{
    static unsigned int regal_flag = 1;
    unsigned int ret = 1;
    unsigned int working_buffer_paddr = manager->work_buffer.workbuffer_paddr;
    char *working_buffer_vaddr = manager->work_buffer.workbuffer_vaddr;
    struct eink_8bpp_image last_image;
    struct eink_8bpp_image cur_image;
    
    // current image
    memset(&cur_image, 0, sizeof(cur_image));
    memcpy(&cur_image, pinfo->img, sizeof(cur_image));
    
    // last image
    memset(&last_image, 0, sizeof(last_image));
    last_image.vaddr = working_buffer_vaddr;
    last_image.paddr = working_buffer_paddr;
    memcpy(&last_image.size, &cur_image.size, sizeof(cur_image.size));
    
    if (regal_flag == 1) {
        ret = eink_get_version();
        pr_info("[%s]regal version = 0x%x\n", __func__, ret);
        regal_flag = 0;
    }
    
    eink_process((u8 *)cur_image.vaddr, (u8 *)last_image.vaddr, manager->panel_info.width, manager->panel_info.height);
    
}

static bool update_mode_mapping_check(unsigned char *mode_buffer_vaddr, unsigned int pitch, unsigned int cur_update_mode, 
                                struct area_info update_area, unsigned char unit_size)
{
    int w, h;
    int offset;
    unsigned char *tmp_vaddr = NULL;
    char mode_buffer_size = 0;
    int size = 0;
    unsigned int update_mode = (cur_update_mode & 0xff);
    bool need_change_du = false;
    
    size = (update_area.x_bottom - update_area.x_top + 1) * unit_size;
    
    for (h = update_area.y_top; h <= update_area.y_bottom; h++) {
        tmp_vaddr = mode_buffer_vaddr + (h * pitch + update_area.x_top) * unit_size;
        for (w = 0; w < size; w++) {
            if (update_mode == EINK_A2_MODE && *tmp_vaddr != EINK_DU_MODE && *tmp_vaddr != EINK_A2_MODE) {
                need_change_du = true;
                break;
            }

            *tmp_vaddr = update_mode;
            tmp_vaddr++;
        }
    }
    
    if (need_change_du) {
        for (h = update_area.y_top; h <= update_area.y_bottom; h++) {
            tmp_vaddr = mode_buffer_vaddr + (h * pitch + update_area.x_top) * unit_size;
            memset(tmp_vaddr, EINK_DU_MODE, (update_area.x_bottom - update_area.x_top + 1) * unit_size);
        }
    }
    
    return need_change_du;
}

//#define TRACE_INDEX_BUFFER
//#define SPECIAL_WAVEFORM_DEBUG

/*
* 1. detect image buffer/collision list is empty or not, if empty, go to sleep
* 2. if image buffer is not empty, request one pipe to display it
* 3. calc index buffer and update working buffer
* 4. config pipe and active it
* 5. got to step 1 and check image buffer
*/

static unsigned long gLastFlushTime = 0;
static struct timeval index_stimer, index_etimer;
//static struct timeval regal_stimer, regal_etimer;
static int eink_pixel_process_thread(void *parg)
{
	int ret = -1;
	int new_process_flag = 0;
	unsigned long flags = 0;
	int pipe_id = -1;
	unsigned int update_master = 0;
	unsigned int index_paddr = 0;
	unsigned char *index_vaddr = NULL;
	unsigned int index_buffer_size = 0;
	struct disp_eink_manager* manager;
	struct eink_buffer_manager *buf_mgr = NULL;
	struct image_buffer_node *img_node = NULL;
	struct eink_8bpp_image *img = NULL;
	struct pipeline_manager *pipe_mgr = NULL;
	struct index_manager *idx_mgr = NULL;
	struct pipe_info pinfo;
	struct area_info max_area;
	unsigned int active_fail = 0;
	unsigned int overlap_size = 0;
	int temperature = 0;
	unsigned int waveform_vaddr = 0;
	unsigned int power_cnt = 0;
	unsigned int used_pipe_state = 0;
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
	unsigned int working_buffer_size = 0;
	char *mode_buffer_vaddr = NULL;
	unsigned int mode_buffer_size = 0;
	bool wait_mode = false;
	int wait_power_off_cnt = 0;
	int already_skip = 0;
	
	manager = (struct disp_eink_manager*) parg;
	if (NULL == manager) {
		__wrn("eink manager is not initial\n");
		return -1;
	}
	buf_mgr = manager->buffer_mgr;
	pipe_mgr = manager->pipeline_mgr;
	idx_mgr = manager->idx_mgr;
	for (;;) {
		if (kthread_should_stop()) {
			break;
		}
	  
		if(bsp_disp_get_eink_method() == 1){
			try_to_freeze();
			set_current_state(TASK_INTERRUPTIBLE);
			if(buf_mgr->new_process_flag){
				schedule();
			}
		}
		
		if ((manager->last_update_mode & 0xff) == EINK_GLK16_MODE || 
			(manager->last_update_mode & 0xff) == EINK_GCK16_MODE) {
			/* if previous update is using eclipse waveform, must check pmic already power off */
			ret = pipe_mgr->wait_for_eink_power_off(pipe_mgr);
			if (ret && (wait_power_off_cnt < 5)) {
				wait_power_off_cnt++;
				continue;
			} else {
				wait_power_off_cnt = 0;
			} 
		}
		
		if (buf_mgr->is_collision_list_empty(buf_mgr) && buf_mgr->is_empty(buf_mgr)) {
			if ((pipe_mgr->wait_frame_sync_flag == true)) {
				pipe_mgr->wait_frame_sync_queue = 0;
				complete(&pipe_mgr->wait_frame_sync_comp);
			}
			schedule();
			continue;
		}
		
		mutex_lock(&buf_mgr->mlock);
		wait_mode = manager->wait_mode;
		mutex_unlock(&buf_mgr->mlock);
		
		if (wait_mode || (IS_REGAL_MODE(pinfo.mode)) || ((cur_update_mode & 0xff) == EINK_GLR16_MODE) ||
														((cur_update_mode & 0xff) == EINK_GLD16_MODE)) {
			ret = pipe_mgr->wait_for_eink_power_off(pipe_mgr);
			if (ret && (wait_power_off_cnt < 5)) {
				wait_power_off_cnt++;
				continue;
			} else {
				wait_power_off_cnt = 0;
			} 
			
			if (gptHWCFG->m_val.bDisplayResolution == 14 && gptHWCFG->m_val.bUIStyle == 2) {
				if (gLastFlushTime && time_before(jiffies, gLastFlushTime + msecs_to_jiffies(manager->low_temp_flush_delay))) {
					if (manager->eink_panel_temperature <= 6) {
						already_skip++;
						continue;
					} 
				} else {
					gLastFlushTime = jiffies;
					if (manager->eink_panel_temperature <= 6 && !already_skip) {
						continue;
					} else
						already_skip = 0;
				}
			}
		}

		used_pipe_state = pipe_mgr->query_used_pipe(pipe_mgr);
		valid_update_flag = buf_mgr->update_collision_state(buf_mgr, used_pipe_state);

		if ((!valid_update_flag) && (true == buf_mgr->is_empty(buf_mgr))) {
			msleep(1);
			continue;
		}
		
		if (valid_update_flag)
			img_node = buf_mgr->get_current_image_from_collision_list(buf_mgr);
		else
			img_node = NULL;
		
		if (NULL == img_node) {
			img_node = buf_mgr->get_current_image(buf_mgr);
			if (NULL != img_node) {
				update_master = 0;
			} else {
				msleep(1);
				continue;
			}
		} else {
			update_master = 1;
			//img_node->img->update_mode = EINK_GU_MODE;
		}

		img = img_node->img;
		order = img_node->update_order;
		
		if ((img->update_mode & 0xFF) != EINK_INIT_MODE && update_master != 1 && !IS_NO_MERGE(img->update_mode)) {
			__debug(">>> memcmp\n");
			int result = 0;
			result = memcmp(img->vaddr, manager->work_buffer.workbuffer_vaddr, (buf_mgr->width * buf_mgr->height));
			__debug("<<< mcmcmp reslut: %d\n", result);
			if (result == 0) {
				__debug("current img Y8 is the same as workingbuffer Y8\n");
				switch(update_master) {
					case 0:
						buf_mgr->dequeue_image(buf_mgr, img_node);
						break;
					case 1:
						buf_mgr->remove_image_from_collision_list(buf_mgr, img_node);
						break;
					default:
						__wrn("unknown update master(%d)\n", update_master);
						break;
				}
				
				if (pipe_mgr->wait_frame_sync_frame == order && pipe_mgr->wait_frame_sync_flag) {
					pipe_mgr->wait_frame_sync_queue = 0;
					complete(&pipe_mgr->wait_frame_sync_comp);
				}
				
				continue;
			}
		}
		
		/*if ((img->update_mode & 0xff) == EINK_GLR16_MODE || (img->update_mode & 0xff) == EINK_GLD16_MODE) {
			if (img->regal_flag != PROCESS_FIN) {
				msleep(1);
				continue;
			}
		}*/
					
		/********************************************************************************************
		*	a. update mask create and make every pixel can be controled
		*	b. output update area/overlap area, mask buffer, index data
		*	c. output collision flag, if collision happened, add it to collision list(and update smaller update area)
		*	d. index and mask ok, then enable pipe to display it
		***********************************************************************************************/
		memset(&pinfo, 0, sizeof(pinfo));
		pinfo.img = img;
		memcpy(&pinfo.org_update_area, &img->update_area, sizeof(img->update_area));
		pinfo.mode = img->update_mode;
		pinfo.update_order = order;

#ifdef TRACE_INDEX_BUFFER
		if (bsp_disp_get_print_level() == 6) {
			trace_last_buffer(order, manager, &pinfo);
		}
#endif

		do_gettimeofday(&index_stimer);
		
		//calculate index by HW for non-overlap part
		ret = calc_index_for_non_overlap_part(manager, &pinfo);
		if (ret < 0) {
			//TODO: deal with fail
			__wrn("calc index by HW fail, ret=%d\n", ret);
		} else if (ret > 0) {
            //index addr has updated to pipeline manager, switch old index and new index buffer
            manager->index_buffer.cur_index = (manager->index_buffer.cur_index)?0:1;
            pipe_mgr->image_update_end();
            pipe_mgr->wait_for_eink_power_off(pipe_mgr);
            switch (update_master) {
                case 0:
                    if ((0 == pinfo.collision_flag) || (pinfo.img->empty_frame_flag)) {
                        buf_mgr->dequeue_image(buf_mgr, img_node);
                    }
                    break;
                case 1:
                    if ((0 == pinfo.collision_flag) || (pinfo.img->empty_frame_flag)) {
                        buf_mgr->remove_image_from_collision_list(buf_mgr, img_node);
                    }
                    break;
                default:
                    __wrn("unkown update master(%d)\n", update_master);
                    break;
            }
            if ((pipe_mgr->wait_frame_sync_flag == true) && (pinfo.update_order == pipe_mgr->wait_frame_sync_frame)) {
                pipe_mgr->wait_frame_sync_queue = 0;
                complete(&pipe_mgr->wait_frame_sync_comp);
            }
            continue;
        }
        
		ret = pipe_mgr->get_collision_relationship(pipe_mgr,&pinfo);


		/*if (pinfo.collision_flag) {
			img_node->collision_flag = pinfo.collision_flag;
			buf_mgr->add_image_to_collision_list(buf_mgr, img_node);
		}*/

		if (bsp_disp_get_print_level() == 3) {
			pr_info("coll_flag = 0x%x order=%d,mode=0x%x,empty=%d,ORG:(%d,%d)~(%d,%d),OVP:(%d,%d)~(%d,%d),UPD:(%d,%d)~(%d,%d)\n",
					pinfo.collision_flag, order, pinfo.mode, pinfo.img->empty_frame_flag,
					pinfo.org_update_area.x_top, pinfo.org_update_area.y_top,
					pinfo.org_update_area.x_bottom, pinfo.org_update_area.y_bottom,
					pinfo.overlap_area.x_top, pinfo.overlap_area.y_top,
					pinfo.overlap_area.x_bottom, pinfo.overlap_area.y_bottom,
					pinfo.update_area.x_top, pinfo.update_area.y_top,
					pinfo.update_area.x_bottom, pinfo.update_area.y_bottom);
		}

		if (rectangle_is_zero(pinfo.overlap_area) && rectangle_is_zero(pinfo.update_area) &&
			(pinfo.img->empty_frame_flag != true)) {
			__debug("whole area collision, overlap type=%d\n", pinfo.overlap_type);
			if (NULL != pinfo.update_image_mask) {
				vfree(pinfo.update_image_mask);
				pinfo.update_image_mask = NULL;
			}
			continue;
		}


		//__debug("overlap_flag=0x%08x, collision_flag=0x%08x\n", pinfo.overlap_flag, pinfo.collision_flag);
		//calculate index by SW, alloc index memory and free by pipelie_mgr

		//calculate collision relationship in overlap area 
		if (pinfo.overlap_flag) {
			if (NULL == pinfo.update_image_mask) {
				__wrn("update_image_mask is null, something is wrong\n");
			} else {
				if (manager->panel_info.bit_num > EINK_BIT_4) {
					if ((pinfo.mode & 0xff) == EINK_DU_MODE || (pinfo.mode & 0xff) == EINK_A2_MODE) {
						ret = calc_index_for_overlap_part_for_du_5bit(manager, &pinfo, &collision_area);
					} else {
						ret = calc_index_for_overlap_part_for_5bit(manager, &pinfo, &collision_area);
					}
				} else {
					if ((pinfo.mode & 0xff) == EINK_DU_MODE || (pinfo.mode & 0xff) == EINK_A2_MODE)
						ret = calc_index_for_overlap_part_for_du_4bit(manager, &pinfo, &collision_area);
					else
						ret = calc_index_for_overlap_part_for_4bit(manager, &pinfo, &collision_area);
				}
				if (pinfo.collision_flag) {
					__debug("order(%d) collision area(%d,%d)~(%d,%d) in overlap area(%d,%d)~(%d,%d)\n", \
						order, collision_area.x_top, collision_area.y_top, collision_area.x_bottom, collision_area.y_bottom, \
						pinfo.overlap_area.x_top, pinfo.overlap_area.y_top, pinfo.overlap_area.x_bottom, pinfo.overlap_area.y_bottom);
					memcpy(&(pinfo.img->update_area), &collision_area, sizeof(collision_area));
					img_node->collision_flag = pinfo.collision_flag;
					buf_mgr->add_image_to_collision_list(buf_mgr, img_node);
					__debug("add_image_to_collision_list\n");
					if (rect_include_judge(collision_area, pinfo.overlap_area)) {
						pinfo.overlap_type = OVERLAP_WHOLE_COLLISION;
						if ((NULL != pinfo.update_image_mask)) {
							vfree(pinfo.update_image_mask);
							pinfo.update_image_mask = NULL;
						}
						//memset(&pinfo.overlap_area, 0, sizeof(pinfo.overlap_area));
					} else {
						pinfo.overlap_type = OVERLAP_PARTIAL_COLLISION;
					}
					memset(&pinfo.overlap_area, 0, sizeof(pinfo.overlap_area));
				}
			}
		}

		index_vaddr = manager->index_buffer.index_vaddr[manager->index_buffer.cur_index];
		index_buffer_size = manager->index_buffer.index_buf_size;
        cur_update_mode = GET_UPDATE_MODE(pinfo.mode);
        if (manager->panel_info.bit_num == EINK_BIT_5) {
            //do_gettimeofday(&regal_stimer);
            if ((IS_REGAL_MODE(pinfo.mode)) ||
                ((cur_update_mode & 0xff) == EINK_GLR16_MODE) ||
                ((cur_update_mode & 0xff) == EINK_GLD16_MODE))  {
                pr_info("[new regal] regal process index \n");
                kernel_neon_begin();
                if (manager->regal_index_func_neon != NULL)
                    manager->regal_index_func_neon((u16 *)index_vaddr, manager->panel_info.width, manager->panel_info.height);
                else
                    eink_process_index_neon((u16 *)index_vaddr, manager->panel_info.width, manager->panel_info.height);
                kernel_neon_end();
            } else if ((cur_update_mode & 0xff) == EINK_GLK16_MODE){
                kernel_neon_begin();
                if (manager->eclipse_index_func_neon != NULL)
                    manager->eclipse_index_func_neon((u16 *)index_vaddr, manager->panel_info.width, manager->panel_info.height);
                else
                    eink_process_index_black_neon((u16 *)index_vaddr, manager->panel_info.width, manager->panel_info.height);
                kernel_neon_end();
            }
            /*do_gettimeofday(&regal_etimer);
            printk("regal image take %d us\n", get_delt_us_timer(regal_stimer, regal_etimer));*/
        }
		disp_eink_cache_sync(index_vaddr, (index_vaddr + index_buffer_size));
		working_buffer_vaddr = manager->work_buffer.workbuffer_vaddr;
		working_buffer_size = manager->work_buffer.workbuffer_size;
		disp_eink_cache_sync(working_buffer_vaddr, (working_buffer_vaddr + working_buffer_size));
		do_gettimeofday(&index_etimer);

		if (rectangle_is_zero(pinfo.overlap_area) && rectangle_is_zero(pinfo.update_area) &&
			(pinfo.img->empty_frame_flag != true)) {
			if (bsp_disp_get_print_level() == 3) {
				pr_info("order=%d, overlap part is collision, overlap type=%d\n", order, pinfo.overlap_type);
			}
			if (pinfo.update_image_mask != NULL) {
				vfree(pinfo.update_image_mask);
				pinfo.update_image_mask = NULL;
			}
			continue;
		}

		//whole screen update too fast
		do_gettimeofday(&cur_update_timer);
		delt_update_time = get_delt_ms_timer(manager->last_update_timer, cur_update_timer);
		cur_update_mode = GET_UPDATE_MODE(pinfo.mode);
		/*if ((delt_update_time < 800) && (EINK_GC16_MODE == cur_update_mode) &&
			(EINK_GC16_MODE == manager->last_update_mode) &&
			(is_whole_screen_update(manager->panel_info, manager->last_update_area_info)) &&
			(is_whole_screen_update(manager->panel_info, pinfo.update_area))) {
			pinfo.mode = EINK_GU16_MODE;
		}*/
		
		/* update mode map buffer, if using A2 mode check map update mode */
		mode_buffer_vaddr = manager->mode_buffer.modebuffer_vaddr;
		mode_buffer_size = manager->mode_buffer.modebuffer_size;
		if (update_mode_mapping_check(mode_buffer_vaddr, img->size.width, cur_update_mode, pinfo.update_area, 1) == true) {
			img->update_mode = (img->update_mode & 0xffff0000) | EINK_DU_MODE;
			pinfo.mode = (pinfo.mode & 0xffff0000) | EINK_DU_MODE;
		}
		
		disp_eink_cache_sync(mode_buffer_vaddr, (mode_buffer_vaddr + mode_buffer_size));
		
		manager->last_update_mode = GET_UPDATE_MODE(pinfo.mode);
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
				pipe_id = pipe_mgr->request_pipe(pipe_mgr);
				if (pipe_id < 0) {
					fail_request_cnt++;
					pipe_mgr->image_update_end();
					msleep(5);
					continue;
				}
			}
			
			// update collision when current pipe finish update
			if (pinfo.collision_flag)
				img_node->collision_flag |= (1 << pipe_id);

			pipe_mgr->init_pipe(pipe_mgr,pipe_id, pinfo);

			index_paddr = manager->index_buffer.index_paddr[manager->index_buffer.cur_index];
			index_vaddr = manager->index_buffer.index_vaddr[manager->index_buffer.cur_index];

			ret = pipe_mgr->update_and_active_pipe(pipe_mgr, pipe_id, index_paddr, index_vaddr, pinfo, &frame);

			if (ret) {
				pipe_mgr->image_update_end();
				__wrn("[DEBUG]hw pipe(%d) too late to insert(t=%d,d=%d,f=%d), wait for power off\n", 
					pipe_id, frame.all_total_frames, frame.all_decode_frame_cnt, frame.all_fresh_frame_cnt);
				pipe_mgr->wait_for_eink_power_off(pipe_mgr);
				fail_init_cnt++;
			}
		}while((0 != ret) && (fail_start_cnt < START_LOCK_FAIL_MAX_CNT) && (fail_request_cnt < REQUST_PIPE_FAIL_MAX_CNT) && (fail_init_cnt < INIT_PIPE_FAIL_MAX_CNT));

		__debug("order=%d, update pipe(%d) ok, wait for power on\n", order, pipe_id);
		
		if (fail_init_cnt < INIT_PIPE_FAIL_MAX_CNT) {
			//init pipe ok, and wait for eink power on
			ret = pipe_mgr->wait_for_eink_power_on(pipe_mgr);
			__debug("wait for power on %s\n", (ret==0)?"ok":"timeout");
		} else {
			__wrn("active pipe fail for a long time, ignore this update\n");
		}
		
		//index addr has updated to pipeline manager, switch old index and new index buffer
		manager->index_buffer.cur_index = (manager->index_buffer.cur_index)?0:1;

		pipe_mgr->image_update_end();
		
		//free image node
		//set_current_state(TASK_INTERRUPTIBLE);
		switch (update_master) {
			case 0:
				if ((0 == pinfo.collision_flag) || (pinfo.img->empty_frame_flag)) {
					if (pipe_mgr->wait_frame_sync_flag && pipe_mgr->wait_frame_sync_frame == order) {
						if (pipe_mgr->wait_frame_sync_queue & (1 << BUF_IMG_QUQUE))
							pipe_mgr->wait_frame_sync_queue = (1 << PIPE_IMG_HW_PIPE_QUEUE);
					}
					buf_mgr->dequeue_image(buf_mgr, img_node);
				}
				break;
			case 1:
				if ((0 == pinfo.collision_flag) || (pinfo.img->empty_frame_flag)) {
					if (pipe_mgr->wait_frame_sync_flag && pipe_mgr->wait_frame_sync_frame == order) {
						if (pipe_mgr->wait_frame_sync_queue & (1 << BUF_IMG_COLL_QUEUE))
							pipe_mgr->wait_frame_sync_queue = (1 << PIPE_IMG_HW_PIPE_QUEUE);
					}
					buf_mgr->remove_image_from_collision_list(buf_mgr, img_node);
				}
				break;
			default:
				__wrn("unkown update master(%d)\n", update_master);
				break;
		}
	}

	if(bsp_disp_get_eink_method() == 1){
		__set_current_state(TASK_RUNNING);
	}

	return ret;
}


static int eink_get_temperature_thread(void *parg)
{
	struct disp_eink_manager* manager;
	unsigned int temperature = 0;

	manager = (struct disp_eink_manager*) parg;
	for (;;) {
		if (kthread_should_stop()) {
			break;
		}

		//temperature = get_temperature(manager);

		#if defined(CONFIG_TPS65185_VCOM) || defined(CONFIG_TPS65185_VCOM_MODULE) || defined(CONFIG_SY7636_VCOM) || \
			defined(CONFIG_JD9930_VCOM)
		/*NOTIC: make sure you board has NTC*/
		tps65185_temperature_get(&temperature);
		if(temperature == -1) {
			//__debug("%s: get temperature=%d fail from tps65185\n", __func__, temperature);
		#if defined(CONFIG_JD9930_VCOM)
			jd9930_temperature_get(&temperature);
			if (temperature == -1) {
				//__debug("%s: get temperature=%d fail from tps65185\n", __func__, temperature);
			} else {
				manager->eink_panel_temperature = temperature;
				//__debug("%s: get temperature=%d degree from tps65185\n", __func__, manager->eink_panel_temperature);
			}
		#endif
		}else {
			manager->eink_panel_temperature = temperature;
			//__debug("%s: get temperature=%d degree from tps65185\n", __func__, manager->eink_panel_temperature);
		}
		#else 
		manager->eink_panel_temperature = 25;
		//__debug("%s: set default temperature to %d degree\n", __func__, manager->eink_panel_temperature);
		#endif
		
		set_current_state(TASK_INTERRUPTIBLE);
		if (kthread_should_stop()) {
			break;
		}
		schedule_timeout(1000);
	}

	return 0;
}

s32 eink_waveform_init(struct disp_eink_manager *manager)
{
	int ret = 0;
	
	if (false == manager->waveform_init_flag) {
		ret = disp_al_init_waveform(manager->waveform_path);
		if (ret) {
			__wrn("fail to load setting wavefile(%s),try default wavefile(%s)\n", manager->waveform_path, DEFAULT_WAVEFORM_PATH);
			ret = disp_al_init_waveform(DEFAULT_WAVEFORM_PATH);
			if (0 == ret) {
				memcpy(manager->waveform_path, DEFAULT_WAVEFORM_PATH, strnlen(DEFAULT_WAVEFORM_PATH, MAX_PATH_LEN));
				__debug("get default wavefile(%s) ok\n", DEFAULT_WAVEFORM_PATH);
				manager->waveform_init_flag = true;
			} else {
				__wrn("both setting wavefile(%s) and default wavefile(%s) cannot found\n", manager->waveform_path, DEFAULT_WAVEFORM_PATH);
				disp_al_free_waveform();
			}
			return ret;
		} else {
			manager->waveform_init_flag = true;
		}
	}

	if (false == manager->vcom_init_flag){
		manager->vcom_voltage = __get_vcom_config_value(manager,manager->vcom_file_path);
	}

	return ret;
}

s32 eink_update_image(struct disp_eink_manager *manager,
				struct disp_layer_config_inner *config,
				unsigned int layer_num,
				enum eink_update_mode mode,
				struct area_info update_area,
				unsigned int g2d_flag,
				unsigned int cfa_use)
{
	int ret = 0;
	
	mutex_lock(&manager->standby_lock);
	if (manager->suspend_state || manager->shutdown_state) {		
		mutex_unlock(&manager->standby_lock);
		__wrn("eink is suspend(%d) or shutdown(%d), don't update\n", manager->suspend_state, manager->shutdown_state);
		return 0;
	}
	mutex_unlock(&manager->standby_lock);
	
	__debug("new update, mode=0x%x, area(%d,%d)~(%d,%d)\n", mode, 
		update_area.x_top, update_area.y_top, update_area.x_bottom, update_area.y_bottom);
	
	//TODO: for test
	//mode = EINK_AUTO_MODE;
	
	ret = manager->enable(manager);
	if (ret) {
		__wrn("enable eink manager fail, ret=%d\n", ret);
		return -2;
	}
	
	ret = manager->buffer_mgr->queue_image(manager->buffer_mgr, config,
							layer_num, mode, update_area, g2d_flag, cfa_use);
	return ret;
}

s32 eink_set_global_clean_counter(struct disp_eink_manager *manager, u32 cnt)
{
	struct eink_buffer_manager *buf_mgr = NULL;
	s32 ret = -1;
	
	if (manager == NULL) {
		__wrn("eink manager is null\n");
		return -1;
	}

	buf_mgr = manager->buffer_mgr;
	if (NULL == buf_mgr) {
		__wrn("buf_mgr is null\n");
		return -2;
	}

	if (manager->buffer_mgr->set_global_clean_counter)
		ret = manager->buffer_mgr->set_global_clean_counter(buf_mgr, cnt);
	
	return ret;
}

s32 eink_set_gamma(struct disp_eink_manager *manager, u32 *gamma_tbl)
{
	struct eink_buffer_manager *buf_mgr = NULL;
	s32 ret = -1;

	
	if (manager == NULL) {
		__wrn("eink manager is null\n");
		return -1;
	}
	
	buf_mgr = manager->buffer_mgr;
	if(NULL == buf_mgr) {
		__wrn("buf_mgr is null\n");
		return -1;
	}
	
	if (manager->buffer_mgr->set_gamma_correction) {
		mutex_lock(&manager->enable_lock);
		ret = manager->buffer_mgr->set_gamma_correction(buf_mgr, gamma_tbl);
		mutex_unlock(&manager->enable_lock);
	}
	
	return ret;
  
}

s32 eink_set_bg_setting(struct disp_eink_manager *manager, struct y8_area_info *background_setting)
{
	struct eink_buffer_manager *buf_mgr = NULL;
	s32 ret = -1;
	
	buf_mgr = manager->buffer_mgr;
	if (NULL == buf_mgr) {
		__wrn("buf_mgr is null\n");
		return -1;
	}
	
	if (buf_mgr->set_bg_setting) {
		mutex_lock(&manager->enable_lock);
		ret = buf_mgr->set_bg_setting(buf_mgr, background_setting);
		mutex_unlock(&manager->enable_lock);
	}
	
	return ret;
}

s32 eink_set_bg_enable(struct disp_eink_manager *manager, bool enable, int cfa_use)
{
	struct eink_buffer_manager *buf_mgr = NULL;
	s32 ret = -1;
	
	buf_mgr = manager->buffer_mgr;
	if (NULL == buf_mgr) {
		__wrn("buf_mgr is null\n");
		return -1;
	}
	
	if (buf_mgr->set_bg_setting) {
		mutex_lock(&manager->enable_lock);
		ret = buf_mgr->set_bg_enable(buf_mgr, enable, cfa_use);
		mutex_unlock(&manager->enable_lock);
	}
	
	return ret;
}

s32 eink_wait_before_lcd_int_complete(struct disp_eink_manager *manager)
{
	struct pipeline_manager * pipeline_mgr = NULL;
	s32 ret = -1;
	
	if (gptHWCFG->m_val.bCustomer == 9)
		return 0;
	
	pipeline_mgr = manager->pipeline_mgr;
	if (NULL == pipeline_mgr) {
		__wrn("pipeline_mgr is null\n");
		return -1;
	}
	
    mutex_lock(&manager->enable_lock);
    ret = pipeline_mgr->wait_before_lcd_int_complete(pipeline_mgr);
    mutex_unlock(&manager->enable_lock);

	
	return ret;
}

s32 eink_wait_frame_sync_complete(struct disp_eink_manager *manager, unsigned int frame_ID)
{
	struct pipeline_manager * pipeline_mgr = NULL;
	struct eink_buffer_manager *buf_mgr = NULL;
	
	s32 ret = -1;
	
	buf_mgr = manager->buffer_mgr;
	if (NULL == buf_mgr) {
		__wrn("buf_mgr is null\n");
		return -1;
	}
	
	if ((frame_ID >= buf_mgr->update_order) || (frame_ID == 0)) {
		__wrn("frame_ID: %d is invalid\n", frame_ID);
		return -1;
	}
	
	pipeline_mgr = manager->pipeline_mgr;
	if (NULL == pipeline_mgr) {
		__wrn("pipeline_mgr is null\n");
		return -1;
	}
	
	ret = pipeline_mgr->wait_frame_sync_complete(pipeline_mgr, frame_ID);
	
	return ret;
}

s32 eink_set_update_control(struct disp_eink_manager *manager, bool enable)
{
	struct pipeline_manager * pipeline_mgr = NULL;
	s32 ret = -1;
	
	pipeline_mgr = manager->pipeline_mgr;
	if (NULL == pipeline_mgr) {
		__wrn("pipeline_mgr is null\n");
	}
	
	mutex_lock(&manager->enable_lock);
	ret = pipeline_mgr->update_frame_time_ctrl(pipeline_mgr, enable);
	mutex_unlock(&manager->enable_lock);
	
	return ret;
  
}

s32 eink_set_ntx_handwrite_enable(struct disp_eink_manager *manager, bool enable)
{
	struct pipeline_manager *pipe_mgr = NULL;
	s32 ret = -1;
	
	pipe_mgr = manager->pipeline_mgr;
	
	if (NULL == pipe_mgr) {
		__wrn("pipeline_mgr is null\n");
	}
	
	if (enable == false)
		ret = pipe_mgr->wait_for_eink_power_off(pipe_mgr);
	
	mutex_lock(&manager->enable_lock);
	manager->ntx_handwrite_solution = enable;
	mutex_unlock(&manager->enable_lock);
	
	return 0;
}

s32 eink_set_minimum_pixels_for_dma(struct disp_eink_manager *manager, int minimum_pixels_for_dma)
{
	s32 ret = -1;
	
	ret = manager->eink_wait_for_complete(manager);
	if (ret) {
		__wrn("eink wait for complete timeout\n");
		return 0;
	}
	
	mutex_lock(&manager->enable_lock);
	manager->minimum_pixels_for_dma = minimum_pixels_for_dma;
	mutex_unlock(&manager->enable_lock);
	
	return 0;
}

s32 eink_set_ntx_cfa_cube_file(struct disp_eink_manager *manager, struct cfa_cube_lut *cube_tbl)
{
	struct eink_buffer_manager *buf_mgr = NULL;
	s32 ret = -1;

	if (manager == NULL) {
		__wrn("eink manager is null\n");
		return -1;
	}

	buf_mgr = manager->buffer_mgr;
	if(NULL == buf_mgr) {
		__wrn("buf_mgr is null\n");
		return -1;
	}

	if (manager->buffer_mgr->set_cfa_cube_file) {
		mutex_lock(&manager->enable_lock);
		ret = manager->buffer_mgr->set_cfa_cube_file(buf_mgr, cube_tbl);
		mutex_unlock(&manager->enable_lock);
	}

	return ret;
}

s32 eink_set_force_region_update_enable(struct disp_eink_manager *manager, bool enable)
{
	s32 ret = -1;
	
	ret = manager->eink_wait_for_complete(manager);
	if (ret) {
		__wrn("eink wait for complete timeout\n");
		return 0;
	}
	
	mutex_lock(&manager->enable_lock);
	manager->force_region_update = enable;
	mutex_unlock(&manager->enable_lock);
	
	return 0;
}

s32 eink_set_soft_calc_region_enable(struct disp_eink_manager *manager, bool enable)
{
	s32 ret = -1;
	
	ret = manager->eink_wait_for_complete(manager);
	if (ret) {
		__wrn("eink wait for complete timeout\n");
		return 0;
	}
	
	mutex_lock(&manager->enable_lock);
	manager->soft_calc_region = enable;
	mutex_unlock(&manager->enable_lock);
	
	return 0;
}

s32 eink_set_wait_mode_enable(struct disp_eink_manager *manager, bool enable)
{
	s32 ret = -1;
	
	ret = manager->eink_wait_for_complete(manager);
	if (ret) {
		__wrn("eink wait for complete timeout\n");
		return 0;
	}
	
	mutex_lock(&manager->enable_lock);
	manager->wait_mode = enable;
	mutex_unlock(&manager->enable_lock);
	
	return 0;
}

s32 eink_set_lcd_interrupt_wait_time(struct disp_eink_manager *manager, unsigned int wait_time)
{
	s32 ret = -1;
	
	ret = manager->eink_wait_for_complete(manager);
	if (ret) {
		__wrn("eink wait for complete timeout\n");
		return 0;
	}
	
	mutex_lock(&manager->enable_lock);
	manager->lcd_int_wait_time = wait_time;
	mutex_unlock(&manager->enable_lock);
	
	return 0;
}

s32 eink_set_low_temp_flush_delay(struct disp_eink_manager *manager, unsigned int delay_time)
{
	s32 ret = -1;
	
	ret = manager->eink_wait_for_complete(manager);
	if (ret) {
		__wrn("eink wait for complete timeout\n");
		return 0;
	}
	
	mutex_lock(&manager->enable_lock);
	manager->low_temp_flush_delay = delay_time;
	mutex_unlock(&manager->enable_lock);
	
	return 0;
}

s32 eink_set_nightmode_test_enable(struct disp_eink_manager *manager, bool enable)
{
	s32 ret = -1;
	
	ret = manager->eink_wait_for_complete(manager);
	if (ret) {
		__wrn("eink wait for complete timeout\n");
		return 0;
	}
	
	mutex_lock(&manager->enable_lock);
	manager->nightmode_test_enable = enable;
	mutex_unlock(&manager->enable_lock);
	
	return 0;    
}

#define MAX_WAIT_CO

#define MAX_WAIT_COMPLETE_CNT	1000
int eink_wait_for_complete(struct disp_eink_manager *manager)
{
	struct eink_buffer_manager *buf_mgr = NULL;
	struct pipeline_manager* pipeline_mgr = NULL;
	int ret = -1;
	u32 cnt = 0;
	
	if (NULL == manager) {
		__wrn("eink manager is null\n");
		return -1;
	}

	buf_mgr = manager->buffer_mgr;
	pipeline_mgr = manager->pipeline_mgr;
	if ((NULL == buf_mgr) || (NULL == pipeline_mgr)) {
		__wrn("buf_mgr or pipeline_mgr is null\n");
		return -2;
	}

	__debug("start to wait epdc complete\n");
	while (cnt < MAX_WAIT_COMPLETE_CNT) {
		
		pipeline_mgr->wait_for_eink_power_off(pipeline_mgr);
		
		if ((true == buf_mgr->is_collision_list_empty(buf_mgr)) && 
			(true == buf_mgr->is_empty(buf_mgr)) &&
			(0 == pipeline_mgr->query_used_pipe(pipeline_mgr))) {

			break;
		} else {
			msleep(10);
			__debug("wait for cnt = %d\n", cnt);
		}
		
		cnt++;
	}
	
	if (cnt >= MAX_WAIT_COMPLETE_CNT) {
		__wrn("wait for epdc complete timeout\n");
		ret = -3;
	} else {
		ret = 0;
		__debug("wait for epdc complete ok\n");
	}
	
	return ret;
}

int eink_wait_for_hardpipe_complete(struct disp_eink_manager *manager)
{
	struct eink_buffer_manager *buf_mgr = NULL;
	struct pipeline_manager* pipeline_mgr = NULL;
	int ret = -1;
	u32 cnt = 0;
	
	if (NULL == manager) {
		__wrn("eink manager is null\n");
		return -1;
	}

	buf_mgr = manager->buffer_mgr;
	pipeline_mgr = manager->pipeline_mgr;
	if ((NULL == buf_mgr) || (NULL == pipeline_mgr)) {
		__wrn("buf_mgr or pipeline_mgr is null\n");
		return -2;
	}

	__debug("start to wait epdc complete\n");
	while (cnt < MAX_WAIT_COMPLETE_CNT) {
		
		if ((true == buf_mgr->is_collision_list_empty(buf_mgr)) && 
			(true == buf_mgr->is_empty(buf_mgr)) &&
            (0 == pipeline_mgr->query_used_pipe(pipeline_mgr))) {
			break;
		} else {
			msleep(10);
			__debug("wait for cnt = %d\n", cnt);
		}
		cnt++;
	}
	
	if (cnt >= MAX_WAIT_COMPLETE_CNT) {
		__wrn("wait for epdc complete timeout\n");
		ret = -3;
    } else {
		ret = 0;
		__debug("wait for epdc complete ok\n");
	}
	
	return ret;
}

int eink_wait_for_softpipe_complete(struct disp_eink_manager *manager)
{
	struct eink_buffer_manager *buf_mgr = NULL;
	struct pipeline_manager* pipeline_mgr = NULL;
	int ret = -1;
	u32 cnt = 0;
	
	if (NULL == manager) {
		__wrn("eink manager is null\n");
		return -1;
	}

	buf_mgr = manager->buffer_mgr;
	pipeline_mgr = manager->pipeline_mgr;
	if ((NULL == buf_mgr) || (NULL == pipeline_mgr)) {
		__wrn("buf_mgr or pipeline_mgr is null\n");
		return -2;
	}

	__debug("start to wait epdc complete\n");
	while (cnt < MAX_WAIT_COMPLETE_CNT) {
		
		if ((true == buf_mgr->is_collision_list_empty(buf_mgr)) && 
			(true == buf_mgr->is_empty(buf_mgr)) &&
            (0 == pipeline_mgr->query_used_soft_pipe(pipeline_mgr))) {
			break;
		} else {
			msleep(10);
			__debug("wait for cnt = %d\n", cnt);
		}
		cnt++;
	}
	
	if (cnt >= MAX_WAIT_COMPLETE_CNT) {
		__wrn("wait for epdc complete timeout\n");
		ret = -3;
    } else {
		ret = 0;
		__debug("wait for epdc complete ok\n");
	}
	
	return ret;
}

static int eink_op_skip(struct disp_eink_manager *manager, u32 skip)
{
	return 0;
}

s32 eink_enable(struct disp_eink_manager* manager)
{
	int ret = 0;
	static int first = 1;

	if ((NULL == manager->convert_mgr) || (NULL == manager->idx_mgr) || (NULL == manager->pipeline_mgr)) {
		__wrn("eink manager is not initial\n");
		return -1;
	}

	mutex_lock(&manager->enable_lock);
	if (true == manager->enable_flag) {
		mutex_unlock(&manager->enable_lock);
		return 0;
	}

	ret = manager->convert_mgr->enable(manager->convert_mgr->disp);
	if (0 != ret) {
		__wrn("fail to enable converter(%d)\n", ret);
		goto convert_enable_fail;
	}

	ret = manager->idx_mgr->enable(manager->idx_mgr->disp);
	if (0 != ret) {
		__wrn("fail to enable index mgr(%d)\n", ret);
		goto index_enable_fail;
	}

	if (first) {
		ret = __eink_clk_enable(manager);
		first = 0;
	}

	ret = manager->pipeline_mgr->enable_pipeline_mgr(manager->pipeline_mgr->disp);
	if (0 != ret) {
		__wrn("fail to enable pipe mgr(%d)\n", ret);
		goto pipe_enable_fail;
	}

	manager->enable_flag = true;
	mutex_unlock(&manager->enable_lock);

	return 0;

index_enable_fail:
	manager->convert_mgr->disable(manager->convert_mgr->disp);

convert_enable_fail:
	manager->pipeline_mgr->disable_pipeline_mgr(manager->pipeline_mgr->disp);

pipe_enable_fail:
	manager->enable_flag = false;
	mutex_unlock(&manager->enable_lock);
	
	return ret;
}

s32 eink_disable(struct disp_eink_manager*  manager)
{
	int ret = 0;

	if ((NULL == manager->convert_mgr) || (NULL == manager->idx_mgr) || (NULL == manager->pipeline_mgr)) {
		__wrn("eink manager is not initial\n");
		return -1;
	}

	mutex_lock(&manager->enable_lock);
	if (false == manager->enable_flag) {
		mutex_unlock(&manager->enable_lock);
		return 0;
	}

	ret = manager->convert_mgr->disable(manager->convert_mgr->disp);
	if (0 != ret) {
		__wrn("fail to disable converter(%d)\n", ret);
		goto convert_disable_fail;
	}

	ret = manager->idx_mgr->disable(manager->idx_mgr->disp);
	if (0 != ret) {
		__wrn("fail to disable index mgr(%d)\n", ret);
		goto index_disable_fail;
	}

	ret = manager->pipeline_mgr->disable_pipeline_mgr(manager->pipeline_mgr->disp);
	if (0 != ret) {
		__wrn("fail to disable pipe(%d)\n", ret);
		goto pipe_disable_fail;
	}
	
	manager->enable_flag = false;
	mutex_unlock(&manager->enable_lock);

	return 0;

pipe_disable_fail:
	manager->idx_mgr->enable(manager->idx_mgr->disp);

index_disable_fail:
	manager->convert_mgr->enable(manager->convert_mgr->disp);

convert_disable_fail:
	manager->enable_flag = true;
	mutex_unlock(&manager->enable_lock);
	
	return ret;

}

void eink_shutdown(struct disp_eink_manager *manager)
{
	int ret = 0;
	struct eink_buffer_manager *buf_mgr = NULL;
	struct pipeline_manager *pipeline_mgr = NULL;
	bool empty = false;
	u32 no_empty_cnt = 0;

	mutex_lock(&manager->standby_lock);
	if (manager->shutdown_state) {
		mutex_unlock(&manager->standby_lock);
		__wrn("eink has already been shutdown\n");
		return;
	}

	buf_mgr = manager->buffer_mgr;
	pipeline_mgr = manager->pipeline_mgr;
	
	if (buf_mgr != NULL) {
		do {
			empty = buf_mgr->is_collision_list_empty(buf_mgr) && buf_mgr->is_empty(buf_mgr);
			if (false == empty) {
				no_empty_cnt++;
			}
			msleep(5);
		} while ((false == empty) && (no_empty_cnt < 2000));	//wait for 10 second
		
		no_empty_cnt = 0;
		do {
			if ((!list_empty(&pipeline_mgr->list.used_list)) || (!list_empty(&pipeline_mgr->list.soft_pipe_used_list)))
				no_empty_cnt++;
			msleep(5);
		} while ((false == empty) && (no_empty_cnt < 2000));	//wait for 10 second
	}
	
	if (no_empty_cnt >= 2000) {
		__wrn("[%s]:wait buf empty timeout, time=%d ms\n", __func__, (5*no_empty_cnt));
	}
	
	
	if (pipeline_mgr != NULL) {
		ret = pipeline_mgr->wait_for_eink_power_off(pipeline_mgr);
	}
	
	if (manager->pixel_process_task) {
		kthread_stop(manager->pixel_process_task);
		__wrn("stop pixel_process_task\n");
		manager->pixel_process_task = NULL;
	}

	__eink_clk_disable(manager);

	disp_sys_disable_irq(manager->irq_no);

	/* disable eink engine. */
	ret = manager->disable(manager);
	if (ret == 0) {
		manager->shutdown_state = 1;
	} else {
		__wrn("fail to disable ee when shutdown, ret=%d\n", ret);
	}
	mutex_unlock(&manager->standby_lock);
	
	return;
}

int eink_set_temperature(struct disp_eink_manager *manager, unsigned int temp)
{
	s32 ret = -1;

	if (manager) {
		manager->eink_panel_temperature = temp;
		ret = 0;
	} else
		__wrn("eink manager is null\n");

	return ret;
}

static unsigned long gLastGetTemperatureTime;
static s32 gTemperature;
unsigned int  eink_get_temperature(struct disp_eink_manager *manager)
{
	s32 temp = 28;
	u32 timeout = 60000; //1 min
	int ret = 0;

	if (manager) {
		if (gLastGetTemperatureTime && time_before(jiffies, gLastGetTemperatureTime + msecs_to_jiffies(timeout))) {
			__debug("No need to get pmic temperture.\n");
		} else {
			ret = tps65185_temperature_get(&gTemperature);
#if defined(CONFIG_JD9930_VCOM)
			if (ret == -1)
				jd9930_temperature_get(&gTemperature);
#endif
			gLastGetTemperatureTime = jiffies;
		}
		
		manager->eink_panel_temperature = gTemperature;
	} else
		__wrn("eink manager is null\n");
	
	temp = gTemperature;

	return temp;
}

s32 eink_resume(struct disp_eink_manager* manager)
{
	int ret = 0;

	mutex_lock(&manager->standby_lock);
	if (0 == manager->suspend_state) {
		mutex_unlock(&manager->standby_lock);
		__wrn("eink has already resume\n");
		return 0;
	}

	/*if (NULL == manager->get_temperature_task) {
		manager->get_temperature_task = kthread_create(eink_get_temperature_thread, (void *)manager, "get temperature proc");
		if (IS_ERR_OR_NULL(manager->get_temperature_task)) {
			__wrn("create getting temperature of eink thread fail!\n");
			ret = PTR_ERR(manager->get_temperature_task);
			return ret;
		} else {
			ret = wake_up_process(manager->get_temperature_task);
			__wrn("resume get_temperature_task\n");
		}
	}*/
	
	/* register eink irq */
	ret = __eink_clk_enable(manager);
	disp_sys_enable_irq(manager->irq_no);

	//no need to enable eink engine here, it will be enable when update image
	manager->suspend_state = 0;	
	
	mutex_unlock(&manager->standby_lock);

	return ret;
}

s32 eink_suspend(struct disp_eink_manager* manager)
{
	int ret = 0;
	struct eink_buffer_manager *buf_mgr = NULL;
	struct pipeline_manager* pipeline_mgr = NULL;
	bool empty = false;
	u32 no_empty_cnt = 0;

	mutex_lock(&manager->standby_lock);
	if (manager->suspend_state) {
		mutex_unlock(&manager->standby_lock);
		__wrn("eink has already been suspend\n");
		return 0;
	}

	buf_mgr = manager->buffer_mgr;
	if (NULL != buf_mgr) {
		do {
			empty = buf_mgr->is_collision_list_empty(buf_mgr) && buf_mgr->is_empty(buf_mgr);
			if (false == empty) {
				no_empty_cnt++;
			}
			msleep(5);
		}while((false == empty) && (no_empty_cnt < 2000));	//wait for 10 second
	}

	if (no_empty_cnt >= 2000) {
		__wrn("wait for buffer empty timeout, time=%d ms\n", (5*no_empty_cnt));
	}

	pipeline_mgr = manager->pipeline_mgr;
	if (NULL != pipeline_mgr) {
		ret = pipeline_mgr->wait_for_eink_power_off(pipeline_mgr);
	}

	/*if (manager->get_temperature_task) {
		kthread_stop(manager->get_temperature_task);
		__wrn("stop get_temperature_task\n");
		manager->get_temperature_task = NULL;
	}*/

	__eink_clk_disable(manager);

	disp_sys_disable_irq(manager->irq_no);

	/* disable eink engine. */
	ret = manager->disable(manager);
	if (0 == ret) {
		manager->suspend_state = 1;
	} else {
		__wrn("fail to disable ee when suspend, ret=%d\n", ret);
	}
	mutex_unlock(&manager->standby_lock);
	
	return ret;
}

int disp_get_eink_resolution(unsigned int disp, unsigned int *xres, unsigned int *yres)
{
	struct disp_eink_manager *mgr = disp_get_eink_manager(disp);

	if (NULL == mgr) {
		__wrn("get eink manager fail, disp=%d\n", disp);
		return -1;
	}
	
	*xres = mgr->panel_info.width;
	*yres = mgr->panel_info.height;
	
	return 0;
}

void dump_pixels(char * title, void *pixels, int x_offset, int y_offset, int x_length, int y_length, int stride,int byteperpixel) {
	int offset = 0;
    int region_size = x_length * y_length;
	//char *buf = (char) vmalloc(region_size * 3);
    char buf[4096];
    //memset(buf, 0 ,sizeof(char) * 8192);

    printk("%s: p:%p\n",title, pixels);

    int i = 0,j = 0;
	for (j = y_offset; j < y_offset + y_length; j++) {
		printk("j = %d\n",j);
		//offset = j * finfo.line_length;
		offset = j * stride;
		char * current_byte = (char *)pixels + offset;
		int len = 0;
        int print_count = 0;
		for (i = x_offset; i < x_offset + x_length; i++) {
			//len += sprintf(buf+len, "i:%d %x%x ",i,current_byte[2*i], current_byte[2*i + 1]);
            if ( print_count % 10 == 0 ) 
			    len += sprintf(buf+len, "i:(%d) ",i);

            if ( byteperpixel == 1 )
    			len += sprintf(buf+len, "%02x ",current_byte[i]);
            else if ( byteperpixel == 2 )
    			len += sprintf(buf+len, "%02x%02x ",current_byte[2*i + 1], current_byte[2*i]);
            else if ( byteperpixel == 4 )
    			len += sprintf(buf+len, "%02x%02x%02x%02x ",current_byte[4*i +3], current_byte[4*i+2], current_byte[4*i+1], current_byte[4*i]);


            print_count++;
		}
		printk("%s\n",buf);
	}

}

static int eink_disp_mode0(struct disp_eink_manager* manager)
{
	int ret = 0, layer_num = 0;
	struct area_info update_area;
	g2d_blt_flags_h g2d_flag;
	u32 cfa_use = 0;
	
	update_area.x_top = 0;
	update_area.y_top = 0;
	update_area.x_bottom = manager->buffer_mgr->width - 1;
	update_area.y_bottom = manager->buffer_mgr->height - 1;
	g2d_flag = G2D_BLT_NONE_H;

	ret = manager->enable(manager);
	if (ret) {
		__wrn("enable eink manager fail, ret=%d\n", ret);
		return -2;
	}
	
	ret = manager->buffer_mgr->queue_image(manager->buffer_mgr, NULL, layer_num, EINK_INIT_MODE, update_area, g2d_flag, cfa_use);
	
	ret = manager->eink_wait_for_complete(manager);
	
	
	return ret;
  
}

int register_libeink_callback_func(struct __eink_libeink *libeink)
{
	struct eink_buffer_manager *buf_mgr = NULL;
	struct disp_eink_manager *mgr = disp_get_eink_manager(0);
	struct __eink_libeink *reg_libeink;
	int libeink_version = 0, i, ret = 0;;
	
	buf_mgr = mgr->buffer_mgr;
	

	for (i = 0; i < REGISTER_LIBEINK_MAX; i++) {
		reg_libeink = &eink_libeink_array[i];
		if (reg_libeink->eink_init == NULL) {
			memcpy(&reg_libeink->name, libeink->name, sizeof(libeink->name));
			reg_libeink->eink_init = libeink->eink_init;
			reg_libeink->regal_func = libeink->regal_func;
			reg_libeink->regal_index_func = libeink->regal_index_func;
			reg_libeink->regal_index_func_neon = libeink->regal_index_func_neon;
			reg_libeink->eclipse_index_func_neon = libeink->eclipse_index_func_neon;
			reg_libeink->get_libeink_version = libeink->get_libeink_version;
			break;
		}
	}
	
	if (strcmp(mgr->panel_info.name, libeink->name) == 0) {
		mutex_lock(&buf_mgr->mlock);
		libeink_version = reg_libeink->get_libeink_version();
		printk("%s panel match eink library version = 0x%x\n", &reg_libeink->name, libeink_version);
		/* set function and Eink init */
		mgr->eink_init = reg_libeink->eink_init;
		mgr->regal_func = reg_libeink->regal_func;
		mgr->regal_index_func = reg_libeink->regal_index_func;
		mgr->regal_index_func_neon = reg_libeink->regal_index_func_neon;
		mgr->eclipse_index_func_neon = reg_libeink->eclipse_index_func_neon;
		
		ret = mgr->eink_init(get_waveform_paddr());
		if(ret) {
			printk("libeink init failed, please check libenik!\n");
		} else {
			ret = regal_waveform_init();
			if (ret) {
				printk("waveform can't init, please check waveform file!\n");
			}
		}
		mutex_unlock(&buf_mgr->mlock);
	} 
	
#if 0 //debug use
	for (i = 0; i < REGISTER_LIBEINK_MAX; i++) {
		reg_libeink = &eink_libeink_array[i];
		if (reg_libeink->eink_init != NULL) {
			libeink_version = reg_libeink->get_libeink_version();
			printk("register %s panel eink library version = 0x%x\n", &reg_libeink->name, libeink_version);
		}
	}
#endif
	
	return 0;    
}
EXPORT_SYMBOL(register_libeink_callback_func);

int unregister_libeink_callback_func(struct __eink_libeink *libeink)
{
	struct eink_buffer_manager *buf_mgr = NULL;
	struct disp_eink_manager *mgr = disp_get_eink_manager(0);
	int libeink_version = 0, i;
	struct __eink_libeink *reg_libeink;
    
	buf_mgr = mgr->buffer_mgr;
    
	if (strcmp(mgr->panel_info.name, libeink->name) == 0) {
		printk("%s panel remove match eink library\n");
		/* set function and Eink init */
		mgr->eink_init = NULL;
		mgr->regal_func = NULL;
		mgr->regal_index_func = NULL;
		mgr->regal_index_func_neon = NULL;
		mgr->eclipse_index_func_neon = NULL;
	} 
	
	for (i = 0; i < REGISTER_LIBEINK_MAX; i++) {
		reg_libeink = &eink_libeink_array[i];
		if (reg_libeink->eink_init == NULL)
			continue;
		
		if (strcmp(reg_libeink->name, libeink->name) == 0) {
			mutex_lock(&buf_mgr->mlock);
			memset(reg_libeink->name, 0, sizeof(reg_libeink->name));
			reg_libeink->eink_init = NULL;
			reg_libeink->regal_func = NULL;
			reg_libeink->regal_index_func = NULL;
			reg_libeink->regal_index_func_neon = NULL;
			reg_libeink->eclipse_index_func_neon = NULL;
			reg_libeink->get_libeink_version = NULL;
			
			regal_waveform_deinit();
			mutex_unlock(&buf_mgr->mlock);
			break;
		}
	}
	
#if 0 //debug use
	for (i = 0; i < REGISTER_LIBEINK_MAX; i++) {
		reg_libeink = &eink_libeink_array[i];
		if (reg_libeink->eink_init != NULL) {
			libeink_version = reg_libeink->get_libeink_version();
			printk("register %s panel eink library version = 0x%x\n", &reg_libeink->name, libeink_version);
		}
	}
#endif
	
	return 0;    
}
EXPORT_SYMBOL(unregister_libeink_callback_func);

int disp_init_eink(struct disp_bsp_init_para * para)
{
	int ret = 0;
	int i = 0, j = 0;
	unsigned int  disp;
	unsigned long image_buf_size;
	unsigned int wavedata_buf_size, hsync, vsync;
	struct eink_panel_info panel_info;
	struct disp_eink_manager* manager = NULL;
	struct eink_index_buf *index_buffer;
	struct eink_workbuffer *work_buffer;
	struct eink_mode_mapbuffer *mode_buffer;
	unsigned int chipid = 0;
	int wd_buf_id = 0;

	__inf("Eink driver version %s%s\n", EINK_DRIVER_VERSION, EINK_VERSION_APPEND_INFO);

	ret = sunxi_efuse_readn(EFUSE_CHIPID_NAME, &chipid, sizeof(chipid));
	if (ret != 0) {
		__wrn("fail to get id\n");
		return -1;
	} else {
		__debug("id: %08x\n", chipid);
		if ((0x1c00 == (chipid & 0xffff)) || (0x0447 == (chipid & 0xffff))) {
			__debug("id check ok\n");
		} else {
			__wrn("id check fail\n");
			return -2;
		}
	}

	eink_manager = (struct disp_eink_manager *)disp_sys_malloc(sizeof(struct disp_eink_manager) * MAX_EINK_ENGINE);
	if(NULL == eink_manager) {
		__wrn("malloc eink manager memory fail!\n");
		ret =  -ENOMEM;
		goto manager_malloc_err;
	}
	memset(eink_manager, 0, sizeof(struct disp_eink_manager) * MAX_EINK_ENGINE);

	for(disp=0; disp<MAX_EINK_ENGINE; disp++) {
		/* get sysconfig and config eink_param */
		manager = &eink_manager[disp];
		
		__eink_get_sys_config(disp, &panel_info);		
		__print_panel_info(disp, panel_info);

		memcpy(&manager->panel_info, &panel_info, sizeof(panel_info));
		manager->disp = disp;
		manager->mgr = disp_get_layer_manager(disp);
		manager->eink_update = eink_update_image;
		manager->eink_wait_for_complete = eink_wait_for_complete;
		manager->eink_wait_for_hardpipe_complete = eink_wait_for_hardpipe_complete;
		manager->eink_wait_for_softpipe_complete = eink_wait_for_softpipe_complete;
		manager->eink_set_global_clean_counter = eink_set_global_clean_counter;
		manager->enable = eink_enable;
		manager->disable = eink_disable;
		manager->shutdown = eink_shutdown;
		manager->resume = eink_resume;
		manager->suspend = eink_suspend;
		manager->set_temperature = eink_set_temperature;	//for debug
		manager->get_temperature = eink_get_temperature;
		manager->eink_panel_temperature = 25;
		manager->waveform_init_flag = false;
		manager->eink_set_gamma = eink_set_gamma;
		manager->eink_set_ntx_cfa_cube_file = eink_set_ntx_cfa_cube_file;
		
		manager->eink_set_bg_setting = eink_set_bg_setting;
		manager->eink_set_bg_enable =  eink_set_bg_enable;
		manager->eink_wait_before_lcd_int_complete = eink_wait_before_lcd_int_complete;
		manager->eink_set_update_control = eink_set_update_control;
		manager->eink_wait_frame_sync_complete = eink_wait_frame_sync_complete;
		manager->eink_set_ntx_handwrite_enable = eink_set_ntx_handwrite_enable;
		manager->eink_set_minimum_pixels_for_dma = eink_set_minimum_pixels_for_dma;
		manager->ntx_handwrite_solution = false;
		manager->minimum_pixels_for_dma = 100;
		
		if (gptHWCFG->m_val.bUIStyle == 2)
			manager->force_region_update = false;
		else
			manager->force_region_update = true;
		manager->eink_set_force_region_update_enable = eink_set_force_region_update_enable;
		manager->nightmode_test_enable = false;
		manager->eink_set_nightmode_test_enable = eink_set_nightmode_test_enable;
		manager->soft_calc_region = false;
		manager->eink_set_soft_calc_region_enable = eink_set_soft_calc_region_enable;
        
		/* for use insmod panel eink library use */
		manager->eink_init = NULL;
		manager->regal_func = NULL;
		manager->regal_index_func = NULL;
		manager->regal_index_func_neon = NULL;
		manager->eclipse_index_func_neon = NULL;
		
		/* continue flush wait mode: 
		 * 		true  : Will wait previous update fresh finsh
		 * 		false : Keep create pipe to fresh, collision behavior will active
		 */
		if (gptHWCFG->m_val.bUIStyle == 2 || manager->panel_info.bit_num <= EINK_BIT_4)
			manager->wait_mode = true;
		else
			manager->wait_mode = false;
		manager->eink_set_wait_mode_enable = eink_set_wait_mode_enable;
		
		manager->eink_init_mode_flush = eink_disp_mode0;
        
        manager->eink_get_working_buffer_data = eink_get_working_buffer_data;
        
        manager->eink_set_lcd_interrupt_wait_time = eink_set_lcd_interrupt_wait_time;
        manager->lcd_int_wait_time = __get_lcd_int_wait_time(disp);
        manager->eink_set_low_temp_flush_delay = eink_set_low_temp_flush_delay;
        manager->low_temp_flush_delay = 500;
		
		manager->enable_flag = false;
		manager->eink_clk = para->mclk[DISP_MOD_EINK];
		manager->edma_clk = para->mclk[DISP_MOD_EDMA];
		manager->eink_base_addr = (unsigned long)para->reg_base[DISP_MOD_EINK];
		manager->irq_no = para->irq_no[DISP_MOD_EINK];

		__msg("eink_clk: 0x%p; edma_clk: 0x%p; base_addr: 0x%p; irq_no: 0x%x\n",
					manager->eink_clk, manager->edma_clk, (void *)manager->eink_base_addr, manager->irq_no);

		disp_al_set_eink_base(disp, manager->eink_base_addr);

#ifdef SUPPORT_WB
		manager->convert_mgr = disp_get_format_manager(disp);
#else
		manager->convert_mgr = NULL;
#endif
		manager->suspend_state = 0;
		manager->shutdown_state = 0;
		mutex_init(&manager->standby_lock);
		mutex_init(&manager->enable_lock);
		INIT_LIST_HEAD(&manager->collision_list);

		/*register image ring buffer, then fill it to eink manager*/
		ring_buffer_manager_init(manager);

		image_buf_size = manager->panel_info.width * manager->panel_info.height;

		/* initial working buffer */
		work_buffer = &manager->work_buffer;
		work_buffer->workbuffer_size = image_buf_size;
		work_buffer->workbuffer_vaddr = disp_eink_malloc(work_buffer->workbuffer_size, (void *)&work_buffer->workbuffer_paddr);
		if(NULL == work_buffer->workbuffer_vaddr) {
			__wrn("malloc working buffer fail\n");
			ret =  -ENOMEM;
			goto alloc_work_buffer_fail;
		}
		memset(work_buffer->workbuffer_vaddr, 0xff, work_buffer->workbuffer_size);
		        
		/* initial mode buffer */
		mode_buffer = &manager->mode_buffer;
		mode_buffer->modebuffer_size = image_buf_size;
		mode_buffer->modebuffer_vaddr = disp_eink_malloc(mode_buffer->modebuffer_size, (void *)&mode_buffer->modebuffer_paddr);
		if (NULL == work_buffer->workbuffer_vaddr) {
			__wrn("malloc mode map buffer fail\n");
			ret =  -ENOMEM;
			goto alloc_mode_buffer_fail;
		}
		memset(mode_buffer->modebuffer_vaddr, 0x0, mode_buffer->modebuffer_size);
		
		/* initial index buffer */
		index_buffer = &manager->index_buffer;
		memset(index_buffer, 0, sizeof(*index_buffer));
		mutex_init(&index_buffer->index_buf_mlock);
		
		if (manager->panel_info.bit_num < EINK_BIT_5) {
			/* 3bits or 4bits */
			index_buffer->index_buf_size = image_buf_size;
		} else {
			/* 5bits */
			index_buffer->index_buf_size = image_buf_size << 1;
		}

		for (j = 0; j < INDEX_BUFFER_NUM; j++) {
			index_buffer->index_vaddr[j] = disp_eink_malloc(index_buffer->index_buf_size, (void *)&index_buffer->index_paddr[j]);
			if(NULL == index_buffer->index_vaddr[j]) {
				__wrn("malloc index buffer(%d) fail\n", j);
				ret =  -ENOMEM;
				goto alloc_index_buffer_fail;
			}
			memset(index_buffer->index_vaddr[j], 0xff, index_buffer->index_buf_size);
		}


		
		memset(manager->waveform_path, 0, MAX_PATH_LEN);
		__eink_get_waveform_path(disp, manager->waveform_path);

		memset(manager->vcom_file_path, 0, MAX_PATH_LEN);
		__eink_get_vcom_path(disp, manager->vcom_file_path);

		pipeline_manager_init(manager);
		manager->idx_mgr = disp_get_index_manager(manager->disp);

		disp_sys_register_irq(manager->irq_no, 0, __eink_interrupt_proc, (void *)manager, 0, 0);
		disp_sys_enable_irq(manager->irq_no);
		manager->pixel_process_task = kthread_create(eink_pixel_process_thread, (void *)manager, "eink pixel proc");
		if (IS_ERR_OR_NULL(manager->pixel_process_task)) {
			__wrn("create eink pixel process thread fail!\n");
			ret = PTR_ERR(manager->pixel_process_task);
			goto alloc_index_buffer_fail;
		}
		ret = wake_up_process(manager->pixel_process_task);

		/*manager->get_temperature_task = kthread_create(eink_get_temperature_thread, (void *)manager, "get temperature proc");
		if (IS_ERR_OR_NULL(manager->get_temperature_task)) {
			__wrn("create getting temperature of eink thread fail!\n");
			ret = PTR_ERR(manager->get_temperature_task);
			goto alloc_index_buffer_fail;
		}
		ret = wake_up_process(manager->get_temperature_task);*/
	}
	
	ret = eink_waveform_init(manager);
	if (ret) {
		__wrn("loading waveform data fail!\n");
		goto alloc_index_buffer_fail;
	}
	
	eink_libeink_array = (struct __eink_libeink *)disp_sys_malloc(sizeof(struct __eink_libeink) * REGISTER_LIBEINK_MAX);
	
	hrtimer_init( &before_write_edma_hrtimer, CLOCK_MONOTONIC, HRTIMER_MODE_REL );
	before_write_edma_hrtimer.function = &before_write_edma_callback;
	before_write_edma_time = ktime_set( 0, g_write_edma_time );//10ms

	return ret;

alloc_index_buffer_fail:
	for (i = 0; i < MAX_EINK_ENGINE; i++) {
		manager = &eink_manager[disp];
		if (NULL == manager) {
			continue;
		}

		index_buffer = &manager->index_buffer;
		for (j = 0; j < INDEX_BUFFER_NUM; j++) {
			if(NULL != index_buffer->index_vaddr[j]) {
				disp_eink_free(index_buffer->index_vaddr[j], index_buffer->index_paddr[j], index_buffer->index_buf_size);
			}
		}
	}
	
alloc_mode_buffer_fail:
	for (disp = 0; disp < MAX_EINK_ENGINE; disp++) {
		manager = &eink_manager[disp];
		if (NULL == manager) {
			continue;
		}
		
		mode_buffer = &manager->mode_buffer;
		if (mode_buffer->modebuffer_vaddr) {
			disp_eink_free(mode_buffer->modebuffer_vaddr, mode_buffer->modebuffer_paddr, image_buf_size);
		}
	}

alloc_work_buffer_fail:
	for (disp = 0; disp < MAX_EINK_ENGINE; disp++) {
		manager = &eink_manager[disp];
		if (NULL == manager) {
			continue;
		}
		
		work_buffer = &manager->work_buffer;
		if (work_buffer->workbuffer_vaddr) {
			disp_eink_free(work_buffer->workbuffer_vaddr, work_buffer->workbuffer_paddr, image_buf_size);
		}
	}

manager_malloc_err:
	if (eink_manager)
		disp_sys_free(eink_manager);

	return ret;
}

int disp_exit_eink(void)
{
	int disp, i;
	struct disp_eink_manager* manager = NULL;
	struct eink_index_buf *index_buffer;
	struct eink_workbuffer *work_buffer;
	
	for(disp=0; disp<MAX_EINK_ENGINE; disp++) {
		manager = &eink_manager[disp];
		
		if (NULL == manager)
			continue;
		
		/*if (manager->get_temperature_task) {
			kthread_stop(manager->get_temperature_task);
			__wrn("stop get_temperature_task\n");
			manager->get_temperature_task = NULL;
		}*/
		
		if (manager->pixel_process_task) {
			kthread_stop(manager->pixel_process_task);
			__wrn("stop pixel_process_task\n");
			manager->pixel_process_task = NULL;
		}
		
		index_buffer = &manager->index_buffer;
		for (i = 0; i < INDEX_BUFFER_NUM; i++) {
			if(NULL != index_buffer->index_vaddr[i]) {
				disp_eink_free(index_buffer->index_vaddr[i], index_buffer->index_paddr[i], index_buffer->index_buf_size);
			}
		}
		
		work_buffer = &manager->work_buffer;
		if (work_buffer->workbuffer_vaddr) {
			disp_eink_free(work_buffer->workbuffer_vaddr, work_buffer->workbuffer_paddr, work_buffer->workbuffer_size);
		}
	}
  
	return 0;
}
#endif
