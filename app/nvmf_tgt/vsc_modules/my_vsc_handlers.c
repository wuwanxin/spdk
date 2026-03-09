#include "spdk/stdinc.h"
#include "spdk/nvme_spec.h"
#include "spdk/nvmf_cmd.h"
#include "spdk/log.h"
#include "spdk/util.h"
#include "spdk/string.h"
#include "spdk/env.h" // For spdk_zmalloc/spdk_free
#include "spdk/nvmf_transport.h" // For struct spdk_nvmf_request's iov and length
#include "nuvcoder_queue.h"
#include "nuvcoder_vsc_opcodes.h" // 修改为您的 VSC opcodes 头文件
#include "nuvcoder_types.h"       // 修改为您的 types 头文件
#include "nuvcoder_codec_api.h"   // !!! 新增：引入 Nuvcoder Codec API 头文件 !!!
#include "nuvcoder_poller.h"

// 全局 Xcoder 状态实例
xcoder_global_state_t g_xcoder_state = { .next_session_id = 0 };

/**
 * @brief 设置NVMe命令完成状态（Target端使用）
 * @param req SPDK请求结构体
 * @param sct 状态码类型（Status Code Type）
 * @param sc 状态码（Status Code）
 * @param cdw0 命令特定结果（可选，默认0）
 * @param cid 命令ID（可选，默认使用请求中的cid）
 */
#if 1
static void set_nvme_completion_status(struct spdk_nvmf_request *req,
                                      uint8_t sct, uint8_t sc,
                                      uint32_t cdw0, uint16_t cid) {
    struct spdk_nvme_cpl *cpl = &req->rsp->nvme_cpl;
    
    // 1. 设置命令特定结果（cdw0）
    cpl->cdw0 = cdw0;
    
    // 2. 设置命令ID（可选）
    if (cid != 0) {
        cpl->cid = cid;
    } else {
        // 默认使用请求中的命令ID
        cpl->cid = req->cmd->nvme_cmd.cid;
    }
    
    // 3. 设置状态字段
    cpl->status.sct = sct;
    cpl->status.sc = sc;
    
    // 4. 可选：设置其他状态位
    cpl->status.p = 0;     // Phase Tag (通常为0)
    cpl->status.crd = 0;   // Command Retry Delay (通常为0)
    cpl->status.m = 0;     // More (通常为0)
    cpl->status.dnr = 0;   // Do Not Retry (通常为0)
    
    // 5. 调试信息
    fprintf(stderr, "DEBUG [set_nvme_completion_status]:\n");
    fprintf(stderr, "  Setting NVMe completion status:\n");
    fprintf(stderr, "    cdw0 = 0x%08x (%u)\n", cpl->cdw0, cpl->cdw0);
    fprintf(stderr, "    cid  = 0x%04x\n", cpl->cid);
    fprintf(stderr, "    sct  = %u, sc = %u (0x%02x)\n", 
            cpl->status.sct, cpl->status.sc, cpl->status.sc);
    fprintf(stderr, "    status_raw = 0x%04x\n", cpl->status_raw);
    
    // 6. 打印原始内存内容用于调试
    fprintf(stderr, "  Memory dump of spdk_nvme_cpl (16 bytes):\n");
    uint8_t *cpl_bytes = (uint8_t *)cpl;
    for (int i = 0; i < 16; i++) {
        if (i % 8 == 0) fprintf(stderr, "    ");
        fprintf(stderr, "%02x ", cpl_bytes[i]);
        if (i % 8 == 7) fprintf(stderr, "\n");
    }
}
#else
static void set_nvme_completion_status(struct spdk_nvmf_request *req,
                                      uint8_t sct_val, uint8_t sc_val, // 参数名称改为 sct_val, sc_val 避免与位域成员混淆
                                      uint32_t cdw0, uint16_t cid) {
    struct spdk_nvme_cpl *cpl = &req->rsp->nvme_cpl;
    
    // 1. 设置命令特定结果（cdw0）
    cpl->cdw0 = cdw0;
    
    // 2. 设置命令ID（可选）
    if (cid != 0) {
        cpl->cid = cid;
    } else {
        // 默认使用请求中的命令ID
        cpl->cid = req->cmd->nvme_cmd.cid;
    }
    
    // 3. 直接通过位操作构造 status_raw
    uint16_t new_status_raw = 0;
    
    // NVMe 规范定义的位域布局：
    // SC (Status Code): Bit 0-7
    // SCT (Status Code Type): Bit 8-10
    // CRD (Command Retry Delay): Bit 11-12
    // M (More): Bit 13
    // DNR (Do Not Retry): Bit 14
    // P (Phase Tag): Bit 15

    new_status_raw |= (sc_val & 0xFF);             // SC (Bit 0-7)
    new_status_raw |= ((sct_val & 0x07) << 8);     // SCT (Bit 8-10)
    // 其他字段默认为 0，如果需要设置，则在此处添加
    // 例如：
    // new_status_raw |= ((0 & 0x03) << 11); // CRD (Bit 11-12)
    // new_status_raw |= ((0 & 0x01) << 13); // M (Bit 13)
    // new_status_raw |= ((0 & 0x01) << 14); // DNR (Bit 14)
    // new_status_raw |= ((0 & 0x01) << 15); // P (Bit 15)

    cpl->status_raw = new_status_raw; // 直接设置联合体中的 status_raw
    
    // 4. 调试信息
    fprintf(stderr, "DEBUG [set_nvme_completion_status]:\n");
    fprintf(stderr, "  Setting NVMe completion status:\n");
    fprintf(stderr, "    cdw0 = 0x%08x (%u)\n", cpl->cdw0, cpl->cdw0);
    fprintf(stderr, "    cid  = 0x%04x\n", cpl->cid);
    // 注意：这里的 cpl->status.sct 和 cpl->status.sc 现在是 Host 编译器对 cpl->status_raw 的解释
    // 如果 Host 端的位域布局与 NVMe 规范不一致，这里打印的值可能与输入参数 sct_val, sc_val 不同
    fprintf(stderr, "    Input sct_val = %u, sc_val = %u\n", sct_val, sc_val);
    fprintf(stderr, "    cpl->status.sct (parsed) = %u, cpl->status.sc (parsed) = %u\n",
            cpl->status.sct, cpl->status.sc);
    fprintf(stderr, "    status_raw (set directly) = 0x%04x\n", cpl->status_raw);
    
    // 5. 打印原始内存内容用于调试
    fprintf(stderr, "  Memory dump of spdk_nvme_cpl (16 bytes):\n");
    uint8_t *cpl_bytes = (uint8_t *)cpl;
    for (int i = 0; i < 16; i++) {
        if (i % 8 == 0) fprintf(stderr, "    ");
        fprintf(stderr, "%02x ", cpl_bytes[i]);
        if (i % 8 == 7) fprintf(stderr, "\n");
    }
}
#endif
// --------------------------------------------------------------------------------------
// 辅助函数：从连续缓冲区拷贝数据到请求的 iov 列表 (C2H - Controller to Host)
// --------------------------------------------------------------------------------------
static int
_copy_to_request_iov(struct spdk_nvmf_request *req, const void *src_buf, uint32_t src_len)
{
    uint32_t bytes_remaining_to_copy = src_len;
    uint32_t current_src_offset = 0;
    uint32_t iov_idx = 0;

    if (src_len == 0) {
        return 0;
    }

    if (req->length < src_len) {
        printf( "Request data length (%u) is too small to fit source data (%u).\n", req->length, src_len);
        return -1;
    }

    while (bytes_remaining_to_copy > 0 && iov_idx < req->iovcnt) {
        size_t len_in_iov = req->iov[iov_idx].iov_len;
        void *iov_base = req->iov[iov_idx].iov_base;
        size_t copy_len = spdk_min((size_t)bytes_remaining_to_copy, len_in_iov);

        memcpy(iov_base, (const char *)src_buf + current_src_offset, copy_len);

        current_src_offset += copy_len;
        bytes_remaining_to_copy -= copy_len;
        iov_idx++;
    }

    if (bytes_remaining_to_copy > 0) {
        printf( "Failed to copy all data to request iov. Remaining %u bytes.\n", bytes_remaining_to_copy);
        return -1;
    }

    // printf( "Copied %u bytes to request iov successfully.\n", src_len);
    return 0;
}

// --------------------------------------------------------------------------------------
// 辅助函数：从请求的 iov 列表拷贝数据到连续缓冲区 (H2C - Host to Controller)
// --------------------------------------------------------------------------------------
static int
_copy_from_request_iov(struct spdk_nvmf_request *req, void *dst_buf, uint32_t dst_len)
{
    uint32_t bytes_remaining_to_copy = dst_len;
    uint32_t current_dst_offset = 0;
    uint32_t iov_idx = 0;

    if (dst_len == 0) {
        return 0;
    }

    if (req->length < dst_len) {
        printf( "Request data length (%u) is too small to provide expected data (%u).\n", req->length, dst_len);
        return -1;
    }

    while (bytes_remaining_to_copy > 0 && iov_idx < req->iovcnt) {
        size_t len_in_iov = req->iov[iov_idx].iov_len;
        void *iov_base = req->iov[iov_idx].iov_base;
        size_t copy_len = spdk_min((size_t)bytes_remaining_to_copy, len_in_iov);

        memcpy((char *)dst_buf + current_dst_offset, iov_base, copy_len);

        current_dst_offset += copy_len;
        bytes_remaining_to_copy -= copy_len;
        iov_idx++;
    }

    if (bytes_remaining_to_copy > 0) {
        printf( "Failed to copy all data from request iov. Remaining %u bytes.\n", bytes_remaining_to_copy);
        return -1;
    }

    printf( "Copied %u bytes from request iov successfully.\n", dst_len);
    return 0;
}


