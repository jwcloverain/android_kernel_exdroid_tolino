/*
* Simple driver for EINK regal and eclipse library driver
* Copyright (C) 2021 Netronixinc
*/

#include <linux/module.h>
#include <linux/ebc.h>

//#include "libeink.h"
#include "lib/libeink.h"

struct __eink_libeink ec060kh6_info = {
    .name = "ec060kh6",
    .eink_init = EInk_Init,
    .regal_func = eink_process,
    .regal_index_func = eink_process_index,
    .regal_index_func_neon = eink_process_index_neon,
    .eclipse_index_func_neon = eink_process_index_black_neon,
    .get_libeink_version = eink_get_version,
};

static int __init panel_init(void)
{
    register_libeink_callback_func(&ec060kh6_info);
    
    return 0;
}

module_init(panel_init);

static void __exit panel_exit(void)
{
    unregister_libeink_callback_func(&ec060kh6_info);
}

module_exit(panel_exit);


MODULE_DESCRIPTION("Netronixinc driver for EINK regal and eclipse library driver");
MODULE_AUTHOR("YP Lin <yp.lin@netronixinc.com>");
MODULE_LICENSE("GPL v2");
