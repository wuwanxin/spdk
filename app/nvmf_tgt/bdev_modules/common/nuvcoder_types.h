// xcoder_types.h
#ifndef XCODER_TYPES_H
#define XCODER_TYPES_H

#include "spdk/stdinc.h"
#include <pthread.h> // For pthread_mutex_t
#include "nuvcoder_codec_api.h"
#include "spdk/nvmf.h"

// !!! 新增：包含 nuvcoder_queue.h !!!
#include "nuvcoder_queue.h"
#include "nuvcoder_poller.h"
// xcoder_types.h

#ifndef HOST_LBA_SIZE
#define HOST_LBA_SIZE 4096
#endif

// 对齐辅助宏
#define ALIGN_UP(v, n) (((v) + (n) - 1) & ~((n) - 1))
#define SIZE_TO_BLOCKS(size) (((size) + HOST_LBA_SIZE - 1) / HOST_LBA_SIZE)

// HugePage 大小（根据你的系统配置）
#define HUGEPAGE_SIZE_2MB           (2 * 1024 * 1024)  // 2MB
#define ALIGN_2MB(size)             ALIGN_UP(size, HUGEPAGE_SIZE_2MB)

// 全局限制
#define MAX_XCODER_SESSIONS         16
#define MAX_INSTANCES_PER_SESSION   8

// 默认视频参数
#define DEFAULT_FRAME_WIDTH         1920
#define DEFAULT_FRAME_HEIGHT        1080

// 根据默认分辨率计算 NV12 帧大小
// NV12: Y平面 + UV半平面 = width * height + width * height / 2 = width * height * 3/2
#define DEFAULT_NV12_FRAME_SIZE     (DEFAULT_FRAME_WIDTH * DEFAULT_FRAME_HEIGHT * 3 / 2)  // 1920*1080*1.5 = 3,110,400 字节

// --- 常量定义（基于默认分辨率自动计算，2MB 对齐）---
// 最大帧缓冲区大小 = 默认帧大小 + 20%裕量，并对齐到 2MB
// 20%裕量是为了处理边界情况和对齐需求
// 2MB 对齐确保分配时正好占用整数个 HugePage
#define XCODER_MAX_BYTES_PER_FRAME  ALIGN_2MB(DEFAULT_NV12_FRAME_SIZE * 12 / 10)  // 3.1MB * 1.2 ≈ 3.7MB，对齐到 2MB = 4MB

// 根据帧大小计算需要的 blocks 数（4KB blocks，用于 NVMe LBA 计算）
#define XCODER_MAX_BLOCKS_PER_FRAME SIZE_TO_BLOCKS(XCODER_MAX_BYTES_PER_FRAME)

// 码流缓冲区大小 = 帧大小的 1.5 倍，并对齐到 2MB
// 对于 1080p 视频，4MB 码流缓冲区通常足够
// #define XCODER_MAX_BITSTREAM_SIZE   ALIGN_2MB(DEFAULT_NV12_FRAME_SIZE * 15 / 10)  // 3.1MB * 1.5 = 4.65MB，对齐到 2MB = 6MB
// 现在由于是在预热的时候提前分配的内存，编码解码使用的同一个queue，所以给码流和帧分配的大小是一致的
#define XCODER_MAX_BITSTREAM_SIZE   XCODER_MAX_BYTES_PER_FRAME

// 会话总 blocks（可选，用于会话级别内存限制）
#define XCODER_BLOCKS_PER_SESSION    262144  // 1GB / 4KB


// 编解码器类型
enum xcoder_codec_type {
    XCODER_CODEC_TYPE_NONE          = 0x00,
    XCODER_CODEC_TYPE_E2E_ENCODER  = 0x01,
    XCODER_CODEC_TYPE_E2E_DECODER  = 0x02,
    XCODER_CODEC_TYPE_E2E_VIDEO_ENCODER  = 0x03,
    XCODER_CODEC_TYPE_E2E_VIDEO_DECODER  = 0x04,
    XCODER_CODEC_TYPE_POSTPROC        = 0x05, // 缩放器
    // ... 其他类型
};

// ** 新增：帧格式枚举 **
// 定义各种帧格式
enum xcoder_frame_format {
    XCODER_FRAME_FORMAT_NONE    = 0,  // 未指定或无效格式
    XCODER_FRAME_FORMAT_YUV420P = 1,  // YUV 4:2:0 平面格式
    XCODER_FRAME_FORMAT_NV12    = 2,  // YUV 4:2:0 半平面格式
    XCODER_FRAME_FORMAT_RGB     = 3,  // RGB 24位格式 (例如 RGB888)
    XCODER_FRAME_FORMAT_RGBA    = 4,  // RGBA 32位格式
    // ... 可以根据需要添加更多格式，例如 MJPEG, H.264 等编码格式
};

