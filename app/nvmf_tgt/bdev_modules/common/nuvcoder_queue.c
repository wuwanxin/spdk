#include "nuvcoder_queue.h"
#include "spdk/log.h" // 使用 SPDK 的日志宏
#include "spdk/env.h" // For spdk_zmalloc

// !!! 关键修改：包含 DPDK 头文件用于内存检查 !!!
#include <rte_malloc.h>
#include <rte_memory.h>
#include <rte_memzone.h>

// !!! 关键修改：直接包含 xcoder_types.h !!!
// 这允许 nuvcoder_queue.c 访问 xcoder_instance_t 的完整定义，
// 以便在日志和参数检查中获取 instance_id。
#include "nuvcoder_types.h" // 确保这个头文件存在且定义了 xcoder_instance

// nuvcoder_queue.c 中的内存检查相关函数

// 内存段信息回调函数上下文
struct mem_info_context {
    size_t total_mem;          // 总内存
    size_t max_continuous;     // 最大连续块
    size_t current_continuous; // 当前连续块
    uint64_t last_iova;        // 上一个段的IOVA地址
    int seg_count;             // 段计数
};

// 内存段遍历回调函数
static int dump_memseg_cb(const struct rte_memseg_list *msl, const struct rte_memseg *ms, 
                          void *arg) {
    struct mem_info_context *ctx = (struct mem_info_context *)arg;
    
    // 只统计主内存，不统计外部堆
    if (msl->external)
        return 0;
    
    ctx->seg_count++;
    ctx->total_mem += ms->len;
    
    // 使用 iova 字段（根据提供的头文件）
    fprintf(stderr, "  Segment %d: addr=%p, iova=0x%lx, len=%zu, socket=%d\n",
            ctx->seg_count, ms->addr, (unsigned long)ms->iova, ms->len, ms->socket_id);
    
    // 检查IOVA连续性
    if (ctx->last_iova != 0) {
        // 检查是否连续：当前段的iova是否等于上一段的iova + 上一段的长度
        if (ctx->last_iova == ms->iova) {
            // 物理连续
            ctx->current_continuous += ms->len;
        } else {
            // 不连续，更新最大连续块
            if (ctx->current_continuous > ctx->max_continuous) {
                ctx->max_continuous = ctx->current_continuous;
            }
            ctx->current_continuous = ms->len;
        }
    } else {
        ctx->current_continuous = ms->len;
    }
    
    ctx->last_iova = ms->iova + ms->len;
    
    return 0;
}

// DPDK 内存信息转储函数
static void dump_dpdk_memory_info(size_t needed_size) {
    struct mem_info_context ctx = {0};
    
    fprintf(stderr, "\n=== DPDK MEMORY POOL DUMP ===\n");
    
    // 遍历所有内存段
    int ret = rte_memseg_walk(dump_memseg_cb, &ctx);
    if (ret < 0) {
        fprintf(stderr, "Failed to walk memsegs\n");
        return;
    }
    
    // 检查最后一个连续块
    if (ctx.current_continuous > ctx.max_continuous) {
        ctx.max_continuous = ctx.current_continuous;
    }
    
    fprintf(stderr, "\nMemory Statistics:\n");
    fprintf(stderr, "  Total memory segments: %d\n", ctx.seg_count);
    fprintf(stderr, "  Total memory size: %zu bytes (%.2f MB)\n", 
            ctx.total_mem, ctx.total_mem / (1024.0 * 1024.0));
    fprintf(stderr, "  Max continuous block: %zu bytes (%.2f MB)\n", 
            ctx.max_continuous, ctx.max_continuous / (1024.0 * 1024.0));
    fprintf(stderr, "  Requested size: %zu bytes (%.2f MB)\n", 
            needed_size, needed_size / (1024.0 * 1024.0));
    
    if (ctx.max_continuous < needed_size) {
        fprintf(stderr, "  ❌ No enough continuous memory for allocation\n");
        fprintf(stderr, "  💡 Suggestion: Increase --mem parameter or reduce allocation size\n");
    } else {
        fprintf(stderr, "  ✅ Enough continuous memory available\n");
    }
    
    fprintf(stderr, "----------- MALLOC STATS -----------\n");
    rte_malloc_dump_stats(stderr, "all");
    
    fprintf(stderr, "----------- MEMORY ZONES -----------\n");
    rte_memzone_dump(stderr);
    
    fprintf(stderr, "=====================================\n");
}

