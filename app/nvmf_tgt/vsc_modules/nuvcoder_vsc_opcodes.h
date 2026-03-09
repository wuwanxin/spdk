#ifndef NUVCODER_VSC_OPCODES_H
#define NUVCODER_VSC_OPCODES_H

#include "spdk/stdinc.h"
#include <stdint.h>

// -----------------------------------------------------------------------------
// Xcoder VSC Opcodes Definition
// 注意：Opcode 的最后两位决定了数据传输方向，不可随意定义。
// -----------------------------------------------------------------------------

// 1. OPEN: 仅使用 Parameter (CDW10-12)，无数据 Payload
// 0xC0 (二进制 ...00) -> No Data Transfer
#define VSC_XCODER_OPEN_OPCODE          0xC0

// 2. CONFIG: Host 发送配置结构体给设备 (Host to Controller)
// 0xC1 (二进制 ...01) -> H2C Data Transfer
// 之前定义为 C2 可能导致 SPDK 认为是 Read 或无数据
#define VSC_XCODER_CONFIG_OPCODE        0xC1

// 3. QUERY: 设备返回状态结构体给 Host (Controller to Host)
// 0xC2 (二进制 ...10) -> C2H Data Transfer
#define VSC_XCODER_QUERY_OPCODE         0xC2

// 4. CLOSE: 仅使用 Parameter，无数据 Payload
// 0xC4 (二进制 ...00) -> No Data Transfer (跳过 C3 双向，使用 C4)
#define VSC_XCODER_CLOSE_OPCODE         0xC4

// -----------------------------------------------------------------------------
// Subtype & Macro Definitions
// -----------------------------------------------------------------------------

// --- VSC_XCODER_OPEN (CDW10) ---
enum xcoder_open_subtype {
    XCODER_OPEN_CREATE_SESSION  = 0x0,
    XCODER_OPEN_ADD_INSTANCE    = 0x1,
};
#define VSC_XCODER_OPEN_GET_SUBTYPE(cdw10)           (((cdw10) >> 0) & 0xFF)
#define VSC_XCODER_OPEN_GET_SESSION_ID_HINT(cdw10)   (((cdw10) >> 8) & 0xFF) // For ADD_INSTANCE

// --- VSC_XCODER_CLOSE (CDW10) ---
enum xcoder_close_subtype {
    XCODER_CLOSE_DESTROY_SESSION    = 0x0,
    XCODER_CLOSE_REMOVE_INSTANCE    = 0x1,
};
#define VSC_XCODER_CLOSE_GET_SUBTYPE(cdw10)         (((cdw10) >> 0) & 0xFF)

// 通用宏：很多命令都在 CDW10 的高位存放 Session ID
#define VSC_XCODER_GET_SESSION_ID(cdw10)            (((cdw10) >> 8) & 0xFF)
// 兼容旧代码宏名
#define VSC_XCODER_CONFIG_GET_SESSION_ID(cdw10)     VSC_XCODER_GET_SESSION_ID(cdw10)

// --- VSC_XCODER_CONFIG (CDW10, CDW11) ---
enum xcoder_config_subtype {
    XCODER_CONFIG_GLOBAL_PARAMS     = 0x0,
    XCODER_CONFIG_SESSION_PARAMS    = 0x1,
    XCODER_CONFIG_INSTANCE_PARAMS   = 0x2,
};
#define VSC_XCODER_CONFIG_GET_SUBTYPE(cdw10)        (((cdw10) >> 0) & 0xFF)

// VSC_XCODER_CONFIG_INSTANCE_PARAMS (CDW11 Sub-fields)
enum xcoder_config_instance_params_type {
    XCODER_INSTANCE_CONFIG_ENCODER  = 0x0,
    XCODER_INSTANCE_CONFIG_DECODER  = 0x1,
    XCODER_INSTANCE_CONFIG_SCALER   = 0x2,
};
// 兼容旧枚举名
#define xcoder_config_instance_params_subtype xcoder_config_instance_params_type

#define VSC_XCODER_CONFIG_INSTANCE_GET_INSTANCE_ID(cdw11) (((cdw11) >> 0) & 0xFF)
#define VSC_XCODER_CONFIG_INSTANCE_GET_PARAM_TYPE(cdw11)  (((cdw11) >> 8) & 0xFF)

// --- VSC_XCODER_QUERY (CDW10, CDW11) ---
enum xcoder_query_subtype {
    XCODER_QUERY_GLOBAL_STATUS      = 0x0,
    XCODER_QUERY_SESSION_STATUS     = 0x1,
    XCODER_QUERY_INSTANCE_STATUS    = 0x2,
    XCODER_QUERY_INPUT_STATUS       = 0x03, // 输入缓冲区状态
    XCODER_QUERY_OUTPUT_STATUS      = 0x04, // 输出缓冲区状态
};
#define VSC_XCODER_QUERY_GET_SESSION_ID(cdw10)  (((cdw10) >> 0) & 0xFF)
#define VSC_XCODER_QUERY_GET_SUBTYPE(cdw11)         (((cdw11) >> 0) & 0xFF)
#define VSC_XCODER_QUERY_GET_INSTANCE_ID(cdw11)     (((cdw11) >> 8) & 0xFF)


// 状态结构体（确保与Host端一致）
typedef struct nuvc_input_status {
    uint32_t available_slots; // 可用的输入缓冲区槽位数量
    uint32_t reserved[6];     // 填充到32字节
} nuvc_input_status_t;

typedef struct nuvc_output_status {
    uint32_t packets_available; // 可用的输出码流包数量
    uint32_t next_packet_size;  // 下一个码流包的字节大小
    uint32_t reserved[6];       // 填充到32字节
} nuvc_output_status_t;

#endif // NUVCODER_VSC_OPCODES_H