void xcoder_global_state_init(void) {
    pthread_mutex_init(&g_xcoder_state.mutex, NULL);
    for (int i = 0; i < MAX_XCODER_SESSIONS; i++) {
        g_xcoder_state.sessions[i] = NULL;
    }
    printf( "Xcoder global state initialized.\n");
}

xcoder_session_t* xcoder_find_session(uint32_t session_id) {
    pthread_mutex_lock(&g_xcoder_state.mutex);
    if (session_id < MAX_XCODER_SESSIONS && g_xcoder_state.sessions[session_id] &&
        g_xcoder_state.sessions[session_id]->is_active) {
        xcoder_session_t *session = g_xcoder_state.sessions[session_id];
        pthread_mutex_unlock(&g_xcoder_state.mutex);
        return session;
    }
    pthread_mutex_unlock(&g_xcoder_state.mutex);
    return NULL;
}

xcoder_instance_t* xcoder_find_instance(xcoder_session_t *session, uint32_t instance_id) {
    if (!session) return NULL;
    // 注意：xcoder_find_instance 内部不应该再 lock session->mutex，因为 xcoder_find_session 已经 lock 了 g_xcoder_state.mutex
    // 并且调用 xcoder_find_instance 的函数 (例如 config/close) 会在自己的逻辑中 lock instance->mutex
    if (instance_id < MAX_INSTANCES_PER_SESSION && session->instances[instance_id] &&
        session->instances[instance_id]->is_active) {
        xcoder_instance_t *instance = session->instances[instance_id];
        return instance;
    }
    return NULL;
}


/**
 * @brief 发送一帧数据到Nuvcoder进行异步处理
 *        只提交输入，不等待输出
 *
 * @param entry 队列条目，包含输入数据和状态
 * @return 0成功，-1失败
 */
int nuvcoder_send_frame(nuvcoder_queue_entry_t *entry) {
    if (!entry || !entry->parent_instance) {
        fprintf(stderr, "Error: nuvcoder_send_frame called with invalid queue entry.\n");
        return -1;
    }

    xcoder_instance_t *instance = entry->parent_instance;

    if (!instance || !instance->nuvcoder_handle) {
        fprintf(stderr, "Error: Instance or nuvcoder_handle not initialized.\n");
        return -1;
    }

    printf("Sending frame for instance %u (type %d), entry %d, input length %zu\n",
           instance->instance_id, instance->config.type, entry->index, entry->input_length);

    int rc = 0;
    size_t submit_len = entry->input_length;
    
    // 根据实例类型区分处理
    if (instance->config.type == XCODER_CODEC_TYPE_E2E_VIDEO_DECODER) {
        // ------------------------------------------------------------
        // 解码器分支：输入是码流，输出是重建帧
        // ------------------------------------------------------------
        
        // 检查输入数据是否有效
        if (entry->input_length == 0 || entry->input_length > XCODER_MAX_BITSTREAM_SIZE) {
            fprintf(stderr, "Error: Invalid input length %zu for decoder\n", entry->input_length);
            return -1;
        }
        
        // 对于解码器，输出缓冲区大小在 queue_init 时已经分配好了
        // 不需要再分配，直接使用已有的 output_buffer_dma
        
        // 提交码流到解码器（异步）
        bool is_dma_memory = true; // 假设输入输出都是DMA内存
        rc = nuvcoder_decoder_submit_bitstream(
            instance->nuvcoder_handle,           // 解码器句柄
            entry->input_buffer_dma,              // 输入码流缓冲区
            entry->input_length,                   // 输入码流长度
            entry->output_buffer_dma,              // 输出YUV缓冲区（已在queue_init时分配）
            XCODER_MAX_BYTES_PER_FRAME,            // 输出缓冲区大小（使用宏定义）
            is_dma_memory,                          // 是否DMA内存
            (void *)entry                           // 用户上下文
        );
        
        if (rc != NUVCODER_STATUS_SUCCESS) {
            fprintf(stderr, "Error: Failed to submit bitstream to decoder, rc = %d.\n", rc);
            nuvcoder_queue_mark_completed(instance->queue, entry, rc, 0);
            return -1;
        }
        
        printf("Bitstream submitted to decoder for instance %u, entry %d, size %zu\n",
               instance->instance_id, entry->index, submit_len);
               
    } else if (instance->config.type == XCODER_CODEC_TYPE_E2E_VIDEO_ENCODER) {
        // ------------------------------------------------------------
        // 编码器分支：输入是原始帧，输出是码流（原有逻辑）
        // ------------------------------------------------------------
        
        // 根据输入格式调整提交的数据长度
        switch (instance->config.input_format) {
            case XCODER_FRAME_FORMAT_YUV420P:
            case XCODER_FRAME_FORMAT_NV12:
                submit_len = (size_t)instance->config.width * instance->config.height * 3 / 2;
                break;
            case XCODER_FRAME_FORMAT_RGB:
                submit_len = (size_t)instance->config.width * instance->config.height * 3;
                break;
            case XCODER_FRAME_FORMAT_RGBA:
                submit_len = (size_t)instance->config.width * instance->config.height * 4;
                break;
            default:
                fprintf(stderr, "Error: Unsupported input format %u\n", instance->config.input_format);
                return -1;
        }

        // 检查输入数据是否足够
        if (entry->input_length < submit_len) {
            fprintf(stderr, "Error: Input data too small: %zu < %zu\n", entry->input_length, submit_len);
            return -1;
        }

        // 提交输入帧（异步）
        bool is_dma_memory = true;
        rc = nuvcoder_codec_submit_frame(
            instance->nuvcoder_handle,
            entry->input_buffer_dma,
            (uint32_t)submit_len,
            entry->output_buffer_dma,
            (uint32_t)XCODER_MAX_BYTES_PER_FRAME,
            is_dma_memory,
            (void *)entry             
        );
    }

    // ========== 统一的后续处理 ==========
    if (rc != 0) {
        fprintf(stderr, "Error: Failed to submit frame to Nuvcoder Codec, rc = %d.\n", rc);
        nuvcoder_queue_mark_completed(instance->queue, entry, rc, 0);
        return -1;
    }

    printf("Frame submitted for instance %u, entry %d, size %zu\n",
           instance->instance_id, entry->index, submit_len);

    nuvcoder_queue_notify_submit(instance->queue, entry);
    instance->processed_frames++;

    return 0;
}


// xcoder_get_bitstream_nonblocking 
xcoder_bitstream_status_t xcoder_get_bitstream_nonblocking(nuvcoder_queue_entry_t *entry, 
                                                           size_t *out_actual_length, 
                                                           void **out_buffer_dma) {
    if (!entry || !entry->parent_instance) {
        fprintf(stderr, "Error: xcoder_get_bitstream_nonblocking called with invalid queue entry.\n");
        return XCODER_BITSTREAM_STATUS_INVALID_ARGS;
    }

    xcoder_instance_t *instance = entry->parent_instance;

    // 锁定 entry 的 mutex 以安全检查其状态
    pthread_mutex_lock(&entry->mutex);

    if (!entry->is_completed) {
        pthread_mutex_unlock(&entry->mutex);
        return XCODER_BITSTREAM_STATUS_NOT_READY;
    }

    if (entry->result_code != 0) {
        fprintf(stderr, "Entry %d (instance %u) completed with error %d.\n",
                entry->index, instance->instance_id, entry->result_code);
        pthread_mutex_unlock(&entry->mutex);
        return XCODER_BITSTREAM_STATUS_FAILED;
    }

    // 完成且成功
    if (out_actual_length) {
        *out_actual_length = entry->output_actual_length;
    }
    if (out_buffer_dma) {
        *out_buffer_dma = entry->output_buffer_dma;
    }

    // 打印调试信息，区分编码器和解码器
    if (instance->config.type == XCODER_CODEC_TYPE_E2E_VIDEO_DECODER) {
        printf("Decoder instance %u: Entry %d completed successfully, output YUV size %zu bytes\n",
               instance->instance_id, entry->index, entry->output_actual_length);
    } else {
        printf("Encoder instance %u: Entry %d completed successfully, output bitstream size %zu bytes\n",
               instance->instance_id, entry->index, entry->output_actual_length);
    }

    pthread_mutex_unlock(&entry->mutex);
    return XCODER_BITSTREAM_STATUS_SUCCESS;
}

uint32_t nuvcoder_calculate_frame_size(uint32_t width, uint32_t height, nuvcoder_frame_format_t format) {
    uint32_t size = 0;
    
    switch (format) {
        case NUVCODER_FRAME_FORMAT_RGB:
        case NUVCODER_FRAME_FORMAT_BGR:
            size = width * height * 3;
            break;
        case NUVCODER_FRAME_FORMAT_YUV420P:
        case NUVCODER_FRAME_FORMAT_NV12:
            // Y分量：width * height
            // UV分量：width * height / 2
            size = width * height * 3 / 2;
            break;
        case NUVCODER_FRAME_FORMAT_YUV444P:
            // YUV各平面大小相同
            size = width * height * 3;
            break;
        case NUVCODER_FRAME_FORMAT_GRAY:
            size = width * height;
            break;
        default:
            fprintf(stderr, "Error: Unknown frame format %u\n", format);
            break;
    }
    
    return size;
}


