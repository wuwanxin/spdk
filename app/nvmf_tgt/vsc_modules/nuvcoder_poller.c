#include "nuvcoder_poller.h"
#include "nuvcoder_types.h"
#include "nuvcoder_queue.h"
#include "nuvcoder_codec_api.h"
#include "spdk/log.h"
#include "spdk/stdinc.h"

void *nuvcoder_output_poller_thread(void *arg) {
    nuvcoder_poller_args_t *poller_args = (nuvcoder_poller_args_t *)arg;
    xcoder_instance_t *instance = poller_args->instance;
    uint32_t instance_id = instance->instance_id;
    
    // !!! 关键修复1：提前获取队列指针，避免后续误用 &instance->queue !!!
    nuvcoder_queue_t *queue = instance->queue;
    
    printf("Nuvcoder Output Poller thread started for instance %u (queue=%p).\n", 
           instance_id, queue);

    // 确保 Nuvcoder 句柄有效，否则线程无法工作
    if (instance->nuvcoder_handle == NULL) {
        printf("Poller: Nuvcoder handle is NULL for instance %u. Thread terminating.\n", instance_id);
        poller_args->stop_flag = true;
        return NULL;
    }
    
    // 确保队列有效
    if (queue == NULL) {
        printf("Poller: Queue is NULL for instance %u. Thread terminating.\n", instance_id);
        poller_args->stop_flag = true;
        return NULL;
    }

    while (!poller_args->stop_flag) {
        uint32_t actual_output_len = 0;
        int codec_internal_result = 0;
        void *returned_user_context = NULL;

        int api_call_rc;

        // 根据实例类型选择不同的API
        if (instance->config.type == XCODER_CODEC_TYPE_E2E_VIDEO_DECODER) {
            // 解码器：使用 nuvcoder_decoder_read_output
            api_call_rc = nuvcoder_decoder_read_output(
                instance->nuvcoder_handle,
                &actual_output_len,
                &codec_internal_result,
                &returned_user_context,
                0 // 0表示立即返回，不等待
            );
        } else {
            // 编码器：使用 nuvcoder_codec_read_output
            api_call_rc = nuvcoder_codec_read_output(
                instance->nuvcoder_handle,
                &actual_output_len,
                &codec_internal_result,
                &returned_user_context,
                0 // 0表示立即返回，不等待
            );
        }

        if (api_call_rc == NUVCODER_STATUS_SUCCESS) {
            // 成功获取到一个已完成的帧结果
            nuvcoder_queue_entry_t *entry = (nuvcoder_queue_entry_t *)returned_user_context;

            // 验证 user_context 是否合法且匹配
            if (entry == NULL) {
                printf("Poller: WARNING - NULL user context returned for instance %u\n", instance_id);
                continue;
            }
            
            // 验证entry的parent_instance是否正确
            if (entry->parent_instance != instance) {
                printf("Poller: WARNING - User context mismatch for instance %u. "
                       "Expected parent %p, got %p. Attempting fallback search.\n",
                       instance_id, instance, entry->parent_instance);
                
                // 尝试通过遍历队列找到正确的 entry（最后的补救措施）
                nuvcoder_queue_entry_t *found_entry = NULL;
                
                pthread_mutex_lock(&queue->lock);
                for (int i = 0; i < XCODER_QUEUE_DEPTH; i++) {
                    nuvcoder_queue_entry_t *potential_entry = &queue->entries[i];
                    // 查找未完成且属于此实例的entry
                    if (!potential_entry->is_completed && 
                        potential_entry->parent_instance == instance) {
                        found_entry = potential_entry;
                        printf("Poller: Found matching entry %d by search as fallback\n", i);
                        break;
                    }
                }
                pthread_mutex_unlock(&queue->lock);
                
                if (found_entry) {
                    entry = found_entry;
                } else {
                    printf("Poller: CRITICAL - Could not find any matching entry for instance %u. Dropping frame.\n", instance_id);
                    continue;
                }
            }

            // !!! 关键修复2：使用 queue 指针，而不是 &instance->queue !!!
            printf("Poller: Got completed entry %d for instance %u (result %d, output len %u).\n",
                   entry->index, instance_id, codec_internal_result, actual_output_len);

            // 标记队列 entry 完成
            nuvcoder_queue_mark_completed(queue, entry, codec_internal_result, actual_output_len);
            
            // 更新实例的最后输出长度（用于查询）
            instance->last_output_len = actual_output_len;

        } else if (api_call_rc == NUVCODER_STATUS_NO_OUTPUT_AVAILABLE) {
            // 没有输出可用，短暂休眠避免CPU空转
            usleep(1000); // 1ms
        } else if (api_call_rc == NUVCODER_STATUS_FAIL) {
            // API调用失败，但可能是临时性的
            printf("Poller: API call returned FAIL for instance %u, continuing...\n", instance_id);
            usleep(10000); // 10ms，失败时休眠稍长
        } else {
            // 其他未知错误
            printf("Poller: %s API call returned unknown code %d for instance %u.\n",
                   (instance->config.type == XCODER_CODEC_TYPE_E2E_VIDEO_DECODER) ? 
                   "nuvcoder_decoder_read_output" : "nuvcoder_codec_read_output",
                   api_call_rc, instance_id);
            
            usleep(10000); // 10ms
        }
    }

    printf("Nuvcoder Output Poller thread stopped for instance %u.\n", instance_id);
    return NULL;
}