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

#define MAX_CUSTOM_WORDS 256
#define MAX_CUSTOM_WORDS32 512

struct test_opts {
        const char *dev_path;
        int mode;
        uint64_t reg;
        uint64_t val;
        uint32_t regcfg_amount;
        uint32_t regcfg_offset;
        uint32_t timeout_ms;
        int use_cmd_bo;
        int repeat;
        uint32_t int_mask;
        uint32_t int_clear;
        int custom_int_clear;
        uint32_t extra_mem_flags;
        uint64_t custom_words[MAX_CUSTOM_WORDS];
        size_t num_custom_words;
        uint32_t custom_words32[MAX_CUSTOM_WORDS32];
        size_t num_custom_words32;
};

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

static void sync_gem_bo_to_device(int fd, const struct rknpu_mem_create *bo,
                                   uint64_t offset, uint64_t size)
{
        struct rknpu_mem_sync sync_req;

        if (!(bo->flags & RKNPU_MEM_CACHEABLE) || !bo->obj_addr || size == 0)
                return;

        memset(&sync_req, 0, sizeof(sync_req));
        sync_req.flags = RKNPU_MEM_SYNC_TO_DEVICE;
        sync_req.obj_addr = bo->obj_addr;
        sync_req.offset = offset;
        sync_req.size = size;

        if (ioctl(fd, DRM_IOCTL_RKNPU_MEM_SYNC, &sync_req) < 0)
                perror("DRM_IOCTL_RKNPU_MEM_SYNC");
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
        case 2:
                return (reg << 32) | (val & 0xffffffffULL);
        case 3:
                return ((val + idx) << 32) | (reg & 0xffffffffULL);
        case 4:
                return (idx == 0) ? ((val << 32) | (reg & 0xffffffffULL))
                                  : 0xdeadbeef00000001ULL;
        case 1:
        default:
                return (val << 32) | (reg & 0xffffffffULL);
        }
}

static int parse_words_csv(const char *csv, uint64_t *out_words, size_t max_words,
                           size_t *out_count)
{
        char *copy;
        char *tok;
        size_t count = 0;

        if (!csv || !*csv)
                return -1;

        copy = strdup(csv);
        if (!copy)
                return -1;

        tok = strtok(copy, ",");
        while (tok) {
                char *endptr = NULL;
                unsigned long long w;

                while (*tok == ' ' || *tok == '\t')
                        tok++;

                errno = 0;
                w = strtoull(tok, &endptr, 0);
                if (errno != 0 || tok == endptr || count >= max_words) {
                        free(copy);
                        return -1;
                }

                out_words[count++] = (uint64_t)w;
                tok = strtok(NULL, ",");
        }

        free(copy);
        if (count == 0)
                return -1;

        *out_count = count;
        return 0;
}

static int parse_words32_csv(const char *csv, uint32_t *out_words, size_t max_words,
                             size_t *out_count)
{
        char *copy;
        char *tok;
        size_t count = 0;

        if (!csv || !*csv)
                return -1;

        copy = strdup(csv);
        if (!copy)
                return -1;

        tok = strtok(copy, ",");
        while (tok) {
                char *endptr = NULL;
                unsigned long w;

                while (*tok == ' ' || *tok == '\t')
                        tok++;

                errno = 0;
                w = strtoul(tok, &endptr, 0);
                if (errno != 0 || tok == endptr || count >= max_words ||
                    w > 0xffffffffUL) {
                        free(copy);
                        return -1;
                }

                out_words[count++] = (uint32_t)w;
                tok = strtok(NULL, ",");
        }

        free(copy);
        if (count == 0)
                return -1;

        *out_count = count;
        return 0;
}

