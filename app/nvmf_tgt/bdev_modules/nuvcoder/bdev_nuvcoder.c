#include "bdev_nuvcoder.h"
#include "spdk/env.h"
#include "spdk/bdev_module.h"
#include "spdk/thread.h"
#include "spdk/string.h"
#include "spdk/log.h"
#include "spdk/dma.h"

#include "nuvcoder_vsc_opcodes.h"

#include <time.h> // For clock_gettime, struct timespec
typedef struct timespec xcoder_timestamp_t;

// 辅助函数：获取当前时间
static inline void xcoder_get_current_time(xcoder_timestamp_t *ts) {
    clock_gettime(CLOCK_MONOTONIC, ts); // 使用单调时钟，不会受系统时间调整影响
}

// 辅助函数：计算两个时间戳之间的微秒差
static inline long long xcoder_get_duration_us(xcoder_timestamp_t *start, xcoder_timestamp_t *end) {
    long long seconds = end->tv_sec - start->tv_sec;
    long long nanoseconds = end->tv_nsec - start->tv_nsec;
    return seconds * 1000000 + nanoseconds / 1000;
}
static xcoder_timestamp_t debug_input_received_start_timestamp;
static xcoder_timestamp_t debug_input_received_end_timestamp;

extern xcoder_session_t *xcoder_find_session(uint32_t session_id);
extern xcoder_instance_t *xcoder_find_instance(xcoder_session_t *session, uint32_t instance_id);
extern int nuvcoder_send_frame(nuvcoder_queue_entry_t *entry);
extern int xcoder_get_bitstream(nuvcoder_queue_entry_t *entry);

// --------------------------------------------------------------------------
// Xcoder Virtual Bdev Private Data Structures
// --------------------------------------------------------------------------

static struct xcoder_bdev *g_xcoder_bdev = NULL;

// 【改动点 1】: 添加内部创建函数的“前向声明”，这样我们在上面的 init 里就能调用下面的代码
static int internal_xcoder_bdev_create(void);

// --------------------------------------------------------------------------
// Bdev Module Callbacks
// --------------------------------------------------------------------------

// 模块初始化函数
static int xcoder_bdev_module_init(void)
{
    printf( "Xcoder bdev module initialized.\n");
    
    // 【改动点 2】: 这里是关键！模块加载时直接创建 Bdev。
    // 这样当 SPDK 解析 JSON 配置文件时，设备已经存在了，就不会报错了。
    int rc = internal_xcoder_bdev_create();
    if (rc != 0) {
        printf("Failed to create xcoder bdev at module init\n");
        return rc;
    }
    
    return 0;
}

// 模块清理函数
static void xcoder_bdev_module_fini(void)
{
    printf( "Xcoder bdev module finalizing.\n");
    if (g_xcoder_bdev) {
        spdk_io_device_unregister(g_xcoder_bdev, NULL);
        spdk_bdev_unregister(&g_xcoder_bdev->bdev, NULL, NULL);
        
        if (g_xcoder_bdev->desc) {
            spdk_bdev_close(g_xcoder_bdev->desc);
        }
        
        free(g_xcoder_bdev->bdev.name);
        free(g_xcoder_bdev->bdev.product_name);
        free(g_xcoder_bdev);
        g_xcoder_bdev = NULL;
    }
    printf( "Xcoder bdev module finalized completely.\n");
}

static struct spdk_bdev_module g_xcoder_module = {
    .name = "xcoder",
    .module_init = xcoder_bdev_module_init,
    .module_fini = xcoder_bdev_module_fini,
    .async_init = false,
    .async_fini = false,
};

SPDK_BDEV_MODULE_REGISTER(xcoder, &g_xcoder_module)

// --------------------------------------------------------------------------
// 辅助函数定义 (保持原样)
// --------------------------------------------------------------------------

static uint64_t xcoder_get_io_lba(struct spdk_bdev_io *bdev_io)
{
    switch (bdev_io->type) {
        case SPDK_BDEV_IO_TYPE_READ:
        case SPDK_BDEV_IO_TYPE_WRITE:
            return bdev_io->u.bdev.offset_blocks;
        default:
            return 0;
    }
}