// 检查系统大页配置
static void check_hugepage_config(void) {
    fprintf(stderr, "\n=== SYSTEM HUGEPAGE CONFIG ===\n");
    
    FILE *fp = fopen("/proc/meminfo", "r");
    if (fp) {
        char line[256];
        while (fgets(line, sizeof(line), fp)) {
            if (strstr(line, "HugePages_Total") || 
                strstr(line, "HugePages_Free") ||
                strstr(line, "Hugepagesize")) {
                // 去掉末尾的换行符
                line[strcspn(line, "\n")] = 0;
                fprintf(stderr, "  %s\n", line);
            }
        }
        fclose(fp);
    } else {
        fprintf(stderr, "  Failed to open /proc/meminfo\n");
    }
    
    fprintf(stderr, "==============================\n");
}

// --------------------------------------------------------------------------
// 公共 API 实现
// --------------------------------------------------------------------------

// 重要的修改：增加了 max_frame_size 参数
int nuvcoder_queue_init(nuvcoder_queue_t *queue, uint32_t instance_id, 
                        size_t input_max_frame_size, size_t output_max_frame_size) {
    // 添加指针有效性检查
    fprintf(stderr, "DEBUG: nuvcoder_queue_init called\n");
    fprintf(stderr, "DEBUG:   queue pointer = %p\n", queue);
    fprintf(stderr, "DEBUG:   queue->entries address = %p\n", queue ? &queue->entries[0] : NULL);
    fprintf(stderr, "DEBUG:   sizeof(nuvcoder_queue_t) = %zu\n", sizeof(nuvcoder_queue_t));
    fprintf(stderr, "DEBUG:   instance_id = %u\n", instance_id);
    fprintf(stderr, "DEBUG:   input_max_frame_size = %zu (0x%zx)\n", 
            input_max_frame_size, input_max_frame_size);
    fprintf(stderr, "DEBUG:   output_max_frame_size = %zu (0x%zx)\n", 
            output_max_frame_size, output_max_frame_size);
    fprintf(stderr, "DEBUG:   XCODER_QUEUE_DEPTH = %d\n", XCODER_QUEUE_DEPTH);
    
    // 检查 queue 结构体是否在可访问的内存区域
    if (queue) {
        // 尝试访问 queue 的成员，看是否会 crash
        fprintf(stderr, "DEBUG:   queue->head initial value = %d\n", queue->head);
        fprintf(stderr, "DEBUG:   queue->tail initial value = %d\n", queue->tail);
        fprintf(stderr, "DEBUG:   queue->count initial value = %d\n", queue->count);
    }
    
    size_t total_memory = XCODER_QUEUE_DEPTH * (input_max_frame_size + output_max_frame_size);
    fprintf(stderr, "DEBUG:   Total DMA memory needed = %zu bytes (%.2f MB)\n", 
            total_memory, total_memory / (1024.0 * 1024.0));
    
    if (!queue) {
        fprintf(stderr, "ERROR: queue pointer is NULL for instance %u\n", instance_id);
        return -1;
    }
    
    if (input_max_frame_size == 0) {
        fprintf(stderr, "ERROR: input_max_frame_size is 0 for instance %u\n", instance_id);
        return -1;
    }
    
    if (output_max_frame_size == 0) {
        fprintf(stderr, "ERROR: output_max_frame_size is 0 for instance %u\n", instance_id);
        return -1;
    }
    
    // 🎯 在分配前检查系统大页配置
    check_hugepage_config();
    
    // 🎯 检查 DPDK 内存池状态
    dump_dpdk_memory_info(input_max_frame_size);

    fprintf(stderr, "INFO: Initializing queue for instance %u with depth %d, input_size=%zu, output_size=%zu\n",
           instance_id, XCODER_QUEUE_DEPTH, input_max_frame_size, output_max_frame_size);

    queue->head = 0;
    queue->tail = 0;
    queue->count = 0;

    if (pthread_mutex_init(&queue->lock, NULL) != 0) {
        fprintf(stderr, "ERROR: Failed to init mutex for instance %u\n", instance_id);
        return -1;
    }
    if (pthread_cond_init(&queue->producer_cond, NULL) != 0) {
        fprintf(stderr, "ERROR: Failed to init producer_cond for instance %u\n", instance_id);
        pthread_mutex_destroy(&queue->lock);
        return -1;
    }
    if (pthread_cond_init(&queue->consumer_cond, NULL) != 0) {
        fprintf(stderr, "ERROR: Failed to init consumer_cond for instance %u\n", instance_id);
        pthread_cond_destroy(&queue->producer_cond);
        pthread_mutex_destroy(&queue->lock);
        return -1;
    }

    for (int i = 0; i < XCODER_QUEUE_DEPTH; i++) {
        nuvcoder_queue_entry_t *entry = &queue->entries[i];
        
        // 初始化 entry 的 mutex 和 cond
        if (pthread_mutex_init(&entry->mutex, NULL) != 0) {
            fprintf(stderr, "ERROR: Failed to init entry mutex for instance %u, entry %d\n", instance_id, i);
            
            // 清理之前成功初始化的 entry mutex/cond
            for (int j = 0; j < i; j++) {
                pthread_mutex_destroy(&queue->entries[j].mutex);
                pthread_cond_destroy(&queue->entries[j].cond);
            }
            // 清理队列级别的资源
            pthread_cond_destroy(&queue->consumer_cond);
            pthread_cond_destroy(&queue->producer_cond);
            pthread_mutex_destroy(&queue->lock);
            return -1;
        }
        
        if (pthread_cond_init(&entry->cond, NULL) != 0) {
            fprintf(stderr, "ERROR: Failed to init entry cond for instance %u, entry %d\n", instance_id, i);
            
            // 清理当前 entry 的 mutex
            pthread_mutex_destroy(&entry->mutex);
            
            // 清理之前成功初始化的 entry
            for (int j = 0; j < i; j++) {
                pthread_mutex_destroy(&queue->entries[j].mutex);
                pthread_cond_destroy(&queue->entries[j].cond);
            }
            // 清理队列级别的资源
            pthread_cond_destroy(&queue->consumer_cond);
            pthread_cond_destroy(&queue->producer_cond);
            pthread_mutex_destroy(&queue->lock);
            return -1;
        }

        entry->index = i;
        entry->parent_instance = NULL;
        entry->user_context = NULL;
        entry->input_length = 0;
        entry->output_actual_length = 0;
        entry->result_code = -1;
        entry->is_completed = false;

        // 分配 DMA 缓冲区
        fprintf(stderr, "DEBUG: Allocating input buffer for entry %d, size %zu\n", i, input_max_frame_size);
        entry->input_buffer_dma = spdk_zmalloc(input_max_frame_size, 0, NULL,
                                              SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);
        if (!entry->input_buffer_dma) {
            fprintf(stderr, "ERROR: Failed to allocate input_buffer_dma for instance %u, entry %d (size %zu)\n",
                   instance_id, i, input_max_frame_size);
            
            // 🎯 分配失败时再次转储内存信息，查看是否有变化
            dump_dpdk_memory_info(input_max_frame_size);
            
            // 清理之前分配的缓冲区
            for (int j = 0; j < i; j++) {
                if (queue->entries[j].input_buffer_dma) {
                    spdk_free(queue->entries[j].input_buffer_dma);
                    queue->entries[j].input_buffer_dma = NULL;
                }
                if (queue->entries[j].output_buffer_dma) {
                    spdk_free(queue->entries[j].output_buffer_dma);
                    queue->entries[j].output_buffer_dma = NULL;
                }
            }
            
            // 清理当前 entry 的 mutex/cond
            pthread_mutex_destroy(&entry->mutex);
            pthread_cond_destroy(&entry->cond);
            
            // 清理所有之前 entry 的 mutex/cond
            for (int j = 0; j < i; j++) {
                pthread_mutex_destroy(&queue->entries[j].mutex);
                pthread_cond_destroy(&queue->entries[j].cond);
            }
            
            // 清理队列级别的资源
            pthread_cond_destroy(&queue->consumer_cond);
            pthread_cond_destroy(&queue->producer_cond);
            pthread_mutex_destroy(&queue->lock);
            return -1;
        }

        fprintf(stderr, "DEBUG: Allocating output buffer for entry %d, size %zu\n", i, output_max_frame_size);
        entry->output_buffer_dma = spdk_zmalloc(output_max_frame_size, 0, NULL,
                                               SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);
        if (!entry->output_buffer_dma) {
            fprintf(stderr, "ERROR: Failed to allocate output_buffer_dma for instance %u, entry %d (size %zu)\n",
                   instance_id, i, output_max_frame_size);
            
            // 🎯 分配失败时再次转储内存信息
            dump_dpdk_memory_info(output_max_frame_size);
            
            // 清理当前 entry 已分配的 input buffer
            if (entry->input_buffer_dma) {
                spdk_free(entry->input_buffer_dma);
                entry->input_buffer_dma = NULL;
            }
            
            // 清理之前分配的缓冲区
            for (int j = 0; j < i; j++) {
                if (queue->entries[j].input_buffer_dma) {
                    spdk_free(queue->entries[j].input_buffer_dma);
                    queue->entries[j].input_buffer_dma = NULL;
                }
                if (queue->entries[j].output_buffer_dma) {
                    spdk_free(queue->entries[j].output_buffer_dma);
                    queue->entries[j].output_buffer_dma = NULL;
                }
            }
            
            // 清理所有 entry 的 mutex/cond（包括当前和之前的）
            for (int j = 0; j <= i; j++) {
                pthread_mutex_destroy(&queue->entries[j].mutex);
                pthread_cond_destroy(&queue->entries[j].cond);
            }
            
            // 清理队列级别的资源
            pthread_cond_destroy(&queue->consumer_cond);
            pthread_cond_destroy(&queue->producer_cond);
            pthread_mutex_destroy(&queue->lock);
            return -1;
        }

        // 清零缓冲区（可选）
        memset(entry->input_buffer_dma, 0, input_max_frame_size);
        memset(entry->output_buffer_dma, 0, output_max_frame_size);
    }

    fprintf(stderr, "INFO: Queue initialized successfully for instance %u\n", instance_id);
    return 0;
}

