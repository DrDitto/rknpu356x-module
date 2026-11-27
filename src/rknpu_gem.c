/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * File: rknpu_gem.c
 * Copyright (C) Rockchip Electronics Co., Ltd.
 * Author: Felix Zeng <felix.zeng@rock-chips.com>
 * Modified by: DrDitto with Google AI assitance
 *
 * Target: RK3566 / RK3568 (Single-Core NPU) | DRM GEM + IOMMU | Kernel >= 7.3.0
 */

#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_prime.h>
#include <drm/drm_vma_manager.h>

#include <linux/compiler.h>
#include <linux/delay.h>
#include <linux/dma-buf.h>
#include <linux/iommu.h>
#include <linux/overflow.h>
#include <linux/pfn.h>
#include <linux/shmem_fs.h>
#include <linux/vmalloc.h>

#include "rknpu_drv.h"
#include "rknpu_gem.h"
#include "rknpu_iommu.h"
#include "rknpu_ioctl.h"

static __always_inline bool rknpu_gem_has_iova_align(
	const struct rknpu_gem_object *rknpu_obj)
{
	return !(rknpu_obj->flags & RKNPU_MEM_IOMMU_LIMIT_IOVA_ALIGNMENT);
}

static void rknpu_gem_free_sgt(struct sg_table **sgt_ptr)
{
	struct sg_table *sgt;

	if (!sgt_ptr || !*sgt_ptr)
		return;

	sgt = *sgt_ptr;
	sg_free_table(sgt);
	kfree(sgt);
	*sgt_ptr = NULL;
}

static int rknpu_gem_get_pages(struct rknpu_gem_object *rknpu_obj)
{
	struct drm_device *drm = rknpu_obj->base.dev;
	bool iova_aligned = rknpu_gem_has_iova_align(rknpu_obj);
	size_t nr_pages;
	int ret;

	rknpu_obj->pages = drm_gem_get_pages(&rknpu_obj->base);
	if (unlikely(IS_ERR(rknpu_obj->pages))) {
		ret = PTR_ERR(rknpu_obj->pages);
		rknpu_obj->pages = NULL;
		LOG_ERROR("failed to get pages: %d\n", ret);
		return ret;
	}

	nr_pages = rknpu_obj->size >> PAGE_SHIFT;
	if (unlikely(!nr_pages || nr_pages > UINT_MAX)) {
		ret = -EOVERFLOW;
		goto put_pages;
	}

	rknpu_obj->num_pages = (unsigned int)nr_pages;

	rknpu_obj->sgt = drm_prime_pages_to_sg(drm, rknpu_obj->pages,
					       rknpu_obj->num_pages);
	if (unlikely(IS_ERR(rknpu_obj->sgt))) {
		ret = PTR_ERR(rknpu_obj->sgt);
		rknpu_obj->sgt = NULL;
		LOG_ERROR("failed to allocate sgt: %d\n", ret);
		goto put_pages;
	}

	ret = rknpu_iommu_dma_map_sg(drm->dev, rknpu_obj->sgt->sgl,
				     rknpu_obj->sgt->orig_nents,
				     DMA_BIDIRECTIONAL, iova_aligned);
	if (unlikely(ret <= 0)) {
		ret = ret < 0 ? ret : -EFAULT;
		LOG_DEV_ERROR(drm->dev, "dma map %zu fail: %d\n",
			      rknpu_obj->size, ret);
		goto free_sgt;
	}
	rknpu_obj->sgt->nents = ret;

	if (rknpu_obj->flags & RKNPU_MEM_KERNEL_MAPPING) {
		rknpu_obj->cookie = vmap(rknpu_obj->pages, rknpu_obj->num_pages,
					 VM_MAP, PAGE_KERNEL);
		if (unlikely(!rknpu_obj->cookie)) {
			ret = -ENOMEM;
			LOG_ERROR("failed to vmap: %d\n", ret);
			goto unmap_sg;
		}
		rknpu_obj->kv_addr = rknpu_obj->cookie;
	}

	rknpu_obj->dma_addr = sg_dma_address(rknpu_obj->sgt->sgl);
	return 0;

unmap_sg:
	rknpu_iommu_dma_unmap_sg(drm->dev, rknpu_obj->sgt->sgl,
				 rknpu_obj->sgt->orig_nents,
				 DMA_BIDIRECTIONAL, iova_aligned);
free_sgt:
	rknpu_gem_free_sgt(&rknpu_obj->sgt);
put_pages:
	drm_gem_put_pages(&rknpu_obj->base, rknpu_obj->pages, false, false);
	rknpu_obj->pages = NULL;
	return ret;
}

