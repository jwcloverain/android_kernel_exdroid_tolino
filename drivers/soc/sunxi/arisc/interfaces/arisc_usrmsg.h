#ifndef ARISC_USRMSG_H //[
#define ARISC_USRMSG_H //][!


#define USRMSG_MAX_BYTES			44
#define USRMSG_CMDRESP_LEN		4
#define USRMSG_DATA_LEN		(USRMSG_MAX_BYTES-USRMSG_CMDRESP_LEN)

#define USRMSG_CMD_GET_CPUS_VERSION			0x01
#define USRMSG_CMD_SET_NTX_HWINFO				0x02

#define USRMSG_CMD_DIR_IN					0x01
#define USRMSG_CMD_DIR_OUT				0x02

#define USRMSG_RET_CMD_NOT_SUPPORTED		0xff
#define USRMSG_RET_CMD_FAIL							0x0
#define USRMSG_RET_CMD_PASS							0x1


struct cpus_hwinfo {
	unsigned char bPCB;
	unsigned char bPCB_LVL;
	unsigned char bPCB_REV;
	unsigned char bReservedA[37];
};

#endif //] ARISC_USRMSG_H


