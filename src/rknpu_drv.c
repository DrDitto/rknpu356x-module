/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * File: rknpu_drv.c
 * Copyright (C) Rockchip Electronics Co., Ltd.
 * Author: Felix Zeng <felix.zeng@rock-chips.com>
 * Modified by: DrDitto with Google AI assitance
 *
 * Target: RK3566 / RK3568 (Single-Core NPU) | DRM GEM + IOMMU | Kernel >= 7.3.0
 */

#include <linux/clk.h>
#include <linux/compiler.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/wait.h>

#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_gem.h>
#include <drm/drm_ioctl.h>

#include "rknpu_devfreq.h"
#include "rknpu_drv.h"
#include "rknpu_fence.h"
#include "rknpu_gem.h"
#include "rknpu_ioctl.h"
#include "rknpu_iommu.h"
#include "rknpu_reset.h"

static bool bypass_irq_handler;
module_param(bypass_irq_handler, bool, 0644);
MODULE_PARM_DESC(bypass_irq_handler, "Bypass irq handler");

static bool bypass_soft_reset;
module_param(bypass_soft_reset, bool, 0644);
MODULE_PARM_DESC(bypass_soft_reset, "Bypass soft reset");

extern irqreturn_t rknpu_platform_irq_handler(int irq, void *dev_id);

int rknpu_power_on(struct rknpu_device *rknpu_dev);
int rknpu_power_off(struct rknpu_device *rknpu_dev);
int rknpu_action(struct rknpu_device *rknpu_dev, struct rknpu_action *args);

/* RK3566 and RK3568 share the identical single-core NPU IP */
static const struct rknpu_config rk356x_rknpu_config = {
        .core_mask = RKNPU_CORE0_MASK,
        .bw_priority_addr = 0xFE880000,
        .bw_priority_length = 0x100,
        .dma_mask = DMA_BIT_MASK(32),
        .pc_data_amount_scale = 1,
        .pc_task_number_bits = 12,
        .pc_task_number_mask = 0xfff,
        .pc_task_status_offset = 0x3c,
        .max_submit_number = (1 << 12) - 1,
};

static const struct of_device_id rknpu_of_match[] = {
        { .compatible = "rockchip,rk3566-npu", .data = &rk356x_rknpu_config },
        { .compatible = "rockchip,rk3568-npu", .data = &rk356x_rknpu_config },
        { .compatible = "rockchip,rk3568-rknpu", .data = &rk356x_rknpu_config },
        { .compatible = "rockchip,rknpu", .data = &rk356x_rknpu_config },
        {},
};
MODULE_DEVICE_TABLE(of, rknpu_of_match);

static int rknpu_register_irq(struct platform_device *pdev,
                              struct rknpu_device *rknpu_dev)
{
        int irq;

        if (unlikely(!pdev || !rknpu_dev || !pdev->dev.of_node))
                return -EINVAL;

        irq = of_irq_get_byname(pdev->dev.of_node, "npu_irq");
        if (irq < 0)
                irq = of_irq_get(pdev->dev.of_node, 0);
        if (irq < 0)
                irq = platform_get_irq_byname(pdev, "npu_irq");

        if (unlikely(irq < 0)) {
                dev_err(&pdev->dev, "Failed to resolve GICv3 virtual IRQ: %d\n",
                        irq);
                return irq;
        }

        dev_info(&pdev->dev, "Assigned virtual IRQ #%d\n", irq);

        return devm_request_threaded_irq(&pdev->dev, irq, NULL,
                                         rknpu_platform_irq_handler,
                                         IRQF_SHARED | IRQF_ONESHOT,
                                         dev_name(&pdev->dev), rknpu_dev);
}

static __always_inline int rknpu_get_drv_version(u32 *version)
{
        if (unlikely(!version))
                return -EINVAL;

        *version = RKNPU_GET_DRV_VERSION_CODE(DRIVER_MAJOR, DRIVER_MINOR,
                                              DRIVER_PATCHLEVEL);
        return 0;
}

static void rknpu_power_off_delay_work(struct work_struct *power_off_work)
{
        struct rknpu_device *rknpu_dev =
                container_of(to_delayed_work(power_off_work),
                             struct rknpu_device, power_off_work);
        int ret = 0;

        if (unlikely(!rknpu_dev))
                return;

        mutex_lock(&rknpu_dev->power_lock);
        if (atomic_dec_if_positive(&rknpu_dev->power_refcount) == 0) {
                ret = rknpu_power_off(rknpu_dev);
                if (unlikely(ret)) {
                        atomic_inc(&rknpu_dev->power_refcount);
                        if (rknpu_dev->power_off_wq)
                                queue_delayed_work(
                                        rknpu_dev->power_off_wq,
                                        &rknpu_dev->power_off_work,
                                        msecs_to_jiffies(
                                                max(rknpu_dev->power_put_delay,
                                                    100U)));
                }
        }
        mutex_unlock(&rknpu_dev->power_lock);
}

