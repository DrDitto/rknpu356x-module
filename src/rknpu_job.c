/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * File: rknpu_job.c
 * Copyright (C) Rockchip Electronics Co., Ltd.
 * Author: Felix Zeng <felix.zeng@rock-chips.com>
 * Modified by: DrDitto with Google AI assitance
 *
 * Target: RK3566 / RK3568 (Single-Core NPU) | DRM GEM + IOMMU | Kernel >= 7.3.0
 */

#include <asm/memory.h>
#include <linux/compiler.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/overflow.h>
#include <linux/slab.h>
#include <linux/sync_file.h>

#include "rknpu_ioctl.h"
#include "rknpu_drv.h"
#include "rknpu_reset.h"
#include "rknpu_gem.h"
#include "rknpu_fence.h"
#include "rknpu_iommu.h"
#include "rknpu_job.h"

#define _REG_READ(base, offset) readl((base) + (offset))
#define _REG_WRITE(base, value, offset) writel((value), (base) + (offset))

#define REG_READ(offset) _REG_READ(rknpu_core_base, (offset))
#define REG_WRITE(value, offset) _REG_WRITE(rknpu_core_base, (value), (offset))

#define RKNPU_RAW_IRQ_BIT16 0x10000U

static __always_inline bool rknpu_is_valid_kptr(unsigned long addr)
{
#ifdef VA_START
        return addr >= (unsigned long)VA_START;
#elif defined(TASK_SIZE_MAX)
        return addr >= (unsigned long)TASK_SIZE_MAX;
#else
        return addr >= (1ULL << 47);
#endif
}

static __always_inline struct rknpu_gem_object *
rknpu_job_get_task_obj(const struct rknpu_submit *args)
{
        unsigned long addr;

        if (unlikely(!args || !args->task_obj_addr))
                return NULL;

        addr = (unsigned long)args->task_obj_addr;
        if (unlikely(!rknpu_is_valid_kptr(addr)))
                return NULL;

        return (struct rknpu_gem_object *)(uintptr_t)addr;
}

static __always_inline int rknpu_get_task_number(const struct rknpu_job *job,
                                                 int core_index)
{
        if (unlikely(core_index != 0 || !job || !job->args)) {
                LOG_ERROR("invalid rknpu core index: %d\n", core_index);
                return 0;
        }

        return (int)job->args->task_number;
}

static void rknpu_job_release_task_obj(struct rknpu_job *job)
{
#ifdef CONFIG_ROCKCHIP_RKNPU_DRM_GEM
        if (unlikely(!job || !job->args))
                return;

        if (job->args->task_number > 0) {
                struct rknpu_gem_object *task_obj =
                        rknpu_job_get_task_obj(job->args);

                if (likely(task_obj)) {
                        rknpu_gem_object_put(&task_obj->base);
                        job->args->task_obj_addr = 0;
                }
        }
#endif
}

static void rknpu_job_free(struct rknpu_job *job)
{
        if (unlikely(!job))
                return;

        rknpu_job_release_task_obj(job);

        if (job->fence) {
                dma_fence_put(job->fence);
                job->fence = NULL;
        }

        if (job->args_owner)
                job->args = NULL;

        kfree(job);
}

static int rknpu_job_cleanup(struct rknpu_job *job)
{
        struct rknpu_device *rknpu_dev;

        if (unlikely(!job))
                return 0;

        rknpu_dev = job->rknpu_dev;
        rknpu_job_free(job);

        if (likely(rknpu_dev))
                rknpu_power_put_delay(rknpu_dev);

        return 0;
}

static void rknpu_job_cleanup_work(struct work_struct *work)
{
        struct rknpu_job *job =
                container_of(work, struct rknpu_job, cleanup_work);

        rknpu_job_cleanup(job);
}

static struct rknpu_job *rknpu_job_alloc(struct rknpu_device *rknpu_dev,
                                         struct rknpu_submit *args)
{
        struct rknpu_job *job = NULL;
        bool need_copy = !!(args->flags & RKNPU_JOB_NONBLOCK);
        size_t alloc_size = sizeof(*job);
#ifdef CONFIG_ROCKCHIP_RKNPU_DRM_GEM
        struct rknpu_gem_object *task_obj;
#endif

        if (unlikely(!rknpu_dev || !args))
                return NULL;

        if (need_copy &&
            unlikely(check_add_overflow(sizeof(*job), sizeof(*args),
                                        &alloc_size)))
                return NULL;

        job = kzalloc(alloc_size, GFP_KERNEL);
        if (unlikely(!job))
                return NULL;

        INIT_LIST_HEAD(&job->head[0]);
        job->timestamp = ktime_get();
        job->rknpu_dev = rknpu_dev;
        job->use_core_num = 1;
        atomic_set(&job->run_count, 1);
        atomic_set(&job->interrupt_count, 1);
        job->iommu_domain_id = args->iommu_domain_id;
        atomic_set(&job->submit_count[0], 0);

        if (!need_copy) {
                job->args = args;
                job->args_owner = false;
        } else {
                job->args = (struct rknpu_submit *)(job + 1);
                *job->args = *args;
                job->args_owner = true;
                INIT_WORK(&job->cleanup_work, rknpu_job_cleanup_work);
        }

#ifdef CONFIG_ROCKCHIP_RKNPU_DRM_GEM
        task_obj = rknpu_job_get_task_obj(job->args);
        if (likely(task_obj))
                rknpu_gem_object_get(&task_obj->base);
#endif

