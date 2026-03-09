#ifndef BDEV_XCODER_H
#define BDEV_XCODER_H

#include "spdk/stdinc.h"
#include "spdk/bdev.h"
#include "spdk/thread.h"
#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/log.h"
#include "spdk/string.h"
#include "spdk/bdev_module.h"

// 引入 common 目录下的类型定义
#include "nuvcoder_types.h"

// 定义虚拟 bdev 的名称和描述
#define BDEV_XCODER_NAME "XcoderBdev"
#define BDEV_XCODER_PRODUCT_NAME "SPDK Xcoder Virtual Bdev"

// LBA 映射宏 (与 Host 端保持一致)
// 假设每个 LBA 对应 4KB (4096 字节)
#define XCODER_LBA_SIZE             4096ULL
#define XCODER_LBA_TO_BYTES(lba)    ((lba) * XCODER_LBA_SIZE)
#define XCODER_BYTES_TO_LBA(bytes)  ((bytes) / XCODER_LBA_SIZE)

#define XCODER_MD_SIZE 64  // metadata size 64 bytes

// LBA 空间划分 (示例，需要根据实际需求调整)
// 假设每个会话的 I/O 空间大小
#define XCODER_SESSION_LBA_SPACE    (XCODER_BYTES_TO_LBA(1ULL * 1024 * 1024 * 1024)) // 1GB LBA 空间
// 每个实例的 I/O 空间大小
#define XCODER_INSTANCE_LBA_SPACE   (XCODER_BYTES_TO_LBA(256ULL * 1024 * 1024)) // 256MB LBA 空间

// 定义 LBA 区域的起始地址
// 0 - (MAX_XCODER_SESSIONS * XCODER_SESSION_LBA_SPACE) - 1
// 用于映射到不同会话和实例的输入缓冲区
#define WR_LBA_START_BLOCK          0ULL
// (MAX_XCODER_SESSIONS * XCODER_SESSION_LBA_SPACE) -
// (MAX_XCODER_SESSIONS * XCODER_SESSION_LBA_SPACE) * 2 - 1
// 用于映射到不同会话和实例的输出缓冲区
#define RD_LBA_START_BLOCK          (WR_LBA_START_BLOCK + (MAX_XCODER_SESSIONS * XCODER_SESSION_LBA_SPACE))

// 计算写入操作的 LBA 偏移
// LBA = WR_LBA_START_BLOCK + (session_id * XCODER_SESSION_LBA_SPACE) + (instance_id * XCODER_INSTANCE_LBA_SPACE) + offset_in_instance_buffer_in_lba
#define GET_WR_LBA_OFFSET(session_id, instance_id, offset_lba) \
    (WR_LBA_START_BLOCK + ((uint64_t)(session_id) * XCODER_SESSION_LBA_SPACE) + \
     ((uint64_t)(instance_id) * XCODER_INSTANCE_LBA_SPACE) + (offset_lba))

// 计算读取操作的 LBA 偏移
// LBA = RD_LBA_START_BLOCK + (session_id * XCODER_SESSION_LBA_SPACE) + (instance_id * XCODER_INSTANCE_LBA_SPACE) + offset_in_instance_buffer_in_lba
#define GET_RD_LBA_OFFSET(session_id, instance_id, offset_lba) \
    (RD_LBA_START_BLOCK + ((uint64_t)(session_id) * XCODER_SESSION_LBA_SPACE) + \
     ((uint64_t)(instance_id) * XCODER_INSTANCE_LBA_SPACE) + (offset_lba))

// 获取 LBA 对应的 session_id (从写入或读取区域)
#define GET_SESSION_ID_FROM_LBA(lba) \
    (((lba) >= WR_LBA_START_BLOCK && (lba) < RD_LBA_START_BLOCK) ? \
     (((lba) - WR_LBA_START_BLOCK) / XCODER_SESSION_LBA_SPACE) : \
     (((lba) >= RD_LBA_START_BLOCK) ? \
      (((lba) - RD_LBA_START_BLOCK) / XCODER_SESSION_LBA_SPACE) : \
      (uint32_t)-1))

// 获取 LBA 对应的 instance_id (从写入或读取区域)
#define GET_INSTANCE_ID_FROM_LBA(lba) \
    (((lba) >= WR_LBA_START_BLOCK && (lba) < RD_LBA_START_BLOCK) ? \
     ((((lba) - WR_LBA_START_BLOCK) % XCODER_SESSION_LBA_SPACE) / XCODER_INSTANCE_LBA_SPACE) : \
     (((lba) >= RD_LBA_START_BLOCK) ? \
      ((((lba) - RD_LBA_START_BLOCK) % XCODER_SESSION_LBA_SPACE) / XCODER_INSTANCE_LBA_SPACE) : \
      (uint32_t)-1))

// 获取 LBA 在实例缓冲区内的偏移 (以 LBA 为单位)
#define GET_OFFSET_IN_INSTANCE_BUFFER_LBA(lba) \
    (((lba) >= WR_LBA_START_BLOCK && (lba) < RD_LBA_START_BLOCK) ? \
     ((((lba) - WR_LBA_START_BLOCK) % XCODER_SESSION_LBA_SPACE) % XCODER_INSTANCE_LBA_SPACE) : \
     (((lba) >= RD_LBA_START_BLOCK) ? \
      ((((lba) - RD_LBA_START_BLOCK) % XCODER_SESSION_LBA_SPACE) % XCODER_INSTANCE_LBA_SPACE) : \
      (uint64_t)-1))

struct xcoder_io_metadata {
    uint32_t total_size;      // 这一帧/数据的期望总大小
    uint32_t last_packet;     // 1: 最后一个包, 0: 不是最后一个
    uint64_t reserved1;       // 保留
    uint64_t reserved2;       // 保留
} __attribute__((packed));

// Xcoder 虚拟 bdev 结构体
struct xcoder_bdev {
    struct spdk_bdev    bdev;           // SPDK bdev 核心结构体
    struct spdk_bdev_desc *desc;        // bdev 描述符
    struct spdk_thread  *thread;        // 关联的 SPDK 线程
    uint64_t            num_blocks;     // bdev 的总 LBA 数量
    uint32_t            block_size;     // bdev 的块大小 (LBA size)
};

// I/O 通道结构体 (每个 bdev I/O 通道对应一个 SPDK 线程)
struct xcoder_io_channel {
    struct spdk_poller  *poller;        // 用于异步处理的 poller
    // 其他 I/O 相关的状态可以放在这里
};


// 函数声明 (供外部使用或注册)
int bdev_xcoder_init(void);
void bdev_xcoder_fini(void);

#endif // BDEV_XCODER_H