static uint64_t xcoder_get_io_num_blocks(struct spdk_bdev_io *bdev_io)
{
    switch (bdev_io->type) {
        case SPDK_BDEV_IO_TYPE_READ:
        case SPDK_BDEV_IO_TYPE_WRITE:
            return bdev_io->u.bdev.num_blocks;
        default:
            return 0;
    }
}

// --------------------------------------------------------------------------
// Bdev I/O Channel Callbacks (保持原样)
// --------------------------------------------------------------------------

static struct spdk_io_channel *xcoder_bdev_get_io_channel(void *ctx)
{
    struct xcoder_bdev *xb = ctx;
    if (!xb) {
        printf("Invalid context for get_io_channel\n");
        return NULL;
    }
    return spdk_get_io_channel(xb);
}

static int xcoder_bdev_io_channel_create(void *io_device, void *ctx_buf)
{
    (void)io_device;
    (void)ctx_buf;
    printf( "Xcoder bdev I/O channel created.\n");
    return 0;
}

static void xcoder_bdev_io_channel_destroy(void *io_device, void *ctx_buf)
{
    (void)io_device;
    (void)ctx_buf;
    printf( "Xcoder bdev I/O channel destroyed.\n");
}

// --------------------------------------------------------------------------
// Bdev I/O Request Processing (保持原样)
// --------------------------------------------------------------------------

static void xcoder_bdev_io_complete(struct spdk_bdev_io *bdev_io, int status)
{
    spdk_bdev_io_complete(bdev_io, status);
}

