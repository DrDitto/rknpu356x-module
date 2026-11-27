/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * File: rknpu_fence.c
 * Copyright (C) Rockchip Electronics Co., Ltd.
 * Author: Felix Zeng <felix.zeng@rock-chips.com>
 * Modified by: DrDitto with Google AI assitance
 *
 * Target: RK3566 / RK3568 (Single-Core NPU) | DRM GEM + IOMMU | Kernel >= 7.3.0
 */

#include <linux/compiler.h>
#include <linux/dma-fence.h>
#include <linux/file.h>
#include <linux/slab.h>
#include <linux/sync_file.h>

#include "rknpu_drv.h"
#include "rknpu_fence.h"
#include "rknpu_job.h"

static __always_inline const char *rknpu_fence_name_inline(void)
{
	return DRIVER_NAME;
}

static const char *rknpu_fence_get_name(struct dma_fence *fence)
{
	(void)fence;
	return rknpu_fence_name_inline();
}

static const struct dma_fence_ops rknpu_fence_ops = {
	.get_driver_name = rknpu_fence_get_name,
	.get_timeline_name = rknpu_fence_get_name,
};

int rknpu_fence_context_alloc(struct rknpu_device *rknpu_dev)
{
	struct rknpu_fence_context *fence_ctx;

	if (unlikely(!rknpu_dev || !rknpu_dev->dev))
		return -EINVAL;

	fence_ctx = devm_kzalloc(rknpu_dev->dev, sizeof(*fence_ctx),
				 GFP_KERNEL);
	if (unlikely(!fence_ctx))
		return -ENOMEM;

	fence_ctx->context = dma_fence_context_alloc(1);
	atomic_set(&fence_ctx->seqno, 0);
	spin_lock_init(&fence_ctx->spinlock);

	rknpu_dev->fence_ctx = fence_ctx;
	return 0;
}

int rknpu_fence_alloc(struct rknpu_job *job)
{
	struct rknpu_fence_context *fence_ctx;
	struct dma_fence *fence;
	u64 seqno;

	if (unlikely(!job || !job->rknpu_dev || !job->rknpu_dev->fence_ctx))
		return -EINVAL;

	if (unlikely(job->fence))
		return -EBUSY;

	fence_ctx = job->rknpu_dev->fence_ctx;

	fence = kzalloc(sizeof(*fence), GFP_KERNEL);
	if (unlikely(!fence))
		return -ENOMEM;

	seqno = (u64)(u32)atomic_inc_return(&fence_ctx->seqno);
	dma_fence_init(fence, &rknpu_fence_ops, &fence_ctx->spinlock,
		       fence_ctx->context, seqno);

	job->fence = fence;
	return 0;
}

int rknpu_fence_get_fd(struct rknpu_job *job)
{
	struct sync_file *sync_file;
	int fence_fd;

	if (unlikely(!job || !job->fence))
		return -EINVAL;

	fence_fd = get_unused_fd_flags(O_CLOEXEC);
	if (unlikely(fence_fd < 0))
		return fence_fd;

	sync_file = sync_file_create(job->fence);
	if (unlikely(!sync_file)) {
		put_unused_fd(fence_fd);
		return -ENOMEM;
	}

	fd_install(fence_fd, sync_file->file);
	return fence_fd;
}
