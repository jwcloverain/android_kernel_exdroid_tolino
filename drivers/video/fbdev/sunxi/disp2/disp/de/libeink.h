/*
 * E Ink - pixel process library
 *
 * Copyright (C) 2020 E Ink Holdings Inc.
 *
 */
#include <asm/neon.h>


/**
 * awf:
 *     The input awff file for reference
 * return: 0 , Success
 *         others , Fail
 */
int EInk_Init(char *awf);

/**
 * image:
 *     The Y8 image to be processed, the content may be modifed.
 *
 * previous:
 *     The previous Y8 image, which's content won't be modified.
 *
 * width, height:
 *     The dimension of both images.
 *     Assume the stride == width.
 *
 * Note:
 *     Do not apply other image prcossing methods after eink_process().
 *
 */
void eink_process(uint8_t *image, uint8_t *previous,
                  uint32_t width, uint32_t height);

/**
 * index:
 *     Combined image, previous image (bit 5~9) and new image (bit 0~4).
 *     The new image (bit 0~4) may be modified.
 *
 * width, height:
 *     The dimension of both images.
 *     Assume the stride == width.
 *
 */
void eink_process_index(uint16_t *index,
                        uint32_t width, uint32_t height);

/**
 * NEON version of eink_process_index
 *
 * The caller should do add some code like following sample:
 * -------------------------------------
 *    #include <asm/neon.h>
 *
 *    kernel_neon_begin();
 *    eink_process_index_neon(index, width, height);
 *    kernel_neon_end();
 * -------------------------------------
 *
 * Refer to Documentation/arm/kernel_mode_neon.txt
 */
void eink_process_index_neon(uint16_t *index, uint32_t width, uint32_t height);

/**
 * For images which has white text on black background.
 *
 * This MUST cooperate with specific waveform and hardware design.
 */
void eink_process_index_black_neon(uint16_t *index, uint32_t width, uint32_t height);

/**
 *
 * return:
 *     The build date in decimal number, Ex: 20200225 => Feb 25, 2020.
 */
uint32_t eink_get_version(void);
