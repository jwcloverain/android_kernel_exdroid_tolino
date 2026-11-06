/*
 * parameters for cilix to call ricoh619_battery charging functions.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef __LINUX_POWER_CILIX_RICOH619_H_
#define __LINUX_POWER_CILIX_RICOH619_H_

#define CILIX_CONN_EB_BAT_LOW	0x1001
#define CILIX_CONN_EB_BAT_MED	0x1002
#define CILIX_CONN_EB_BAT_HIGH	0x1003
#define CILIX_CHARGER			0x1004
#define CILIX_CHARGER_DISCONN	0x1005
#define CILIX_CHARGER_05C		0x1006
#define CILIX_CHARGER_02C		0x1007

#endif