static void xcoder_get_buf_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io, bool success)
{
    (void)ch;
    
    if (!success) {
        xcoder_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
        return;
    }

    uint64_t lba = xcoder_get_io_lba(bdev_io);
    uint64_t num_blocks = xcoder_get_io_num_blocks(bdev_io);
    size_t io_byte_len = XCODER_LBA_TO_BYTES(num_blocks);

    // 计算该 LBA 属于哪个 Session 的起始位置
    uint64_t session_start_lba = (lba / XCODER_BLOCKS_PER_SESSION) * XCODER_BLOCKS_PER_SESSION;
    
    uint32_t session_id = GET_SESSION_ID_FROM_LBA(lba);
    uint32_t instance_id = 0; // 暂时只支持 Instance 0
    
    xcoder_session_t *session = xcoder_find_session(session_id);
    xcoder_instance_t *instance = NULL;

    if (session) {
        instance = xcoder_find_instance(session, instance_id);
    }
    
    // OS 扫描静默处理
    if (!session || !instance) {
        if (bdev_io->type == SPDK_BDEV_IO_TYPE_READ) {
            if (bdev_io->u.bdev.iovs) {
                 memset(bdev_io->u.bdev.iovs[0].iov_base, 0, io_byte_len);
            }
            xcoder_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
            return;
        } else {
            printf("[TGT ERROR] LBA %lu maps to Sess:%u. Not Found.\n", lba, session_id);
            xcoder_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
            return;
        }
    }
    
    printf("[TOP] Thread %lu: Attempting to lock instance mutex for instance %u.\n", pthread_self(), instance->instance_id);
    pthread_mutex_lock(&instance->mutex);
    printf("[TOP] Thread %lu: Instance mutex locked for instance %u.\n", pthread_self(), instance->instance_id);
    
    // 计算相对 LBA 偏移
    uint64_t lba_offset = lba - session_start_lba; 
    
    // Read 区域偏移处理
    if (lba_offset >= (16ULL * XCODER_BLOCKS_PER_SESSION)) {
        lba_offset -= (16ULL * XCODER_BLOCKS_PER_SESSION);
    }

    if (bdev_io->type == SPDK_BDEV_IO_TYPE_WRITE) {
        // --- Write 逻辑 ---
        
        // 🟢 根据实例类型判断是否带 metadata
        bool has_metadata = (instance->config.type == XCODER_CODEC_TYPE_E2E_VIDEO_DECODER);
        
        int rc = 0;
        
        // 1. 如果是新帧的开始（没有当前写入的entry）
        if (!instance->current_write_entry) {
            
            xcoder_get_current_time(&debug_input_received_start_timestamp);
            
            // 获取新的队列entry
            instance->current_write_entry = nuvcoder_queue_get_entry_and_prepare(
                instance->queue, instance, NULL);
            
            if (!instance->current_write_entry) {
                printf("Failed to get queue entry for new frame\n");
                pthread_mutex_unlock(&instance->mutex);
                xcoder_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
                return;
            }
            
            uint8_t *iov_base = (uint8_t *)bdev_io->u.bdev.iovs[0].iov_base;
            size_t iov_len = bdev_io->u.bdev.iovs[0].iov_len;
            
            if (has_metadata) {
                // 🟢 解码器：数据带 metadata，需要特殊处理
                
                // 解析第一个块的 metadata（在块的最后 64 字节）
                if (iov_len >= HOST_LBA_SIZE) {
                    uint8_t *meta_ptr = iov_base + HOST_LBA_SIZE - 64;  // 块末尾的 metadata
                    struct xcoder_io_metadata *md = (struct xcoder_io_metadata *)meta_ptr;
                    printf("Decoder metadata: total_size=%u, last_packet=%u\n",
                        md->total_size, md->last_packet);
                    
                    if (md->total_size > 0 && md->total_size <= XCODER_MAX_BYTES_PER_FRAME) {
                        instance->expected_frame_size = md->total_size;
                        printf("Updated trigger size from metadata: %u\n", md->total_size);
                    }
                }
                
                // 计算这个 Write 中的块数
                uint32_t num_blocks_this_io = iov_len / HOST_LBA_SIZE;
                const size_t USABLE_DATA_PER_BLOCK = HOST_LBA_SIZE - 64;  // 4032字节
                
                // 拷贝数据（跳过每个块末尾的 metadata）
                uint8_t *src_ptr = iov_base;
                uint8_t *dst_ptr = (uint8_t *)instance->current_write_entry->input_buffer_dma;
                
                for (uint32_t i = 0; i < num_blocks_this_io; i++) {
                    memcpy(dst_ptr + i * USABLE_DATA_PER_BLOCK, 
                           src_ptr + i * HOST_LBA_SIZE, 
                           USABLE_DATA_PER_BLOCK);
                }
                
                // 设置 input_length（只累加有效数据）
                instance->current_write_entry->input_length = num_blocks_this_io * USABLE_DATA_PER_BLOCK;
                
                printf("Decoder first write: entry %d, received %zu bytes data (%u blocks)\n", 
                    instance->current_write_entry->index, 
                    instance->current_write_entry->input_length,
                    num_blocks_this_io);
                
            } else {
                // 🟢 编码器：数据不带 metadata，直接拷贝
                
                // 计算这个 Write 中的块数
                uint32_t num_blocks_this_io = iov_len / HOST_LBA_SIZE;
                
                // 直接拷贝所有数据
                uint8_t *src_ptr = iov_base;
                uint8_t *dst_ptr = (uint8_t *)instance->current_write_entry->input_buffer_dma;
                
                memcpy(dst_ptr, src_ptr, iov_len);
                
                // 设置 input_length
                instance->current_write_entry->input_length = iov_len;
                
                printf("Encoder first write: entry %d, received %zu bytes (%u blocks)\n", 
                    instance->current_write_entry->index, 
                    iov_len,
                    num_blocks_this_io);
            }
            
            // 检查是否一帧就完成了
            uint64_t trigger_size = instance->expected_frame_size;
            if (trigger_size == 0) {
                fprintf(stderr, "ERROR: trigger_size is 0 for instance %u! Frame cannot be completed without valid size.\n", 
                        instance->instance_id);
                fprintf(stderr, "       This indicates a protocol error: Host must provide frame size via metadata or configuration.\n");
                
                // 释放当前 entry
                if (instance->current_write_entry) {
                    nuvcoder_queue_mark_completed(instance->queue, instance->current_write_entry, -1, 0);  
                    instance->current_write_entry = NULL;
                }
                
                pthread_mutex_unlock(&instance->mutex);
                xcoder_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
                return;
            }
            
            if (instance->current_write_entry->input_length >= trigger_size) {
                printf("\n[TGT] Frame Complete (Size: %lu) in first write. Actual received: %zu bytes\n", 
                       trigger_size, instance->current_write_entry->input_length);
                
                xcoder_get_current_time(&debug_input_received_end_timestamp);
                long long read_io_duration = xcoder_get_duration_us(&debug_input_received_start_timestamp,
                                                                    &debug_input_received_end_timestamp);
                fprintf(stdout, "TIMING: Frame took %lld us.\n", read_io_duration);

                rc = nuvcoder_send_frame(instance->current_write_entry);  
                
                if (rc == 0) {
                    nuvcoder_queue_notify_submit(instance->queue, instance->current_write_entry);  
                    printf("Frame submitted to Nuvcoder internal, entry %d\n", instance->current_write_entry->index);
                } else {
                    nuvcoder_queue_mark_completed(instance->queue, instance->current_write_entry, rc, 0);  
                    fprintf(stderr, "Error: Failed to submit frame via nuvcoder_send_frame for entry %d, rc=%d\n", 
                            instance->current_write_entry->index, rc);
                }
                
                // 重置当前写入状态
                instance->current_write_entry = NULL;
                instance->expected_frame_size = 0;
            }
            
            pthread_mutex_unlock(&instance->mutex);
            xcoder_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
            return;
        }
        
        // 2. 如果不是新帧开始（后续的 Write 命令）
        
        nuvcoder_queue_entry_t *entry = instance->current_write_entry;
        
        if (has_metadata) {
            // 🟢 解码器：带 metadata，需要特殊处理
            
            const size_t USABLE_DATA_PER_BLOCK = HOST_LBA_SIZE - 64;  // 4032字节
            uint64_t blocks_received = entry->input_length / USABLE_DATA_PER_BLOCK;
            
            // 验证 LBA 是否连续
            if (lba_offset != blocks_received) {
                printf("Decoder: Non-continuous write: expected LBA offset %lu, got %lu (data offset %zu)\n",
                    blocks_received, lba_offset, entry->input_length);
                pthread_mutex_unlock(&instance->mutex);
                xcoder_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
                return;
            }
            
            uint8_t *iov_base = (uint8_t *)bdev_io->u.bdev.iovs[0].iov_base;
            size_t iov_len = bdev_io->u.bdev.iovs[0].iov_len;
            
            uint32_t num_blocks_this_io = iov_len / HOST_LBA_SIZE;
            
            // 安全检查：防止溢出
            if (entry->input_length + num_blocks_this_io * USABLE_DATA_PER_BLOCK > XCODER_MAX_BYTES_PER_FRAME) {
                printf("Decoder write overflow: data offset %zu + %lu > MaxBuf %u\n", 
                    entry->input_length, num_blocks_this_io * USABLE_DATA_PER_BLOCK, XCODER_MAX_BYTES_PER_FRAME);
                pthread_mutex_unlock(&instance->mutex);
                xcoder_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
                return;
            }
            
            // 拷贝数据（跳过每个块末尾的 metadata）
            uint8_t *src_ptr = iov_base;
            uint8_t *dst_ptr = (uint8_t *)entry->input_buffer_dma + entry->input_length;
            
            for (uint32_t i = 0; i < num_blocks_this_io; i++) {
                memcpy(dst_ptr + i * USABLE_DATA_PER_BLOCK, 
                       src_ptr + i * HOST_LBA_SIZE, 
                       USABLE_DATA_PER_BLOCK);
            }
            
            // 更新 entry 的输入长度
            entry->input_length += num_blocks_this_io * USABLE_DATA_PER_BLOCK;
            
            printf("Decoder后续: LBA offset=%lu, data offset=%zu, blocks=%u, total_data=%zu\n",
                lba_offset, entry->input_length - num_blocks_this_io * USABLE_DATA_PER_BLOCK, 
                num_blocks_this_io, entry->input_length);
            
        } else {
            // 🟢 编码器：不带 metadata，直接处理
            
            uint64_t expected_lba_offset = entry->input_length / HOST_LBA_SIZE;
            
            // 验证 LBA 是否连续
            if (lba_offset != expected_lba_offset) {
                printf("Encoder: Non-continuous write: expected LBA offset %lu, got %lu (data offset %zu)\n",
                    expected_lba_offset, lba_offset, entry->input_length);
                pthread_mutex_unlock(&instance->mutex);
                xcoder_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
                return;
            }
            
            uint8_t *iov_base = (uint8_t *)bdev_io->u.bdev.iovs[0].iov_base;
            size_t iov_len = bdev_io->u.bdev.iovs[0].iov_len;
            
            // 安全检查：防止溢出
            if (entry->input_length + iov_len > XCODER_MAX_BYTES_PER_FRAME) {
                printf("Encoder write overflow: data offset %zu + %zu > MaxBuf %u\n", 
                    entry->input_length, iov_len, XCODER_MAX_BYTES_PER_FRAME);
                pthread_mutex_unlock(&instance->mutex);
                xcoder_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
                return;
            }
            
            // 直接拷贝数据
            uint8_t *src_ptr = iov_base;
            uint8_t *dst_ptr = (uint8_t *)entry->input_buffer_dma + entry->input_length;
            memcpy(dst_ptr, src_ptr, iov_len);
            
            // 更新 entry 的输入长度
            entry->input_length += iov_len;
            
            printf("Encoder后续: LBA offset=%lu, data offset=%zu, len=%zu, total_data=%zu\n",
                lba_offset, entry->input_length - iov_len, iov_len, entry->input_length);
        }
        
        // 3. 触发逻辑
        uint64_t trigger_size = instance->expected_frame_size;
        if (trigger_size == 0) {
            fprintf(stderr, "ERROR: trigger_size is 0 for instance %u! Cannot complete frame.\n", 
                    instance->instance_id);
            
            // 释放当前 entry
            if (entry) {
                nuvcoder_queue_mark_completed(instance->queue, entry, -1, 0);  
                instance->current_write_entry = NULL;
            }
            
            pthread_mutex_unlock(&instance->mutex);
            xcoder_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
            return;
        }
        
        // 判断帧是否完成
        if (entry->input_length >= trigger_size) {
            printf("\n[TGT] Frame Complete (Size: %lu). Actual received: %zu bytes\n", 
                   trigger_size, entry->input_length);
            
            xcoder_get_current_time(&debug_input_received_end_timestamp);
            long long read_io_duration = xcoder_get_duration_us(&debug_input_received_start_timestamp,
                                                                &debug_input_received_end_timestamp);
            fprintf(stdout, "TIMING: Frame took %lld us.\n", read_io_duration);

            rc = nuvcoder_send_frame(entry);  
            
            if (rc == 0) {
                nuvcoder_queue_notify_submit(instance->queue, entry);  
                printf("Frame submitted to Nuvcoder internal, entry %d\n", entry->index);
            } else {
                nuvcoder_queue_mark_completed(instance->queue, entry, rc, 0);  
                fprintf(stderr, "Error: Failed to submit frame via nuvcoder_send_frame for entry %d, rc=%d\n", 
                        entry->index, rc);
            }
            
            // 重置当前写入状态
            instance->current_write_entry = NULL;
            // instance->expected_frame_size = 0;
        }
        
        pthread_mutex_unlock(&instance->mutex);
        xcoder_bdev_io_complete(bdev_io, rc == 0 ? SPDK_BDEV_IO_STATUS_SUCCESS : SPDK_BDEV_IO_STATUS_FAILED);
        
    } else if (bdev_io->type == SPDK_BDEV_IO_TYPE_READ) {
        // --- Read 逻辑 ---
        // printf("[READ] Thread %lu: Attempting to lock instance mutex for instance %u.\n", pthread_self(), instance->instance_id);
        // pthread_mutex_lock(&instance->mutex);
        // printf("[READ] Thread %lu: Instance mutex locked for instance %u.\n", pthread_self(), instance->instance_id);
        nuvcoder_queue_entry_t *entry_to_read = NULL;
        size_t output_len_to_read = 0;
        int rc_read_status = SPDK_BDEV_IO_STATUS_FAILED;

        // 🎯 修改：使用指针访问
        pthread_mutex_lock(&instance->queue->lock);
        if (instance->queue->count > 0) {
            int tail_index = instance->queue->tail;
            nuvcoder_queue_entry_t *candidate_entry = &instance->queue->entries[tail_index];
            
            pthread_mutex_lock(&candidate_entry->mutex);
            if (candidate_entry->is_completed && candidate_entry->result_code == 0) {
                // 找到了一个已完成且成功的 entry
                entry_to_read = candidate_entry;
                output_len_to_read = entry_to_read->output_actual_length;
                rc_read_status = SPDK_BDEV_IO_STATUS_SUCCESS;
                printf("[READ] Instance %u: Found completed entry %d for read, output %zu bytes.\n", 
                       instance->instance_id, entry_to_read->index, output_len_to_read);
            } else if (candidate_entry->is_completed && candidate_entry->result_code != 0) {
                // entry 已完成但失败了
                fprintf(stderr, "[READ] Instance %u: Entry %d completed with error %d. Returning zeros.\n",
                        instance->instance_id, candidate_entry->index, candidate_entry->result_code);
                // 此时也应该释放这个失败的 entry
                nuvcoder_queue_release_entry(instance->queue, candidate_entry);  
            } else {
                // 队列尾部有 entry 但未完成，此时不能读取
                printf("[READ] Instance %u: Entry %d not yet completed. Returning zeros.\n", 
                       instance->instance_id, candidate_entry->index);
            }
            pthread_mutex_unlock(&candidate_entry->mutex);
        }
        pthread_mutex_unlock(&instance->queue->lock);  // 🎯 修改：使用指针

        // 如果没有可读取的已完成 entry，返回全 0
        if (!entry_to_read || output_len_to_read == 0 || rc_read_status != SPDK_BDEV_IO_STATUS_SUCCESS) {
            memset(bdev_io->u.bdev.iovs[0].iov_base, 0, io_byte_len);
            printf("[READ] Thread %lu: Attempting to unlock instance mutex for instance %u.\n", pthread_self(), instance->instance_id);
            pthread_mutex_unlock(&instance->mutex);
            printf("[READ] Thread %lu: Instance mutex unlocked for instance %u.\n", pthread_self(), instance->instance_id);
            xcoder_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS); // 即使失败也返回成功，填充0
            return;
        }
        
        // 此时 entry_to_read 是一个已完成且有数据的 entry
        uint64_t lba_offset = lba - session_start_lba; 
        if (lba_offset >= (16ULL * XCODER_BLOCKS_PER_SESSION)) {
            lba_offset -= (16ULL * XCODER_BLOCKS_PER_SESSION);
        }
        uint64_t byte_offset = XCODER_LBA_TO_BYTES(lba_offset);

        // 5. 计算可读取的数据并拷贝
        size_t available = 0;
        if (byte_offset < output_len_to_read) {
            available = output_len_to_read - byte_offset;
        }
        size_t to_copy = (io_byte_len < available) ? io_byte_len : available;
        
        if (to_copy > 0) {
            // 数据已经由 nuvcoder_codec_read_output 拷贝到 entry_to_read->output_buffer_dma
            memcpy(bdev_io->u.bdev.iovs[0].iov_base,
                (uint8_t *)entry_to_read->output_buffer_dma + byte_offset,
                to_copy);
        }
        
        // 填充剩余部分为0
        if (io_byte_len > to_copy) {
            memset((uint8_t *)bdev_io->u.bdev.iovs[0].iov_base + to_copy, 
                0, io_byte_len - to_copy);
        }
        
        // 6. 如果这次读取消费完了整个 output_len_to_read，则释放 entry
        //    这里需要考虑 Host 是以什么方式读取的。如果 Host 每次都从 LBA 0 开始读，
        //    或者只读取部分数据，那么简单地在 `byte_offset + io_byte_len >= output_len_to_read` 时释放
        //    可能会导致 Host 无法读取剩余部分。
        //    更安全的做法是：Host 发送一个特殊的 VSC 命令来显式释放，
        //    或者当 Host 再次 QUERY 发现 next_packet_size 为 0 时，Target 内部进行释放。
        //    目前先按“读完就释放”的简化逻辑：
        if (byte_offset + io_byte_len >= output_len_to_read) {
             printf("[READ] Instance %u: Releasing entry %d after full read (byte_offset=%lu, io_byte_len=%zu, output_len=%zu).\n", 
                    instance->instance_id, entry_to_read->index, byte_offset, io_byte_len, output_len_to_read);
            nuvcoder_queue_release_entry(instance->queue, entry_to_read);  
        }
        
        printf("[READ] Thread %lu: Attempting to unlock instance mutex for instance %u.\n", pthread_self(), instance->instance_id);
        pthread_mutex_unlock(&instance->mutex);
        printf("[READ] Thread %lu: Instance mutex unlocked for instance %u.\n", pthread_self(), instance->instance_id);
        xcoder_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
    } else {
        printf("[NONE] Thread %lu: Attempting to unlock instance mutex for instance %u.\n", pthread_self(), instance->instance_id);
        pthread_mutex_unlock(&instance->mutex);
        printf("[NONE] Thread %lu: Instance mutex unlocked for instance %u.\n", pthread_self(), instance->instance_id);
        xcoder_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
    }
}