// SPDK_NVME_SC_SUCCESS, SPDK_NVME_SC_INTERNAL_DEVICE_ERROR, etc.
// SPDK_NVME_SCT_GENERIC
// xcoder_session_t, xcoder_instance_t, xcoder_find_session, xcoder_find_instance, etc.
// nuvcoder_codec_create, nuvcoder_codec_reset, nuvcoder_calculate_frame_size, etc.
// _copy_from_request_iov, ALIGN_UP, HOST_LBA_SIZE, XCODER_MAX_BYTES_PER_FRAME

// Helper function to print hex dump of a memory region
void print_hex_dump(const char *label, const void *addr, size_t len) {
    const uint8_t *data = (const uint8_t *)addr;
    size_t i;
    fprintf(stderr, "DEBUG: %s (addr: %p, len: %zu):\n", label, addr, len);
    for (i = 0; i < len; i++) {
        if (i % 16 == 0) {
            fprintf(stderr, "  %04zx: ", i);
        }
        fprintf(stderr, "%02x ", data[i]);
        if (i % 16 == 15 || i == len - 1) {
            fprintf(stderr, "\n");
        }
    }
}

// --------------------------------------------------------------------------
// VSC Admin Command Handlers
// --------------------------------------------------------------------------

// VSC_XCODER_OPEN 处理函数
// VSC_XCODER_OPEN 处理函数
// --------------------------------------------------------------------------
// VSC Admin Command Handlers
// --------------------------------------------------------------------------

// VSC_XCODER_OPEN 处理函数
int xcoder_vsc_open_hdlr(struct spdk_nvmf_request *req) {
    struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
    uint32_t cdw10 = le32toh(cmd->cdw10);
    uint32_t cdw11 = le32toh(cmd->cdw11);
    uint32_t cdw12 = le32toh(cmd->cdw12);

    enum xcoder_open_subtype subtype = VSC_XCODER_OPEN_GET_SUBTYPE(cdw10);
    uint32_t session_id_param = VSC_XCODER_OPEN_GET_SESSION_ID_HINT(cdw10);

    printf("VSC_XCODER_OPEN_OPCODE received. Subtype: %u, Session ID Param: %u\n", subtype, session_id_param);

    uint8_t sct = SPDK_NVME_SCT_GENERIC;
    uint8_t sc = SPDK_NVME_SC_SUCCESS;

    if (subtype == XCODER_OPEN_CREATE_SESSION) {
        pthread_mutex_lock(&g_xcoder_state.mutex);
        uint32_t new_session_id = (uint32_t)-1;

        // 查找可用的 session_id
        bool found_slot = false;
        for (int i = 0; i < MAX_XCODER_SESSIONS; i++) {
            if (g_xcoder_state.sessions[i] == NULL || !g_xcoder_state.sessions[i]->is_active) {
                new_session_id = i;
                found_slot = true;
                break;
            }
        }

        if (!found_slot) {
            printf("Failed to create session: Max sessions reached (%u).\n", MAX_XCODER_SESSIONS);
            pthread_mutex_unlock(&g_xcoder_state.mutex);
            sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
            goto complete_request;
        }

        xcoder_session_t *session = spdk_zmalloc(sizeof(xcoder_session_t), 0, NULL, 
                                                SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);
        if (!session) {
            printf("Failed to allocate session memory.\n");
            pthread_mutex_unlock(&g_xcoder_state.mutex);
            sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
            goto complete_request;
        }

        session->session_id = new_session_id;
        session->is_active = true;
        session->num_instances = 0;
        pthread_mutex_init(&session->mutex, NULL);
        // 初始化 instances 数组
        for(int i = 0; i < MAX_INSTANCES_PER_SESSION; i++) {
            session->instances[i] = NULL;
        }
        g_xcoder_state.sessions[new_session_id] = session;

        printf("Session %u created.\n", new_session_id);
        pthread_mutex_unlock(&g_xcoder_state.mutex);

        set_nvme_completion_status(req, 
                                  SPDK_NVME_SCT_GENERIC,
                                  SPDK_NVME_SC_SUCCESS,
                                  new_session_id,
                                  req->cmd->nvme_cmd.cid);
        return 0;

    } else if (subtype == XCODER_OPEN_ADD_INSTANCE) {
        uint32_t session_id = session_id_param;
        xcoder_session_t *session = xcoder_find_session(session_id);

        if (!session) {
            printf("Failed to add instance: Session %u not found or inactive.\n", session_id);
            sc = SPDK_NVME_SC_INVALID_FIELD;
            goto complete_request;
        }

        pthread_mutex_lock(&session->mutex);
        uint32_t new_instance_id = (uint32_t)-1;
        bool found_slot = false;
        for (int i = 0; i < MAX_INSTANCES_PER_SESSION; i++) {
            if (session->instances[i] == NULL || !session->instances[i]->is_active) {
                new_instance_id = i;
                found_slot = true;
                break;
            }
        }

        if (!found_slot) {
            printf("Failed to add instance: Max instances reached for session %u (%u).\n", 
                   session_id, MAX_INSTANCES_PER_SESSION);
            pthread_mutex_unlock(&session->mutex);
            sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
            goto complete_request;
        }

        xcoder_instance_t *instance = spdk_zmalloc(sizeof(xcoder_instance_t), 0, NULL,
                                                  SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);
        if (!instance) {
            printf("Failed to allocate instance memory.\n");
            pthread_mutex_unlock(&session->mutex);
            sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
            goto complete_request;
        }

        // --- 初始化 xcoder_instance_t 的字段 ---
        memset(instance, 0, sizeof(xcoder_instance_t));
        instance->instance_id = new_instance_id;
        instance->is_active = true;
        instance->config.type = cdw12 & 0xFF;
        instance->config.width = 0;
        instance->config.height = 0;
        instance->config.fps = 0;
        instance->config.bitrate_kbps = 0;
        instance->config.input_format = XCODER_FRAME_FORMAT_NONE;
        instance->config.gop_size = 0;

        instance->expected_frame_size = 0;
        instance->nuvcoder_handle = NULL;     // 尚未创建

        // !!! 关键修改：不再初始化实例中的队列，改为使用池 !!!
        // 队列将在需要时从池中获取
        instance->queue = NULL;  // 初始化为 NULL
        
        pthread_mutex_init(&instance->mutex, NULL);

        session->instances[new_instance_id] = instance;
        session->num_instances++;

        printf("Instance %u (type %u) added to session %u. Awaiting CONFIG command.\n", 
               new_instance_id, instance->config.type, session_id);
        pthread_mutex_unlock(&session->mutex);

        set_nvme_completion_status(req,
                                  SPDK_NVME_SCT_GENERIC,
                                  SPDK_NVME_SC_SUCCESS,
                                  new_instance_id,
                                  req->cmd->nvme_cmd.cid);
        return 0;

    } else {
        printf("Invalid VSC_XCODER_OPEN subtype: %u\n", subtype);
        sc = SPDK_NVME_SC_INVALID_FIELD;
        goto complete_request;
    }

complete_request:
    set_nvme_completion_status(req, sct, sc, 0, req->cmd->nvme_cmd.cid);
    return 0;
}