static void rknpu_gem_put_pages(struct rknpu_gem_object *rknpu_obj)
{
	struct drm_device *drm = rknpu_obj->base.dev;
	bool iova_aligned = rknpu_gem_has_iova_align(rknpu_obj);

	if (rknpu_obj->flags & RKNPU_MEM_KERNEL_MAPPING) {
		if (rknpu_obj->kv_addr) {
			vunmap(rknpu_obj->kv_addr);
			rknpu_obj->kv_addr = NULL;
			rknpu_obj->cookie = NULL;
		}
	}

	if (rknpu_obj->sgt) {
		rknpu_iommu_dma_unmap_sg(drm->dev, rknpu_obj->sgt->sgl,
					 rknpu_obj->sgt->orig_nents,
					 DMA_BIDIRECTIONAL, iova_aligned);
		rknpu_gem_free_sgt(&rknpu_obj->sgt);
	}

	if (rknpu_obj->pages) {
		drm_gem_put_pages(&rknpu_obj->base, rknpu_obj->pages, true,
				  true);
		rknpu_obj->pages = NULL;
	}
}

static int rknpu_gem_alloc_buf(struct rknpu_gem_object *rknpu_obj)
{
	struct drm_device *drm = rknpu_obj->base.dev;
	size_t nr_pages_sz;
	unsigned int nr_pages;
	struct sg_table *sgt;
	gfp_t gfp_mask = GFP_KERNEL | __GFP_DMA32;
	int ret;

	if (rknpu_obj->dma_addr)
		return 0;

	if (rknpu_obj->flags & RKNPU_MEM_NON_CONTIGUOUS)
		return rknpu_gem_get_pages(rknpu_obj);

	rknpu_obj->dma_attrs = DMA_ATTR_FORCE_CONTIGUOUS;

	if (rknpu_obj->flags & RKNPU_MEM_CACHEABLE) {
#ifdef DMA_ATTR_NON_CONSISTENT
		rknpu_obj->dma_attrs |= DMA_ATTR_NON_CONSISTENT;
#endif
	} else if (rknpu_obj->flags & RKNPU_MEM_WRITE_COMBINE) {
		rknpu_obj->dma_attrs |= DMA_ATTR_WRITE_COMBINE;
	}

	if (!(rknpu_obj->flags & RKNPU_MEM_KERNEL_MAPPING))
		rknpu_obj->dma_attrs |= DMA_ATTR_NO_KERNEL_MAPPING;

	if (rknpu_obj->flags & RKNPU_MEM_ZEROING)
		gfp_mask |= __GFP_ZERO;

	nr_pages_sz = rknpu_obj->size >> PAGE_SHIFT;
	if (unlikely(!nr_pages_sz || nr_pages_sz > UINT_MAX))
		return -EOVERFLOW;

	nr_pages = (unsigned int)nr_pages_sz;
	rknpu_obj->pages = rknpu_gem_alloc_page(nr_pages);
	if (unlikely(!rknpu_obj->pages))
		return -ENOMEM;

	rknpu_obj->cookie = dma_alloc_attrs(drm->dev, rknpu_obj->size,
					    &rknpu_obj->dma_addr, gfp_mask,
					    rknpu_obj->dma_attrs);
	if (unlikely(!rknpu_obj->cookie)) {
		rknpu_gem_free_page(rknpu_obj->pages);
		rknpu_obj->pages = NULL;
		rknpu_obj->flags |= RKNPU_MEM_NON_CONTIGUOUS;
		return rknpu_gem_get_pages(rknpu_obj);
	}

	if (rknpu_obj->flags & RKNPU_MEM_KERNEL_MAPPING)
		rknpu_obj->kv_addr = rknpu_obj->cookie;

	sgt = kzalloc(sizeof(*sgt), GFP_KERNEL);
	if (unlikely(!sgt)) {
		ret = -ENOMEM;
		goto err_free_dma;
	}

	ret = dma_get_sgtable_attrs(drm->dev, sgt, rknpu_obj->cookie,
				    rknpu_obj->dma_addr, rknpu_obj->size,
				    rknpu_obj->dma_attrs);
	if (unlikely(ret < 0))
		goto err_free_sgt;

	ret = drm_prime_sg_to_page_array(sgt, rknpu_obj->pages, nr_pages);
	if (unlikely(ret < 0))
		goto err_free_sg_table;

	rknpu_obj->sgt = sgt;
	return 0;

err_free_sg_table:
	sg_free_table(sgt);
err_free_sgt:
	kfree(sgt);
err_free_dma:
	dma_free_attrs(drm->dev, rknpu_obj->size, rknpu_obj->cookie,
		       rknpu_obj->dma_addr, rknpu_obj->dma_attrs);
	rknpu_obj->cookie = NULL;
	rknpu_obj->kv_addr = NULL;
	rknpu_obj->dma_addr = 0;
	rknpu_gem_free_page(rknpu_obj->pages);
	rknpu_obj->pages = NULL;
	return ret;
}

