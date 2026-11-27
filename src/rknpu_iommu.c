/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * File: rknpu_iommu.c
 * Copyright (C) Rockchip Electronics Co., Ltd.
 * Author: Felix Zeng <felix.zeng@rock-chips.com>
 * Modified by: DrDitto with Google AI assitance
 *
 * Target: RK3566 / RK3568 (Single-Core NPU) | DRM GEM + IOMMU | Kernel >= 7.3.0
 */

#include <linux/atomic.h>
#include <linux/compiler.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/iommu.h>
#include <linux/jiffies.h>
#include <linux/mutex.h>
#include <linux/scatterlist.h>

#include "rknpu_drv.h"
#include "rknpu_iommu.h"

#define RKNPU_SWITCH_DOMAIN_WAIT_TIME_MS 6000

static __always_inline int rknpu_iommu_map_sg_internal(
	struct device *dev, struct scatterlist *sg, int nents,
	enum dma_data_direction dir, bool iova_aligned)
{
	(void)iova_aligned;

	if (unlikely(!dev || !sg || nents <= 0))
		return -EINVAL;

	return dma_map_sg(dev, sg, nents, dir);
}

static __always_inline void rknpu_iommu_unmap_sg_internal(
	struct device *dev, struct scatterlist *sg, int nents,
	enum dma_data_direction dir, bool iova_aligned)
{
	(void)iova_aligned;

	if (unlikely(!dev || !sg || nents <= 0))
		return;

	dma_unmap_sg(dev, sg, nents, dir);
}

int rknpu_iommu_dma_map_sg(struct device *dev, struct scatterlist *sg,
			   int nents, enum dma_data_direction dir,
			   bool iova_aligned)
{
	return rknpu_iommu_map_sg_internal(dev, sg, nents, dir, iova_aligned);
}

void rknpu_iommu_dma_unmap_sg(struct device *dev, struct scatterlist *sg,
			      int nents, enum dma_data_direction dir,
			      bool iova_aligned)
{
	rknpu_iommu_unmap_sg_internal(dev, sg, nents, dir, iova_aligned);
}

#if defined(CONFIG_IOMMU_API)

int rknpu_iommu_init_domain(struct rknpu_device *rknpu_dev)
{
	if (unlikely(!rknpu_dev || !rknpu_dev->dev))
		return -EINVAL;

	if (!rknpu_dev->iommu_domains[0]) {
		rknpu_dev->iommu_domain_id = 0;
		rknpu_dev->iommu_domains[0] =
			iommu_get_domain_for_dev(rknpu_dev->dev);
		rknpu_dev->iommu_domain_num =
			rknpu_dev->iommu_domains[0] ? 1 : 0;
	}

	return 0;
}

int rknpu_iommu_switch_domain(struct rknpu_device *rknpu_dev, int domain_id)
{
	struct iommu_domain *src_domain = NULL;
	struct iommu_domain *dst_domain = NULL;
	int src_domain_id;
	int ret = -EINVAL;

	if (unlikely(!rknpu_dev || !rknpu_dev->dev))
		return -EINVAL;

	if (unlikely(!rknpu_dev->iommu_en))
		return -EINVAL;

	if (unlikely(domain_id < 0 || domain_id >= RKNPU_MAX_IOMMU_DOMAIN_NUM)) {
		LOG_DEV_ERROR(
			rknpu_dev->dev,
			"invalid iommu domain id: %d, reuse domain id: %d\n",
			domain_id, rknpu_dev->iommu_domain_id);
		return -EINVAL;
	}

	src_domain_id = rknpu_dev->iommu_domain_id;
	if (domain_id == src_domain_id)
		return 0;

	src_domain = iommu_get_domain_for_dev(rknpu_dev->dev);
	dst_domain = rknpu_dev->iommu_domains[domain_id];

	if (likely(dst_domain)) {
		if (src_domain && src_domain != dst_domain)
			iommu_detach_device(src_domain, rknpu_dev->dev);

		ret = iommu_attach_device(dst_domain, rknpu_dev->dev);
		if (unlikely(ret)) {
			LOG_DEV_ERROR(
				rknpu_dev->dev,
				"failed to attach dst iommu domain, id: %d, ret: %d\n",
				domain_id, ret);
			if (src_domain && src_domain != dst_domain)
				iommu_attach_device(src_domain, rknpu_dev->dev);
			return ret;
		}

		rknpu_dev->iommu_domain_id = domain_id;
	} else {
		LOG_DEV_ERROR(
			rknpu_dev->dev,
			"dynamic domain allocation is not supported on kernel 7.x natively\n");
		return -ENOSYS;
	}

	LOG_INFO("switch iommu domain from %d to %d\n", src_domain_id,
		 domain_id);

	return 0;
}

