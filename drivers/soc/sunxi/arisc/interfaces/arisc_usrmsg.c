/*
 *  drivers/arisc/interfaces/arisc_usrmsg.c
 *
 * Copyright (c) 2020 Allwinner.
 * 2020-11-10 Written by xuqiang (xuqiang@allwinnertech.com).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include "../arisc_i.h"
#include "arisc_usrmsg.h"

#include "../../../../../arch/arm/mach-sunxi/ntx_hwconfig.h"


extern volatile NTX_HWCONFIG *gptHWCFG;

//#define DEBUG 1
//#define TEST 1
#ifdef TEST //[
/**
 * send usr message to data.
 * @message: message buff pointer;
 * @lenth:   the message lenth[unit: words].
 *
 * return: result, 0 - read register successed,
 *                !0 - read register failed or the len more then max len;
 */
int arisc_usr_message(u32 *message, u32 lenth)
{
	int result;
	volatile u32 paras[22] ;

	if (lenth > sizeof(paras)) {
		ARISC_WRN("The message must be less than %d words.\n", sizeof(paras));
		lenth = (lenth > sizeof(paras)) ? sizeof(paras) : lenth;
		return -EINVAL;
	}

	memset(paras, 4*sizeof(paras), 0);
	/* copy message  data to user data buffer */
	memcpy(paras, message, 4*lenth);

	/* FIXME: if the runtime sever enable the mmu & dcache,
	 * should not use flush cache here.
	 */
	/* send message use hwmsgbox */
	result = invoke_scp_fn_smc(ARM_SVC_ARISC_USR_MESSAGE_REQ, virt_to_phys(paras), 0, 0);

	/* copy message readout data to user data buffer */
	memcpy(message, paras, 4*lenth);

	return result;
}
EXPORT_SYMBOL(arisc_usr_message);




static int __init test_demo_init(void)
{
	int ret = 0;
	int i = 0;
	volatile u32 msg[22]={0x1,0x2,0x3,0x4,0x5, \
		0xa,0xb,0xc,0xd,0xe,0xf,
		0x6,0x7,0x8,0x9,0x0, \
		0x1,0x2,0x3,0x4,0x5, \
	};

	for (i=0; i<sizeof(msg)/sizeof(msg[0]); i++)
	{
		printk("arisc msg: before msg[%d]:0x%08x\n", i, msg[i]);
	}

	ret = arisc_usr_message(msg, sizeof(msg)/sizeof(msg[0]));

	printk("arisc msg: ret:%d\n", ret);
	for (i=0; i<sizeof(msg)/sizeof(msg[0]); i++)
	{
		printk("arisc msg: after msg[%d]:0x%08x\n", i, msg[i]);
	}

}

module_init(test_demo_init);
#else //][!TEST

/**
 * send usr message to data.
 * @message: message buff pointer;
 * @lenth:   the message lenth[unit: words].
 *
 * return: result, 0 - read register successed,
 *                !0 - read register failed or the len more then max len;
 */