// 实例配置
typedef struct xcoder_instance_config {
    enum xcoder_codec_type type;
    uint32_t width;
    uint32_t height;
    uint32_t bitrate_kbps; // 编码器特有
    uint32_t fps;          // 帧率，例如 30, 60
    enum xcoder_frame_format input_format; // <--- 这里改为使用新的枚举类型
    uint32_t gop_size;     // 关键帧间隔
    uint32_t reserved;     // 保留字段，对齐用
} xcoder_instance_config_t;

typedef struct xcoder_instance_dec_config {
    enum xcoder_codec_type type;
    uint32_t width;
    uint32_t height;
    uint32_t fps;          // 帧率，例如 30, 60
    enum xcoder_frame_format output_format; // <--- 这里改为使用新的枚举类型
    uint32_t reserved;     // 保留字段，对齐用
} xcoder_instance_dec_config_t;

// 实例结构体
typedef struct xcoder_instance {
    uint32_t instance_id;
    bool is_active;
    xcoder_instance_config_t config;
    xcoder_instance_dec_config_t dec_config;
    uint32_t processed_frames; // 统计处理的帧数
	
	// !!! --- 新增：用于 Nuvcoder Output Poller 线程的字段 --- !!!
    nuvcoder_poller_args_t *poller_args; // 指向线程参数的指针
    bool poller_active;                  // 标志 Poller 线程是否正在运行
    // !!! --- 新增部分结束 --- !!!
	
    size_t last_output_len;    // 上次处理生成的输出数据长度

    size_t expected_frame_size; // 用于校验 Host 传入的帧长度

    nuvcoder_codec_handle_t nuvcoder_handle; // Nuvcoder Codec API 句柄

    pthread_mutex_t mutex;     // 保护实例状态的互斥锁

    bool input_buffer_available;  // 指示此实例的输入缓冲区是否准备好接收数据
    bool output_data_available;   // 指示此实例的输出缓冲区是否有编码完成的输出数据可用
    size_t output_packet_size;    // 如果 output_data_available 为 true，则为下一个输出包的大小

    nuvcoder_queue_entry_t *current_write_entry;  
    
    // !!! 🎯 关键修改：改为指针，从池中获取 !!!
    nuvcoder_queue_t *queue; // 指向从池中获取的队列，而不是嵌入对象
    
} xcoder_instance_t;

// 会话结构体 (保持不变)
typedef struct xcoder_session {
    uint32_t session_id;
    bool is_active;
    uint32_t num_instances;
    xcoder_instance_t *instances[MAX_INSTANCES_PER_SESSION]; // 实例数组

    pthread_mutex_t mutex;     // 保护会话状态的互斥锁
} xcoder_session_t;

// 全局状态结构体
typedef struct xcoder_global_state {
    uint32_t next_session_id; // 下一个可用的 session ID 提示
    xcoder_session_t *sessions[MAX_XCODER_SESSIONS]; // 会话数组

    pthread_mutex_t mutex;     // 保护全局状态的互斥锁
} xcoder_global_state_t;

// 全局状态实例 (在 .c 文件中定义，这里是声明)
extern xcoder_global_state_t g_xcoder_state;

// 函数声明 (为了消除 -Wmissing-declarations 警告，通常会在一个单独的 .h 文件中进行声明)
// 例如，如果有一个 vsc_modules/my_vsc_handlers.h，你可以把这些声明放在那里。
// 这里为了快速解决，我直接放在这里，但更推荐为每个 .c 文件创建对应的 .h 文件。
int xcoder_vsc_open_hdlr(struct spdk_nvmf_request *req);
int xcoder_vsc_close_hdlr(struct spdk_nvmf_request *req);
int xcoder_vsc_config_hdlr(struct spdk_nvmf_request *req);
int xcoder_vsc_query_hdlr(struct spdk_nvmf_request *req);


// 函数声明
void xcoder_global_state_init(void);
xcoder_session_t* xcoder_find_session(uint32_t session_id);
xcoder_instance_t* xcoder_find_instance(xcoder_session_t *session, uint32_t instance_id);

int nuvcoder_send_frame(nuvcoder_queue_entry_t *entry);

typedef enum {
    XCODER_BITSTREAM_STATUS_SUCCESS = 0,
    XCODER_BITSTREAM_STATUS_FAILED = -1,
    XCODER_BITSTREAM_STATUS_NOT_READY = 1, // 新增：表示尚未完成
    XCODER_BITSTREAM_STATUS_INVALID_ARGS = -2 // 无效参数
} xcoder_bitstream_status_t;
xcoder_bitstream_status_t xcoder_get_bitstream_nonblocking(nuvcoder_queue_entry_t *entry, size_t *out_actual_length, void **out_buffer_dma);

#endif // XCODER_TYPES_H
