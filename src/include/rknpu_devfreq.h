/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
- Copyright (C) Rockchip Electronics Co., Ltd.
- Author: Felix Zeng <felix.zeng@rock-chips.com>
- Modified by: DrDitto with Google AI assitance
  *
- Target: RK3566 / RK3568 (Single-Core NPU) | DRM GEM + IOMMU | Kernel >= 7.3.0
 */

#ifndef __LINUX_RKNPU_DEVFREQ_H
#define __LINUX_RKNPU_DEVFREQ_H

#include <linux/version.h>

#if LINUX_VERSION_CODE < KERNEL_VERSION(7, 3, 0)
#error "This driver requires Linux Kernel 7.3.0 or newer"
#endif

struct device;
struct rknpu_device;

#if defined(CONFIG_PM_DEVFREQ) && !defined(RKNPU_NO_DEVFREQ)
void rknpu_devfreq_lock(struct rknpu_device *rknpu_dev);
void rknpu_devfreq_unlock(struct rknpu_device *rknpu_dev);
int rknpu_devfreq_init(struct rknpu_device *rknpu_dev);
void rknpu_devfreq_remove(struct rknpu_device *rknpu_dev);
int rknpu_devfreq_runtime_suspend(struct device *dev);
int rknpu_devfreq_runtime_resume(struct device *dev);
#else
static inline int rknpu_devfreq_init(struct rknpu_device *rknpu_dev)
{
	return 0;
}

static inline void rknpu_devfreq_remove(struct rknpu_device *rknpu_dev)
{
}

static inline void rknpu_devfreq_lock(struct rknpu_device *rknpu_dev)
{
}

static inline void rknpu_devfreq_unlock(struct rknpu_device *rknpu_dev)
{
}

static inline int rknpu_devfreq_runtime_suspend(struct device *dev)
{
	return 0;
}

static inline int rknpu_devfreq_runtime_resume(struct device *dev)
{
	return 0;
}
#endif /* CONFIG_PM_DEVFREQ && !RKNPU_NO_DEVFREQ */

#endif /* __LINUX_RKNPU_DEVFREQ_H */
