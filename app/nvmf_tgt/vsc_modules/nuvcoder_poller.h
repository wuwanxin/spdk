#ifndef NUVCODER_POLLER_H
#define NUVCODER_POLLER_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

// 前向声明，避免循环依赖
struct xcoder_instance; // 声明 xcoder_instance_t 结构体

/**
 * @brief Nuvcoder Output Poller 线程参数结构体。
 *        每个 xcoder_instance 对应一个 Poller 线程。
 */
typedef struct nuvcoder_poller_args {
    struct xcoder_instance *instance; // 指向所属的 xcoder_instance
    pthread_t thread_id;              // 存储线程ID
    volatile bool stop_flag;          // 停止线程的标志，使用 volatile 确保多线程可见性
} nuvcoder_poller_args_t;

/**
 * @brief Nuvcoder 输出轮询线程函数。
 *        负责持续从 Nuvcoder 后端拉取已完成的编码/解码结果，并更新队列条目状态。
 * @param arg 指向 nuvcoder_poller_args_t 结构体的指针。
 * @return NULL。
 */
void *nuvcoder_output_poller_thread(void *arg);


#endif // NUVCODER_POLLER_H