static void rknpu_gem_free_buf(struct rknpu_gem_object *rknpu_obj)
{
	struct drm_device *drm = rknpu_obj->base.dev;

	if (!rknpu_obj->dma_addr)
		return;

	if (rknpu_obj->flags & RKNPU_MEM_NON_CONTIGUOUS) {
		rknpu_gem_put_pages(rknpu_obj);
		rknpu_obj->dma_addr = 0;
		return;
	}

	rknpu_gem_free_sgt(&rknpu_obj->sgt);

	if (rknpu_obj->cookie) {
		dma_free_attrs(drm->dev, rknpu_obj->size, rknpu_obj->cookie,
			       rknpu_obj->dma_addr, rknpu_obj->dma_attrs);
		rknpu_obj->cookie = NULL;
		rknpu_obj->kv_addr = NULL;
	}

	if (rknpu_obj->pages) {
		rknpu_gem_free_page(rknpu_obj->pages);
		rknpu_obj->pages = NULL;
	}

	rknpu_obj->dma_addr = 0;
}

static const struct drm_gem_object_funcs rknpu_gem_object_funcs = {
	.free = rknpu_gem_free_object,
	.mmap = rknpu_gem_mmap_obj,
};

static struct rknpu_gem_object *rknpu_gem_init(struct drm_device *drm,
					       unsigned long size)
{
	struct rknpu_gem_object *rknpu_obj;
	struct drm_gem_object *obj;
	gfp_t gfp_mask;
	int ret;

	rknpu_obj = kzalloc(sizeof(*rknpu_obj), GFP_KERNEL);
	if (unlikely(!rknpu_obj))
		return ERR_PTR(-ENOMEM);

	obj = &rknpu_obj->base;
	obj->funcs = &rknpu_gem_object_funcs;

	ret = drm_gem_object_init(drm, obj, size);
	if (unlikely(ret < 0)) {
		kfree(rknpu_obj);
		return ERR_PTR(ret);
	}

	rknpu_obj->size = obj->size;

	if (obj->filp && obj->filp->f_mapping) {
		gfp_mask = mapping_gfp_mask(obj->filp->f_mapping);
		gfp_mask &= ~__GFP_HIGHMEM;
		gfp_mask |= __GFP_DMA32;
		mapping_set_gfp_mask(obj->filp->f_mapping, gfp_mask);
	}

	return rknpu_obj;
}

static void rknpu_gem_release(struct rknpu_gem_object *rknpu_obj)
{
	if (!rknpu_obj)
		return;

	drm_gem_object_release(&rknpu_obj->base);
	kfree(rknpu_obj);
}

struct rknpu_gem_object *
rknpu_gem_object_create(struct drm_device *drm, unsigned int flags,
			unsigned long size, int iommu_domain_id,
			unsigned int core_mask)
{
	struct rknpu_device *rknpu_dev;
	struct rknpu_gem_object *rknpu_obj;
	size_t aligned_size;
	int ret;

	if (unlikely(!drm || !drm->dev_private || !size))
		return ERR_PTR(-EINVAL);

	if (unlikely(check_add_overflow(size, (size_t)PAGE_SIZE - 1,
					&aligned_size)))
		return ERR_PTR(-EOVERFLOW);

	aligned_size &= ~((size_t)PAGE_SIZE - 1);
	if (unlikely(!aligned_size))
		return ERR_PTR(-EOVERFLOW);

	rknpu_dev = drm->dev_private;

	rknpu_obj = rknpu_gem_init(drm, aligned_size);
	if (unlikely(IS_ERR(rknpu_obj)))
		return rknpu_obj;

	ret = rknpu_iommu_domain_get_and_switch(rknpu_dev, iommu_domain_id);
	if (unlikely(ret)) {
		rknpu_gem_release(rknpu_obj);
		return ERR_PTR(ret);
	}

	rknpu_obj->iommu_domain_id = iommu_domain_id;
	rknpu_obj->core_mask = core_mask;
	rknpu_obj->flags = flags | RKNPU_MEM_IOMMU;

	ret = rknpu_gem_alloc_buf(rknpu_obj);
	rknpu_iommu_domain_put(rknpu_dev);
	if (unlikely(ret < 0)) {
		rknpu_gem_release(rknpu_obj);
		return ERR_PTR(ret);
	}

	return rknpu_obj;
}