static void xcoder_bdev_process_frame_async(void *arg)
{
    struct spdk_bdev_io *bdev_io = (struct spdk_bdev_io *)arg;
    
    if (bdev_io->type == SPDK_BDEV_IO_TYPE_WRITE) {
        // printf(" xcoder_bdev_process_frame_async SPDK_BDEV_IO_TYPE_WRITE \n");
        uint64_t num_blocks = xcoder_get_io_num_blocks(bdev_io);
        size_t io_byte_len = XCODER_LBA_TO_BYTES(num_blocks);
        spdk_bdev_io_get_buf(bdev_io, xcoder_get_buf_cb, io_byte_len);
    } else if (bdev_io->type == SPDK_BDEV_IO_TYPE_READ) {
        // printf(" xcoder_bdev_process_frame_async SPDK_BDEV_IO_TYPE_READ \n");
        xcoder_get_buf_cb(NULL, bdev_io, true);
    } else {
        printf("Unsupported I/O type: %d\n", bdev_io->type);
        xcoder_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
    }
}

static void xcoder_bdev_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
    (void)ch;
    struct xcoder_bdev *xb = (struct xcoder_bdev *)bdev_io->bdev->ctxt;
    spdk_thread_send_msg(xb->thread, xcoder_bdev_process_frame_async, bdev_io);
}