        return job;
}

static int rknpu_job_wait(struct rknpu_job *job)
{
        struct rknpu_device *rknpu_dev = job->rknpu_dev;
        struct rknpu_submit *args = job->args;
        struct rknpu_task *last_task = NULL;
        struct rknpu_subcore_data *subcore_data = &rknpu_dev->subcore_data;
        struct rknpu_job *entry, *q;
        void __iomem *rknpu_core_base = NULL;
        unsigned long flags;
        u64 timeout_us;
        int wait_count = 0;
        bool continue_wait = false;
        int ret = -EINVAL;

        if (unlikely(check_mul_overflow((u64)args->timeout, 1000ULL,
                                        &timeout_us)))
                timeout_us = U64_MAX;

        do {
                ret = wait_event_timeout(
                        subcore_data->job_done_wq,
                        (job->flags & RKNPU_JOB_DONE) ||
                                rknpu_dev->soft_reseting,
                        msecs_to_jiffies(args->timeout));

                if (++wait_count >= 3)
                        break;

                if (ret == 0) {
                        s64 elapse_time_us = 0;
                        s64 commit_delta_us = 0;

                        spin_lock_irqsave(&rknpu_dev->irq_lock, flags);
                        elapse_time_us = ktime_us_delta(ktime_get(),
                                                        job->hw_commit_time);
                        continue_wait =
                                (job->hw_commit_time == 0) ?
                                        true :
                                        (elapse_time_us > 0 &&
                                         (u64)elapse_time_us < timeout_us);
                        spin_unlock_irqrestore(&rknpu_dev->irq_lock, flags);

                        commit_delta_us = (job->hw_commit_time == 0) ?
                                                  0 :
                                                  elapse_time_us;

                        LOG_ERROR(
                                "job: %p, mask: %#x, job iommu domain id: %d, dev iommu domain id: %d, wait_count: %d, continue wait: %d, commit elapse time: %lldus, wait time: %lldus, timeout: %llu us\n",
                                job, args->core_mask, job->iommu_domain_id,
                                rknpu_dev->iommu_domain_id, wait_count,
                                continue_wait,
                                (long long)commit_delta_us,
                                (long long)ktime_us_delta(ktime_get(),
                                                           job->timestamp),
                                (unsigned long long)timeout_us);
                }
        } while (ret == 0 && continue_wait);

        last_task = job->last_task;
        if (unlikely(!last_task)) {
                spin_lock_irqsave(&rknpu_dev->irq_lock, flags);
                list_for_each_entry_safe(entry, q, &subcore_data->todo_list,
                                         head[0]) {
                        if (entry == job) {
                                list_del_init(&job->head[0]);
                                break;
                        }
                }
                spin_unlock_irqrestore(&rknpu_dev->irq_lock, flags);

                LOG_ERROR("job commit failed\n");
                return ret < 0 ? ret : -EINVAL;
        }

        last_task->int_status = job->int_status[0];

        if (unlikely(ret <= 0)) {
                args->task_counter = 0;
                rknpu_core_base = rknpu_dev->base;
                if (args->flags & RKNPU_JOB_PC) {
                        u32 task_status = REG_READ(
                                rknpu_dev->config->pc_task_status_offset);
                        args->task_counter =
                                (task_status &
                                 rknpu_dev->config->pc_task_number_mask);
                }

                LOG_ERROR(
                        "failed to wait job, task counter: %u, flags: %#x, ret = %d, elapsed time: %lldus\n",
                        args->task_counter, args->flags, ret,
                        (long long)ktime_us_delta(ktime_get(),
                                                   job->timestamp));

                return ret < 0 ? ret : -ETIMEDOUT;
        }

        if (unlikely(!(job->flags & RKNPU_JOB_DONE)))
                return -EINVAL;

        args->task_counter = args->task_number;
        args->hw_elapse_time = job->hw_elapse_time;

        return 0;
}

