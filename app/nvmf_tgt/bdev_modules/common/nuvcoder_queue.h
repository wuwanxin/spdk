// nuvcoder_queue.h (修改后)
#ifndef NUVCODER_QUEUE_H
#define NUVCODER_QUEUE_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h> // For pthread_mutex_t, pthread_cond_t
#include <stddef.h>  // For size_t

#define XCODER_QUEUE_DEPTH 16
struct xcoder_instance; // 前向声明，避免循环依赖

/**
 * @brief 队列中的单个请求条目。
 *        它存储一个通用指针 user_context，以及异步操作所需的输入/输出缓冲区和状态。
 */
typedef struct nuvcoder_queue_entry {
    void *user_context;               // 指向用户自定义的请求上下文（由提交请求方传入）
    void *input_buffer_dma;           // SPDK DMA 输入缓冲区指针，在 init 时分配
    size_t input_length;              // 输入数据长度（实际写入的长度）
    void *output_buffer_dma;          // SPDK DMA 输出缓冲区指针，在 init 时分配
    size_t output_actual_length;      // 实际输出长度（由异步处理线程填充）
    int result_code;                  // 异步操作的处理结果
    bool is_completed;                // 标记此请求是否已完成
    pthread_mutex_t mutex;            // 保护此 entry 的状态
    pthread_cond_t cond;              // 用于等待此 entry 完成
    struct xcoder_instance *parent_instance; // 指向所属的 xcoder_instance，用于日志上下文
    int index;                        // 此 entry 在其所属队列数组中的索引，方便调试和查找
} nuvcoder_queue_entry_t;

/**
 * @brief 异步请求处理队列结构体。
 *        实现生产者-消费者模型，管理固定数量的请求条目。
 *        此结构体应嵌入到 xcoder_instance_t 中，或者由 xcoder_instance_t 持有其指针。
 */
typedef struct nuvcoder_queue {
    nuvcoder_queue_entry_t entries[XCODER_QUEUE_DEPTH]; // 请求条目数组
    int head; // 队头索引 (下一个要提交的请求)
    int tail; // 队尾索引 (下一个要读取结果的请求)
    int count; // 当前队列中的请求数量 (已提交但未读取结果的)

    pthread_mutex_t lock;           // 保护队列结构体本身（head, tail, count）
    pthread_cond_t producer_cond;   // 生产者 (提交请求方) 等待队列空间
    pthread_cond_t consumer_cond;   // 消费者 (读取结果方) 等待请求完成
} nuvcoder_queue_t;

/**
 * @brief 初始化一个 Nuvcoder 异步请求队列。
 *        会为每个 entry 分配独立的 DMA 缓冲区。
 *
 * @param queue 待初始化的队列指针。
 * @param instance_id 用于日志记录的实例 ID。
 * @param input_max_frame_size 每个 entry 输入缓冲区的最大帧大小。
 * @param output_max_frame_size 每个 entry 输出缓冲区的最大帧大小。
 * @return 0 on success, -1 on error.
 */
int nuvcoder_queue_init(nuvcoder_queue_t *queue, uint32_t instance_id, size_t input_max_frame_size, size_t output_max_frame_size) ;

/**
 * @brief 销毁一个 Nuvcoder 异步请求队列，并释放相关资源。
 *        会等待所有未完成的请求，并释放为每个 entry 分配的 DMA 缓冲区。
 *
 * @param queue 待销毁的队列指针。
 * @param instance_id 用于日志记录的实例 ID。
 */
void nuvcoder_queue_destroy(nuvcoder_queue_t *queue, uint32_t instance_id);

/**
 * @brief 从队列中获取一个可用的 entry 并初始化。
 *        此函数是阻塞的，直到队列中有空间可用。
 *        调用者应使用返回的 entry 进行数据拷贝和异步操作的提交。
 *
 * @param queue 队列指针。
 * @param parent_instance 指向拥有此队列的 xcoder_instance 实例，用于设置 entry 的 `parent_instance` 字段。
 * @param user_ctx 用户自定义的上下文指针，将存储在 entry 中，并在请求完成后返回给调用者。
 * @return nuvcoder_queue_entry_t* 如果成功获取到 entry，否则返回 NULL。
 */