void rknpu_gem_object_destroy(struct rknpu_gem_object *rknpu_obj)
{
	struct drm_gem_object *obj;
	struct rknpu_device *rknpu_dev;
	int wait_count = 0;
	int ret;

	if (unlikely(!rknpu_obj || !rknpu_obj->base.dev ||
		     !rknpu_obj->base.dev->dev_private))
		return;

	obj = &rknpu_obj->base;
	rknpu_dev = obj->dev->dev_private;

	do {
		ret = rknpu_iommu_domain_get_and_switch(
			rknpu_dev, rknpu_obj->iommu_domain_id);
		if (ret && ++wait_count >= 3)
			return;
	} while (ret);

	if (obj->import_attach) {
		drm_prime_gem_destroy(obj, rknpu_obj->sgt);
		rknpu_obj->sgt = NULL;
		if (rknpu_obj->pages) {
			rknpu_gem_free_page(rknpu_obj->pages);
			rknpu_obj->pages = NULL;
		}
	} else {
		rknpu_gem_free_buf(rknpu_obj);
	}

	rknpu_iommu_domain_put(rknpu_dev);
	rknpu_gem_release(rknpu_obj);
}

int rknpu_gem_create_ioctl(struct drm_device *drm, void *data,
			   struct drm_file *file_priv)
{
	struct rknpu_mem_create *args = data;
	struct rknpu_gem_object *rknpu_obj;
	int ret;

	if (unlikely(!drm || !args || !file_priv))
		return -EINVAL;

	rknpu_obj = rknpu_gem_object_find(file_priv, args->handle);
	if (!rknpu_obj) {
		rknpu_obj = rknpu_gem_object_create(drm, args->flags,
						    args->size,
						    args->iommu_domain_id,
						    args->core_mask);
		if (unlikely(IS_ERR(rknpu_obj)))
			return PTR_ERR(rknpu_obj);

		ret = drm_gem_handle_create(file_priv, &rknpu_obj->base,
					    &args->handle);
		if (unlikely(ret)) {
			rknpu_gem_object_destroy(rknpu_obj);
			return ret;
		}
		rknpu_gem_object_put(&rknpu_obj->base);
	}

	args->size = rknpu_obj->size;
	args->sram_size = 0;
	args->obj_addr = (__u64)(uintptr_t)rknpu_obj;
	args->dma_addr = rknpu_obj->dma_addr;

	return 0;
}

int rknpu_gem_map_ioctl(struct drm_device *dev, void *data,
			struct drm_file *file_priv)
{
	struct rknpu_mem_map *args = data;
	struct drm_gem_object *obj;
	int ret;

	if (unlikely(!dev || !args || !file_priv))
		return -EINVAL;

	obj = drm_gem_object_lookup(file_priv, args->handle);
	if (unlikely(!obj))
		return -ENOENT;

	ret = drm_gem_create_mmap_offset(obj);
	if (unlikely(ret)) {
		drm_gem_object_put(obj);
		return ret;
	}

	args->offset = drm_vma_node_start(&obj->vma_node) << PAGE_SHIFT;
	drm_gem_object_put(obj);
	return 0;
}

int rknpu_gem_destroy_ioctl(struct drm_device *drm, void *data,
			    struct drm_file *file_priv)
{
	struct rknpu_device *rknpu_dev;
	struct rknpu_gem_object *rknpu_obj;
	struct rknpu_mem_destroy *args = data;
	int ret;
	int wait_count = 0;

	if (unlikely(!drm || !drm->dev_private || !args || !file_priv))
		return -EINVAL;

	rknpu_dev = drm->dev_private;