static int rknpu_job_subcore_commit_pc(struct rknpu_job *job, int core_index)
{
        struct rknpu_device *rknpu_dev = job->rknpu_dev;
        struct rknpu_submit *args = job->args;
        struct rknpu_gem_object *task_obj = rknpu_job_get_task_obj(args);
        struct rknpu_task *task_base = NULL;
        struct rknpu_task *first_task = NULL;
        struct rknpu_task *last_task = NULL;
        void __iomem *rknpu_core_base = rknpu_dev->base;
        int submit_index = atomic_read(&job->submit_count[core_index]);
        u64 max_submit_number = rknpu_dev->config->max_submit_number;
        u32 pc_data_amount_scale = rknpu_dev->config->pc_data_amount_scale;
        u32 pc_task_number_bits = rknpu_dev->config->pc_task_number_bits;
        u32 task_pp_en = (args->flags & RKNPU_JOB_PINGPONG) ? 1U : 0U;
        u64 offset_items = 0;
        u64 rem_items = 0;
        u64 batch_items = 0;
        u64 task_start_u64 = 0;
        int task_start = 0;
        int task_end = 0;
        int task_number = 0;
        size_t max_tasks = 0;
        size_t task_bytes = 0;
        size_t task_offset_bytes = 0;
        u64 raw_dma_addr = 0;
        u64 req_cmd_addr = 0;
        u64 stream_off = 0;
        u32 base_dma_addr = 0;
        u32 regcmd_dma_addr = 0;
        u32 eff_int_mask = 0;

        if (unlikely(!task_obj || max_submit_number == 0 ||
                     pc_data_amount_scale == 0)) {
                job->ret = -EINVAL;
                return -EINVAL;
        }

        task_base = task_obj->kv_addr;
        if (unlikely(!task_base)) {
                LOG_ERROR("task_obj kv_addr is NULL!\n");
                job->ret = -EINVAL;
                return -EINVAL;
        }

        max_tasks = task_obj->size / sizeof(struct rknpu_task);
        if (unlikely(max_tasks == 0 || args->task_start < 0 ||
                     args->task_number <= 0 ||
                     (size_t)args->task_start >= max_tasks ||
                     (size_t)args->task_number >
                             max_tasks - (size_t)args->task_start)) {
                LOG_ERROR("invalid task range: start=%d num=%u max=%zu\n",
                          args->task_start, args->task_number, max_tasks);
                job->ret = -EINVAL;
                return -EINVAL;
        }

        if (task_obj->dma_addr)
                raw_dma_addr = (u64)task_obj->dma_addr;
        else if (task_obj->iova_start)
                raw_dma_addr = (u64)task_obj->iova_start;
        else
                raw_dma_addr = (u64)args->task_base_addr;

        if (unlikely(!raw_dma_addr || raw_dma_addr > U32_MAX)) {
                LOG_ERROR("DMA/IOVA address out of 32-bit IOMMU range: %#llx\n",
                          (unsigned long long)raw_dma_addr);
                job->ret = -EINVAL;
                return -EINVAL;
        }

        base_dma_addr = (u32)raw_dma_addr;
        args->task_base_addr = base_dma_addr;

        if (unlikely(submit_index < 0 ||
                     check_mul_overflow((u64)submit_index, max_submit_number,
                                        &offset_items) ||
                     check_add_overflow((u64)args->task_start, offset_items,
                                        &task_start_u64) ||
                     task_start_u64 > (u64)INT_MAX ||
                     (size_t)task_start_u64 >= max_tasks)) {
                job->ret = -EINVAL;
                return -EINVAL;
        }

        task_start = (int)task_start_u64;
        if ((u64)args->task_number <= offset_items) {
                job->ret = -EINVAL;
                return -EINVAL;
        }

        rem_items = (u64)args->task_number - offset_items;
        batch_items = min(rem_items, max_submit_number);
        task_number = (int)batch_items;
        task_end = task_start + task_number - 1;

        first_task = &task_base[task_start];
        last_task = &task_base[task_end];

        if (unlikely(check_mul_overflow((size_t)task_start,
                                        sizeof(struct rknpu_task),
                                        &task_offset_bytes) ||
                     check_mul_overflow((size_t)task_number,
                                        sizeof(struct rknpu_task),
                                        &task_bytes) ||
                     task_offset_bytes > (size_t)(U32_MAX - base_dma_addr))) {
                job->ret = -EOVERFLOW;
                return -EOVERFLOW;
        }

        req_cmd_addr = first_task->regcmd_addr;
        stream_off = (u64)task_offset_bytes;
        if (req_cmd_addr >= raw_dma_addr &&
            req_cmd_addr < raw_dma_addr + (u64)task_obj->size) {
                stream_off = req_cmd_addr - raw_dma_addr;
                if (unlikely(stream_off > (u64)(U32_MAX - base_dma_addr))) {
                        job->ret = -EOVERFLOW;
                        return -EOVERFLOW;
                }
                regcmd_dma_addr = base_dma_addr + (u32)stream_off;
        } else if (req_cmd_addr != 0) {
                if (unlikely(req_cmd_addr > U32_MAX)) {
                        job->ret = -EOVERFLOW;
                        return -EOVERFLOW;
                }
                regcmd_dma_addr = (u32)req_cmd_addr;
        } else {
                if (unlikely(stream_off > (u64)(U32_MAX - base_dma_addr))) {
                        job->ret = -EOVERFLOW;
                        return -EOVERFLOW;
                }
                regcmd_dma_addr = base_dma_addr + (u32)stream_off;
        }

        if (task_obj->flags & RKNPU_MEM_CACHEABLE) {
                dma_sync_single_range_for_device(rknpu_dev->dev,
                                                 base_dma_addr,
                                                 task_offset_bytes,
                                                 task_bytes, DMA_TO_DEVICE);
        }

        eff_int_mask = (last_task->int_mask ? last_task->int_mask : 0x1U) |
                       RKNPU_RAW_IRQ_BIT16;

        REG_WRITE(regcmd_dma_addr, RKNPU_OFFSET_PC_DATA_ADDR);

        REG_WRITE((((u32)first_task->regcfg_amount +
                    RKNPU_PC_DATA_EXTRA_AMOUNT + pc_data_amount_scale - 1U) /
                   pc_data_amount_scale) -
                          1U,
                  RKNPU_OFFSET_PC_DATA_AMOUNT);

        REG_WRITE(eff_int_mask, RKNPU_OFFSET_INT_MASK);
        REG_WRITE(RKNPU_INT_CLEAR | eff_int_mask, RKNPU_OFFSET_INT_CLEAR);
        REG_WRITE(((0x6U | task_pp_en) << pc_task_number_bits) |
                          (u32)task_number,
                  RKNPU_OFFSET_PC_TASK_CONTROL);
        REG_WRITE(args->task_base_addr, RKNPU_OFFSET_PC_DMA_BASE_ADDR);

        job->first_task = first_task;
        job->last_task = last_task;
        job->int_mask[core_index] = eff_int_mask;

        {
                u64 first_word = 0;

                if (stream_off + sizeof(u64) <= (u64)task_obj->size)
                        memcpy(&first_word, (u8 *)task_base + stream_off,
                               sizeof(first_word));

                LOG_ERROR(
                        "pre PC_OP_EN: off=%#llx cmd_addr=%#x word0=%#llx status_raw=%#x raw_irq=%#x\n",
                        (unsigned long long)stream_off,
                        regcmd_dma_addr,
                        (unsigned long long)first_word,
                        REG_READ(rknpu_dev->config->pc_task_status_offset),
                        REG_READ(RKNPU_OFFSET_INT_RAW_STATUS));
                LOG_ERROR(
                        "cfg regs: pc_data_addr=%#x dma_base=%#x int_mask=%#x int_clear=%#x\n",
                        REG_READ(RKNPU_OFFSET_PC_DATA_ADDR),
                        REG_READ(RKNPU_OFFSET_PC_DMA_BASE_ADDR),
                        REG_READ(RKNPU_OFFSET_INT_MASK),
                        REG_READ(RKNPU_OFFSET_INT_CLEAR));
                LOG_ERROR(
                        "hw info: ver=%#x ver_num=%#x power_ref=%d\n",
                        REG_READ(RKNPU_OFFSET_VERSION),
                        REG_READ(RKNPU_OFFSET_VERSION_NUM) & 0xffffU,
                        atomic_read(&rknpu_dev->power_refcount));
        }

        REG_WRITE(0x1, RKNPU_OFFSET_PC_OP_EN);
        REG_WRITE(0x0, RKNPU_OFFSET_PC_OP_EN);

        LOG_ERROR(
                "post PC_OP_EN: status_raw=%#x raw_irq=%#x irq_stat=%#x pc_op=%#x amount=%#x ctrl=%#x\n",
                REG_READ(rknpu_dev->config->pc_task_status_offset),
                REG_READ(RKNPU_OFFSET_INT_RAW_STATUS),
                REG_READ(RKNPU_OFFSET_INT_STATUS),
                REG_READ(RKNPU_OFFSET_PC_OP_EN),
                REG_READ(RKNPU_OFFSET_PC_DATA_AMOUNT),
                REG_READ(RKNPU_OFFSET_PC_TASK_CONTROL));

        return 0;
}