void nuvcoder_queue_destroy(nuvcoder_queue_t *queue, uint32_t instance_id) {
    if (!queue) {
        return;
    }

    // 等待所有队列中的请求完成，确保安全销毁
    pthread_mutex_lock(&queue->lock);
    while (queue->count > 0) {
        SPDK_WARNLOG("Warning: Destroying queue for instance %u with %d pending requests. Waiting...\n",
                     instance_id, queue->count);
        // 等待消费者消耗完所有请求
        pthread_cond_wait(&queue->consumer_cond, &queue->lock);
    }
    pthread_mutex_unlock(&queue->lock);

    for (int i = 0; i < XCODER_QUEUE_DEPTH; i++) {
        nuvcoder_queue_entry_t *entry = &queue->entries[i];
        // 释放为每个 entry 分配的 DMA 缓冲区
        if (entry->input_buffer_dma) {
            spdk_free(entry->input_buffer_dma);
            entry->input_buffer_dma = NULL;
        }
        if (entry->output_buffer_dma) {
            spdk_free(entry->output_buffer_dma);
            entry->output_buffer_dma = NULL;
        }
        pthread_mutex_destroy(&entry->mutex);
        pthread_cond_destroy(&entry->cond);
    }

    pthread_mutex_destroy(&queue->lock);
    pthread_cond_destroy(&queue->producer_cond);
    pthread_cond_destroy(&queue->consumer_cond);

    printf("Xcoder queue destroyed for instance %u.\n", instance_id);
}

