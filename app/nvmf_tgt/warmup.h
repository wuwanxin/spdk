/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2025. All rights reserved.
 */

#ifndef XCODER_WARMUP_H
#define XCODER_WARMUP_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 预热编码器（模拟完整的 host 调用流程）
 */
int xcoder_warmup_encoder(void);

/**
 * @brief 预热解码器（模拟完整的 host 调用流程）
 */
int xcoder_warmup_decoder(void);

/**
 * @brief 启动预热（同时预热编解码器）
 */
int xcoder_start_warmup(void);

/**
 * @brief 检查预热是否完成
 */
bool xcoder_is_warmup_done(void);

/**
 * @brief 等待预热完成
 */
int xcoder_wait_warmup(int timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* XCODER_WARMUP_H */