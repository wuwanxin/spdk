/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2025. All rights reserved.
 */

#include "warmup.h"
#include "spdk/log.h"
#include "nuvcoder_codec_api.h"
#include "nuvcoder_queue.h"
#include "nuvcoder_types.h"  // 🎯 包含 XCODER_MAX_BYTES_PER_FRAME 的定义

#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <string.h>

// ==================== 全局状态 ====================
static int g_warmup_done = 0;
static int g_warmup_result = 0;
static pthread_mutex_t g_warmup_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_warmup_cond = PTHREAD_COND_INITIALIZER;

// 获取当前时间（毫秒）
static int64_t get_current_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + (int64_t)ts.tv_nsec / 1000000;
}

// ==================== 同步预热接口 ====================

int xcoder_start_warmup(void) {
    pthread_mutex_lock(&g_warmup_mutex);
    
    if (g_warmup_done) {
        pthread_mutex_unlock(&g_warmup_mutex);
        SPDK_NOTICELOG("[Warmup] Already done, result: %d\n", g_warmup_result);
        return g_warmup_result;
    }
    
    pthread_mutex_unlock(&g_warmup_mutex);
    
    SPDK_NOTICELOG("[Warmup] ========================================\n");
    SPDK_NOTICELOG("[Warmup] Starting Xcoder warmup\n");
    SPDK_NOTICELOG("[Warmup] Using XCODER_MAX_BYTES_PER_FRAME = %u bytes (%.2f MB)\n", 
                   XCODER_MAX_BYTES_PER_FRAME, 
                   XCODER_MAX_BYTES_PER_FRAME / (1024.0 * 1024.0));
    SPDK_NOTICELOG("[Warmup] ========================================\n");
    
    int64_t start_time = get_current_time_ms();
    
    // ==================== 🎯 创建共享队列池（使用 XCODER_MAX_BYTES_PER_FRAME）====================
    SPDK_NOTICELOG("[Warmup] Creating queue pool with buffer size %u...\n", 
                   XCODER_MAX_BYTES_PER_FRAME);
    
    // 使用 XCODER_MAX_BYTES_PER_FRAME 作为输入和输出缓冲区大小
    int ret = nuvcoder_queue_pool_create(
        XCODER_MAX_BYTES_PER_FRAME,  // input buffer size
        XCODER_MAX_BYTES_PER_FRAME   // output buffer size
    );
    
    if (ret != 0) {
        SPDK_ERRLOG("[Warmup] ❌ Failed to create queue pool\n");
        g_warmup_result = -1;
        g_warmup_done = 1;
        return -1;
    }
    
    SPDK_NOTICELOG("[Warmup] ✅ Queue pool created successfully (size=%d, each buffer=%u bytes)\n", 
                   NUVCODER_QUEUE_POOL_SIZE, XCODER_MAX_BYTES_PER_FRAME);
    
    // ==================== 编码器预热 ====================
    SPDK_NOTICELOG("[Warmup] Warming up encoder...\n");
    
    nuvcoder_codec_config_t warmup_config;
    memset(&warmup_config, 0, sizeof(nuvcoder_codec_config_t));
    
    int warmup_ret = nuvcoder_codec_warmup(&warmup_config);
    
    if (warmup_ret != NUVCODER_STATUS_SUCCESS) {
        SPDK_ERRLOG("[Warmup] ❌ Encoder warmup failed with code %d\n", warmup_ret);
        g_warmup_result = -1;
        g_warmup_done = 1;
        return -1;
    }
    
    SPDK_NOTICELOG("[Warmup] ✅ Encoder warmup completed\n");
    
    // ==================== 解码器预热 ====================
    SPDK_NOTICELOG("[Warmup] Warming up decoder...\n");
    
    warmup_ret = nuvcoder_decoder_warmup(&warmup_config);
    
    if (warmup_ret != NUVCODER_STATUS_SUCCESS) {
        SPDK_ERRLOG("[Warmup] ❌ Decoder warmup failed with code %d\n", warmup_ret);
        g_warmup_result = -1;
        g_warmup_done = 1;
        return -1;
    }
    
    SPDK_NOTICELOG("[Warmup] ✅ Decoder warmup completed\n");
    
    int64_t end_time = get_current_time_ms();
    int64_t duration = end_time - start_time;
    
    SPDK_NOTICELOG("[Warmup] ========================================\n");
    SPDK_NOTICELOG("[Warmup] ✅ ALL WARMUP COMPLETED in %ld ms\n", duration);
    SPDK_NOTICELOG("[Warmup]    Models are now loaded and cached\n");
    SPDK_NOTICELOG("[Warmup]    Queue pool created with XCODER_MAX_BYTES_PER_FRAME=%u\n", 
                   XCODER_MAX_BYTES_PER_FRAME);
    SPDK_NOTICELOG("[Warmup]    Queue pool size=%d, each buffer=%u bytes (%.2f MB)\n",
                   NUVCODER_QUEUE_POOL_SIZE,
                   XCODER_MAX_BYTES_PER_FRAME,
                   XCODER_MAX_BYTES_PER_FRAME / (1024.0 * 1024.0));
    SPDK_NOTICELOG("[Warmup] ========================================\n");
    
    pthread_mutex_lock(&g_warmup_mutex);
    g_warmup_result = 0;
    g_warmup_done = 1;
    pthread_cond_broadcast(&g_warmup_cond);
    pthread_mutex_unlock(&g_warmup_mutex);
    
    return 0;
}

bool xcoder_is_warmup_done(void) {
    pthread_mutex_lock(&g_warmup_mutex);
    int done = g_warmup_done;
    pthread_mutex_unlock(&g_warmup_mutex);
    return done;
}

int xcoder_wait_warmup(int timeout_ms) {
    pthread_mutex_lock(&g_warmup_mutex);
    
    if (g_warmup_done) {
        pthread_mutex_unlock(&g_warmup_mutex);
        return 0;
    }
    
    if (timeout_ms <= 0) {
        // 无限等待
        while (!g_warmup_done) {
            pthread_cond_wait(&g_warmup_cond, &g_warmup_mutex);
        }
        pthread_mutex_unlock(&g_warmup_mutex);
        return 0;
    }
    
    // 带超时的等待
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (timeout_ms % 1000) * 1000000;
    if (ts.tv_nsec >= 1000000000) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000;
    }
    
    int ret = 0;
    while (!g_warmup_done) {
        int rc = pthread_cond_timedwait(&g_warmup_cond, &g_warmup_mutex, &ts);
        if (rc == ETIMEDOUT) {
            SPDK_WARNLOG("[Warmup] Wait timeout after %d ms\n", timeout_ms);
            ret = -1;
            break;
        }
    }
    
    pthread_mutex_unlock(&g_warmup_mutex);
    return ret;
}

// ==================== 为了向后兼容保留的函数 ====================

int xcoder_warmup_encoder(void) {
    SPDK_NOTICELOG("[Warmup] xcoder_warmup_encoder called (forwarding to start_warmup)\n");
    return xcoder_start_warmup();
}

int xcoder_warmup_decoder(void) {
    SPDK_NOTICELOG("[Warmup] xcoder_warmup_decoder called (forwarding to start_warmup)\n");
    return xcoder_start_warmup();
}