// 重要的修改：移除了 in_buf, in_len, out_buf, out_actual_len_ptr 参数
nuvcoder_queue_entry_t* nuvcoder_queue_get_entry_and_prepare(nuvcoder_queue_t *queue, struct xcoder_instance *parent_instance, void *user_ctx) {
    if (!queue || !parent_instance) {
        printf("Error: Invalid parameters for nuvcoder_queue_get_entry_and_prepare (instance %u).\n",
                    parent_instance ? parent_instance->instance_id : 0);
        return NULL;
    }

    pthread_mutex_lock(&queue->lock);

    // 阻塞等待，直到队列中有空间可用
    while (queue->count == XCODER_QUEUE_DEPTH) {
        printf("Xcoder queue full for instance %u, waiting for space...\n",
                      parent_instance->instance_id);
        pthread_cond_wait(&queue->producer_cond, &queue->lock);
    }

    nuvcoder_queue_entry_t *entry = &queue->entries[queue->head];

    // 初始化 entry
    entry->user_context = user_ctx; // 存储用户上下文
    // input_buffer_dma 和 output_buffer_dma 已经在 init 时分配好了
    entry->input_length = 0; // 重置为0，等待数据写入
    entry->output_actual_length = 0; // 重置为0
    entry->result_code = -1; // 重置结果码
    entry->is_completed = false; // 重置完成标志
    entry->parent_instance = parent_instance; // 在这里设置 parent_instance

    pthread_mutex_unlock(&queue->lock); // 先解锁队列，让其他线程可以访问队列
    printf("Xcoder queue instance %u: Prepared entry %d.\n",
                  parent_instance->instance_id, entry->index);
    return entry; // 返回准备好的 entry
}

