#include <linux/module.h>
#include <linux/printk.h>
//#include <linux/types.h>
#include <arm_neon.h>

struct area_info {
	unsigned int x_top;
	unsigned int y_top;
	unsigned int x_bottom;
	unsigned int y_bottom;
};

int neon_argb_to_gray(u8 *src, u8 *dest, void *area, unsigned int x_offset, unsigned int y_offset, unsigned int pitch)
{
	u8 *tmp_src = NULL, *tmp_dest = NULL;
	struct area_info *upd_area = area;
	unsigned int w, h;
	unsigned int src_offset, dst_offset;
	int stride_pixels = 8;
	int neon_bottom, neon_width;
	
	u16 result[8];
	u8 tmp_result[8];
	
	uint16x8_t temp_16x8;
	uint8x8_t gray_8x8;
	uint8x8x4_t argb_8x8x4;
	uint8x8_t r = vdup_n_u8(38);
	uint8x8_t g = vdup_n_u8(75);
	uint8x8_t b = vdup_n_u8(15);
	uint16x8_t alpha = vdupq_n_u16(255 << 8);
	uint8x8_t mask = vdup_n_u8(64);
	
	neon_width = upd_area->x_bottom - upd_area->x_top + 1;
	neon_bottom = ALIGN((neon_width - ((stride_pixels) - 1)), stride_pixels);
	for (h = upd_area->y_top; h <= upd_area->y_bottom; h++) {
		dst_offset = h * pitch + upd_area->x_top ;
		src_offset = ((h - y_offset) * pitch + upd_area->x_top - x_offset)*4;
		tmp_src = src + src_offset;
		tmp_dest = dest + dst_offset;
		for (w = upd_area->x_top; w <= upd_area->x_bottom; w += stride_pixels) {
			argb_8x8x4 = vld4_u8(tmp_src);
			temp_16x8 = vmull_u8(argb_8x8x4.val[1], r);
			temp_16x8 = vmlal_u8(temp_16x8, argb_8x8x4.val[2], g);
			temp_16x8 = vmlal_u8(temp_16x8, argb_8x8x4.val[3], b);
			temp_16x8 = vaddw_u8(temp_16x8, mask);
			gray_8x8 = vshrn_n_u16(temp_16x8, 7);
			vst1_u8(tmp_dest, gray_8x8);
			
			tmp_src += (stride_pixels * 4);
			tmp_dest += stride_pixels;
		}
	}
	
	return 0;
}
EXPORT_SYMBOL(neon_argb_to_gray);

int neon_memcpy(u8 *dst, u8 *src, unsigned int len)
{
	uint8x16_t tmp_src_8x16;
	unsigned int i = 0;

	for (i = 0; i < len; i += 16) {
		tmp_src_8x16 = vld1q_u8((u8 *)(src + i));
		vst1q_u8((u8 *)(dst + i), tmp_src_8x16);
	}
	return i;
}
EXPORT_SYMBOL(neon_memcpy);

bool neon_memcpy_check(u8 *dst, u8 *src, unsigned int len)
{
	bool ret = true;
	unsigned int i = 0;
	u8 *tmp_dst = NULL, *tmp_src = NULL;

	tmp_dst = dst;
	tmp_src = src;
	for (i = 0; i < len; i++) {
		if (*tmp_dst != *tmp_src) {
			ret = false;
			printk("i=%d, src=0x%x, dst=0x%x\n", i, *tmp_src, *tmp_dst);
			break;
		}
		tmp_dst++;
		tmp_src++;
	}

	return ret;
}
EXPORT_SYMBOL(neon_memcpy_check);