int rknpu_power_get(struct rknpu_device *rknpu_dev)
{
        int ret = 0;

        if (unlikely(!rknpu_dev))
                return -EINVAL;

        mutex_lock(&rknpu_dev->power_lock);
        if (atomic_inc_return(&rknpu_dev->power_refcount) == 1) {
                ret = rknpu_power_on(rknpu_dev);
                if (unlikely(ret))
                        atomic_dec(&rknpu_dev->power_refcount);
        }
        mutex_unlock(&rknpu_dev->power_lock);

        return ret;
}

int rknpu_power_put(struct rknpu_device *rknpu_dev)
{
        int ret = 0;

        if (unlikely(!rknpu_dev))
                return -EINVAL;

        mutex_lock(&rknpu_dev->power_lock);
        if (atomic_dec_if_positive(&rknpu_dev->power_refcount) == 0) {
                ret = rknpu_power_off(rknpu_dev);
                if (unlikely(ret))
                        atomic_inc(&rknpu_dev->power_refcount);
        }
        mutex_unlock(&rknpu_dev->power_lock);

        return ret;
}

int rknpu_power_put_delay(struct rknpu_device *rknpu_dev)
{
        if (unlikely(!rknpu_dev))
                return -EINVAL;

        if (rknpu_dev->power_put_delay == 0)
                return rknpu_power_put(rknpu_dev);

        mutex_lock(&rknpu_dev->power_lock);
        if (atomic_read(&rknpu_dev->power_refcount) == 1) {
                if (likely(rknpu_dev->power_off_wq))
                        queue_delayed_work(
                                rknpu_dev->power_off_wq,
                                &rknpu_dev->power_off_work,
                                msecs_to_jiffies(rknpu_dev->power_put_delay));
                else
                        atomic_dec_if_positive(&rknpu_dev->power_refcount);
        } else {
                atomic_dec_if_positive(&rknpu_dev->power_refcount);
        }
        mutex_unlock(&rknpu_dev->power_lock);

        return 0;
}

int rknpu_power_on(struct rknpu_device *rknpu_dev)
{
        struct device *dev;
        int ret;

        if (unlikely(!rknpu_dev || !rknpu_dev->dev))
                return -EINVAL;

        dev = rknpu_dev->dev;

        if (rknpu_dev->vdd) {
                ret = regulator_enable(rknpu_dev->vdd);
                if (unlikely(ret))
                        return ret;
        }

        if (rknpu_dev->mem) {
                ret = regulator_enable(rknpu_dev->mem);
                if (unlikely(ret)) {
                        if (rknpu_dev->vdd)
                                regulator_disable(rknpu_dev->vdd);
                        return ret;
                }
        }

        ret = clk_bulk_prepare_enable(rknpu_dev->num_clks, rknpu_dev->clks);
        if (unlikely(ret)) {
                if (rknpu_dev->mem)
                        regulator_disable(rknpu_dev->mem);
                if (rknpu_dev->vdd)
                        regulator_disable(rknpu_dev->vdd);
                return ret;
        }

        if (rknpu_dev->clk_npu_src && rknpu_dev->clk_gpll) {
                ret = clk_set_parent(rknpu_dev->clk_npu_src, rknpu_dev->clk_gpll);
                if (ret)
                        dev_warn(dev, "clk_set_parent(clk_npu_src, gpll) failed: %d\n", ret);
        }

        if (rknpu_dev->clk_core) {
                ret = clk_set_rate(rknpu_dev->clk_core, 600000000UL);
                if (ret)
                        dev_warn(dev, "clk_set_rate(clk) failed: %d\n", ret);
        }

        if (rknpu_dev->clk_aclk) {
                ret = clk_set_rate(rknpu_dev->clk_aclk, 300000000UL);
                if (ret)
                        dev_warn(dev, "clk_set_rate(aclk) failed: %d\n", ret);
        }

        if (rknpu_dev->clk_hclk) {
                ret = clk_set_rate(rknpu_dev->clk_hclk, 150000000UL);
                if (ret)
                        dev_warn(dev, "clk_set_rate(hclk) failed: %d\n", ret);
        }

        ret = pm_runtime_get_sync(dev);
        if (unlikely(ret < 0)) {
                pm_runtime_put_noidle(dev);
                clk_bulk_disable_unprepare(rknpu_dev->num_clks, rknpu_dev->clks);
                if (rknpu_dev->mem)
                        regulator_disable(rknpu_dev->mem);
                if (rknpu_dev->vdd)
                        regulator_disable(rknpu_dev->vdd);
                return ret;
        }

        return 0;
}

