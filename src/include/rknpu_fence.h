/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
- Copyright (C) Rockchip Electronics Co., Ltd.
- Author: Felix Zeng <felix.zeng@rock-chips.com>
- Modified by: DrDitto with Google AI assitance
  *
- Target: RK3566 / RK3568 (Single-Core NPU) | DRM GEM + IOMMU | Kernel >= 7.3.0
 */

#ifndef __LINUX_RKNPU_FENCE_H_
#define __LINUX_RKNPU_FENCE_H_

#include <linux/atomic.h>
#include <linux/version.h>

#if LINUX_VERSION_CODE < KERNEL_VERSION(7, 3, 0)
#error "This driver requires Linux Kernel 7.3.0 or newer"
#endif

#include "rknpu_job.h"

struct rknpu_fence_context {
	unsigned int context;
	atomic_t seqno;
	spinlock_t spinlock;
};

int rknpu_fence_context_alloc(struct rknpu_device *rknpu_dev);

int rknpu_fence_alloc(struct rknpu_job *job);

int rknpu_fence_get_fd(struct rknpu_job *job);

#endif /* __LINUX_RKNPU_FENCE_H_ */