/*
combine mask data, 32 byte for one time
src_mask = 32 * u8 byte
dst_mask = 4 *u16  byte
*/
int neon_mask_combine(u16 *dst_mask, char *src_mask)
{
	uint8x8x4_t	src_mask_8x8x4;
	uint8x8_t		dst_mask_8x8;

	unsigned int i = 0;

	/*
		*******Load mask data as this format********
		m28	......		m8		m4		m0
		m29	......		m9		m5		m1
		m30	......		m10	m6		m2
		m31	......		m11	m7		m3
		****************************************

		uint8x8x4_t vld4_u8 (const uint8_t *)
		Form of expected instruction(s): vld4.8 {d0, d1, d2, d3}, [r0]
	*/
	src_mask_8x8x4 = vld4_u8(src_mask);

	/*******************************************************************
		Left shift 0/2/4/6 bit for each line of mask data
		***********************************************
		m28<<0	......		m8<<0		m4<<0		m0<<0
		m29<<2	......		m9<<2		m5<<2		m1<<2
		m30<<4	......		m10<<4	m6<<4		m2<<4
		m31<<6	......		m11<<6	m7<<6		m3<<6
		***********************************************

		shift and insert then together, you can used VADD as well
		******************************************************************
		m4<<0 + m5<<2 + m6<<4 + m7<<6  | m0<<0 + m1<<2 + m2<<4 + m3<<6
		******************************************************************

		uint8x8_t vshl_n_u8 (uint8x8_t, const int)
		Form of expected instruction(s): vshl.i8 d0, d0, #0

		uint8x8_t vsli_n_u8 (uint8x8_t, uint8x8_t, const int)
		Form of expected instruction(s): vsli.8 d0, d0, #0
	*****************************************************************/
	dst_mask_8x8 = vshl_n_u8(src_mask_8x8x4.val[0], 0);
	dst_mask_8x8 = vsli_n_u8(dst_mask_8x8, src_mask_8x8x4.val[1], 2);
	dst_mask_8x8 = vsli_n_u8(dst_mask_8x8, src_mask_8x8x4.val[2], 4);
	dst_mask_8x8 = vsli_n_u8(dst_mask_8x8, src_mask_8x8x4.val[3], 6);

	/*************************************************************
		Store register data to memory, two u8 data combine into one u16 data

		void vst1_u8 (uint8_t *, uint8x8_t)
		Form of expected instruction(s): vst1.8 {d0}, [r0]
	**************************************************************/
	vst1_u8((u8 *)dst_mask, dst_mask_8x8);

	return 32;
}
EXPORT_SYMBOL(neon_mask_combine);

bool neon_mask_combine_check(u16 *neon_mask_result, u8 *src_mask, unsigned int len)
{
	bool ret = true;
	unsigned int i = 0;
	u8 tmp_array[8] = {0};
	u16 *tmp_dst_mask = neon_mask_result;
	u16 tmp_src_mask = 0;

	for (i = 0; i < 32; i += 8) {
		memcpy(tmp_array, (src_mask+i), 8);
		tmp_src_mask = (tmp_array[0]<<0) |
			(tmp_array[1]<<2) |
			(tmp_array[2]<<4) |
			(tmp_array[3]<<6) |
			(tmp_array[4]<<8) |
			(tmp_array[5]<<10) |
			(tmp_array[6]<<12) |
			(tmp_array[7]<<14);

		if (*tmp_dst_mask != tmp_src_mask) {
			ret = false;
			break;
		}
		tmp_dst_mask++;
	}
	return ret;
}
EXPORT_SYMBOL(neon_mask_combine_check);