void nuvcoder_queue_notify_submit(nuvcoder_queue_t *queue, nuvcoder_queue_entry_t *entry) {
    if (!queue || !entry) {
        printf("Error: Invalid parameters for nuvcoder_queue_notify_submit.\n");
        return;
    }

    pthread_mutex_lock(&queue->lock);

    // 检查 entry 是否在预期位置（head），以防止逻辑错误
    // 更好的检查是：确认这个 entry 属于当前 head 指向的那个 slot，而不是直接比较指针
    // 因为 entry 可能被复制或移动。这里假设 entry 数组是静态的，指针比较有效。
    if (entry != &queue->entries[queue->head]) {
        printf("Error: Attempted to submit entry %d which is not at queue head %d (instance %u). This indicates a logic error.\n",
                    entry->index, queue->head, entry->parent_instance ? entry->parent_instance->instance_id : 0);
        pthread_mutex_unlock(&queue->lock);
        return;
    }

    // 递增 head 和 count
    queue->head = (queue->head + 1) % XCODER_QUEUE_DEPTH;
    queue->count++;

    pthread_mutex_unlock(&queue->lock);

    printf("Xcoder queue instance %u: Notified submit for entry %d. Current count: %d.\n",
                  entry->parent_instance ? entry->parent_instance->instance_id : 0, entry->index, queue->count);
}

nuvcoder_queue_entry_t* nuvcoder_queue_peek_tail(nuvcoder_queue_t *queue) {
    if (!queue) {
        return NULL;
    }

    pthread_mutex_lock(&queue->lock);
    
    nuvcoder_queue_entry_t *entry = NULL;
    if (queue->count > 0) {
        entry = &queue->entries[queue->tail];
    }
    
    pthread_mutex_unlock(&queue->lock);
    return entry;
}

int nuvcoder_queue_read_output_wait(nuvcoder_queue_t *queue, nuvcoder_queue_entry_t **entry_out) {
    if (!queue || !entry_out) {
        printf("Error: Invalid parameters for nuvcoder_queue_read_output_wait\n");
        return -1;
    }

    pthread_mutex_lock(&queue->lock);

    // 等待队列中有已完成的请求
    while (queue->count == 0 || !queue->entries[queue->tail].is_completed) {
        printf("Xcoder queue %p: Empty or next entry %d not completed, waiting...\n",
                      queue, queue->tail);
        pthread_cond_wait(&queue->consumer_cond, &queue->lock);
    }

    // 获取队尾已完成的 entry
    nuvcoder_queue_entry_t *entry = &queue->entries[queue->tail];
    pthread_mutex_lock(&entry->mutex);

    // 确保entry已完成
    while (!entry->is_completed) {
        printf("Queue %p: Entry %d not completed, waiting on entry cond\n",
                      queue, entry->index);
        pthread_cond_wait(&entry->cond, &entry->mutex);
    }

    // 返回entry指针（不清理状态）
    *entry_out = entry;
    int result = entry->result_code;

    pthread_mutex_unlock(&entry->mutex);
    pthread_mutex_unlock(&queue->lock);

    printf("Xcoder queue %p: Got completed entry %d (result: %d).\n",
                  queue, entry->index, result);

    return result;
}

