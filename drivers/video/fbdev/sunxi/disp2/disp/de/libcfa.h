/*
 * Copyright (C) 2017 - 2019 E Ink Holdings Inc. 
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef LIBCFA_H
#define LIBCFA_H

#if defined(__cplusplus)
extern "C" {
#endif

//#include <inttypes.h>

/**
 * Description : for short stride
 *  WIDTH : image width
 *  HEIGHT : image height
 *	color_buffer : input image RGB888 raw data
 *  gray_buffer : output data
 */
//void image_to_cfa_grayscale(int WIDTH, int HEIGHT,unsigned char *color_buffer, unsigned char *gray_buffer);
//void eink_image_to_cfa_grayscale(int width, int height,unsigned char *color_buffer, unsigned char *gray_buffer);
//void image_to_cfa_grayscale_gen1_g(int WIDTH, int HEIGHT,unsigned char *color_buffer, unsigned char *gray_buffer);
//void image_to_cfa_grayscale_gen2_r(int WIDTH, int HEIGHT,unsigned char *color_buffer, unsigned char *gray_buffer);



 /**
 *
 * width, height:
 *     The dimension of both images.
 *     Assume the stride == width.
 *
 * color_buffer: 
 *		The RGB image, which's content won't be modified.
 *
 * gray_buffer: 
 *		The output image, which will modify the color buffer to fit CFA format
 * 
 * HSL_LUT:
 *      Input HSL buffer.
 **/
void image_to_cfa_grayscale_gen1_g_HSL(int width, int height,unsigned char *color_buffer, unsigned char *gray_buffer, unsigned char *HSL_LUT);
void image_to_cfa_grayscale_gen2_r_HSL(int width, int height,unsigned char *color_buffer, unsigned char *gray_buffer, unsigned char *HSL_LUT);


#if defined(__cplusplus)
}
#endif

#endif /* LIBCFA_H */