/*
for 5-bit panel, index data need 10bit(5bit for old data, 5bit for new data), but in DU mode,
old/new data only has two status(black/white), so we can use 2bit to replace them.

src_index_u16: 32 * u16 byte
dst_index_u8: 32 * u8 byte

*/
int neon_index_bit_reduce(u8 *dst_index_u8, u16 *src_index_u16)
{
	int i = 0;
	uint16x8_t src_index_u16x8, tmp1_src_index_u16x8, tmp2_src_index_u16x8;
	uint8x8_t dst_index_u8x8;

	/*
		index data = (old_data << 5) | new_data, old/new data only 5bit is valid

		1. load index data to register

		uint16x8_t vld1q_u16 (const uint16_t *)
		Form of expected instruction(s): vld1.16 {d0, d1}, [r0]

		2. right shift 8 bit of index data, let MSB of old data locat in bit1

		uint16x8_t vshrq_n_u16 (uint16x8_t, const int)
		Form of expected instruction(s): vshr.u16 q0, q0, #0

		3. left shift 11 bit of index data, to clear all of the old_data

		uint16x8_t vshlq_n_u16 (uint16x8_t, const int)
		Form of expected instruction(s): vshl.i16 q0, q0, #0

		4. right shift 15 bit of the result in step 3, and insert to the result of step 2

		uint16x8_t vsriq_n_u16 (uint16x8_t, uint16x8_t, const int)
		Form of expected instruction(s): vsri.16 q0, q0, #0

		5. narrow data from 16bit to 8bit

		uint8x8_t vmovn_u16 (uint16x8_t)
		Form of expected instruction(s): vmovn.i16 d0, q0

		6. restore the result from register to memory

		void vst1_u8 (uint8_t *, uint8x8_t)
		Form of expected instruction(s): vst1.8 {d0}, [r0]
	*/

	for (i = 0; i < 32; i += 8) {
		src_index_u16x8 = vld1q_u16((src_index_u16 + i));
		tmp1_src_index_u16x8 = vshrq_n_u16(src_index_u16x8, 8);
		tmp2_src_index_u16x8 = vshlq_n_u16(src_index_u16x8, 11);
		src_index_u16x8 = vsriq_n_u16(tmp1_src_index_u16x8, tmp2_src_index_u16x8, 15);
		dst_index_u8x8 = vmovn_u16(src_index_u16x8);
		vst1_u8((u8 *)(dst_index_u8 + i), dst_index_u8x8);
	}
	return 32;
}
EXPORT_SYMBOL(neon_index_bit_reduce);


bool neon_index_bit_reduce_check(u8 *neon_index_result, u16 *src_index_u16)
{
	bool ret = true;
	u8 new_data = 0, old_data = 0, index = 0;
	u8 *tmp_neon_index = neon_index_result;
	u16 *tmp_src_index = src_index_u16;
	int i = 0;

	for (i = 0; i < 32; i++) {
		new_data = (*tmp_src_index & 0x1f) >> 4;
		old_data = (*tmp_src_index >> 8) & 0x02;
		index = old_data | new_data;
		if (index != *tmp_neon_index) {
			ret = false;
			break;
		}
		tmp_neon_index++;
		tmp_src_index++;
	}

	return ret;
}
EXPORT_SYMBOL(neon_index_bit_reduce_check);

int neon_lookup_waveform(u16 *wvdata, u8 *index, u8 *waveform)
{
	int i = 0;
	uint8x8_t waveform_8x8, index_8x8;
	uint8x8_t pixel_8x8, wvdata_8x8;
	uint8x8x4_t pixel_8x8x4;
	u8 tmp_data_array[32] = {0};

	waveform_8x8 = vld1_u8(waveform);
	for (i = 0; i < 32; i += 8) {
		index_8x8 = vld1_u8(index + i);
		pixel_8x8 = vtbl1_u8(waveform_8x8, index_8x8);
		vst1_u8(&tmp_data_array[i], pixel_8x8);
	}

	pixel_8x8x4 = vld4_u8(tmp_data_array);

	wvdata_8x8 = vshl_n_u8(pixel_8x8x4.val[0], 0);
	wvdata_8x8 = vsli_n_u8(wvdata_8x8, pixel_8x8x4.val[1], 2);
	wvdata_8x8 = vsli_n_u8(wvdata_8x8, pixel_8x8x4.val[2], 4);
	wvdata_8x8 = vsli_n_u8(wvdata_8x8, pixel_8x8x4.val[3], 6);

	vst1_u8((u8 *)wvdata, wvdata_8x8);

	return 32;
}
EXPORT_SYMBOL(neon_lookup_waveform);

bool neon_lookup_waveform_check(u16 *neon_wvdata_result, u8 *index, u8 *waveform)
{
	u16 *tmp_wvdata = neon_wvdata_result;
	u8 *tmp_index = index;
	bool ret = true;
	u8 i = 0, j = 0;
	u16 wvdata = 0;

	for (i = 0; i < 4; i++) {
		wvdata = 0;
		for (j = 0; j < 8; j++) {
			wvdata |= ((*(waveform + *tmp_index)) << (j * 2));
			tmp_index++;
		}

		if (*tmp_wvdata != wvdata) {
			ret = false;
			break;
		}
		tmp_wvdata++;
	}

	return ret;
}
EXPORT_SYMBOL(neon_lookup_waveform_check);

