/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
- Copyright (C) Rockchip Electronics Co., Ltd.
- Author: Felix Zeng <felix.zeng@rock-chips.com>
- Modified by: DrDitto with Google AI assitance
  *
- Target: RK3566 / RK3568 (Single-Core NPU) | DRM GEM + IOMMU | Kernel >= 7.3.0
 */

#ifndef __LINUX_RKNPU_IOMMU_H
#define __LINUX_RKNPU_IOMMU_H

#include <linux/version.h>
#include <linux/types.h>
#include <linux/mutex.h>
#include <linux/seq_file.h>
#include <linux/iommu.h>
#include <linux/iova.h>
#include <linux/dma-direction.h>

#if LINUX_VERSION_CODE < KERNEL_VERSION(7, 3, 0)
#error "rknpu356x-module requires Linux kernel 7.3.0 or newer"
#endif

struct device;
struct scatterlist;
struct rknpu_device;

struct rknpu_iommu_dma_cookie {
	struct iova_domain iovad;
};

static inline bool rockchip_iommu_is_enabled(struct device *dev)
{
	struct iommu_group *group = iommu_group_get(dev);

	if (group) {
		iommu_group_put(group);
		return true;
	}

	return false;
}

static inline void rockchip_iommu_disable(struct device *dev)
{
	(void)dev;
}

static inline void rockchip_iommu_enable(struct device *dev)
{
	(void)dev;
}

dma_addr_t rknpu_iommu_dma_alloc_iova(struct iommu_domain *domain, size_t size,
				      u64 dma_limit, struct device *dev,
				      bool size_aligned);

void rknpu_iommu_dma_free_iova(struct rknpu_iommu_dma_cookie *cookie,
			       dma_addr_t iova, size_t size, bool size_aligned);

int rknpu_iommu_dma_map_sg(struct device *dev, struct scatterlist *sg,
			   int nents, enum dma_data_direction dir,
			   bool iova_aligned);

void rknpu_iommu_dma_unmap_sg(struct device *dev, struct scatterlist *sg,
			      int nents, enum dma_data_direction dir,
			      bool iova_aligned);

int rknpu_iommu_init_domain(struct rknpu_device *rknpu_dev);
int rknpu_iommu_switch_domain(struct rknpu_device *rknpu_dev, int domain_id);
void rknpu_iommu_free_domains(struct rknpu_device *rknpu_dev);
int rknpu_iommu_domain_get_and_switch(struct rknpu_device *rknpu_dev,
				      int domain_id);
int rknpu_iommu_domain_put(struct rknpu_device *rknpu_dev);

#endif /* __LINUX_RKNPU_IOMMU_H */
