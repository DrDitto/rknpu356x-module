/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
- Copyright (C) Rockchip Electronics Co., Ltd.
- Author: Felix Zeng <felix.zeng@rock-chips.com>
- Modified by: DrDitto with Google AI assitance
  *
- Target: RK3566 / RK3568 (Single-Core NPU) | DRM GEM + IOMMU | Kernel >= 7.3.0
 */

/* Stub header for out-of-tree rknpu module build */

#ifndef __SOC_ROCKCHIP_IPA_H
#define __SOC_ROCKCHIP_IPA_H

#include <linux/version.h>

#if LINUX_VERSION_CODE < KERNEL_VERSION(7, 3, 0)
#error "rknpu356x-module requires Linux kernel 7.3.0 or newer"
#endif

struct ipa_power_model_data {
	/* stub */
};

#endif /* __SOC_ROCKCHIP_IPA_H */