static bool xcoder_bdev_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
    (void)ctx;
    return (io_type == SPDK_BDEV_IO_TYPE_READ || io_type == SPDK_BDEV_IO_TYPE_WRITE);
}

// --------------------------------------------------------------------------
// Xcoder Virtual Bdev Internal Create Function
// --------------------------------------------------------------------------

static const struct spdk_bdev_fn_table xcoder_bdev_fn_table = {
    .destruct            = NULL,
    .submit_request      = xcoder_bdev_submit_request,
    .io_type_supported   = xcoder_bdev_io_type_supported,
    .get_io_channel      = xcoder_bdev_get_io_channel,
};

// 【改动点 3】: 将原来的 bdev_xcoder_init 改名为 internal_xcoder_bdev_create
// 并设为 static，专门供 module_init 调用
static int internal_xcoder_bdev_create(void)
{
    if (g_xcoder_bdev != NULL) {
        printf("Xcoder bdev already initialized.\n");
        return -EEXIST;
    }

    g_xcoder_bdev = calloc(1, sizeof(struct xcoder_bdev));
    if (!g_xcoder_bdev) {
        printf("Failed to allocate Xcoder bdev.\n");
        return -ENOMEM;
    }

    g_xcoder_bdev->bdev.name = strdup(BDEV_XCODER_NAME);
    g_xcoder_bdev->bdev.product_name = strdup(BDEV_XCODER_PRODUCT_NAME);
    if (!g_xcoder_bdev->bdev.name || !g_xcoder_bdev->bdev.product_name) {
        printf("Failed to allocate name strings.\n");
        free(g_xcoder_bdev->bdev.name);
        free(g_xcoder_bdev->bdev.product_name);
        free(g_xcoder_bdev);
        g_xcoder_bdev = NULL;
        return -ENOMEM;
    }

    g_xcoder_bdev->bdev.blocklen = XCODER_LBA_SIZE;
    g_xcoder_bdev->bdev.blockcnt = RD_LBA_START_BLOCK + (MAX_XCODER_SESSIONS * XCODER_SESSION_LBA_SPACE);

    // 设置 metadata
    g_xcoder_bdev->bdev.md_len = XCODER_MD_SIZE;           // metadata size 64 bytes
    g_xcoder_bdev->bdev.md_interleave = true;               // metadata interleaved with data

    g_xcoder_bdev->bdev.module = &g_xcoder_module;
    g_xcoder_bdev->bdev.fn_table = &xcoder_bdev_fn_table;
    g_xcoder_bdev->bdev.ctxt = g_xcoder_bdev;
    g_xcoder_bdev->thread = spdk_get_thread();
    g_xcoder_bdev->bdev.io_type_supported = (1 << SPDK_BDEV_IO_TYPE_READ) | (1 << SPDK_BDEV_IO_TYPE_WRITE);

    // 必须先注册 I/O 设备
    spdk_io_device_register(g_xcoder_bdev, xcoder_bdev_io_channel_create,
                           xcoder_bdev_io_channel_destroy,
                           sizeof(struct xcoder_io_channel),
                           "bdev_XcoderBdev");

    printf( "Registering Xcoder bdev: %s, total blocks %ju, block size %u.\n",
                 g_xcoder_bdev->bdev.name, g_xcoder_bdev->bdev.blockcnt, g_xcoder_bdev->bdev.blocklen);

    // 再注册 bdev
    int rc = spdk_bdev_register(&g_xcoder_bdev->bdev);
    if (rc != 0) {
        printf("Failed to register Xcoder bdev: %s\n", spdk_strerror(-rc));
        spdk_io_device_unregister(g_xcoder_bdev, NULL);
        free(g_xcoder_bdev->bdev.name);
        free(g_xcoder_bdev->bdev.product_name);
        free(g_xcoder_bdev);
        g_xcoder_bdev = NULL;
        return rc;
    }

    printf( "Xcoder bdev registered successfully.\n");
    return 0;
}

// 【改动点 4】: 保留原来的公共接口，避免 breakdown compilation
// 但把它变成一个空函数，因为初始化已经在 module_init 做完了。
// 如果你的 main.c 里调用了这个函数，它将不做任何事直接返回，这就安全了。
int bdev_xcoder_init(void)
{
    // 如果想要更严谨，可以判断是否已经初始化
    if (g_xcoder_bdev != NULL) {
        return 0; // Already initialized in module_init, success.
    }
    // 理论上不会走到这，除非 module_init 失败了。
    return internal_xcoder_bdev_create(); 
}

void bdev_xcoder_fini(void)
{
    printf( "Xcoder bdev finalized (stub).\n");
}