/*
Function:
	according to mask/index/waveform, to calculate wvdata
Input:
	mask: mask pixel to show or not
	index: index = (old_data << 5) | new_data, is an index to lookup table
	waveform: a lookup table, range is 0~2
Output:
	wvdata: output wave data to driver E-ink panel
*/
void neon_encode_core_function(u8 *mask, u16 *index, u8 *waveform, u32 *wvdata)
{
	uint8x8x4_t	src_mask_8x8x4;
	uint8x8_t		dst_mask_8x8;
	uint32x4_t   dst_mask_32x4, dst_tmp_mask_32x4, dst_shift_mask_32x4;
	int32x4_t  dst_neg_mask_32x4;
	uint16x4_t        dst_mask_16x4;
	uint16x8_t src_index_u16x8, tmp1_src_index_u16x8, tmp2_src_index_u16x8;
	uint8x8_t dst_index_u8x8;
	uint8x8_t waveform_8x8, index_8x8;
	uint8x8_t pixel_8x8, wvdata_8x8;
	uint32x4_t shift_wvdata_32x4, wvdata_32x4;
	uint32x4_t tmp_wvdata_32x4, rst_wvdata_32x4;
	uint8x8x4_t pixel_8x8x4;
	u8 tmp_data_array[32] = {0};
	u32 tmp_mask[2] = {0};
	u32 tmp_shift_mask[4] = {0xff0000ff, 0xff0000ff, 0xff0000ff, 0xff0000ff};

	unsigned int i = 0, j = 0;

	/*
		*******Load mask data as this format********
		m28	......		m8		m4		m0
		m29	......		m9		m5		m1
		m30	......		m10	m6		m2
		m31	......		m11	m7		m3
		****************************************

		uint8x8x4_t vld4_u8 (const uint8_t *)
		Form of expected instruction(s): vld4.8 {d0, d1, d2, d3}, [r0]
	*/
	src_mask_8x8x4 = vld4_u8(mask);

	/*******************************************************************
		Left shift 0/2/4/6 bit for each line of mask data
		***********************************************
		m28<<0	......		m8<<0		m4<<0		m0<<0
		m29<<2	......		m9<<2		m5<<2		m1<<2
		m30<<4	......		m10<<4	m6<<4		m2<<4
		m31<<6	......		m11<<6	m7<<6		m3<<6
		***********************************************

		shift and insert then together, you can used VADD as well
		******************************************************************
		m4<<0 + m5<<2 + m6<<4 + m7<<6  | m0<<0 + m1<<2 + m2<<4 + m3<<6
		******************************************************************

		uint8x8_t vshl_n_u8 (uint8x8_t, const int)
		Form of expected instruction(s): vshl.i8 d0, d0, #0

		uint8x8_t vsli_n_u8 (uint8x8_t, uint8x8_t, const int)
		Form of expected instruction(s): vsli.8 d0, d0, #0
	*****************************************************************/
	dst_mask_8x8 = vshl_n_u8(src_mask_8x8x4.val[0], 0);
	dst_mask_8x8 = vsli_n_u8(dst_mask_8x8, src_mask_8x8x4.val[1], 2);
	dst_mask_8x8 = vsli_n_u8(dst_mask_8x8, src_mask_8x8x4.val[2], 4);
	dst_mask_8x8 = vsli_n_u8(dst_mask_8x8, src_mask_8x8x4.val[3], 6);

	/*************************************************************
		Store register data to memory, two u8 data combine into one u16 data

		void vst1_u8 (uint8_t *, uint8x8_t)
		Form of expected instruction(s): vst1.8 {d0}, [r0]
	**************************************************************/
	vst1_u8((u8 *)tmp_mask, dst_mask_8x8);

	//all pixel(32) has been masked
	if ((tmp_mask[0] == 0xffffffff) && (tmp_mask[1] == 0xffffffff)) {
		return;
	}

	/*
		index data = (old_data << 5) | new_data, old/new data only 5bit is valid

		1. load index data to register

		uint16x8_t vld1q_u16 (const uint16_t *)
		Form of expected instruction(s): vld1.16 {d0, d1}, [r0]

		2. right shift 8 bit of index data, let MSB of old data locat in bit1

		uint16x8_t vshrq_n_u16 (uint16x8_t, const int)
		Form of expected instruction(s): vshr.u16 q0, q0, #0

		3. left shift 11 bit of index data, to clear all of the old_data

		uint16x8_t vshlq_n_u16 (uint16x8_t, const int)
		Form of expected instruction(s): vshl.i16 q0, q0, #0

		4. right shift 15 bit of the result in step 3, and insert to the result of step 2

		uint16x8_t vsriq_n_u16 (uint16x8_t, uint16x8_t, const int)
		Form of expected instruction(s): vsri.16 q0, q0, #0

		5. narrow data from 16bit to 8bit

		uint8x8_t vmovn_u16 (uint16x8_t)
		Form of expected instruction(s): vmovn.i16 d0, q0

		6. lookup table of waveform

		uint8x8_t vtbl1_u8 (uint8x8_t, uint8x8_t)
		Form of expected instruction(s): vtbl.8 d0, {d0}, d0
	*/

	waveform_8x8 = vld1_u8(waveform);
	for (i = 0; i < 4; i++) {
		j = i << 3;
		src_index_u16x8 = vld1q_u16((index + j));
		tmp1_src_index_u16x8 = vshrq_n_u16(src_index_u16x8, 8);
		tmp2_src_index_u16x8 = vshlq_n_u16(src_index_u16x8, 11);
		src_index_u16x8 = vsriq_n_u16(tmp1_src_index_u16x8, tmp2_src_index_u16x8, 15);
		dst_index_u8x8 = vmovn_u16(src_index_u16x8);
		pixel_8x8 = vtbl1_u8(waveform_8x8, dst_index_u8x8);
		vst1_u8(&tmp_data_array[j], pixel_8x8);
	}

	pixel_8x8x4 = vld4_u8(tmp_data_array);

	wvdata_8x8 = vshl_n_u8(pixel_8x8x4.val[0], 0);
	wvdata_8x8 = vsli_n_u8(wvdata_8x8, pixel_8x8x4.val[1], 2);
	wvdata_8x8 = vsli_n_u8(wvdata_8x8, pixel_8x8x4.val[2], 4);
	wvdata_8x8 = vsli_n_u8(wvdata_8x8, pixel_8x8x4.val[3], 6);

	shift_wvdata_32x4 = vmovl_u16((uint16x4_t)wvdata_8x8);
	wvdata_32x4 = vshlq_n_u32(shift_wvdata_32x4, 8);			//00 2B wavedata 00

	/*
		1. expand mask from u8x8(=u16x4) to u16x8(=u32x4)
		2. left shift mask for 8bit
		3. mask data | 0xff0000ff
	*/
	dst_shift_mask_32x4 = vld1q_u32(tmp_shift_mask);
	dst_tmp_mask_32x4 = vmovl_u16((uint16x4_t)dst_mask_8x8);
	dst_mask_32x4 = vsliq_n_u32(dst_shift_mask_32x4, dst_tmp_mask_32x4, 8);	//mask_flag
	dst_neg_mask_32x4 = vnegq_s32((int32x4_t)dst_mask_32x4);				//~mask_flag

	tmp_wvdata_32x4 = vld1q_u32(wvdata);
	tmp_wvdata_32x4 = vandq_u32(tmp_wvdata_32x4, dst_mask_32x4);
	wvdata_32x4 = vandq_u32(wvdata_32x4, (uint32x4_t)dst_neg_mask_32x4);
	tmp_wvdata_32x4 = vorrq_u32(tmp_wvdata_32x4, wvdata_32x4);

	vst1q_u32(wvdata, tmp_wvdata_32x4);

	return 32;
}
EXPORT_SYMBOL(neon_encode_core_function);

static int __init encode_module_init(void)
{
	printk("encode module init\n");
	return 0;
}

static void __exit encode_module_exit(void)
{
	printk("encode module exit\n");
	return;
}

module_init(encode_module_init);
module_exit(encode_module_exit);

MODULE_AUTHOR("oujunxi");
MODULE_DESCRIPTION("eink encode driver");
MODULE_LICENSE("GPL");