void nuvcoder_queue_release_entry(nuvcoder_queue_t *queue, nuvcoder_queue_entry_t *entry) {
    if (!queue || !entry) {
        printf("Error: Invalid parameters for nuvcoder_queue_release_entry\n");
        return;
    }

    pthread_mutex_lock(&queue->lock);
    pthread_mutex_lock(&entry->mutex);

    // 检查这个entry是否是当前tail（确保按顺序释放）
    if (entry != &queue->entries[queue->tail]) {
        printf("Error: Attempted to release entry %d which is not at queue tail %d\n",
                    entry->index, queue->tail);
        pthread_mutex_unlock(&entry->mutex);
        pthread_mutex_unlock(&queue->lock);
        return;
    }

    // 清理entry状态，使其可以被重用
    entry->user_context = NULL;
    entry->input_length = 0;
    entry->output_actual_length = 0;
    entry->result_code = -1;
    entry->is_completed = false;
    entry->parent_instance = NULL;

    pthread_mutex_unlock(&entry->mutex);

    // 移动tail和减少count
    queue->tail = (queue->tail + 1) % XCODER_QUEUE_DEPTH;
    queue->count--;

    pthread_cond_signal(&queue->producer_cond); // 唤醒可能的生产者
    pthread_mutex_unlock(&queue->lock);

    printf("Xcoder queue %p: Released entry %d. Current count: %d\n",
                  queue, entry->index, queue->count);
}

void nuvcoder_queue_mark_completed(nuvcoder_queue_t *queue, nuvcoder_queue_entry_t *entry, int result_code, size_t actual_output_len) {
    if (!queue || !entry) {
        printf("Error: nuvcoder_queue_mark_completed received NULL queue or entry.\n");
        return;
    }

    pthread_mutex_lock(&entry->mutex);

    // 更新 entry 状态
    entry->result_code = result_code;
    entry->output_actual_length = actual_output_len; // 直接赋值给 size_t 成员
    entry->is_completed = true;

    pthread_cond_signal(&entry->cond); // 唤醒可能正在等待此 entry 完成的线程

    pthread_mutex_unlock(&entry->mutex);

    // 唤醒等待队列中有已完成请求的消费者
    pthread_mutex_lock(&queue->lock);
    pthread_cond_signal(&queue->consumer_cond);
    pthread_mutex_unlock(&queue->lock);

    printf("Xcoder Queue instance %u: Entry %d marked completed (result %d, output len %zu).\n",
                  entry->parent_instance ? entry->parent_instance->instance_id : 0,
                  entry->index, result_code, actual_output_len);
}


// ==================== 全局队列池 ====================
static nuvcoder_queue_t *g_queue_pool = NULL;
static bool *g_queue_in_use = NULL;
static int g_pool_size = NUVCODER_QUEUE_POOL_SIZE;  // 使用宏
static bool g_pool_created = false;
static pthread_mutex_t g_pool_mutex = PTHREAD_MUTEX_INITIALIZER;