	rknpu_obj = rknpu_gem_object_find(file_priv, args->handle);
	if (unlikely(!rknpu_obj))
		return -EINVAL;

	do {
		ret = rknpu_iommu_domain_get_and_switch(
			rknpu_dev, rknpu_obj->iommu_domain_id);
		if (ret && ++wait_count >= 3)
			return ret;
	} while (ret);

	ret = drm_gem_handle_delete(file_priv, args->handle);
	rknpu_iommu_domain_put(rknpu_dev);

	return ret;
}

static int __vm_map_pages(struct vm_area_struct *vma, struct page **pages,
			  unsigned long num, unsigned long offset)
{
	unsigned long count = vma_pages(vma);
	unsigned long uaddr = vma->vm_start;
	unsigned long i;
	int ret;

	if (unlikely(!pages || offset >= num || count > num - offset))
		return -ENXIO;

	for (i = 0; i < count; i++) {
		ret = vm_insert_page(vma, uaddr, pages[offset + i]);
		if (unlikely(ret < 0))
			return ret;
		uaddr += PAGE_SIZE;
	}

	return 0;
}

static int rknpu_gem_mmap_pages(struct rknpu_gem_object *rknpu_obj,
				struct vm_area_struct *vma)
{
	struct drm_device *drm = rknpu_obj->base.dev;
	int ret;

	vm_flags_set(vma, VM_MIXEDMAP);

	ret = __vm_map_pages(vma, rknpu_obj->pages, rknpu_obj->num_pages,
			     vma->vm_pgoff);
	if (unlikely(ret < 0))
		LOG_DEV_ERROR(drm->dev, "failed to map pages into vma: %d\n",
			      ret);

	return ret;
}

static int rknpu_gem_mmap_buffer(struct rknpu_gem_object *rknpu_obj,
				 struct vm_area_struct *vma)
{
	struct drm_device *drm = rknpu_obj->base.dev;
	unsigned long vm_size = vma->vm_end - vma->vm_start;
	unsigned long old_pgoff;
	int ret;

	vm_flags_set(vma, VM_DONTCOPY | VM_DONTEXPAND | VM_DONTDUMP | VM_IO);
	vm_flags_clear(vma, VM_PFNMAP);

	if (unlikely(vm_size > rknpu_obj->size))
		return -EINVAL;

	if (rknpu_obj->flags & RKNPU_MEM_NON_CONTIGUOUS)
		return rknpu_gem_mmap_pages(rknpu_obj, vma);

	old_pgoff = vma->vm_pgoff;
	vma->vm_pgoff = 0;
	ret = dma_mmap_attrs(drm->dev, vma, rknpu_obj->cookie,
			     rknpu_obj->dma_addr, rknpu_obj->size,
			     rknpu_obj->dma_attrs);
	vma->vm_pgoff = old_pgoff;

	if (unlikely(ret < 0))
		LOG_DEV_ERROR(drm->dev, "failed to mmap: %d\n", ret);

	return ret;
}

void rknpu_gem_free_object(struct drm_gem_object *obj)
{
	struct rknpu_device *rknpu_dev;

	if (unlikely(!obj || !obj->dev || !obj->dev->dev_private))
		return;

	rknpu_dev = obj->dev->dev_private;

	rknpu_power_get(rknpu_dev);
	rknpu_gem_object_destroy(to_rknpu_obj(obj));
	rknpu_power_put_delay(rknpu_dev);
}

int rknpu_gem_dumb_create(struct drm_file *file_priv, struct drm_device *drm,
			  struct drm_mode_create_dumb *args)
{
	struct rknpu_gem_object *rknpu_obj;
	unsigned int flags = RKNPU_MEM_NON_CONTIGUOUS | RKNPU_MEM_WRITE_COMBINE;
	u64 pitch64, size64;
	int ret;

	if (unlikely(!file_priv || !drm || !args || !args->width ||
		     !args->height || !args->bpp))
		return -EINVAL;

	if (unlikely(check_add_overflow((u64)args->bpp, 7ULL, &pitch64)))
		return -EOVERFLOW;

	pitch64 = ((u64)args->width * (pitch64 >> 3));
	if (unlikely(pitch64 == 0 || pitch64 > U32_MAX))
		return -EOVERFLOW;

	if (unlikely(check_mul_overflow(pitch64, (u64)args->height, &size64)))
		return -EOVERFLOW;