static int run_once(int fd, const struct test_opts *opts, int iter)
{
        const uint64_t task_sz = 4096;
        const uint64_t cmd_sz = 4096;
        const uint32_t base_mem_flags =
                RKNPU_MEM_CONTIGUOUS | RKNPU_MEM_IOMMU |
                RKNPU_MEM_ZEROING | RKNPU_MEM_KERNEL_MAPPING |
                opts->extra_mem_flags;
        struct rknpu_mem_create task_bo, cmd_bo;
        struct rknpu_task *tasks = NULL;
        uint8_t *cmd_bytes = NULL;
        uint8_t *stream_bytes = NULL;
        struct rknpu_submit submit;
        struct rknpu_action act;
        uint32_t effective_amount = opts->regcfg_amount;
        size_t stream_bytes_len;
        size_t total_words;
        size_t stream_offset = sizeof(struct rknpu_task);
        uint64_t cmd_dma;
        uint32_t clear_val = opts->custom_int_clear ? opts->int_clear : opts->int_mask;
        int ret;

        if (opts->num_custom_words32 > 0) {
                stream_bytes_len = opts->num_custom_words32 * sizeof(uint32_t);
                total_words = (stream_bytes_len + sizeof(uint64_t) - 1U) /
                              sizeof(uint64_t);
                if (effective_amount == 0)
                        effective_amount = (uint32_t)total_words;
        } else {
                if (opts->num_custom_words > 0 && effective_amount == 0) {
                        effective_amount =
                                (opts->num_custom_words > RKNPU_PC_DATA_EXTRA_AMOUNT) ?
                                        (uint32_t)(opts->num_custom_words -
                                                   RKNPU_PC_DATA_EXTRA_AMOUNT) :
                                        1U;
                }

                total_words = (size_t)effective_amount + RKNPU_PC_DATA_EXTRA_AMOUNT;
                if (opts->num_custom_words > total_words)
                        total_words = opts->num_custom_words;

                stream_bytes_len = total_words * sizeof(uint64_t);
        }

        if (stream_offset + stream_bytes_len > task_sz ||
            stream_bytes_len > cmd_sz) {
                fprintf(stderr, "Buffer size exceeded for bytes=%zu\n",
                        stream_bytes_len);
                return -1;
        }

        if (alloc_gem_bo(fd, task_sz, base_mem_flags, &task_bo, (void **)&tasks) < 0)
                return -1;

        if (alloc_gem_bo(fd, cmd_sz, base_mem_flags, &cmd_bo, (void **)&cmd_bytes) < 0) {
                free_gem_bo(fd, task_sz, &task_bo, tasks);
                return -1;
        }

        if (opts->use_cmd_bo) {
                stream_bytes = cmd_bytes;
                cmd_dma = cmd_bo.dma_addr;
        } else {
                stream_bytes = (uint8_t *)tasks + stream_offset;
                cmd_dma = task_bo.dma_addr + stream_offset;
        }

        if (opts->num_custom_words32 > 0) {
                memcpy(stream_bytes, opts->custom_words32, stream_bytes_len);
                if (opts->use_cmd_bo)
                        memcpy(cmd_bytes, opts->custom_words32, stream_bytes_len);
        } else {
                uint64_t *dst64 = (uint64_t *)stream_bytes;
                uint64_t *cmd64 = (uint64_t *)cmd_bytes;

                for (size_t i = 0; i < total_words; i++) {
                        uint64_t w;

                        if (opts->num_custom_words > 0) {
                                w = (i < opts->num_custom_words) ?
                                            opts->custom_words[i] :
                                            0ULL;
                        } else {
                                w = make_word(opts->mode, opts->reg, opts->val, i);
                        }

                        dst64[i] = w;
                        cmd64[i] = w;
                }
        }

        tasks[0].flags = RKNPU_JOB_PC | RKNPU_JOB_BLOCK;
        tasks[0].enable_mask = 0x1U;
        tasks[0].int_mask = opts->int_mask;
        tasks[0].int_clear = clear_val;
        tasks[0].regcfg_amount = effective_amount;
        tasks[0].regcfg_offset = opts->regcfg_offset;
        tasks[0].regcmd_addr = cmd_dma;

        sync_gem_bo_to_device(fd, &cmd_bo, 0, stream_bytes_len);
        sync_gem_bo_to_device(fd, &task_bo, 0, stream_offset + stream_bytes_len);

        memset(&submit, 0, sizeof(submit));
        submit.flags = RKNPU_JOB_PC | RKNPU_JOB_BLOCK;
        submit.timeout = opts->timeout_ms;
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
        printf("[iter=%d mask=%#x clear=%#x words=%zu bytes=%zu amt=%u off=%u cmd_bo=%d] "
               "ret=%d errno=%d (%s) counter=%u elapse=%lld task_dma=%#llx cmd_dma=%#llx\n",
               iter, opts->int_mask, clear_val, total_words, stream_bytes_len,
               effective_amount, opts->regcfg_offset, opts->use_cmd_bo,
               ret, ret < 0 ? errno : 0, ret < 0 ? strerror(errno) : "ok",
               submit.task_counter, (long long)submit.hw_elapse_time,
               (unsigned long long)task_bo.dma_addr, (unsigned long long)cmd_dma);

        free_gem_bo(fd, cmd_sz, &cmd_bo, cmd_bytes);
        free_gem_bo(fd, task_sz, &task_bo, tasks);
        return ret;
}

