
#ifndef __NTX_COMMON_H__
#define __NTX_COMMON_H__

////////////////////////////////
#include "ntx_hwconfig.h"
extern volatile NTX_HWCONFIG *gptHWCFG;
extern int gIsCustomerUi;
void ntx_parse_cmdline(void);
////////////////////////////////

#endif