int nuvcoder_queue_pool_create(size_t input_size, size_t output_size) {
    if (g_pool_created) {
        printf("Queue pool already created\n");
        return 0;
    }
    
    printf("Creating queue pool with size %d\n", g_pool_size);
    
    // 分配队列数组
    g_queue_pool = (nuvcoder_queue_t *)calloc(g_pool_size, sizeof(nuvcoder_queue_t));
    if (!g_queue_pool) {
        fprintf(stderr, "Failed to allocate queue pool\n");
        return -1;
    }
    
    // 分配使用状态数组
    g_queue_in_use = (bool *)calloc(g_pool_size, sizeof(bool));
    if (!g_queue_in_use) {
        fprintf(stderr, "Failed to allocate in-use array\n");
        free(g_queue_pool);
        g_queue_pool = NULL;
        return -1;
    }
    
    // 初始化每个队列
    for (int i = 0; i < g_pool_size; i++) {
        int ret = nuvcoder_queue_init(&g_queue_pool[i], i, input_size, output_size);
        if (ret != 0) {
            fprintf(stderr, "Failed to init queue %d\n", i);
            // 清理已初始化的
            for (int j = 0; j < i; j++) {
                nuvcoder_queue_destroy(&g_queue_pool[j], j);
            }
            free(g_queue_in_use);
            free(g_queue_pool);
            g_queue_in_use = NULL;
            g_queue_pool = NULL;
            return -1;
        }
        g_queue_in_use[i] = false;  // 初始都空闲
    }
    
    g_pool_created = true;
    printf("Queue pool created successfully: %d queues\n", g_pool_size);
    return 0;
}

nuvcoder_queue_t* nuvcoder_queue_pool_get_handler(uint32_t instance_id) {
    if (!g_pool_created) {
        fprintf(stderr, "Queue pool not created!\n");
        return NULL;
    }
    
    pthread_mutex_lock(&g_pool_mutex);
    
    // 查找空闲队列
    for (int i = 0; i < g_pool_size; i++) {
        if (!g_queue_in_use[i]) {
            g_queue_in_use[i] = true;
            printf("Queue %d acquired for instance %u\n", i, instance_id);
            pthread_mutex_unlock(&g_pool_mutex);
            return &g_queue_pool[i];
        }
    }
    
    fprintf(stderr, "No free queue for instance %u\n", instance_id);
    pthread_mutex_unlock(&g_pool_mutex);
    return NULL;
}

void nuvcoder_queue_pool_reset_handler(nuvcoder_queue_t *queue, uint32_t instance_id) {
    if (!queue || !g_pool_created) {
        return;
    }
    
    // 找到队列索引
    int idx = -1;
    for (int i = 0; i < g_pool_size; i++) {
        if (&g_queue_pool[i] == queue) {
            idx = i;
            break;
        }
    }
    
    if (idx < 0) {
        fprintf(stderr, "Invalid queue for instance %u\n", instance_id);
        return;
    }
    
    pthread_mutex_lock(&g_pool_mutex);
    
    if (!g_queue_in_use[idx]) {
        fprintf(stderr, "Queue %d already free for instance %u\n", idx, instance_id);
        pthread_mutex_unlock(&g_pool_mutex);
        return;
    }
    
    // !!! 关键修复：真正重置队列的内部状态 !!!
    pthread_mutex_lock(&queue->lock);
    
    // 重置环形缓冲区指针
    queue->head = 0;
    queue->tail = 0;
    queue->count = 0;
    
    // 重置所有entry的状态
    for (int i = 0; i < XCODER_QUEUE_DEPTH; i++) {
        nuvcoder_queue_entry_t *entry = &queue->entries[i];
        pthread_mutex_lock(&entry->mutex);
        
        // 重置entry状态，但保留buffer（属于池）
        entry->user_context = NULL;
        entry->input_length = 0;
        entry->output_actual_length = 0;
        entry->result_code = -1;
        entry->is_completed = false;
        entry->parent_instance = NULL;
        
        pthread_mutex_unlock(&entry->mutex);
    }
    
    printf("Queue %d reset by instance %u (head=%d, tail=%d, count=%d)\n", 
           idx, instance_id, queue->head, queue->tail, queue->count);
    
    pthread_mutex_unlock(&queue->lock);
    
    // 标记队列为空闲
    g_queue_in_use[idx] = false;
    
    pthread_mutex_unlock(&g_pool_mutex);
}

void nuvcoder_queue_pool_destroy(void) {
    if (!g_pool_created) {
        return;
    }
    
    printf("Destroying queue pool...\n");
    
    // 销毁所有队列
    for (int i = 0; i < g_pool_size; i++) {
        nuvcoder_queue_destroy(&g_queue_pool[i], i);
    }
    
    free(g_queue_in_use);
    free(g_queue_pool);
    
    g_queue_in_use = NULL;
    g_queue_pool = NULL;
    g_pool_created = false;
    
    printf("Queue pool destroyed\n");
}