int rknpu_power_off(struct rknpu_device *rknpu_dev)
{
        struct device *dev;

        if (unlikely(!rknpu_dev || !rknpu_dev->dev))
                return -EINVAL;

        dev = rknpu_dev->dev;

        pm_runtime_put_sync(dev);
        clk_bulk_disable_unprepare(rknpu_dev->num_clks, rknpu_dev->clks);
        if (rknpu_dev->mem)
                regulator_disable(rknpu_dev->mem);
        if (rknpu_dev->vdd)
                regulator_disable(rknpu_dev->vdd);

        return 0;
}

int rknpu_action(struct rknpu_device *rknpu_dev, struct rknpu_action *args)
{
        int ret = -EINVAL;

        if (unlikely(!rknpu_dev || !args))
                return -EINVAL;

        switch (args->flags) {
        case RKNPU_GET_HW_VERSION:
                ret = rknpu_get_hw_version(rknpu_dev, &args->value);
                break;
        case RKNPU_GET_DRV_VERSION:
                ret = rknpu_get_drv_version(&args->value);
                break;
        case RKNPU_GET_FREQ:
                if (unlikely(!rknpu_dev->clks || rknpu_dev->num_clks < 1 ||
                             !rknpu_dev->clks[0].clk))
                        return -ENODEV;
                args->value = clk_get_rate(rknpu_dev->clks[0].clk);
                ret = 0;
                break;
        case RKNPU_GET_VOLT:
                args->value = rknpu_dev->vdd ?
                                      regulator_get_voltage(rknpu_dev->vdd) :
                                      850000;
                ret = 0;
                break;
        case RKNPU_ACT_RESET:
                ret = rknpu_soft_reset(rknpu_dev);
                break;
        case RKNPU_GET_BW_PRIORITY:
                ret = rknpu_get_bw_priority(rknpu_dev, &args->value, NULL, NULL);
                break;
        case RKNPU_SET_BW_PRIORITY:
                ret = rknpu_set_bw_priority(rknpu_dev, args->value, 0, 0);
                break;
        case RKNPU_ACT_CLR_TOTAL_RW_AMOUNT:
                ret = rknpu_clear_rw_amount(rknpu_dev);
                break;
        case RKNPU_GET_DT_WR_AMOUNT:
                ret = rknpu_get_rw_amount(rknpu_dev, &args->value, NULL, NULL);
                break;
        case RKNPU_GET_DT_RD_AMOUNT:
                ret = rknpu_get_rw_amount(rknpu_dev, NULL, &args->value, NULL);
                break;
        case RKNPU_GET_WT_RD_AMOUNT:
                ret = rknpu_get_rw_amount(rknpu_dev, NULL, NULL, &args->value);
                break;
        case RKNPU_GET_TOTAL_RW_AMOUNT:
                ret = rknpu_get_total_rw_amount(rknpu_dev, &args->value);
                break;
        case RKNPU_GET_IOMMU_EN:
                args->value = rknpu_dev->iommu_en ? 1 : 0;
                ret = 0;
                break;
        case RKNPU_SET_PROC_NICE:
                set_user_nice(current, *(s32 *)&args->value);
                ret = 0;
                break;
        case RKNPU_GET_IOMMU_DOMAIN_ID:
                args->value = rknpu_dev->iommu_domain_id;
                ret = 0;
                break;
        case RKNPU_SET_IOMMU_DOMAIN_ID:
                ret = rknpu_iommu_domain_get_and_switch(rknpu_dev,
                                                        *(s32 *)&args->value);
                if (!ret)
                        rknpu_iommu_domain_put(rknpu_dev);
                break;
        default:
                ret = -EINVAL;
                break;
        }

        return ret;
}
EXPORT_SYMBOL(rknpu_action);

static int rknpu_action_ioctl(struct drm_device *dev, void *data,
                              struct drm_file *file_priv)
{
        struct rknpu_device *rknpu_dev;

        if (unlikely(!dev || !dev->dev_private || !data))
                return -EINVAL;