// VSC_XCODER_CLOSE 处理函数 - 需要修改以释放队列
int xcoder_vsc_close_hdlr(struct spdk_nvmf_request *req) {
    struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
    uint32_t cdw10 = le32toh(cmd->cdw10);
    uint32_t cdw11 = le32toh(cmd->cdw11);

    enum xcoder_close_subtype subtype = VSC_XCODER_CLOSE_GET_SUBTYPE(cdw10);
    uint32_t session_id = VSC_XCODER_CONFIG_GET_SESSION_ID(cdw10);

    printf("VSC_XCODER_CLOSE received. Subtype: %u, Session ID: %u\n", subtype, session_id);

    uint8_t sct = SPDK_NVME_SCT_GENERIC;
    uint8_t sc = SPDK_NVME_SC_SUCCESS;

    xcoder_session_t *session = xcoder_find_session(session_id);
    if (!session) {
        printf("Failed to close: Session %u not found or inactive.\n", session_id);
        sc = SPDK_NVME_SC_INVALID_FIELD;
        goto complete_request;
    }

    if (subtype == XCODER_CLOSE_DESTROY_SESSION) {
        pthread_mutex_lock(&session->mutex);
        for (int i = 0; i < MAX_INSTANCES_PER_SESSION; i++) {
            if (session->instances[i]) {
                // 1. 停止 Poller 线程
                if (session->instances[i]->poller_active && session->instances[i]->poller_args) {
                    SPDK_NOTICELOG("Stopping Nuvcoder Output Poller thread for instance %u during session destroy.\n", 
                                session->instances[i]->instance_id);
                    session->instances[i]->poller_args->stop_flag = true;
                    pthread_join(session->instances[i]->poller_args->thread_id, NULL);
                    spdk_free(session->instances[i]->poller_args);
                    session->instances[i]->poller_args = NULL;
                    session->instances[i]->poller_active = false;
                    SPDK_NOTICELOG("Nuvcoder Output Poller thread for instance %u stopped.\n", 
                                session->instances[i]->instance_id);
                }
                
                // 2. 根据实例类型销毁 nuvcoder_handle
                if (session->instances[i]->nuvcoder_handle) {
                    if (session->instances[i]->config.type == XCODER_CODEC_TYPE_E2E_VIDEO_DECODER) {
                        // 解码器：先 flush 再 destroy
                        SPDK_NOTICELOG("Flushing decoder instance %u during session destroy.\n", 
                                    session->instances[i]->instance_id);
                        nuvcoder_decoder_flush(session->instances[i]->nuvcoder_handle);
                        
                        SPDK_NOTICELOG("Destroying decoder instance %u during session destroy.\n", 
                                    session->instances[i]->instance_id);
                        nuvcoder_decoder_reset(session->instances[i]->nuvcoder_handle);
                    } else {
                        // 编码器或其他：直接用 codec_destroy
                        SPDK_NOTICELOG("Destroying codec instance %u during session destroy.\n", 
                                    session->instances[i]->instance_id);
                        nuvcoder_codec_reset(session->instances[i]->nuvcoder_handle);
                    }
                    session->instances[i]->nuvcoder_handle = NULL;
                }
                
                // 3. 重置队列（释放 handler 回池）
                if (session->instances[i]->queue) {
                    nuvcoder_queue_pool_reset_handler(session->instances[i]->queue, 
                                                      session->instances[i]->instance_id);
                    session->instances[i]->queue = NULL;
                }
                
                // 4. 释放实例内存
                pthread_mutex_destroy(&session->instances[i]->mutex);
                spdk_free(session->instances[i]);
                session->instances[i] = NULL;
            }
        }
            session->is_active = false;
            session->num_instances = 0;
            pthread_mutex_unlock(&session->mutex);
            pthread_mutex_destroy(&session->mutex);
            pthread_mutex_lock(&g_xcoder_state.mutex);
            g_xcoder_state.sessions[session_id] = NULL;
            pthread_mutex_unlock(&g_xcoder_state.mutex);
            spdk_free(session);
            printf("Session %u destroyed.\n", session_id);
            sc = SPDK_NVME_SC_SUCCESS;
            goto complete_request;
    } else if (subtype == XCODER_CLOSE_REMOVE_INSTANCE) {
        uint32_t instance_id = cdw11 & 0xFF;
        xcoder_instance_t *instance = xcoder_find_instance(session, instance_id);
        if (!instance) {
            printf("Failed to remove instance: Instance %u not found or inactive in session %u.\n", 
                instance_id, session_id);
            sc = SPDK_NVME_SC_INVALID_FIELD;
            goto complete_request;
        }

        {
            printf("===== CLOSE DEBUG =====\n");
            printf("Instance ID: %u\n", instance->instance_id);
            printf("Instance type: %d\n", instance->config.type);
            printf("Is decoder? %s\n", 
                   (instance->config.type == XCODER_CODEC_TYPE_E2E_VIDEO_DECODER) ? "YES" : "NO");
            printf("nuvcoder_handle: %p\n", instance->nuvcoder_handle);
            printf("poller_active: %d\n", instance->poller_active);
            printf("poller_args: %p\n", instance->poller_args);
            printf("========================\n");
        }

        pthread_mutex_lock(&session->mutex);
        printf("[VSC CLOSE] Thread %lu: Attempting to lock instance mutex for instance %u.\n", pthread_self(), instance->instance_id);
        pthread_mutex_lock(&instance->mutex);
        printf("[VSC CLOSE] Thread %lu: Instance mutex locked for instance %u.\n", pthread_self(), instance->instance_id);
        
        // 1. 停止 Poller 线程（所有类型都需要）
        if (instance->poller_active && instance->poller_args) {
            SPDK_NOTICELOG("Stopping Nuvcoder Output Poller thread for instance %u during instance removal.\n", instance->instance_id);
            instance->poller_args->stop_flag = true;
            pthread_join(instance->poller_args->thread_id, NULL);
            spdk_free(instance->poller_args);
            instance->poller_args = NULL;
            instance->poller_active = false;
            SPDK_NOTICELOG("Nuvcoder Output Poller thread for instance %u stopped.\n", instance->instance_id);
        }

        // 2. 根据实例类型进行不同的清理
        if (instance->config.type == XCODER_CODEC_TYPE_E2E_VIDEO_DECODER) {
            // ===== 解码器清理 =====
            SPDK_NOTICELOG("Shutting down decoder instance %u...\n", instance->instance_id);
            
            // FLUSH decoder
            if (instance->nuvcoder_handle) {
                SPDK_NOTICELOG("Flushing decoder instance %u...\n", instance->instance_id);
                int flush_ret = nuvcoder_decoder_flush(instance->nuvcoder_handle);
                if (flush_ret == NUVCODER_STATUS_SUCCESS) {
                    SPDK_NOTICELOG("Decoder instance %u flushed successfully.\n", instance->instance_id);
                } else {
                    SPDK_WARNLOG("Warning: Failed to flush decoder instance %u, ret=%d\n", 
                                instance->instance_id, flush_ret);
                }
            }
            
            // DESTROY decoder
            if (instance->nuvcoder_handle) {
                SPDK_NOTICELOG("Destroying decoder instance %u...\n", instance->instance_id);
                nuvcoder_decoder_reset(instance->nuvcoder_handle);
                instance->nuvcoder_handle = NULL;
                SPDK_NOTICELOG("Decoder instance %u destroyed.\n", instance->instance_id);
            }
            
        } else if (instance->config.type == XCODER_CODEC_TYPE_E2E_VIDEO_ENCODER) {
            // ===== 编码器清理 - 简化版本，只使用实际存在的成员 =====
            SPDK_NOTICELOG("Shutting down encoder instance %u...\n", instance->instance_id);
            
            // 编码器使用 codec_destroy
            if (instance->nuvcoder_handle) {
                nuvcoder_codec_reset(instance->nuvcoder_handle);
                instance->nuvcoder_handle = NULL;
                SPDK_NOTICELOG("Encoder instance %u destroyed.\n", instance->instance_id);
            }
            
            
        }

        // 3. 重置队列（释放 handler 回池）
        if (instance->queue) {
            nuvcoder_queue_pool_reset_handler(instance->queue, instance->instance_id);
            instance->queue = NULL;
        }
        
        // 4. 释放实例内存
        pthread_mutex_destroy(&instance->mutex);
        spdk_free(instance);
        session->instances[instance_id] = NULL;
        session->num_instances--;
        pthread_mutex_unlock(&session->mutex);
        
        printf("Instance %u removed from session %u.\n", instance_id, session_id);
        sc = SPDK_NVME_SC_SUCCESS;
        goto complete_request;
    } else {
        printf("Invalid VSC_XCODER_CLOSE subtype: %u\n", subtype);
        sc = SPDK_NVME_SC_INVALID_FIELD;
        goto complete_request;
    }

complete_request:
    req->rsp->nvme_cpl.status.sct = sct;
    req->rsp->nvme_cpl.status.sc = sc;
    return 0;
}