static u8 gbUsrMsgA[USRMSG_MAX_BYTES];
int arisc_ntx_cmd(u8 *pbCmdResp,u8 *pbData ,u32 dwDataLen)
{
	int result;


	if ((dwDataLen+USRMSG_CMDRESP_LEN) > USRMSG_MAX_BYTES) {
		ARISC_WRN("The message must be less than %d words.\n",USRMSG_MAX_BYTES-USRMSG_CMDRESP_LEN);
		return -EINVAL;
	}

	memset(gbUsrMsgA,USRMSG_MAX_BYTES, 0);

	gbUsrMsgA[0] = pbCmdResp[0];
	gbUsrMsgA[1] = pbCmdResp[1];
	gbUsrMsgA[2] = pbCmdResp[2];
	gbUsrMsgA[3] = pbCmdResp[3];


	if( pbCmdResp[2]&USRMSG_CMD_DIR_OUT ) {
		/* copy message  data to user data buffer */
		memcpy(&gbUsrMsgA[USRMSG_CMDRESP_LEN], pbData, dwDataLen);
	}
#ifdef DEBUG//[
	{
		int i,j;
		printk("before sndmsg {",gbUsrMsgA[i]);
		for(j=0,i=0;j<2;j++) {
			for(;i<16;i++)
				printk("0x%02x,",gbUsrMsgA[i]);
			printk("\n ");
		}
		printk("}\n",gbUsrMsgA[i]);
	}
#endif //] DEBUG
	/* FIXME: if the runtime sever enable the mmu & dcache,
	 * should not use flush cache here.
	 */
	/* send message use hwmsgbox */
	result = invoke_scp_fn_smc(ARM_SVC_ARISC_USR_MESSAGE_REQ, virt_to_phys(gbUsrMsgA), 0, 0);

#ifdef DEBUG//[
	{
		int i,j;
		printk("after sndmsg {",gbUsrMsgA[i]);
		for(j=0,i=0;j<2;j++) {
			for(;i<16;i++)
				printk("0x%02x,",gbUsrMsgA[i]);
			printk("\n ");
		}
		printk("}\n",gbUsrMsgA[i]);
	}
#endif //] DEBUG

	if( pbCmdResp[2]&USRMSG_CMD_DIR_IN ) {
		/* copy message readout data to user data buffer */
		memcpy(pbData, &gbUsrMsgA[USRMSG_CMDRESP_LEN], dwDataLen);
		pbCmdResp[3] = gbUsrMsgA[3];
	}

	return result;
}
EXPORT_SYMBOL(arisc_ntx_cmd);


char gcCPUS_versionA[USRMSG_DATA_LEN];
static int __init init_get_cpus_version(void)
{
	int iRet;

	//printk("%s()\n",__func__);
	u8 bCmdRespA[4] = {
		USRMSG_CMD_GET_CPUS_VERSION,
		0,
		USRMSG_CMD_DIR_IN,
		0,
	}	;

	iRet = arisc_ntx_cmd(bCmdRespA,gcCPUS_versionA ,USRMSG_DATA_LEN);
	gcCPUS_versionA[USRMSG_DATA_LEN-1] = 0;
	printk("CPUS version : %s\n",gcCPUS_versionA);

	return iRet;
}
module_init(init_get_cpus_version);


struct cpus_hwinfo gtCPUS_HWINFO ;
static int __init init_set_cpus_hwinfo(void)
{
	int iRet;

	//printk("%s()\n",__func__);
	u8 bCmdRespA[4] = {
		USRMSG_CMD_SET_NTX_HWINFO,
		0,
		USRMSG_CMD_DIR_OUT|USRMSG_CMD_DIR_IN,
		0,
	}	;

	gtCPUS_HWINFO.bPCB = gptHWCFG->m_val.bPCB;
	gtCPUS_HWINFO.bPCB_LVL = gptHWCFG->m_val.bPCB_LVL;
	gtCPUS_HWINFO.bPCB_REV = gptHWCFG->m_val.bPCB_REV;

	iRet = arisc_ntx_cmd(bCmdRespA,(u8 *)&gtCPUS_HWINFO,sizeof(gtCPUS_HWINFO));
#ifdef DEBUG //[
	printk("%s() : set hwinfo %s ,pcb=0x%02x,pcb_lvl=0x%02x,pcb_rev=0x%02x,reserved [%02x%02x%02x]\n",__func__,
			(0==iRet)?"ok":"failed",
			gtCPUS_HWINFO.bPCB,
			gtCPUS_HWINFO.bPCB_LVL,
			gtCPUS_HWINFO.bPCB_REV,
			gtCPUS_HWINFO.bReservedA[0],
			gtCPUS_HWINFO.bReservedA[1],
			gtCPUS_HWINFO.bReservedA[2]);

#endif //] DEBUG
	return iRet;
}

module_init(init_set_cpus_hwinfo);


#endif //] TEST