static void print_usage(const char *prog)
{
        fprintf(stderr,
                "Uso: %s [dev] [mode] [reg] [val] [regcfg_amount] [regcfg_offset] "
                "[timeout_ms] [use_cmd_bo] [repeat] [int_mask] [words_csv]\n"
                "  --mask <hex/dec>       Mascara de interrupcion (ej. 0x1, 0x2, 0x1ff)\n"
                "  --clear <hex/dec>      Valor de int_clear (por defecto igual a mask)\n"
                "  --words <w0,w1,...>    Stream de palabras de 64 bits separado por coma\n"
                "  --words32 <w0,w1,...>  Stream de palabras de 32 bits separado por coma\n"
                "  --cacheable            Anade RKNPU_MEM_CACHEABLE + MEM_SYNC\n",
                prog);
}

int main(int argc, char **argv)
{
        struct test_opts opts;
        int pos = 0;
        int fd;

        memset(&opts, 0, sizeof(opts));
        opts.dev_path = "/dev/dri/renderD129";
        opts.mode = 1;
        opts.reg = RKNPU_OFFSET_INT_RAW_STATUS;
        opts.val = 1ULL;
        opts.regcfg_amount = 1U;
        opts.regcfg_offset = 0U;
        opts.timeout_ms = 100U;
        opts.use_cmd_bo = 1;
        opts.repeat = 1;
        opts.int_mask = 0x1U;

        for (int i = 1; i < argc; i++) {
                if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
                        print_usage(argv[0]);
                        return 0;
                } else if (strcmp(argv[i], "--mask") == 0 && i + 1 < argc) {
                        opts.int_mask = (uint32_t)strtoul(argv[++i], NULL, 0);
                } else if (strcmp(argv[i], "--clear") == 0 && i + 1 < argc) {
                        opts.int_clear = (uint32_t)strtoul(argv[++i], NULL, 0);
                        opts.custom_int_clear = 1;
                } else if (strcmp(argv[i], "--words") == 0 && i + 1 < argc) {
                        if (parse_words_csv(argv[++i], opts.custom_words,
                                            MAX_CUSTOM_WORDS, &opts.num_custom_words) < 0) {
                                fprintf(stderr, "Error parseando --words\n");
                                return 1;
                        }
                } else if (strcmp(argv[i], "--words32") == 0 && i + 1 < argc) {
                        if (parse_words32_csv(argv[++i], opts.custom_words32,
                                              MAX_CUSTOM_WORDS32,
                                              &opts.num_custom_words32) < 0) {
                                fprintf(stderr, "Error parseando --words32\n");
                                return 1;
                        }
                } else if (strcmp(argv[i], "--cacheable") == 0) {
                        opts.extra_mem_flags |= RKNPU_MEM_CACHEABLE;
                } else {
                        pos++;
                        switch (pos) {
                        case 1:
                                if (argv[i][0] == '/')
                                        opts.dev_path = argv[i];
                                break;
                        case 2:
                                opts.mode = atoi(argv[i]);
                                break;
                        case 3:
                                opts.reg = strtoull(argv[i], NULL, 0);
                                break;
                        case 4:
                                opts.val = strtoull(argv[i], NULL, 0);
                                break;
                        case 5:
                                opts.regcfg_amount = (uint32_t)strtoul(argv[i], NULL, 0);
                                break;
                        case 6:
                                opts.regcfg_offset = (uint32_t)strtoul(argv[i], NULL, 0);
                                break;
                        case 7:
                                opts.timeout_ms = (uint32_t)strtoul(argv[i], NULL, 0);
                                break;
                        case 8:
                                opts.use_cmd_bo = atoi(argv[i]);
                                break;
                        case 9:
                                opts.repeat = atoi(argv[i]);
                                break;
                        case 10:
                                opts.int_mask = (uint32_t)strtoul(argv[i], NULL, 0);
                                break;
                        case 11:
                                if (parse_words_csv(argv[i], opts.custom_words,
                                                    MAX_CUSTOM_WORDS,
                                                    &opts.num_custom_words) < 0) {
                                        fprintf(stderr, "Error parseando words_csv\n");
                                        return 1;
                                }
                                break;
                        default:
                                break;
                        }
                }
        }

        fd = open(opts.dev_path, O_RDWR | O_CLOEXEC);
        if (fd < 0) {
                perror("open");
                return 1;
        }

        for (int i = 0; i < opts.repeat; i++)
                run_once(fd, &opts, i);

        close(fd);
        return 0;
}