        rknpu_dev = dev->dev_private;
        return rknpu_action(rknpu_dev, (struct rknpu_action *)data);
}

#define RKNPU_IOCTL(func)                                                      \
        static int __##func(struct drm_device *dev, void *data,               \
                            struct drm_file *file_priv)                        \
        {                                                                      \
                struct rknpu_device *rknpu_dev;                                \
                int ret;                                                       \
                if (unlikely(!dev || !dev->dev_private))                       \
                        return -EINVAL;                                        \
                rknpu_dev = dev->dev_private;                                  \
                ret = rknpu_power_get(rknpu_dev);                              \
                if (unlikely(ret))                                             \
                        return ret;                                            \
                ret = func(dev, data, file_priv);                              \
                rknpu_power_put_delay(rknpu_dev);                              \
                return ret;                                                    \
        }

RKNPU_IOCTL(rknpu_action_ioctl);
RKNPU_IOCTL(rknpu_submit_ioctl);
RKNPU_IOCTL(rknpu_gem_create_ioctl);
RKNPU_IOCTL(rknpu_gem_map_ioctl);
RKNPU_IOCTL(rknpu_gem_destroy_ioctl);
RKNPU_IOCTL(rknpu_gem_sync_ioctl);

static const struct drm_ioctl_desc rknpu_ioctls[] = {
        DRM_IOCTL_DEF_DRV(RKNPU_ACTION, __rknpu_action_ioctl, DRM_RENDER_ALLOW),
        DRM_IOCTL_DEF_DRV(RKNPU_SUBMIT, __rknpu_submit_ioctl, DRM_RENDER_ALLOW),
        DRM_IOCTL_DEF_DRV(RKNPU_MEM_CREATE, __rknpu_gem_create_ioctl, DRM_RENDER_ALLOW),
        DRM_IOCTL_DEF_DRV(RKNPU_MEM_MAP, __rknpu_gem_map_ioctl, DRM_RENDER_ALLOW),
        DRM_IOCTL_DEF_DRV(RKNPU_MEM_DESTROY, __rknpu_gem_destroy_ioctl, DRM_RENDER_ALLOW),
        DRM_IOCTL_DEF_DRV(RKNPU_MEM_SYNC, __rknpu_gem_sync_ioctl, DRM_RENDER_ALLOW),
};

DEFINE_DRM_GEM_FOPS(rknpu_drm_driver_fops);

static struct drm_driver rknpu_drm_driver = {
        .driver_features = DRIVER_GEM | DRIVER_RENDER,
        .dumb_create = rknpu_gem_dumb_create,
        .dumb_map_offset = drm_gem_dumb_map_offset,
        .prime_handle_to_fd = drm_gem_prime_handle_to_fd,
        .prime_fd_to_handle = drm_gem_prime_fd_to_handle,
        .gem_prime_import = drm_gem_prime_import,
        .gem_prime_import_sg_table = rknpu_gem_prime_import_sg_table,
        .ioctls = rknpu_ioctls,
        .num_ioctls = ARRAY_SIZE(rknpu_ioctls),
        .fops = &rknpu_drm_driver_fops,
        .name = DRIVER_NAME,
        .desc = DRIVER_DESC,
        .major = DRIVER_MAJOR,
        .minor = DRIVER_MINOR,
        .patchlevel = DRIVER_PATCHLEVEL,
};

static int rknpu_drm_probe(struct rknpu_device *rknpu_dev)
{
        struct drm_device *drm_dev;
        int ret;

        if (unlikely(!rknpu_dev || !rknpu_dev->dev))
                return -EINVAL;

        drm_dev = drm_dev_alloc(&rknpu_drm_driver, rknpu_dev->dev);
        if (unlikely(IS_ERR(drm_dev)))
                return PTR_ERR(drm_dev);

        drm_dev->dev_private = rknpu_dev;
        rknpu_dev->drm_dev = drm_dev;

        ret = drm_dev_register(drm_dev, 0);
        if (unlikely(ret < 0)) {
                drm_dev_put(drm_dev);
                rknpu_dev->drm_dev = NULL;
                return ret;
        }

        return 0;
}

static void rknpu_drm_remove(struct rknpu_device *rknpu_dev)
{
        struct drm_device *drm_dev;

        if (unlikely(!rknpu_dev || !rknpu_dev->drm_dev))
                return;

        drm_dev = rknpu_dev->drm_dev;
        drm_dev_unregister(drm_dev);
        drm_dev_put(drm_dev);
        rknpu_dev->drm_dev = NULL;
}