int rknpu_iommu_domain_get_and_switch(struct rknpu_device *rknpu_dev,
				      int domain_id)
{
	unsigned long timeout_jiffies =
		msecs_to_jiffies(RKNPU_SWITCH_DOMAIN_WAIT_TIME_MS);
	unsigned long start = jiffies;
	int ret;

	if (unlikely(!rknpu_dev || !rknpu_dev->dev))
		return -EINVAL;

	while (true) {
		mutex_lock(&rknpu_dev->domain_lock);

		if (domain_id == rknpu_dev->iommu_domain_id) {
			atomic_inc(&rknpu_dev->iommu_domain_refcount);
			mutex_unlock(&rknpu_dev->domain_lock);
			return 0;
		}

		if (atomic_read(&rknpu_dev->iommu_domain_refcount) == 0) {
			ret = rknpu_iommu_switch_domain(rknpu_dev, domain_id);
			if (unlikely(ret)) {
				LOG_DEV_ERROR(
					rknpu_dev->dev,
					"failed to switch iommu domain, id: %d, ret: %d\n",
					domain_id, ret);
				mutex_unlock(&rknpu_dev->domain_lock);
				return ret;
			}

			atomic_inc(&rknpu_dev->iommu_domain_refcount);
			mutex_unlock(&rknpu_dev->domain_lock);
			return 0;
		}

		mutex_unlock(&rknpu_dev->domain_lock);

		usleep_range(10, 100);
		if (unlikely(time_after(jiffies, start + timeout_jiffies))) {
			LOG_DEV_ERROR(
				rknpu_dev->dev,
				"switch iommu domain time out, failed to switch iommu domain, id: %d\n",
				domain_id);
			return -ETIMEDOUT;
		}
	}
}

int rknpu_iommu_domain_put(struct rknpu_device *rknpu_dev)
{
	if (unlikely(!rknpu_dev))
		return -EINVAL;

	if (unlikely(atomic_dec_if_positive(&rknpu_dev->iommu_domain_refcount) < 0)) {
		LOG_WARN("iommu_domain_refcount underflow prevented\n");
		return -EFAULT;
	}

	return 0;
}

void rknpu_iommu_free_domains(struct rknpu_device *rknpu_dev)
{
	int i;

	if (unlikely(!rknpu_dev || !rknpu_dev->dev))
		return;

	if (unlikely(rknpu_iommu_domain_get_and_switch(rknpu_dev, 0))) {
		LOG_DEV_ERROR(rknpu_dev->dev, "%s error\n", __func__);
		return;
	}

	for (i = 1; i < RKNPU_MAX_IOMMU_DOMAIN_NUM; i++) {
		struct iommu_domain *domain = rknpu_dev->iommu_domains[i];

		if (!domain)
			continue;

		iommu_detach_device(domain, rknpu_dev->dev);
		iommu_domain_free(domain);
		rknpu_dev->iommu_domains[i] = NULL;
	}

	rknpu_iommu_domain_put(rknpu_dev);
}

#else

int rknpu_iommu_init_domain(struct rknpu_device *rknpu_dev)
{
	return 0;
}

int rknpu_iommu_switch_domain(struct rknpu_device *rknpu_dev, int domain_id)
{
	return 0;
}

int rknpu_iommu_domain_get_and_switch(struct rknpu_device *rknpu_dev,
				      int domain_id)
{
	return 0;
}

int rknpu_iommu_domain_put(struct rknpu_device *rknpu_dev)
{
	return 0;
}

void rknpu_iommu_free_domains(struct rknpu_device *rknpu_dev)
{
}

#endif