	args->pitch = (u32)pitch64;
	args->size = size64;

	rknpu_obj = rknpu_gem_object_create(drm, flags, args->size, 0, 0);
	if (unlikely(IS_ERR(rknpu_obj)))
		return PTR_ERR(rknpu_obj);

	ret = drm_gem_handle_create(file_priv, &rknpu_obj->base, &args->handle);
	if (unlikely(ret)) {
		rknpu_gem_object_destroy(rknpu_obj);
		return ret;
	}

	rknpu_gem_object_put(&rknpu_obj->base);
	return 0;
}

int rknpu_gem_mmap_obj(struct drm_gem_object *obj, struct vm_area_struct *vma)
{
	struct rknpu_gem_object *rknpu_obj;
	int ret;

	if (unlikely(!obj || !vma))
		return -EINVAL;

	rknpu_obj = to_rknpu_obj(obj);

	if (rknpu_obj->flags & RKNPU_MEM_CACHEABLE)
		vma->vm_page_prot = vm_get_page_prot(vma->vm_flags);
	else if (rknpu_obj->flags & RKNPU_MEM_WRITE_COMBINE)
		vma->vm_page_prot =
			pgprot_writecombine(vm_get_page_prot(vma->vm_flags));
	else
		vma->vm_page_prot =
			pgprot_noncached(vm_get_page_prot(vma->vm_flags));

	ret = rknpu_gem_mmap_buffer(rknpu_obj, vma);
	if (unlikely(ret))
		drm_gem_vm_close(vma);

	return ret;
}

int rknpu_gem_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct drm_gem_object *obj;
	int ret;

	ret = drm_gem_mmap(filp, vma);
	if (unlikely(ret < 0))
		return ret;

	obj = vma->vm_private_data;
	if (!obj)
		return -EINVAL;

	if (obj->import_attach)
		return dma_buf_mmap(obj->dma_buf, vma, 0);

	return rknpu_gem_mmap_obj(obj, vma);
}

struct sg_table *rknpu_gem_prime_get_sg_table(struct drm_gem_object *obj)
{
	struct rknpu_gem_object *rknpu_obj;
	size_t npages;

	if (unlikely(!obj))
		return ERR_PTR(-EINVAL);

	rknpu_obj = to_rknpu_obj(obj);
	npages = rknpu_obj->size >> PAGE_SHIFT;

	return drm_prime_pages_to_sg(rknpu_obj->base.dev, rknpu_obj->pages,
				     npages);
}

struct drm_gem_object *
rknpu_gem_prime_import_sg_table(struct drm_device *dev,
				struct dma_buf_attachment *attach,
				struct sg_table *sgt)
{
	struct rknpu_gem_object *rknpu_obj;
	size_t npages_sz;
	unsigned int npages;
	int ret;

	if (unlikely(!dev || !attach || !attach->dmabuf || !sgt || !sgt->sgl))
		return ERR_PTR(-EINVAL);

	rknpu_obj = rknpu_gem_init(dev, attach->dmabuf->size);
	if (unlikely(IS_ERR(rknpu_obj)))
		return ERR_CAST(rknpu_obj);

	rknpu_obj->dma_addr = sg_dma_address(sgt->sgl);
	npages_sz = rknpu_obj->size >> PAGE_SHIFT;
	if (unlikely(!npages_sz || npages_sz > UINT_MAX)) {
		ret = -EOVERFLOW;
		goto err_release;
	}
	npages = (unsigned int)npages_sz;

	rknpu_obj->pages = rknpu_gem_alloc_page(npages);
	if (unlikely(!rknpu_obj->pages)) {
		ret = -ENOMEM;
		goto err_release;
	}

	ret = drm_prime_sg_to_page_array(sgt, rknpu_obj->pages, npages);
	if (unlikely(ret < 0))
		goto err_free_pages;

	rknpu_obj->num_pages = npages;
	rknpu_obj->sgt = sgt;
	rknpu_obj->flags |= (sgt->nents == 1) ? RKNPU_MEM_CONTIGUOUS :
						RKNPU_MEM_NON_CONTIGUOUS;

	return &rknpu_obj->base;

err_free_pages:
	rknpu_gem_free_page(rknpu_obj->pages);
	rknpu_obj->pages = NULL;
err_release:
	rknpu_gem_release(rknpu_obj);
	return ERR_PTR(ret);
}