// VSC_XCODER_CONFIG 处理函数
int xcoder_vsc_config_hdlr(struct spdk_nvmf_request *req) {
    struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
    uint32_t cdw10 = le32toh(cmd->cdw10);
    uint32_t cdw11 = le32toh(cmd->cdw11);
    
    enum xcoder_config_subtype subtype = VSC_XCODER_CONFIG_GET_SUBTYPE(cdw10);
    uint32_t session_id = VSC_XCODER_CONFIG_GET_SESSION_ID(cdw10);

    fprintf(stderr, "VSC_XCODER_CONFIG_OPCODE received. Subtype: %u, Session ID: %u\n", subtype, session_id);

    uint8_t sct = SPDK_NVME_SCT_GENERIC;
    uint8_t sc = SPDK_NVME_SC_SUCCESS;
    void *config_data_buf = NULL;

    // 1. 拷贝 Host 发来的 Payload 数据
    if (req->length > 0) {
        config_data_buf = spdk_zmalloc(req->length, 0, NULL, SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);
        if (!config_data_buf) {
            fprintf(stderr, "ERROR: Failed to allocate memory for config data (len %u).\n", req->length);
            sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
            set_nvme_completion_status(req, sct, sc, 0, req->cmd->nvme_cmd.cid);
            return 0;
        }

        if (_copy_from_request_iov(req, config_data_buf, req->length) != 0) {
            fprintf(stderr, "ERROR: Failed to copy config data from Host for VSC_XCODER_CONFIG.\n");
            sc = SPDK_NVME_SC_DATA_TRANSFER_ERROR;
            if (config_data_buf) spdk_free(config_data_buf);
            set_nvme_completion_status(req, sct, sc, 0, req->cmd->nvme_cmd.cid);
            return 0;
        }
    }

    // 2. 分类处理
    if (subtype == XCODER_CONFIG_INSTANCE_PARAMS) {
        uint32_t instance_id = VSC_XCODER_CONFIG_INSTANCE_GET_INSTANCE_ID(cdw11);
        enum xcoder_config_instance_params_subtype param_type = VSC_XCODER_CONFIG_INSTANCE_GET_PARAM_TYPE(cdw11);
        
        xcoder_session_t *session = xcoder_find_session(session_id);
        xcoder_instance_t *instance = NULL;

        if (session) {
            pthread_mutex_lock(&session->mutex);
            instance = xcoder_find_instance(session, instance_id);
            pthread_mutex_unlock(&session->mutex);
        }

        if (!instance) {
            fprintf(stderr, "ERROR: Config failed: Instance %u not found or inactive in session %u.\n", 
                    instance_id, session_id);
            sc = SPDK_NVME_SC_INVALID_FIELD;
            if (config_data_buf) spdk_free(config_data_buf);
            set_nvme_completion_status(req, sct, sc, 0, req->cmd->nvme_cmd.cid);
            return 0;
        }

        printf("[VSC CONFIG] Thread %lu: Attempting to lock instance mutex for instance %u.\n", pthread_self(), instance->instance_id);
        pthread_mutex_lock(&instance->mutex);
        printf("[VSC CONFIG] Thread %lu: Instance mutex locked for instance %u.\n", pthread_self(), instance->instance_id);

        if (param_type == XCODER_INSTANCE_CONFIG_ENCODER || 
            param_type == XCODER_INSTANCE_CONFIG_SCALER) {

            if (req->length < sizeof(xcoder_instance_config_t)) {
                fprintf(stderr, "ERROR: Invalid config data length (%u) for instance config (expected %zu).\n", 
                        req->length, sizeof(xcoder_instance_config_t));
                sc = SPDK_NVME_SC_INVALID_FIELD;
                goto unlock_and_complete;
            }

            xcoder_instance_config_t *cfg = (xcoder_instance_config_t *)config_data_buf;
            
            if (instance->config.type != cfg->type) {
                fprintf(stderr, "ERROR: Config.type(%d) does not match current instance type(%d). Instance type cannot be changed after creation.\n", 
                        cfg->type, instance->config.type);
                sc = SPDK_NVME_SC_INVALID_FIELD;
                goto unlock_and_complete;
            }
            if ((cfg->width == 0) || (cfg->height == 0)) {
                fprintf(stderr, "ERROR: Width(%d) or height(%d) cannot be 0.\n", cfg->width, cfg->height);
                sc = SPDK_NVME_SC_INVALID_FIELD;
                goto unlock_and_complete;
            }
            
            // !!! 关键修改：保存配置信息 !!!
            instance->config = *cfg;
            if (instance->config.fps == 0) {
                instance->config.fps = 25;
                fprintf(stderr, "INFO: FPS is 0, set default fps(%d)\n", instance->config.fps);
            }

            // 计算期望的帧大小
            nuvcoder_frame_format_t nuvcoder_format;
            switch (instance->config.input_format) {
                case XCODER_FRAME_FORMAT_RGB:
                    nuvcoder_format = NUVCODER_FRAME_FORMAT_RGB;
                    break;
                case XCODER_FRAME_FORMAT_YUV420P:
                    nuvcoder_format = NUVCODER_FRAME_FORMAT_YUV420P;
                    break;
                case XCODER_FRAME_FORMAT_NV12:
                    nuvcoder_format = NUVCODER_FRAME_FORMAT_NV12;
                    break;
                default:
                    fprintf(stderr, "ERROR: Unsupported input format %u for instance %u.\n", 
                           instance->config.input_format, instance_id);
                    sc = SPDK_NVME_SC_INVALID_FIELD;
                    goto unlock_and_complete;
            }
            
            instance->expected_frame_size = nuvcoder_calculate_frame_size(
                instance->config.width, 
                instance->config.height, 
                nuvcoder_format);
            
            if (instance->expected_frame_size == 0) {
                fprintf(stderr, "ERROR: Failed to calculate frame size for format %u, width %u, height %u\n",
                       instance->config.input_format, instance->config.width, instance->config.height);
                sc = SPDK_NVME_SC_INVALID_FIELD;
                goto unlock_and_complete;
            }

            // 检查帧大小是否超过限制
            if (instance->expected_frame_size == 0 || instance->expected_frame_size > XCODER_MAX_BYTES_PER_FRAME) {
                fprintf(stderr, "ERROR: Configured resolution too large (%zu bytes) or invalid! Max allowed %u.\n",
                        instance->expected_frame_size, XCODER_MAX_BYTES_PER_FRAME);
                sc = SPDK_NVME_SC_INVALID_FIELD;
                instance->expected_frame_size = 0;
                goto unlock_and_complete;
            }

            // !!! 关键修改：如果已有 Codec 句柄，先销毁 !!!
            if (instance->nuvcoder_handle) {
                nuvcoder_codec_reset(instance->nuvcoder_handle);
                instance->nuvcoder_handle = NULL;
                fprintf(stderr, "INFO: Destroyed existing Nuvcoder Codec instance for Xcoder instance %u due to config update.\n", 
                        instance_id);
            }
            
            // !!! 关键修改：不再初始化队列，队列将从池中获取 !!!
            // 这里只是确保之前如果有队列，先释放
            if (instance->queue) {
                nuvcoder_queue_pool_reset_handler(instance->queue, instance_id);
                instance->queue = NULL;
            }

            instance->queue = nuvcoder_queue_pool_get_handler(instance_id);
            if (!instance->queue) {
                fprintf(stderr, "ERROR: Failed to get queue handler for instance %u\n", instance_id);
                nuvcoder_codec_reset(instance->nuvcoder_handle);
                instance->nuvcoder_handle = NULL;
                sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
                goto unlock_and_complete;
            }
            fprintf(stderr, "INFO: Instance %u acquired queue handler\n", instance_id);

            // 创建 Nuvcoder Codec 实例
            nuvcoder_codec_config_t codec_config;
            memset(&codec_config, 0, sizeof(nuvcoder_codec_config_t));
            codec_config.image_width = instance->config.width;
            codec_config.image_height = instance->config.height;
            codec_config.format = nuvcoder_format;
            codec_config.max_input_frame_bytes = XCODER_MAX_BYTES_PER_FRAME;
            
            // 根据编码器类型设置内存模式
            if (instance->config.type == XCODER_CODEC_TYPE_E2E_ENCODER || 
                instance->config.type == XCODER_CODEC_TYPE_E2E_VIDEO_ENCODER) {
                codec_config.mem_mode = NUVCODER_MEM_MODE_AUTO;
            } else {
                codec_config.mem_mode = NUVCODER_MEM_MODE_CPU_COPY;
            }
            codec_config.max_frames_in_flight = 4;
            if(instance->config.gop_size == 1) {
                codec_config.all_i = 1;
            }else{
                codec_config.all_i = 0;
            }

            instance->nuvcoder_handle = nuvcoder_codec_create(&codec_config);
            if (instance->nuvcoder_handle == NULL) {
                fprintf(stderr, "ERROR: Failed to create Nuvcoder Codec instance for Xcoder instance %u after config.\n", 
                        instance_id);
                sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
                goto unlock_and_complete;
            }
            
            fprintf(stderr, "INFO: Nuvcoder Codec instance created/reconfigured for Xcoder instance %u with %ux%u format %u.\n", 
                   instance_id, instance->config.width, instance->config.height, instance->config.input_format);
				   
			// !!! --- 新增：启动 Nuvcoder Output Poller 线程 --- !!!
            instance->poller_args = spdk_zmalloc(sizeof(nuvcoder_poller_args_t), 0, NULL, SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);
            if (!instance->poller_args) {
                SPDK_ERRLOG("ERROR: Failed to allocate poller_args for instance %u.\n", instance_id);
                nuvcoder_codec_reset(instance->nuvcoder_handle); // 清理 Nuvcoder Codec 句柄
                instance->nuvcoder_handle = NULL;
                sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
                goto unlock_and_complete;
            }
            instance->poller_args->instance = instance;
            instance->poller_args->stop_flag = false;

            int thread_rc = pthread_create(&instance->poller_args->thread_id, NULL, nuvcoder_output_poller_thread, instance->poller_args);
            if (thread_rc != 0) {
                SPDK_ERRLOG("ERROR: Failed to create Nuvcoder output poller thread for instance %u, rc=%d.\n", instance_id, thread_rc);
                spdk_free(instance->poller_args); // 释放 poller_args
                instance->poller_args = NULL;
                nuvcoder_codec_reset(instance->nuvcoder_handle); // 清理 Nuvcoder Codec 句柄
                instance->nuvcoder_handle = NULL;
                sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
                goto unlock_and_complete;
            }
            instance->poller_active = true;
            SPDK_NOTICELOG("Nuvcoder Output Poller thread started for instance %u (thread ID: %lu).\n", instance_id, (unsigned long)instance->poller_args->thread_id);
            // !!! --- 新增部分结束 --- !!!

            // 成功配置完成
            printf("[VSC CONFIG] Thread %lu: Attempting to unlock instance mutex for instance %u.\n", pthread_self(), instance->instance_id);
            pthread_mutex_unlock(&instance->mutex);
            printf("[VSC CONFIG] Thread %lu: Instance mutex unlocked for instance %u.\n", pthread_self(), instance->instance_id);
            if (config_data_buf) spdk_free(config_data_buf);
            set_nvme_completion_status(req, 
                                      SPDK_NVME_SCT_GENERIC,
                                      SPDK_NVME_SC_SUCCESS,
                                      0,
                                      req->cmd->nvme_cmd.cid);
            return 0;

        } else if (param_type == XCODER_INSTANCE_CONFIG_DECODER) {
            
            if (req->length < sizeof(xcoder_instance_dec_config_t)) {
                fprintf(stderr, "ERROR: Invalid config data length (%u) for decoder config (expected %zu).\n", 
                        req->length, sizeof(xcoder_instance_dec_config_t));
                sc = SPDK_NVME_SC_INVALID_FIELD;
                goto unlock_and_complete;
            }

            xcoder_instance_dec_config_t *cfg = (xcoder_instance_dec_config_t *)config_data_buf;
            
            // 检查类型一致性
            if (instance->config.type != cfg->type) {
                fprintf(stderr, "ERROR: Config.type(%d) does not match current instance type(%d).\n", 
                        cfg->type, instance->config.type);
                sc = SPDK_NVME_SC_INVALID_FIELD;
                goto unlock_and_complete;
            }
            
            // 保存解码器配置
            instance->dec_config = *cfg;
            
            // 检查外部是否配置了输出分辨率
            bool has_external_size = (cfg->width > 0 && cfg->height > 0);
            
            if (!has_external_size) {
                fprintf(stderr, "INFO: External width/height not configured. Will use original stream resolution.\n");
            } else {
                fprintf(stderr, "INFO: External width/height configured: %ux%u. Output will be scaled to this resolution.\n",
                        cfg->width, cfg->height);
            }
            
            // 处理FPS配置
            if (cfg->fps == 0) {
                fprintf(stderr, "INFO: FPS not configured. Will use stream original frame rate.\n");
            } else {
                fprintf(stderr, "INFO: FPS configured: %u fps. Output will be adjusted to this frame rate.\n", cfg->fps);
            }
            // 检查输出格式
            nuvcoder_frame_format_t nuvcoder_format;
            switch (cfg->output_format) {
                case XCODER_FRAME_FORMAT_RGB:
                    nuvcoder_format = NUVCODER_FRAME_FORMAT_RGB;
                    break;
                case XCODER_FRAME_FORMAT_YUV420P:
                    nuvcoder_format = NUVCODER_FRAME_FORMAT_YUV420P;
                    break;
                case XCODER_FRAME_FORMAT_NV12:
                    nuvcoder_format = NUVCODER_FRAME_FORMAT_NV12;
                    break;
                default:
                    fprintf(stderr, "ERROR: Unsupported output format %u for decoder instance %u.\n", 
                            cfg->output_format, instance_id);
                    sc = SPDK_NVME_SC_INVALID_FIELD;
                    goto unlock_and_complete;
            }
            
            // 对于解码器，expected_frame_size 表示码流最大大小
            // 使用 XCODER_MAX_BYTES_PER_FRAME 作为码流缓冲区的最大大小
            instance->expected_frame_size = XCODER_MAX_BYTES_PER_FRAME;
            
            fprintf(stderr, "INFO: Decoder instance %u: max bitstream size set to %zu bytes.\n",
                    instance_id, instance->expected_frame_size);
            
            // 队列将从池中获取
            if (instance->queue) {
                fprintf(stderr, "WARNING: Instance %u already has queue handler, releasing first\n", instance_id);
                nuvcoder_queue_pool_reset_handler(instance->queue, instance_id);
                instance->queue = NULL;
            }

            // 获取新的队列 handler
            instance->queue = nuvcoder_queue_pool_get_handler(instance_id);
            if (!instance->queue) {
                fprintf(stderr, "ERROR: Failed to get queue handler for instance %u\n", instance_id);
                nuvcoder_codec_reset(instance->nuvcoder_handle);
                instance->nuvcoder_handle = NULL;
                sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
                goto unlock_and_complete;
            }
            fprintf(stderr, "INFO: Instance %u acquired queue handler\n", instance_id);
            
            // 创建 Nuvcoder 解码器实例
            // 如果已有解码器句柄，先销毁
            if (instance->nuvcoder_handle) {
                nuvcoder_decoder_reset(instance->nuvcoder_handle);
                instance->nuvcoder_handle = NULL;
                fprintf(stderr, "INFO: Destroyed existing Nuvcoder decoder instance for Xcoder instance %u.\n", 
                        instance_id);
            }
            
            // 配置解码器
            nuvcoder_codec_config_t decoder_config;
            memset(&decoder_config, 0, sizeof(nuvcoder_codec_config_t));
            
            // 解码器配置
            decoder_config.image_width = cfg->width;        // 输出宽度（0表示自动从码流获取）
            decoder_config.image_height = cfg->height;      // 输出高度（0表示自动从码流获取）
            decoder_config.format = nuvcoder_format;        // 输出格式
            decoder_config.max_input_frame_bytes = XCODER_MAX_BITSTREAM_SIZE; // 最大输入码流大小
            decoder_config.mem_mode = NUVCODER_MEM_MODE_AUTO; // 内存模式
            decoder_config.max_frames_in_flight = 4;        // 并行帧数
            decoder_config.all_i = 0;                        // 解码器不需要这个
            
            // 创建解码器实例
            instance->nuvcoder_handle = nuvcoder_decoder_create(&decoder_config);
            if (instance->nuvcoder_handle == NULL) {
                fprintf(stderr, "ERROR: Failed to create Nuvcoder decoder instance for Xcoder instance %u.\n", 
                        instance_id);
                sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
                goto unlock_and_complete;
            }
            
            fprintf(stderr, "INFO: Nuvcoder decoder instance created for Xcoder instance %u.\n", instance_id);
            if (has_external_size) {
                fprintf(stderr, "      Output will be scaled to %ux%u format %u.\n",
                        cfg->width, cfg->height, cfg->output_format);
            } else {
                fprintf(stderr, "      Output will use original stream resolution.\n");
            }
            
            // 启动解码器输出轮询线程
            instance->poller_args = spdk_zmalloc(sizeof(nuvcoder_poller_args_t), 0, NULL, 
                                                SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);
            if (!instance->poller_args) {
                SPDK_ERRLOG("ERROR: Failed to allocate poller_args for decoder instance %u.\n", instance_id);
                nuvcoder_decoder_reset(instance->nuvcoder_handle);
                instance->nuvcoder_handle = NULL;
                sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
                goto unlock_and_complete;
            }
            instance->poller_args->instance = instance;
            instance->poller_args->stop_flag = false;
            
            int thread_rc = pthread_create(&instance->poller_args->thread_id, NULL, 
                                        nuvcoder_output_poller_thread, instance->poller_args);
            if (thread_rc != 0) {
                SPDK_ERRLOG("ERROR: Failed to create output poller thread for decoder instance %u, rc=%d.\n", 
                            instance_id, thread_rc);
                spdk_free(instance->poller_args);
                instance->poller_args = NULL;
                nuvcoder_decoder_reset(instance->nuvcoder_handle);
                instance->nuvcoder_handle = NULL;
                sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
                goto unlock_and_complete;
            }
            instance->poller_active = true;
            SPDK_NOTICELOG("Decoder output poller thread started for instance %u (thread ID: %lu).\n", 
                        instance_id, (unsigned long)instance->poller_args->thread_id);
            
            // 成功配置完成
            fprintf(stderr, "INFO: Decoder instance %u configured successfully.\n", instance_id);
            
            pthread_mutex_unlock(&instance->mutex);
            if (config_data_buf) spdk_free(config_data_buf);
            set_nvme_completion_status(req, 
                                    SPDK_NVME_SCT_GENERIC,
                                    SPDK_NVME_SC_SUCCESS,
                                    0,
                                    req->cmd->nvme_cmd.cid);
            return 0;
        } else {
            fprintf(stderr, "ERROR: Unknown instance config param type: %u.\n", param_type);
            sc = SPDK_NVME_SC_INVALID_FIELD;
        }

unlock_and_complete:
        printf("[VSC CONFIG] Thread %lu: Attempting to unlock instance mutex for instance %u.\n", pthread_self(), instance->instance_id);
        pthread_mutex_unlock(&instance->mutex);
        printf("[VSC CONFIG] Thread %lu: Instance mutex unlocked for instance %u.\n", pthread_self(), instance->instance_id);

    } else {
        fprintf(stderr, "ERROR: Invalid VSC_XCODER_CONFIG subtype: %u\n", subtype);
        sc = SPDK_NVME_SC_INVALID_FIELD;
    }

complete_request:
    if (config_data_buf) {
        spdk_free(config_data_buf);
    }
    
    set_nvme_completion_status(req, sct, sc, 0, req->cmd->nvme_cmd.cid);
    return 0;
}