static void rknpu_put_clks(struct rknpu_device *rknpu_dev)
{
        if (rknpu_dev->clk_npu_src) {
                clk_put(rknpu_dev->clk_npu_src);
                rknpu_dev->clk_npu_src = NULL;
        }
        if (rknpu_dev->clk_gpll) {
                clk_put(rknpu_dev->clk_gpll);
                rknpu_dev->clk_gpll = NULL;
        }
}

static int rknpu_probe(struct platform_device *pdev)
{
        struct resource *res;
        struct rknpu_device *rknpu_dev;
        struct device *dev = &pdev->dev;
        const struct rknpu_config *config;
        int ret;

        if (unlikely(!dev->of_node))
                return -ENODEV;

        config = of_device_get_match_data(dev);
        if (unlikely(!config))
                return -ENODEV;

        if (unlikely(!of_property_present(dev->of_node, "iommus"))) {
                dev_err(dev,
                        "Missing mandatory 'iommus' property in Device Tree\n");
                return -ENODEV;
        }

        ret = dma_set_mask_and_coherent(dev, config->dma_mask);
        if (unlikely(ret)) {
                dev_err(dev, "Failed to set 32-bit DMA mask: %d\n", ret);
                return ret;
        }

        rknpu_dev = devm_kzalloc(dev, sizeof(*rknpu_dev), GFP_KERNEL);
        if (unlikely(!rknpu_dev))
                return -ENOMEM;

        rknpu_dev->config = config;
        rknpu_dev->dev = dev;
        rknpu_dev->iommu_en = true;
        platform_set_drvdata(pdev, rknpu_dev);

        rknpu_dev->iommu_group = iommu_group_get(dev);
        if (unlikely(!rknpu_dev->iommu_group)) {
                dev_err(dev, "Failed to get IOMMU group\n");
                return -EPROBE_DEFER;
        }

        rknpu_dev->bypass_irq_handler = bypass_irq_handler;
        rknpu_dev->bypass_soft_reset = bypass_soft_reset;

        ret = rknpu_reset_get(rknpu_dev);
        if (unlikely(ret < 0))
                goto err_put_iommu;

        rknpu_dev->num_clks = devm_clk_bulk_get_all(dev, &rknpu_dev->clks);
        if (unlikely(rknpu_dev->num_clks < 1)) {
                ret = rknpu_dev->num_clks < 0 ? rknpu_dev->num_clks : -ENODEV;
                goto err_put_iommu;
        }

        rknpu_dev->clk_core = devm_clk_get_optional(dev, "clk");
        if (IS_ERR(rknpu_dev->clk_core))
                rknpu_dev->clk_core = NULL;

        rknpu_dev->clk_aclk = devm_clk_get_optional(dev, "aclk");
        if (IS_ERR(rknpu_dev->clk_aclk))
                rknpu_dev->clk_aclk = NULL;

        rknpu_dev->clk_hclk = devm_clk_get_optional(dev, "hclk");
        if (IS_ERR(rknpu_dev->clk_hclk))
                rknpu_dev->clk_hclk = NULL;

        rknpu_dev->clk_npu_src = clk_get(dev, "clk_npu_src");
        if (IS_ERR(rknpu_dev->clk_npu_src))
                rknpu_dev->clk_npu_src = NULL;

        rknpu_dev->clk_gpll = clk_get(dev, "gpll");
        if (IS_ERR(rknpu_dev->clk_gpll))
                rknpu_dev->clk_gpll = NULL;

        rknpu_dev->vdd = devm_regulator_get_optional(dev, "rknpu");
        if (IS_ERR(rknpu_dev->vdd)) {
                if (PTR_ERR(rknpu_dev->vdd) != -ENODEV) {
                        ret = PTR_ERR(rknpu_dev->vdd);
                        goto err_put_clks;
                }
                rknpu_dev->vdd = NULL;
        }

        rknpu_dev->mem = devm_regulator_get_optional(dev, "mem");
        if (IS_ERR(rknpu_dev->mem)) {
                if (PTR_ERR(rknpu_dev->mem) != -ENODEV) {
                        ret = PTR_ERR(rknpu_dev->mem);
                        goto err_put_clks;
                }
                rknpu_dev->mem = NULL;
        }

        spin_lock_init(&rknpu_dev->lock);
        spin_lock_init(&rknpu_dev->irq_lock);
        mutex_init(&rknpu_dev->power_lock);
        mutex_init(&rknpu_dev->reset_lock);
        mutex_init(&rknpu_dev->domain_lock);
        mutex_init(&rknpu_dev->devfreq_lock);

        INIT_LIST_HEAD(&rknpu_dev->subcore_data.todo_list);
        init_waitqueue_head(&rknpu_dev->subcore_data.job_done_wq);
        rknpu_dev->subcore_data.task_num = 0;

        res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
        if (unlikely(!res)) {
                ret = -ENXIO;
                goto err_put_clks;
        }

        rknpu_dev->base = devm_ioremap_resource(dev, res);
        if (PTR_ERR(rknpu_dev->base) == -EBUSY)
                rknpu_dev->base = devm_ioremap(dev, res->start,
                                               resource_size(res));

        if (unlikely(IS_ERR(rknpu_dev->base))) {
                ret = PTR_ERR(rknpu_dev->base);
                goto err_put_clks;
        }

        if (!rknpu_dev->bypass_irq_handler) {
                ret = rknpu_register_irq(pdev, rknpu_dev);
                if (unlikely(ret))
                        goto err_put_clks;
        }

        ret = rknpu_drm_probe(rknpu_dev);
        if (unlikely(ret)) {
                dev_err(dev, "Failed to register DRM device: %d\n", ret);
                goto err_put_clks;
        }

#ifdef CONFIG_ROCKCHIP_RKNPU_FENCE
        ret = rknpu_fence_context_alloc(rknpu_dev);
        if (unlikely(ret))
                goto err_remove_drm;
#endif

        pm_runtime_enable(dev);

        ret = rknpu_power_on(rknpu_dev);
        if (unlikely(ret))
                goto err_disable_pm;

        ret = rknpu_devfreq_init(rknpu_dev);
        if (unlikely(ret))
                goto err_power_off;

        rknpu_power_off(rknpu_dev);
        atomic_set(&rknpu_dev->power_refcount, 0);

        rknpu_dev->power_put_delay = 3000;
        rknpu_dev->power_off_wq = create_freezable_workqueue("rknpu_power_off_wq");
        if (likely(rknpu_dev->power_off_wq))
                INIT_DEFERRABLE_WORK(&rknpu_dev->power_off_work,
                                     rknpu_power_off_delay_work);

        dev_info(dev, "RK356x NPU initialized (DRM Render Node + IOMMU)\n");
        return 0;

err_power_off:
        rknpu_power_off(rknpu_dev);
err_disable_pm:
        pm_runtime_disable(dev);
#ifdef CONFIG_ROCKCHIP_RKNPU_FENCE
err_remove_drm:
#endif
        rknpu_drm_remove(rknpu_dev);
err_put_clks:
        rknpu_put_clks(rknpu_dev);
err_put_iommu:
        if (rknpu_dev->iommu_group) {
                iommu_group_put(rknpu_dev->iommu_group);
                rknpu_dev->iommu_group = NULL;
        }
        return ret;
}

