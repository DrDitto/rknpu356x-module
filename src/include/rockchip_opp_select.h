/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
- Copyright (C) Rockchip Electronics Co., Ltd.
- Author: Felix Zeng <felix.zeng@rock-chips.com>
- Modified by: DrDitto with Google AI assitance
  *
- Target: RK3566 / RK3568 (Single-Core NPU) | DRM GEM + IOMMU | Kernel >= 7.3.0
 */

/* Stub header for out-of-tree rknpu module build */

#ifndef __SOC_ROCKCHIP_OPP_SELECT_H
#define __SOC_ROCKCHIP_OPP_SELECT_H

#include <linux/version.h>
#include <linux/of.h>
#include <linux/types.h>

#if LINUX_VERSION_CODE < KERNEL_VERSION(7, 3, 0)
#error "rknpu356x-module requires Linux kernel 7.3.0 or newer"
#endif

struct rockchip_opp_info {
	int dummy;
};

static inline int rockchip_nvmem_cell_read_u8(struct device_node *np,
					      const char *cell_id, u8 *val)
{
	(void)np;
	(void)cell_id;
	if (val)
		*val = 0;
	return -ENOSYS;
}

#endif /* __SOC_ROCKCHIP_OPP_SELECT_H */