static __always_inline int rknpu_job_subcore_commit(struct rknpu_job *job,
                                                    int core_index)
{
        struct rknpu_submit *args = job->args;

        if (unlikely(!(args->flags & RKNPU_JOB_PC))) {
                job->ret = -EINVAL;
                return -EINVAL;
        }

        return rknpu_job_subcore_commit_pc(job, core_index);
}

static void rknpu_job_commit(struct rknpu_job *job)
{
        if (likely(job->args->core_mask == RKNPU_CORE0_MASK)) {
                rknpu_job_subcore_commit(job, 0);
        } else {
                LOG_ERROR("Unsupported core mask for RK356x: %#x\n",
                          job->args->core_mask);
                job->ret = -EINVAL;
        }
}

static void rknpu_job_next(struct rknpu_device *rknpu_dev, int core_index)
{
        struct rknpu_job *job = NULL;
        struct rknpu_subcore_data *subcore_data = &rknpu_dev->subcore_data;
        unsigned long flags;

        if (unlikely(rknpu_dev->soft_reseting || core_index != 0))
                return;

        spin_lock_irqsave(&rknpu_dev->irq_lock, flags);

        if (subcore_data->job || list_empty(&subcore_data->todo_list)) {
                spin_unlock_irqrestore(&rknpu_dev->irq_lock, flags);
                return;
        }

        job = list_first_entry(&subcore_data->todo_list, struct rknpu_job,
                               head[0]);

        list_del_init(&job->head[0]);
        subcore_data->job = job;
        job->hw_commit_time = ktime_get();
        job->hw_recoder_time = job->hw_commit_time;
        spin_unlock_irqrestore(&rknpu_dev->irq_lock, flags);

        if (atomic_dec_and_test(&job->run_count))
                rknpu_job_commit(job);
}

