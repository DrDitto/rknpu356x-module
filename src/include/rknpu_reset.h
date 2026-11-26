/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
- Copyright (C) Rockchip Electronics Co., Ltd.
- Author: Felix Zeng <felix.zeng@rock-chips.com>
- Modified by: DrDitto with Google AI assitance
  *
- Target: RK3566 / RK3568 (Single-Core NPU) | DRM GEM + IOMMU | Kernel >= 7.3.0
 */

#ifndef __LINUX_RKNPU_RESET_H
#define __LINUX_RKNPU_RESET_H

#include <linux/version.h>
#include <linux/reset.h>

#if LINUX_VERSION_CODE < KERNEL_VERSION(7, 3, 0)
#error "rknpu356x-module requires Linux kernel 7.3.0 or newer"
#endif

struct rknpu_device;

int rknpu_reset_get(struct rknpu_device *rknpu_dev);

int rknpu_soft_reset(struct rknpu_device *rknpu_dev);

#endif /* __LINUX_RKNPU_RESET_H */