static void rknpu_remove(struct platform_device *pdev)
{
        struct rknpu_device *rknpu_dev = platform_get_drvdata(pdev);

        if (unlikely(!rknpu_dev))
                return;

        if (rknpu_dev->power_off_wq) {
                cancel_delayed_work_sync(&rknpu_dev->power_off_work);
                destroy_workqueue(rknpu_dev->power_off_wq);
                rknpu_dev->power_off_wq = NULL;
        }

        rknpu_devfreq_remove(rknpu_dev);
        rknpu_drm_remove(rknpu_dev);

        mutex_lock(&rknpu_dev->power_lock);
        if (atomic_read(&rknpu_dev->power_refcount) > 0) {
                atomic_set(&rknpu_dev->power_refcount, 0);
                rknpu_power_off(rknpu_dev);
        }
        mutex_unlock(&rknpu_dev->power_lock);

        pm_runtime_disable(&pdev->dev);
        rknpu_put_clks(rknpu_dev);

        if (rknpu_dev->iommu_group) {
                iommu_group_put(rknpu_dev->iommu_group);
                rknpu_dev->iommu_group = NULL;
        }
}

static struct platform_driver rknpu_driver = {
        .driver = {
                .name = "rknpu",
                .of_match_table = rknpu_of_match,
        },
        .probe = rknpu_probe,
        .remove = rknpu_remove,
};

module_platform_driver(rknpu_driver);

MODULE_AUTHOR("Rockchip Electronics");
MODULE_DESCRIPTION("RKNPU Driver for RK3566/RK3568 (Linux >= 7.2)");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("DMA_BUF");