nuvcoder_queue_entry_t* nuvcoder_queue_get_entry_and_prepare(nuvcoder_queue_t *queue, struct xcoder_instance *parent_instance, void *user_ctx);

/**
 * @brief 通知队列一个 entry 已经被提交给底层异步操作，并正式将其添加到队列的“待处理”状态。
 *        此函数应该在底层异步操作调用成功后调用。
 *        它会递增队列的 head 和 count。
 *
 * @param queue 队列指针。
 * @param entry 之前通过 nuvcoder_queue_get_entry_and_prepare 获取的 entry。
 */
void nuvcoder_queue_notify_submit(nuvcoder_queue_t *queue, nuvcoder_queue_entry_t *entry);

/**
 * @brief 获取队列尾部的entry（不消费）
 *        用于检查是否有已完成的entry可读
 *
 * @param queue 队列指针
 * @return nuvcoder_queue_entry_t* 尾部entry指针，NULL表示队列为空
 */
nuvcoder_queue_entry_t* nuvcoder_queue_peek_tail(nuvcoder_queue_t *queue);

/**
 * @brief 等待并获取队列中下一个已完成的异步操作的结果。
 *        此函数是阻塞的，直到有操作完成。
 *        返回entry指针，调用者负责在读取后调用 release_entry。
 *
 * @param queue 队列指针。
 * @param entry_out 输出参数，返回已完成的entry指针。
 * @return 异步操作的处理结果（例如 0 表示成功，非 0 表示错误）。
 */
int nuvcoder_queue_read_output_wait(nuvcoder_queue_t *queue, nuvcoder_queue_entry_t **entry_out);

/**
 * @brief 释放已读取的entry，使其可以被重用。
 *        在调用者读取完输出数据后调用。
 *
 * @param queue 队列指针。
 * @param entry 要释放的entry。
 */
void nuvcoder_queue_release_entry(nuvcoder_queue_t *queue, nuvcoder_queue_entry_t *entry);

/**
 * @brief 由外部模块（如 xcoder_nvmf_target）调用，通知队列一个 entry 已完成。
 *        此函数是队列处理异步完成事件的唯一入口，它会更新 entry 的状态并唤醒等待的线程。
 *
 * @param queue 队列指针。
 * @param entry 指向已完成的队列条目。
 * @param result_code 异步操作的处理结果。
 * @param actual_output_len 实际输出数据长度。
 */
void nuvcoder_queue_mark_completed(nuvcoder_queue_t *queue, nuvcoder_queue_entry_t *entry, int result_code, size_t actual_output_len);

//======pool======
#define NUVCODER_QUEUE_POOL_SIZE 1
/**
 * @brief 创建全局队列池（预热时调用）
 * @param input_size 输入缓冲区大小
 * @param output_size 输出缓冲区大小
 * @return 0成功，-1失败
 */
int nuvcoder_queue_pool_create(size_t input_size, size_t output_size);

/**
 * @brief 获取一个空闲队列handler
 * @param instance_id 实例ID
 * @return 队列指针，NULL表示无空闲队列
 */
nuvcoder_queue_t* nuvcoder_queue_pool_get_handler(uint32_t instance_id);

/**
 * @brief 重置队列状态（释放handler）
 * @param queue 要重置的队列
 * @param instance_id 实例ID
 */
void nuvcoder_queue_pool_reset_handler(nuvcoder_queue_t *queue, uint32_t instance_id);

/**
 * @brief 销毁全局队列池（程序退出时调用）
 */
void nuvcoder_queue_pool_destroy(void);

#endif // NUVCODER_QUEUE_H