// VSC_XCODER_QUERY 处理函数
int xcoder_vsc_query_hdlr(struct spdk_nvmf_request *req) {
    struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
    uint32_t cdw10 = le32toh(cmd->cdw10);
    uint32_t cdw11 = le32toh(cmd->cdw11);

#if 0
    printf("\n=== DEBUG: VSC_QUERY Command Details ===\n");
    printf("Raw CDW10: 0x%08x (le32toh: 0x%08x)\n", cmd->cdw10, cdw10);
    printf("Raw CDW11: 0x%08x (le32toh: 0x%08x)\n", cmd->cdw11, cdw11);
    printf("Request length: %u\n", req->length);
    
    // 打印字节内容
    printf("CDW10 bytes: ");
    for (int i = 0; i < 4; i++) {
        printf("%02x ", ((uint8_t*)&cdw10)[i]);
    }
    printf("\n");
    
    printf("CDW11 bytes: ");
    for (int i = 0; i < 4; i++) {
        printf("%02x ", ((uint8_t*)&cdw11)[i]);
    }
    printf("\n");
#endif

    enum xcoder_query_subtype subtype = VSC_XCODER_QUERY_GET_SUBTYPE(cdw11);
    uint32_t session_id = VSC_XCODER_QUERY_GET_SESSION_ID(cdw10); 
	uint32_t instance_id = 0; 
	
	if (subtype == XCODER_QUERY_INSTANCE_STATUS ||
        subtype == XCODER_QUERY_INPUT_STATUS ||
        subtype == XCODER_QUERY_OUTPUT_STATUS) {
        instance_id = VSC_XCODER_QUERY_GET_INSTANCE_ID(cdw11);
		// printf( "VSC_XCODER_QUERY received. Subtype: %u, Session ID: %u, Instance ID: %u\n", subtype, session_id, instance_id);
    }else{
		// printf( "VSC_XCODER_QUERY received. Subtype: %u, Session ID: %u\n", subtype, session_id);
	}
    

    

    uint8_t sct = SPDK_NVME_SCT_GENERIC;
    uint8_t sc = SPDK_NVME_SC_SUCCESS;
    void *response_buf = NULL; // 用于构建响应数据
    uint32_t actual_response_len = 0; // 实际要返回给 Host 的数据长度

    // Host 传输数据的长度 (req->length) 用于指示期望的响应数据长度
    if (req->length == 0) {
        printf( "VSC_XCODER_QUERY: Host requested 0 bytes, cannot return status.\n");
        sc = SPDK_NVME_SC_INVALID_FIELD; // 或 SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID
        goto complete_request;
    }

    if (subtype == XCODER_QUERY_SESSION_STATUS) {
        xcoder_session_t *session = xcoder_find_session(session_id); // 内部已加解锁 g_xcoder_state.mutex
        if (!session) {
            printf( "Query failed: Session %u not found or inactive.\n", session_id);
            sc = SPDK_NVME_SC_INVALID_FIELD; // 或 SPDK_NVME_SC_INVALID_FIELD_IN_COMMAND
            goto complete_request;
        }

        // 定义要返回的会话状态结构体
        struct session_status {
            uint32_t session_id;
            bool     is_active;
            uint32_t num_instances;
        } __attribute__((packed));

        actual_response_len = sizeof(struct session_status);
        if (req->length < actual_response_len) {
            printf( "Response buffer too small for session status (req len %u, expected %u).\n", req->length, actual_response_len);
            sc = SPDK_NVME_SC_DATA_TRANSFER_ERROR; // 或 SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID
            goto complete_request;
        }

        pthread_mutex_lock(&session->mutex); // 锁定 session 访问其内部状态
        response_buf = spdk_zmalloc(actual_response_len, 0, NULL, SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);
        if (!response_buf) {
            printf( "Failed to allocate memory for session status response.\n");
            pthread_mutex_unlock(&session->mutex);
            sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
            goto complete_request;
        }
        struct session_status *status = (struct session_status *)response_buf;
        status->session_id = htole32(session->session_id); // 确保字节序
        status->is_active = session->is_active;
        status->num_instances = htole32(session->num_instances); // 确保字节序

        if (_copy_to_request_iov(req, response_buf, actual_response_len) != 0) {
            printf( "Failed to copy session status to Host.\n");
            sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR; // 或 SPDK_NVME_SC_DATA_TRANSFER_ERROR
        }
        printf( "Queried session %u status: active=%d, instances=%u.\n",
                     session->session_id, status->is_active, le32toh(status->num_instances));
        pthread_mutex_unlock(&session->mutex);

    } else if (subtype == XCODER_QUERY_INSTANCE_STATUS) {
        xcoder_session_t *session = xcoder_find_session(session_id); // 内部已加解锁 g_xcoder_state.mutex
        xcoder_instance_t *instance = NULL;

        if (session) {
            pthread_mutex_lock(&session->mutex); // 锁定 session 才能查找 instance
            instance = xcoder_find_instance(session, instance_id); // 此函数不加锁 instance->mutex
            pthread_mutex_unlock(&session->mutex);
        }

        if (!instance) {
            printf( "Query failed: Instance %u not found or inactive in session %u.\n", instance_id, session_id);
            sc = SPDK_NVME_SC_INVALID_FIELD; // 或 SPDK_NVME_SC_INVALID_FIELD_IN_COMMAND
            goto complete_request;
        }

        // 定义要返回的实例状态结构体
        struct instance_status {
            uint32_t instance_id;
            bool     is_active;
            uint32_t codec_type;
            uint32_t processed_frames;
            uint32_t configured_width;
            uint32_t configured_height;
            uint64_t expected_frame_size; // 新增：返回预期帧大小
            uint64_t last_output_len;     // 新增：返回上次处理的输出长度
            bool     nuvcoder_handle_valid; // 新增：Nuvcoder句柄是否有效
        } __attribute__((packed));

        actual_response_len = sizeof(struct instance_status);
        if (req->length < actual_response_len) {
            printf( "Response buffer too small for instance status (req len %u, expected %u).\n", req->length, actual_response_len);
            sc = SPDK_NVME_SC_DATA_TRANSFER_ERROR; // 或 SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID
            goto complete_request;
        }

        printf("[VSC QUERY INSTANCE STATUS] Thread %lu: Attempting to lock instance mutex for instance %u.\n", pthread_self(), instance->instance_id);
        pthread_mutex_lock(&instance->mutex);
        printf("[VSC QUERY INSTANCE STATUS] Thread %lu: Instance mutex locked for instance %u.\n", pthread_self(), instance->instance_id);
        response_buf = spdk_zmalloc(actual_response_len, 0, NULL, SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);
        if (!response_buf) {
            printf( "Failed to allocate memory for instance status response.\n");
            printf("[VSC QUERY INSTANCE STATUS] Thread %lu: Attempting to unlock instance mutex for instance %u.\n", pthread_self(), instance->instance_id);
            pthread_mutex_unlock(&instance->mutex);
            printf("[VSC QUERY INSTANCE STATUS] Thread %lu: Instance mutex unlocked for instance %u.\n", pthread_self(), instance->instance_id);
            sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
            goto complete_request;
        }
        struct instance_status *status = (struct instance_status *)response_buf;
        status->instance_id = htole32(instance->instance_id); // 确保字节序
        status->is_active = instance->is_active;
        status->codec_type = htole32(instance->config.type); // 确保字节序
        status->processed_frames = htole32(instance->processed_frames); // 确保字节序
        status->configured_width = htole32(instance->config.width);
        status->configured_height = htole32(instance->config.height);
        status->expected_frame_size = htole64(instance->expected_frame_size); // 64位字节序
        status->last_output_len = htole64(instance->last_output_len); // 64位字节序
        status->nuvcoder_handle_valid = (instance->nuvcoder_handle != NULL); // 检查句柄是否有效

        if (_copy_to_request_iov(req, response_buf, actual_response_len) != 0) {
            printf( "Failed to copy instance status to Host.\n");
            sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR; // 或 SPDK_NVME_SC_DATA_TRANSFER_ERROR
        }
        printf( "Queried instance %u (session %u) status: active=%d, type=%u, frames=%u, %ux%u, expected_size=%lu, last_output=%lu, nuvcoder_valid=%d.\n",
                     instance->instance_id, session_id, status->is_active, le32toh(status->codec_type), le32toh(status->processed_frames),
                     le32toh(status->configured_width), le32toh(status->configured_height),
                     le64toh(status->expected_frame_size), le64toh(status->last_output_len), status->nuvcoder_handle_valid);
        printf("[VSC QUERY INSTANCE STATUS] Thread %lu: Attempting to unlock instance mutex for instance %u.\n", pthread_self(), instance->instance_id);
        pthread_mutex_unlock(&instance->mutex);
        printf("[VSC QUERY INSTANCE STATUS] Thread %lu: Instance mutex unlocked for instance %u.\n", pthread_self(), instance->instance_id);

    } else if (subtype == XCODER_QUERY_GLOBAL_STATUS) {
        // 定义要返回的全局状态结构体
        struct global_status {
            uint32_t total_sessions_active;
            uint32_t total_instances_active;
        } __attribute__((packed));

        actual_response_len = sizeof(struct global_status);
        if (req->length < actual_response_len) {
            printf( "Response buffer too small for global status (req len %u, expected %u).\n", req->length, actual_response_len);
            sc = SPDK_NVME_SC_DATA_TRANSFER_ERROR; // 或 SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID
            goto complete_request;
        }

        response_buf = spdk_zmalloc(actual_response_len, 0, NULL, SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);
        if (!response_buf) {
            printf( "Failed to allocate memory for global status response.\n");
            sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
            goto complete_request;
        }
        struct global_status *status = (struct global_status *)response_buf;
        status->total_sessions_active = 0;
        status->total_instances_active = 0;

        pthread_mutex_lock(&g_xcoder_state.mutex);
        for (int i = 0; i < MAX_XCODER_SESSIONS; i++) {
            if (g_xcoder_state.sessions[i] && g_xcoder_state.sessions[i]->is_active) {
                status->total_sessions_active++;
                pthread_mutex_lock(&g_xcoder_state.sessions[i]->mutex); // 锁定会话以安全读取 num_instances
                status->total_instances_active += g_xcoder_state.sessions[i]->num_instances;
                pthread_mutex_unlock(&g_xcoder_state.sessions[i]->mutex);
            }
        }
        pthread_mutex_unlock(&g_xcoder_state.mutex);

        status->total_sessions_active = htole32(status->total_sessions_active); // 确保字节序
        status->total_instances_active = htole32(status->total_instances_active); // 确保字节序

        if (_copy_to_request_iov(req, response_buf, actual_response_len) != 0) {
            printf( "Failed to copy global status to Host.\n");
            sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR; // 或 SPDK_NVME_SC_DATA_TRANSFER_ERROR
        }
        printf( "Queried global status: active_sessions=%u, active_instances=%u.\n",
                     le32toh(status->total_sessions_active), le32toh(status->total_instances_active));

    } else if (subtype == XCODER_QUERY_INPUT_STATUS) {
        xcoder_session_t *session = xcoder_find_session(session_id);
        xcoder_instance_t *instance = NULL;

        if (session) {
            pthread_mutex_lock(&session->mutex);
            instance = xcoder_find_instance(session, instance_id);
            pthread_mutex_unlock(&session->mutex);
        }

        if (!instance) {
            printf("Query failed: Instance %u not found or inactive in session %u for input status.\n", 
                instance_id, session_id);
            sc = SPDK_NVME_SC_INVALID_FIELD;
            goto complete_request;
        }

        // 直接使用头文件中定义的结构体
        actual_response_len = sizeof(nuvc_input_status_t);
        if (req->length < actual_response_len) {
            printf("Response buffer too small for input status (req len %u, expected %u).\n", 
                req->length, actual_response_len);
            sc = SPDK_NVME_SC_DATA_TRANSFER_ERROR;
            goto complete_request;
        }

        // printf("[VSC QUERY INPUT STATUS] Thread %lu: Attempting to lock instance mutex for instance %u.\n", pthread_self(), instance->instance_id);
        pthread_mutex_lock(&instance->mutex);
        // printf("[VSC QUERY INPUT STATUS] Thread %lu: Instance mutex locked for instance %u.\n", pthread_self(), instance->instance_id);
        response_buf = spdk_zmalloc(actual_response_len, 0, NULL, SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);
        if (!response_buf) {
            printf("[VSC QUERY INPUT STATUS] Failed to allocate memory for input status response.\n");
            printf("[VSC QUERY INPUT STATUS] Thread %lu: Attempting to unlock instance mutex for instance %u.\n", pthread_self(), instance->instance_id);
            pthread_mutex_unlock(&instance->mutex);
            printf("[VSC QUERY INPUT STATUS] Thread %lu: Instance mutex unlocked for instance %u.\n", pthread_self(), instance->instance_id);
            sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
            goto complete_request;
        }
        nuvc_input_status_t *status = (nuvc_input_status_t *)response_buf;
        memset(status, 0, sizeof(nuvc_input_status_t));

        // !!! 关键实现：判断是否可以开始新的一帧 !!!
        // 条件：1. 队列有空闲entry 2. 没有当前正在写入的entry
        
        int available_slots = 0;

        // 先检查是否有队列
        if (!instance->queue) {
            // 没有队列，需要先获取，说明还没有config
            // fprintf(stderr, "[VSC QUERY INPUT STATUS] ERROR: instance %u has no queue\n", 
            //         instance->instance_id);
            available_slots = 0;
        } else if (instance->current_write_entry != NULL) {
            // 正在写入一帧
            printf("[VSC QUERY INPUT STATUS] current_write_entry is busy for instance %u\n", 
                instance->instance_id);
            available_slots = 0;
        } else {
            // 正常情况：检查队列空闲 entry
            pthread_mutex_lock(&instance->queue->lock);
            available_slots = XCODER_QUEUE_DEPTH - instance->queue->count;
            printf("[VSC QUERY INPUT STATUS] instance %u: queue_free=%d, queue_count=%d\n",
                instance->instance_id, available_slots, instance->queue->count);
            pthread_mutex_unlock(&instance->queue->lock);
        }

        status->available_slots = htole32((uint32_t)available_slots);

        if (_copy_to_request_iov(req, response_buf, actual_response_len) != 0) {
            printf("Failed to copy input status to Host.\n");
            sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
        }

        if (instance->queue){
            printf("Queried instance %u (session %u) input status: available_slots=%u.\n",
                instance->instance_id, session_id, (uint32_t)available_slots);
        }
        // printf("[VSC QUERY INPUT STATUS] Thread %lu: Attempting to unlock instance mutex for instance %u.\n", pthread_self(), instance->instance_id);
        pthread_mutex_unlock(&instance->mutex);
        // printf("[VSC QUERY INPUT STATUS] Thread %lu: Instance mutex unlocked for instance %u.\n", pthread_self(), instance->instance_id);
    } else if (subtype == XCODER_QUERY_OUTPUT_STATUS) {
        xcoder_session_t *session = xcoder_find_session(session_id);
        xcoder_instance_t *instance = NULL;

        if (session) {
            pthread_mutex_lock(&session->mutex);
            instance = xcoder_find_instance(session, instance_id);
            pthread_mutex_unlock(&session->mutex);
        }

        if (!instance) {
            printf("Query failed: Instance %u not found or inactive in session %u for output status.\n", 
                   instance_id, session_id);
            sc = SPDK_NVME_SC_INVALID_FIELD;
            goto complete_request;
        }

        actual_response_len = sizeof(nuvc_output_status_t);
        if (req->length < actual_response_len) {
            printf("Response buffer too small for output status (req len %u, expected %u).\n", 
                   req->length, actual_response_len);
            sc = SPDK_NVME_SC_DATA_TRANSFER_ERROR;
            goto complete_request;
        }

        response_buf = spdk_zmalloc(actual_response_len, 0, NULL, SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);
        if (!response_buf) {
            printf("Failed to allocate memory for output status response.\n");
            sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
            goto complete_request;
        }
        nuvc_output_status_t *status = (nuvc_output_status_t *)response_buf;
        memset(status, 0, sizeof(nuvc_output_status_t));

        int packets_available = 0;
        size_t next_packet_size = 0;
        
        nuvcoder_queue_entry_t *tail_entry = NULL;
        
        // 1. 获取队列头部 Entry（如果存在）
        if (instance->queue) {
            pthread_mutex_lock(&instance->queue->lock);
            if (instance->queue->count > 0) {
                tail_entry = &instance->queue->entries[instance->queue->tail];
            }
            pthread_mutex_unlock(&instance->queue->lock);
        }

        if (tail_entry) {
            // 2. 调用非阻塞函数检查 Entry 状态
            xcoder_bitstream_status_t bs_status = xcoder_get_bitstream_nonblocking(tail_entry, &next_packet_size, NULL);
            // 注意：这里传入 NULL 给 out_buffer_dma，因为 QUERY 不需要实际的数据指针

            if (bs_status == XCODER_BITSTREAM_STATUS_SUCCESS) {
                packets_available = 1;
                // next_packet_size 已经由 xcoder_get_bitstream_nonblocking 填充
                printf("[QUERY] Instance %u: Found completed entry %d for output status, len %zu.\n",
                       instance->instance_id, tail_entry->index, next_packet_size);
            } else if (bs_status == XCODER_BITSTREAM_STATUS_FAILED) {
                // Entry 完成但失败了。 Host 应该知情。
                // 我们可以标记为 packets_available = 1，但 next_packet_size = 0
                // 让 Host 在 READ 时获取到 0 长度数据并触发 Entry 释放。
                packets_available = 1; 
                next_packet_size = 0; // 或者约定一个特殊值代表错误，但 0 是最简单的
                fprintf(stderr, "[QUERY] Instance %u: Entry %d completed with error %d. Signaling 0 length.\n",
                        instance->instance_id, tail_entry->index, tail_entry->result_code);
            }
            // else if (bs_status == XCODER_BITSTREAM_STATUS_NOT_READY || bs_status == XCODER_BITSTREAM_STATUS_INVALID_ARGS)
            // { packets_available 和 next_packet_size 保持 0，这是默认值 }
        }
        // else (tail_entry == NULL) { packets_available 和 next_packet_size 保持 0 }

        // 设置返回状态
        status->packets_available = htole32(packets_available);
        status->next_packet_size = htole32((uint32_t)next_packet_size); // 转换为 uint32_t，注意溢出

        if (_copy_to_request_iov(req, response_buf, actual_response_len) != 0) {
            printf("Failed to copy output status to Host.\n");
            sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
        }
        // printf("Queried instance %u (session %u) output status: packets_available=%u, next_packet_size=%u.\n",
        //        instance->instance_id, session_id, 
        //        packets_available, (uint32_t)next_packet_size);
	}else {
        printf( "Invalid VSC_XCODER_QUERY subtype: %u\n", subtype);
        sc = SPDK_NVME_SC_INVALID_FIELD; // 或 SPDK_NVME_SC_INVALID_FIELD_IN_COMMAND
    }

complete_request:
    if (response_buf) {
        spdk_free(response_buf); // 释放为响应分配的内存
    }
    req->rsp->nvme_cpl.status.sct = sct;
    req->rsp->nvme_cpl.status.sc = sc;
    return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE; // 确认这个返回值的定义是否匹配你的spdk版本
}