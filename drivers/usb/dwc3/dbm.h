/* Copyright (c) 2012-2016, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef __DBM_H
#define __DBM_H

#include <linux/device.h>
#include <linux/types.h>

#define DBM_EN_EP		0x00000001
#define USB3_EPNUM		0x0000003E
#define DBM_BAM_PIPE_NUM	0x000000C0
#define DBM_PRODUCER		0x00000100
#define DBM_DISABLE_WB		0x00000200
#define DBM_INT_RAM_ACC		0x00000400

#define DBM_DATA_FIFO_SIZE_MASK	0x0000ffff

#define DBM_GEVNTSIZ_MASK	0x0000ffff

#define DBM_ENABLE_IOC_MASK	0x0000000f

#define DBM_SFT_RST_EP0		0x00000001
#define DBM_SFT_RST_EP1		0x00000002
#define DBM_SFT_RST_EP2		0x00000004
#define DBM_SFT_RST_EP3		0x00000008
#define DBM_SFT_RST_EPS_MASK	0x0000000F
#define DBM_SFT_RST_MASK	0x80000000
#define DBM_EN_MASK		0x00000002

#define DBM_TRB_BIT		0x80000000
#define DBM_TRB_DATA_SRC	0x40000000
#define DBM_TRB_DMA		0x20000000
#define DBM_TRB_EP_NUM(ep)	(ep<<24)

struct dbm;

struct dbm *usb_get_dbm_by_phandle(struct device *dev, const char *phandle);

int dbm_soft_reset(struct dbm *dbm, bool enter_reset);
int dbm_ep_config(struct dbm  *dbm, u8 usb_ep, u8 bam_pipe, bool producer,
			bool disable_wb, bool internal_mem, bool ioc);
int dbm_ep_unconfig(struct dbm *dbm, u8 usb_ep);
int dbm_get_num_of_eps_configured(struct dbm *dbm);
int dbm_event_buffer_config(struct dbm *dbm, u32 addr_lo, u32 addr_hi,
				int size);
int dwc3_dbm_disable_update_xfer(struct dbm *dbm, u8 usb_ep);
int dbm_data_fifo_config(struct dbm *dbm, u8 dep_num, phys_addr_t addr,
				u32 size, u8 dst_pipe_idx);
void dbm_set_speed(struct dbm *dbm, bool speed);
void dbm_enable(struct dbm *dbm);
int dbm_ep_soft_reset(struct dbm *dbm, u8 usb_ep, bool enter_reset);
bool dbm_reset_ep_after_lpm(struct dbm *dbm);
bool dbm_l1_lpm_interrupt(struct dbm *dbm);

#endif 