static void rknpu_job_done(struct rknpu_job *job, int ret, int core_index)
{
        struct rknpu_device *rknpu_dev = job->rknpu_dev;
        struct rknpu_subcore_data *subcore_data = &rknpu_dev->subcore_data;
        void __iomem *rknpu_core_base = rknpu_dev->base;
        ktime_t now;
        unsigned long flags;
        u64 max_submit_number = rknpu_dev->config->max_submit_number;
        u64 total_batches;

        total_batches = ((u64)rknpu_get_task_number(job, core_index) +
                         max_submit_number - 1ULL) /
                        max_submit_number;

        if ((u64)atomic_inc_return(&job->submit_count[core_index]) <
            total_batches) {
                rknpu_job_subcore_commit(job, core_index);
                return;
        }

        REG_WRITE(0x0, RKNPU_OFFSET_PC_OP_EN);

        spin_lock_irqsave(&rknpu_dev->irq_lock, flags);
        if (subcore_data->job == job)
                subcore_data->job = NULL;
        subcore_data->task_num -= rknpu_get_task_number(job, core_index);
        now = ktime_get();
        job->hw_elapse_time = ktime_sub(now, job->hw_commit_time);
        subcore_data->timer.busy_time += ktime_sub(now, job->hw_recoder_time);
        spin_unlock_irqrestore(&rknpu_dev->irq_lock, flags);

        if (atomic_dec_and_test(&job->interrupt_count)) {
                rknpu_iommu_domain_put(rknpu_dev);

                job->flags |= RKNPU_JOB_DONE;
                job->ret = ret;

                if (job->fence)
                        dma_fence_signal(job->fence);

                if ((job->flags & RKNPU_JOB_ASYNC) && job->args_owner)
                        schedule_work(&job->cleanup_work);

                wake_up(&subcore_data->job_done_wq);
        }

        rknpu_job_next(rknpu_dev, core_index);
}

static int rknpu_job_schedule(struct rknpu_job *job)
{
        struct rknpu_device *rknpu_dev = job->rknpu_dev;
        struct rknpu_subcore_data *subcore_data = &rknpu_dev->subcore_data;
        unsigned long flags;
        int ret;

        ret = rknpu_power_get(rknpu_dev);
        if (unlikely(ret)) {
                job->ret = ret;
                return ret;
        }

        if (job->args->core_mask == RKNPU_CORE_AUTO_MASK) {
                job->args->core_mask = RKNPU_CORE0_MASK;
                job->use_core_num = 1;
                atomic_set(&job->run_count, 1);
                atomic_set(&job->interrupt_count, 1);
        }

        ret = rknpu_iommu_domain_get_and_switch(rknpu_dev,
                                                job->iommu_domain_id);
        if (unlikely(ret)) {
                rknpu_power_put_delay(rknpu_dev);
                job->ret = ret;
                return ret;
        }

        spin_lock_irqsave(&rknpu_dev->irq_lock, flags);
        list_add_tail(&job->head[0], &subcore_data->todo_list);
        subcore_data->task_num += rknpu_get_task_number(job, 0);
        spin_unlock_irqrestore(&rknpu_dev->irq_lock, flags);

        job->ret = 0;
        rknpu_job_next(rknpu_dev, 0);
        return 0;
}

static void rknpu_job_abort(struct rknpu_job *job)
{
        struct rknpu_device *rknpu_dev = job->rknpu_dev;
        struct rknpu_subcore_data *subcore_data = &rknpu_dev->subcore_data;
        void __iomem *rknpu_core_base = rknpu_dev->base;
        unsigned long flags;

        msleep(100);

        spin_lock_irqsave(&rknpu_dev->irq_lock, flags);
        if (job == subcore_data->job && !job->irq_entry[0]) {
                subcore_data->job = NULL;
                subcore_data->task_num -= rknpu_get_task_number(job, 0);
        }
        spin_unlock_irqrestore(&rknpu_dev->irq_lock, flags);

        REG_WRITE(0x0, RKNPU_OFFSET_PC_OP_EN);

        if (job->ret == -ETIMEDOUT) {
                LOG_ERROR(
                        "pc regs: addr=%#x amount=%#x ctrl=%#x status_raw=%#x\n",
                        REG_READ(RKNPU_OFFSET_PC_DATA_ADDR),
                        REG_READ(RKNPU_OFFSET_PC_DATA_AMOUNT),
                        REG_READ(RKNPU_OFFSET_PC_TASK_CONTROL),
                        REG_READ(rknpu_dev->config->pc_task_status_offset));
                LOG_ERROR(
                        "abort extra regs: dma_base=%#x int_mask=%#x int_clear=%#x\n",
                        REG_READ(RKNPU_OFFSET_PC_DMA_BASE_ADDR),
                        REG_READ(RKNPU_OFFSET_INT_MASK),
                        REG_READ(RKNPU_OFFSET_INT_CLEAR));
                LOG_ERROR(
                        "abort hw info: ver=%#x ver_num=%#x power_ref=%d\n",
                        REG_READ(RKNPU_OFFSET_VERSION),
                        REG_READ(RKNPU_OFFSET_VERSION_NUM) & 0xffffU,
                        atomic_read(&rknpu_dev->power_refcount));
                LOG_ERROR(
                        "job timeout, flags: %#x, core 0 irq status: %#x, raw status: %#x, require mask: %#x, task counter: %#x, elapsed time: %lldus\n",
                        job->flags, REG_READ(RKNPU_OFFSET_INT_STATUS),
                        REG_READ(RKNPU_OFFSET_INT_RAW_STATUS),
                        job->int_mask[0],
                        (REG_READ(rknpu_dev->config->pc_task_status_offset) &
                         rknpu_dev->config->pc_task_number_mask),
                        (long long)ktime_us_delta(ktime_get(),
                                                   job->timestamp));
                rknpu_soft_reset(rknpu_dev);
        } else {
                LOG_ERROR(
                        "job abort, flags: %#x, ret: %d, elapsed time: %lldus\n",
                        job->flags, job->ret,
                        (long long)ktime_us_delta(ktime_get(),
                                                   job->timestamp));
        }

        rknpu_job_cleanup(job);
}

