#ifndef __EINK_ENCODE_H_
#define __EINK_ENCODE_H_

int neon_memcpy(u8 *dst, u8 *src, unsigned int len);
bool neon_memcpy_check(u8 *dst, u8 *src, unsigned int len);
int neon_mask_combine(u16 *dst_mask, char *src_mask);
bool neon_mask_combine_check(u16 *neon_mask_result, u8 *src_mask, unsigned int len);
int neon_index_bit_reduce(u8 *dst_index_u8, u16 *src_index_u16);
bool neon_index_bit_reduce_check(u8 *neon_index_result, u16 *src_index_u16);
int neon_lookup_waveform(u16 *wvdata, u8 *index, u8 *waveform);
bool neon_lookup_waveform_check(u16 * neon_wvdata_result, u8 * index, u8 * waveform);
void neon_encode_core_function(u8 *mask, u16 *index, u8 *waveform, u32 *wvdata);

int neon_argb_to_gray(u8 *src, u8 *dest, void *area, unsigned int x_offset, unsigned int y_offset, unsigned int pitch);
#endif