int rknpu_gem_prime_vmap(struct drm_gem_object *obj, struct iosys_map *map)
{
	struct rknpu_gem_object *rknpu_obj;
	void *vaddr;

	if (unlikely(!obj || !map))
		return -EINVAL;

	rknpu_obj = to_rknpu_obj(obj);
	if (unlikely(!rknpu_obj->pages || !rknpu_obj->num_pages))
		return -EINVAL;

	vaddr = vmap(rknpu_obj->pages, rknpu_obj->num_pages, VM_MAP,
		     PAGE_KERNEL);
	if (unlikely(!vaddr))
		return -ENOMEM;

	iosys_map_set_vaddr(map, vaddr);
	return 0;
}

void rknpu_gem_prime_vunmap(struct drm_gem_object *obj, struct iosys_map *map)
{
	if (!map || !map->vaddr)
		return;

	vunmap(map->vaddr);
	iosys_map_clear(map);
}

int rknpu_gem_prime_mmap(struct drm_gem_object *obj, struct vm_area_struct *vma)
{
	int ret;

	if (unlikely(!obj || !vma))
		return -EINVAL;

	ret = drm_gem_mmap_obj(obj, obj->size, vma);
	if (unlikely(ret < 0))
		return ret;

	return rknpu_gem_mmap_obj(obj, vma);
}

int rknpu_gem_sync_ioctl(struct drm_device *dev, void *data,
			 struct drm_file *file_priv)
{
	struct rknpu_device *rknpu_dev;
	struct rknpu_mem_sync *args = data;
	struct rknpu_gem_object *rknpu_obj;
	struct scatterlist *sg;
	dma_addr_t sg_dma_addr;
	unsigned long length, offset, sg_offset, sg_left, size;
	unsigned long end_offset;
	unsigned long len = 0;
	int i;

	if (unlikely(!dev || !dev->dev_private || !args))
		return -EINVAL;

	rknpu_dev = dev->dev_private;
	rknpu_obj = (struct rknpu_gem_object *)(uintptr_t)args->obj_addr;
	if (unlikely(!rknpu_obj || !(rknpu_obj->flags & RKNPU_MEM_CACHEABLE)))
		return -EINVAL;

	if (unlikely(check_add_overflow((unsigned long)args->offset,
					(unsigned long)args->size,
					&end_offset) ||
		     end_offset > rknpu_obj->size))
		return -EINVAL;

	if (unlikely((rknpu_obj->flags & RKNPU_MEM_NON_CONTIGUOUS) &&
		     !rknpu_obj->sgt))
		return -EINVAL;

	if (unlikely(rknpu_iommu_domain_get_and_switch(
			     rknpu_dev, rknpu_obj->iommu_domain_id)))
		return -EINVAL;

	if (!(rknpu_obj->flags & RKNPU_MEM_NON_CONTIGUOUS)) {
		if (args->flags & RKNPU_MEM_SYNC_TO_DEVICE)
			dma_sync_single_range_for_device(
				dev->dev, rknpu_obj->dma_addr, args->offset,
				args->size, DMA_TO_DEVICE);
		if (args->flags & RKNPU_MEM_SYNC_FROM_DEVICE)
			dma_sync_single_range_for_cpu(dev->dev,
						      rknpu_obj->dma_addr,
						      args->offset, args->size,
						      DMA_FROM_DEVICE);
	} else {
		length = args->size;
		offset = args->offset;

		for_each_sg(rknpu_obj->sgt->sgl, sg, rknpu_obj->sgt->nents, i) {
			if (length == 0)
				break;

			if (unlikely(check_add_overflow(len, (unsigned long)sg->length,
							&len)))
				break;

			if (len <= offset)
				continue;

			sg_dma_addr = sg_dma_address(sg);
			sg_left = len - offset;
			sg_offset = sg->length - sg_left;
			size = min(length, sg_left);

			if (args->flags & RKNPU_MEM_SYNC_TO_DEVICE)
				dma_sync_single_range_for_device(
					dev->dev, sg_dma_addr, sg_offset, size,
					DMA_TO_DEVICE);

			if (args->flags & RKNPU_MEM_SYNC_FROM_DEVICE)
				dma_sync_single_range_for_cpu(
					dev->dev, sg_dma_addr, sg_offset, size,
					DMA_FROM_DEVICE);

			offset += size;
			length -= size;
		}
	}

	rknpu_iommu_domain_put(rknpu_dev);
	return 0;
}