static __always_inline u32 rknpu_fuzz_status(u32 status)
{
        u32 fuzz_status = 0;

        if (status & 0x3U)
                fuzz_status |= 0x3U;
        if (status & 0xcU)
                fuzz_status |= 0xcU;
        if (status & 0x30U)
                fuzz_status |= 0x30U;
        if (status & 0xc0U)
                fuzz_status |= 0xc0U;
        if (status & 0x300U)
                fuzz_status |= 0x300U;
        if (status & 0xc00U)
                fuzz_status |= 0xc00U;
        if (status & RKNPU_RAW_IRQ_BIT16)
                fuzz_status |= RKNPU_RAW_IRQ_BIT16;

        return fuzz_status;
}

static irqreturn_t rknpu_irq_handler(int irq, void *data, int core_index)
{
        struct rknpu_device *rknpu_dev = data;
        void __iomem *rknpu_core_base;
        struct rknpu_subcore_data *subcore_data;
        struct rknpu_job *job = NULL;
        u32 status = 0;
        u32 raw_status = 0;
        u32 eff_status = 0;
        u32 cur_mask = 0;
        u32 task_cnt = 0;
        unsigned long flags;
        bool already_done = false;

        if (unlikely(!rknpu_dev || core_index != 0))
                return IRQ_NONE;

        rknpu_core_base = rknpu_dev->base;
        subcore_data = &rknpu_dev->subcore_data;

        status = REG_READ(RKNPU_OFFSET_INT_STATUS);
        raw_status = REG_READ(RKNPU_OFFSET_INT_RAW_STATUS);
        eff_status = status ? status : raw_status;
        task_cnt = REG_READ(rknpu_dev->config->pc_task_status_offset) &
                   rknpu_dev->config->pc_task_number_mask;

        REG_WRITE(RKNPU_INT_CLEAR | eff_status, RKNPU_OFFSET_INT_CLEAR);

        /* Break level-triggered storm if bit 16 cannot be cleared */
        if (REG_READ(RKNPU_OFFSET_INT_RAW_STATUS) & RKNPU_RAW_IRQ_BIT16) {
                cur_mask = REG_READ(RKNPU_OFFSET_INT_MASK);
                REG_WRITE(cur_mask & ~RKNPU_RAW_IRQ_BIT16,
                          RKNPU_OFFSET_INT_MASK);
        }

        spin_lock_irqsave(&rknpu_dev->irq_lock, flags);
        job = subcore_data->job;
        if (!job) {
                spin_unlock_irqrestore(&rknpu_dev->irq_lock, flags);
                rknpu_job_next(rknpu_dev, core_index);
                return IRQ_HANDLED;
        }
        already_done = !!(job->flags & RKNPU_JOB_DONE);
        job->irq_entry[core_index] = true;
        spin_unlock_irqrestore(&rknpu_dev->irq_lock, flags);

        if (already_done)
                return IRQ_HANDLED;

        job->int_status[core_index] = eff_status;

        if (eff_status & RKNPU_RAW_IRQ_BIT16) {
                rknpu_job_done(job, 0, core_index);
                return IRQ_HANDLED;
        }

        if (unlikely(rknpu_fuzz_status(eff_status) !=
                     rknpu_fuzz_status(job->int_mask[core_index]))) {
                LOG_ERROR(
                        "invalid irq status: %#x, raw status: %#x, require mask: %#x, task counter: %#x\n",
                        status, raw_status, job->int_mask[core_index],
                        task_cnt);
                return IRQ_HANDLED;
        }

        rknpu_job_done(job, 0, core_index);
        return IRQ_HANDLED;
}

irqreturn_t rknpu_core0_irq_handler(int irq, void *data)
{
        return rknpu_irq_handler(irq, data, 0);
}

irqreturn_t rknpu_platform_irq_handler(int irq, void *dev_id)
{
        return rknpu_irq_handler(irq, dev_id, 0);
}

static void rknpu_job_timeout_clean(struct rknpu_device *rknpu_dev,
                                    int core_mask)
{
        struct rknpu_job *job = NULL;
        unsigned long flags;
        struct rknpu_subcore_data *subcore_data = &rknpu_dev->subcore_data;

        if (!(core_mask & RKNPU_CORE0_MASK))
                return;

        spin_lock_irqsave(&rknpu_dev->irq_lock, flags);
        job = subcore_data->job;
        if (job && job->args_owner &&
            (s64)ktime_us_delta(ktime_get(), job->timestamp) >=
                    (s64)job->args->timeout * 1000LL) {
                if (subcore_data->job == job)
                        subcore_data->job = NULL;
                if (!list_empty(&job->head[0]))
                        list_del_init(&job->head[0]);
                spin_unlock_irqrestore(&rknpu_dev->irq_lock, flags);

                rknpu_soft_reset(rknpu_dev);
                schedule_work(&job->cleanup_work);
                return;
        }
        spin_unlock_irqrestore(&rknpu_dev->irq_lock, flags);

        do {
                spin_lock_irqsave(&rknpu_dev->irq_lock, flags);
                if (!list_empty(&subcore_data->todo_list)) {
                        job = list_first_entry(&subcore_data->todo_list,
                                               struct rknpu_job, head[0]);
                        list_del_init(&job->head[0]);
                } else {
                        job = NULL;
                }
                spin_unlock_irqrestore(&rknpu_dev->irq_lock, flags);

                if (job && job->args_owner)
                        schedule_work(&job->cleanup_work);
        } while (job);
}

