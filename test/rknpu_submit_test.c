#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "rknpu_ioctl.h"

#ifndef RKNPU_OFFSET_INT_MASK
#define RKNPU_OFFSET_INT_MASK       0x20U
#endif
#ifndef RKNPU_OFFSET_INT_CLEAR
#define RKNPU_OFFSET_INT_CLEAR      0x24U
#endif
#ifndef RKNPU_OFFSET_INT_STATUS
#define RKNPU_OFFSET_INT_STATUS     0x28U
#endif
#ifndef RKNPU_OFFSET_INT_RAW_STATUS
#define RKNPU_OFFSET_INT_RAW_STATUS 0x2cU
#endif

static int alloc_gem_bo(int fd, uint64_t size, uint32_t flags,
                        struct rknpu_mem_create *out_bo, void **out_ptr)
{
        struct rknpu_mem_map map_req;
        void *ptr;
        int ret;

        memset(out_bo, 0, sizeof(*out_bo));
        out_bo->size = size;
        out_bo->flags = flags;

        ret = ioctl(fd, DRM_IOCTL_RKNPU_MEM_CREATE, out_bo);
        if (ret < 0) {
                fprintf(stderr, "MEM_CREATE failed: %s (errno=%d)\n", strerror(errno), errno);
                return -1;
        }

        memset(&map_req, 0, sizeof(map_req));
        map_req.handle = out_bo->handle;

        ret = ioctl(fd, DRM_IOCTL_RKNPU_MEM_MAP, &map_req);
        if (ret < 0) {
                fprintf(stderr, "MEM_MAP failed: %s (errno=%d)\n", strerror(errno), errno);
                return -1;
        }

        ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED,
                   fd, (off_t)map_req.offset);
        if (ptr == MAP_FAILED) {
                fprintf(stderr, "mmap failed: %s (errno=%d)\n", strerror(errno), errno);
                return -1;
        }

        *out_ptr = ptr;
        return 0;
}

static void free_gem_bo(int fd, uint64_t size, struct rknpu_mem_create *bo, void *ptr)
{
        struct rknpu_mem_destroy destroy;

        if (ptr && ptr != MAP_FAILED)
                munmap(ptr, size);

        memset(&destroy, 0, sizeof(destroy));
        destroy.handle = bo->handle;
        destroy.obj_addr = bo->obj_addr;
        ioctl(fd, DRM_IOCTL_RKNPU_MEM_DESTROY, &destroy);
}

static uint64_t make_word(int mode, uint64_t reg, uint64_t val, size_t idx)
{
        switch (mode) {
        case 2: /* (reg << 32) | val */
                return (reg << 32) | (val & 0xffffffffULL);
        case 3: /* patron variado por indice */
                return ((val + idx) << 32) | (reg & 0xffffffffULL);
        case 4: /* marcador no nulo en palabras extra */
                return (idx == 0) ? ((val << 32) | (reg & 0xffffffffULL))
                                  : 0xdeadbeef00000001ULL;
        case 1:
        default: /* (val << 32) | reg */
                return (val << 32) | (reg & 0xffffffffULL);
        }
}