static int rknpu_submit(struct rknpu_device *rknpu_dev,
                        struct rknpu_submit *args)
{
        struct rknpu_job *job = NULL;
        int ret = -EINVAL;

        if (unlikely(!rknpu_dev || !args || !rknpu_dev->config))
                return -EINVAL;

        if (unlikely(args->task_number == 0)) {
                LOG_ERROR("invalid rknpu task number: %u\n", args->task_number);
                return -EINVAL;
        }

        if (unlikely(args->core_mask > rknpu_dev->config->core_mask)) {
                LOG_ERROR("invalid rknpu core mask: %#x\n", args->core_mask);
                return -EINVAL;
        }

        job = rknpu_job_alloc(rknpu_dev, args);
        if (unlikely(!job)) {
                LOG_ERROR("failed to allocate rknpu job!\n");
                return -ENOMEM;
        }

        if (args->flags & RKNPU_JOB_FENCE_IN) {
#ifdef CONFIG_ROCKCHIP_RKNPU_FENCE
                struct dma_fence *in_fence;

                in_fence = sync_file_get_fence(args->fence_fd);
                if (unlikely(!in_fence)) {
                        LOG_ERROR("invalid fence in fd, fd: %d\n",
                                  args->fence_fd);
                        rknpu_job_free(job);
                        return -EINVAL;
                }
                args->fence_fd = -1;

                ret = 0;
                if (!dma_fence_match_context(in_fence,
                                             rknpu_dev->fence_ctx->context))
                        ret = dma_fence_wait_timeout(
                                in_fence, true,
                                msecs_to_jiffies(args->timeout));
                dma_fence_put(in_fence);
                if (unlikely(ret < 0)) {
                        if (ret != -ERESTARTSYS)
                                LOG_ERROR("Error (%d) waiting for fence!\n",
                                          ret);
                        rknpu_job_free(job);
                        return ret;
                }
                ret = 0;
#else
                LOG_ERROR(
                        "failed to use rknpu fence, please enable rknpu fence config!\n");
                rknpu_job_free(job);
                return -EINVAL;
#endif
        }

        if (args->flags & RKNPU_JOB_FENCE_OUT) {
#ifdef CONFIG_ROCKCHIP_RKNPU_FENCE
                ret = rknpu_fence_alloc(job);
                if (unlikely(ret)) {
                        rknpu_job_free(job);
                        return ret;
                }
                args->fence_fd = rknpu_fence_get_fd(job);
                if (unlikely(args->fence_fd < 0)) {
                        ret = args->fence_fd;
                        rknpu_job_free(job);
                        return ret;
                }
                job->args->fence_fd = args->fence_fd;
#else
                LOG_ERROR(
                        "failed to use rknpu fence, please enable rknpu fence config!\n");
                rknpu_job_free(job);
                return -EINVAL;
#endif
        }

        if (args->flags & RKNPU_JOB_NONBLOCK) {
                job->flags |= RKNPU_JOB_ASYNC;
                rknpu_job_timeout_clean(rknpu_dev, job->args->core_mask);
                ret = rknpu_job_schedule(job);
                if (unlikely(ret)) {
                        rknpu_job_free(job);
                        return ret;
                }
                return 0;
        }

        ret = rknpu_job_schedule(job);
        if (unlikely(ret)) {
                rknpu_job_free(job);
                return ret;
        }

        if (args->flags & RKNPU_JOB_PC)
                job->ret = rknpu_job_wait(job);

        args->task_counter = job->args->task_counter;
        ret = job->ret;
        if (!ret)
                rknpu_job_cleanup(job);
        else
                rknpu_job_abort(job);

        return ret;
}

#ifdef CONFIG_ROCKCHIP_RKNPU_DRM_GEM
int rknpu_submit_ioctl(struct drm_device *dev, void *data,
                       struct drm_file *file_priv)
{
        struct rknpu_device *rknpu_dev;

        if (unlikely(!dev || !data))
                return -EINVAL;

        rknpu_dev = dev_get_drvdata(dev->dev);
        return rknpu_submit(rknpu_dev, (struct rknpu_submit *)data);
}
#endif

int rknpu_get_hw_version(struct rknpu_device *rknpu_dev, u32 *version)
{
        void __iomem *rknpu_core_base;

        if (unlikely(!rknpu_dev || !version))
                return -EINVAL;

        rknpu_core_base = rknpu_dev->base;
        *version = REG_READ(RKNPU_OFFSET_VERSION) +
                   (REG_READ(RKNPU_OFFSET_VERSION_NUM) & 0xffffU);

        return 0;
}

int rknpu_get_bw_priority(struct rknpu_device *rknpu_dev, u32 *priority,
                          u32 *expect, u32 *tw)
{
        void __iomem *base;

        if (unlikely(!rknpu_dev || !rknpu_dev->bw_priority_base))
                return -EINVAL;

        base = rknpu_dev->bw_priority_base;
        spin_lock(&rknpu_dev->lock);

        if (priority)
                *priority = _REG_READ(base, 0x0);
        if (expect)
                *expect = _REG_READ(base, 0x8);
        if (tw)
                *tw = _REG_READ(base, 0xc);

        spin_unlock(&rknpu_dev->lock);

        return 0;
}