static int run_once(int fd, int mode, uint64_t reg, uint64_t val,
                    uint32_t regcfg_amount, uint32_t regcfg_offset,
                    uint32_t timeout_ms, int use_cmd_bo, int iter)
{
        const uint64_t task_sz = 4096;
        const uint64_t cmd_sz = 4096;
        struct rknpu_mem_create task_bo, cmd_bo;
        struct rknpu_task *tasks = NULL;
        uint64_t *cmd_words = NULL;
        uint64_t *stream_ptr = NULL;
        struct rknpu_submit submit;
        struct rknpu_action act;
        size_t total_words = (size_t)regcfg_amount + RKNPU_PC_DATA_EXTRA_AMOUNT;
        size_t stream_offset = sizeof(struct rknpu_task);
        uint64_t cmd_dma;
        int ret;

        if (stream_offset + (total_words * sizeof(uint64_t)) > task_sz ||
            total_words * sizeof(uint64_t) > cmd_sz) {
                fprintf(stderr, "Buffer size exceeded for regcfg_amount=%u\n", regcfg_amount);
                return -1;
        }

        if (alloc_gem_bo(fd, task_sz,
                         RKNPU_MEM_IOMMU | RKNPU_MEM_ZEROING | RKNPU_MEM_KERNEL_MAPPING,
                         &task_bo, (void **)&tasks) < 0)
                return -1;

        if (alloc_gem_bo(fd, cmd_sz,
                         RKNPU_MEM_IOMMU | RKNPU_MEM_ZEROING | RKNPU_MEM_KERNEL_MAPPING,
                         &cmd_bo, (void **)&cmd_words) < 0) {
                free_gem_bo(fd, task_sz, &task_bo, tasks);
                return -1;
        }

        if (use_cmd_bo) {
                stream_ptr = cmd_words;
                cmd_dma = cmd_bo.dma_addr;
        } else {
                stream_ptr = (uint64_t *)((uint8_t *)tasks + stream_offset);
                cmd_dma = task_bo.dma_addr + stream_offset;
        }

        for (size_t i = 0; i < total_words; i++) {
                uint64_t w = make_word(mode, reg, val, i);
                stream_ptr[i] = w;
                cmd_words[i] = w;
        }

        tasks[0].flags = RKNPU_JOB_PC | RKNPU_JOB_BLOCK;
        tasks[0].enable_mask = 0x1U;
        tasks[0].int_mask = 0x1U;
        tasks[0].int_clear = 0x1U;
        tasks[0].regcfg_amount = regcfg_amount;
        tasks[0].regcfg_offset = regcfg_offset;
        tasks[0].regcmd_addr = cmd_dma;

        memset(&submit, 0, sizeof(submit));
        submit.flags = RKNPU_JOB_PC | RKNPU_JOB_BLOCK;
        submit.timeout = timeout_ms;
        submit.task_start = 0;
        submit.task_number = 1;
        submit.task_obj_addr = task_bo.obj_addr;
        submit.iommu_domain_id = 0;
        submit.task_base_addr = task_bo.dma_addr;
        submit.core_mask = 1;
        submit.fence_fd = -1;

        memset(&act, 0, sizeof(act));
        act.flags = RKNPU_ACT_RESET;
        if (ioctl(fd, DRM_IOCTL_RKNPU_ACTION, &act) < 0)
                perror("RKNPU_ACT_RESET");

        ret = ioctl(fd, DRM_IOCTL_RKNPU_SUBMIT, &submit);
        printf("[iter=%d mode=%d reg=%#llx val=%#llx amt=%u off=%u cmd_bo=%d] "
               "ret=%d errno=%d (%s) counter=%u elapse=%lld task_dma=%#llx cmd_dma=%#llx\n",
               iter, mode, (unsigned long long)reg, (unsigned long long)val,
               regcfg_amount, regcfg_offset, use_cmd_bo,
               ret, ret < 0 ? errno : 0, ret < 0 ? strerror(errno) : "ok",
               submit.task_counter, (long long)submit.hw_elapse_time,
               (unsigned long long)task_bo.dma_addr, (unsigned long long)cmd_dma);

        free_gem_bo(fd, cmd_sz, &cmd_bo, cmd_words);
        free_gem_bo(fd, task_sz, &task_bo, tasks);
        return ret;
}

int main(int argc, char **argv)
{
        const char *dev_path = "/dev/dri/renderD129";
        int mode = 1;
        uint64_t reg = RKNPU_OFFSET_INT_RAW_STATUS;
        uint64_t val = 1ULL;
        uint32_t regcfg_amount = 1U;
        uint32_t regcfg_offset = 0U;
        uint32_t timeout_ms = 100U;
        int use_cmd_bo = 0;
        int repeat = 1;
        int fd;

        /* Uso: [dev] [mode] [reg] [val] [regcfg_amount] [regcfg_offset] [timeout_ms] [use_cmd_bo] [repeat] */
        if (argc > 1 && argv[1][0] == '/')
                dev_path = argv[1];
        if (argc > 2) mode = atoi(argv[2]);
        if (argc > 3) reg = strtoull(argv[3], NULL, 0);
        if (argc > 4) val = strtoull(argv[4], NULL, 0);
        if (argc > 5) regcfg_amount = (uint32_t)strtoul(argv[5], NULL, 0);
        if (argc > 6) regcfg_offset = (uint32_t)strtoul(argv[6], NULL, 0);
        if (argc > 7) timeout_ms = (uint32_t)strtoul(argv[7], NULL, 0);
        if (argc > 8) use_cmd_bo = atoi(argv[8]);
        if (argc > 9) repeat = atoi(argv[9]);

        fd = open(dev_path, O_RDWR | O_CLOEXEC);
        if (fd < 0) {
                perror("open");
                return 1;
        }

        for (int i = 0; i < repeat; i++)
                run_once(fd, mode, reg, val, regcfg_amount, regcfg_offset,
                         timeout_ms, use_cmd_bo, i);

        close(fd);
        return 0;
}