int rknpu_set_bw_priority(struct rknpu_device *rknpu_dev, u32 priority,
                          u32 expect, u32 tw)
{
        void __iomem *base;

        if (unlikely(!rknpu_dev || !rknpu_dev->bw_priority_base))
                return -EINVAL;

        base = rknpu_dev->bw_priority_base;
        spin_lock(&rknpu_dev->lock);

        if (priority != 0)
                _REG_WRITE(base, priority, 0x0);
        if (expect != 0)
                _REG_WRITE(base, expect, 0x8);
        if (tw != 0)
                _REG_WRITE(base, tw, 0xc);

        spin_unlock(&rknpu_dev->lock);

        return 0;
}

int rknpu_clear_rw_amount(struct rknpu_device *rknpu_dev)
{
        void __iomem *rknpu_core_base;
        const struct rknpu_config *config;
        unsigned long flags;

        if (unlikely(!rknpu_dev || !rknpu_dev->config))
                return -EINVAL;

        rknpu_core_base = rknpu_dev->base;
        config = rknpu_dev->config;

        if (!config->amount_top) {
                LOG_WARN("Clear rw_amount is not supported on this device!\n");
                return 0;
        }

        if (config->pc_dma_ctrl) {
                u32 pc_data_addr;

                spin_lock_irqsave(&rknpu_dev->irq_lock, flags);
                pc_data_addr = REG_READ(RKNPU_OFFSET_PC_DATA_ADDR);

                REG_WRITE(0x1, RKNPU_OFFSET_PC_DATA_ADDR);
                REG_WRITE(0x80000101U, config->amount_top->offset_clr_all);
                REG_WRITE(0x00000101U, config->amount_top->offset_clr_all);
                if (config->amount_core) {
                        REG_WRITE(0x80000101U,
                                  config->amount_core->offset_clr_all);
                        REG_WRITE(0x00000101U,
                                  config->amount_core->offset_clr_all);
                }
                REG_WRITE(pc_data_addr, RKNPU_OFFSET_PC_DATA_ADDR);
                spin_unlock_irqrestore(&rknpu_dev->irq_lock, flags);
        } else {
                spin_lock(&rknpu_dev->lock);
                REG_WRITE(0x80000101U, config->amount_top->offset_clr_all);
                REG_WRITE(0x00000101U, config->amount_top->offset_clr_all);
                if (config->amount_core) {
                        REG_WRITE(0x80000101U,
                                  config->amount_core->offset_clr_all);
                        REG_WRITE(0x00000101U,
                                  config->amount_core->offset_clr_all);
                }
                spin_unlock(&rknpu_dev->lock);
        }

        return 0;
}

int rknpu_get_rw_amount(struct rknpu_device *rknpu_dev, u32 *dt_wr, u32 *dt_rd,
                        u32 *wd_rd)
{
        void __iomem *rknpu_core_base;
        const struct rknpu_config *config;
        u32 amount_scale;

        if (unlikely(!rknpu_dev || !rknpu_dev->config))
                return -EINVAL;

        rknpu_core_base = rknpu_dev->base;
        config = rknpu_dev->config;

        if (!config->amount_top) {
                LOG_WARN("Get rw_amount is not supported on this device!\n");
                return 0;
        }

        amount_scale = (u32)max(1, config->pc_data_amount_scale);
        spin_lock(&rknpu_dev->lock);

        if (dt_wr) {
                *dt_wr = REG_READ(config->amount_top->offset_dt_wr) *
                         amount_scale;
                if (config->amount_core)
                        *dt_wr += REG_READ(config->amount_core->offset_dt_wr) *
                                  amount_scale;
        }

        if (dt_rd) {
                *dt_rd = REG_READ(config->amount_top->offset_dt_rd) *
                         amount_scale;
                if (config->amount_core)
                        *dt_rd += REG_READ(config->amount_core->offset_dt_rd) *
                                  amount_scale;
        }

        if (wd_rd) {
                *wd_rd = REG_READ(config->amount_top->offset_wt_rd) *
                         amount_scale;
                if (config->amount_core)
                        *wd_rd += REG_READ(config->amount_core->offset_wt_rd) *
                                  amount_scale;
        }

        spin_unlock(&rknpu_dev->lock);

        return 0;
}

int rknpu_get_total_rw_amount(struct rknpu_device *rknpu_dev, u32 *amount)
{
        const struct rknpu_config *config;
        u32 dt_wr = 0, dt_rd = 0, wd_rd = 0;
        int ret;

        if (unlikely(!rknpu_dev || !rknpu_dev->config))
                return -EINVAL;

        config = rknpu_dev->config;
        if (!config->amount_top) {
                LOG_WARN(
                        "Get total_rw_amount is not supported on this device!\n");
                return 0;
        }

        ret = rknpu_get_rw_amount(rknpu_dev, &dt_wr, &dt_rd, &wd_rd);
        if (unlikely(ret))
                return ret;

        if (amount)
                *amount = dt_wr + dt_rd + wd_rd;

        return 0;
}
