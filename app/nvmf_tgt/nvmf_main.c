
/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/nvmf.h"
#include "spdk/nvmf_cmd.h"
#include "spdk/log.h"
#include "spdk/string.h"

// 引入你定义的 VSC opcodes
#include "vsc_modules/nuvcoder_vsc_opcodes.h"

// 引入 bdev_nuvcoder 头文件
#include "bdev_modules/nuvcoder/bdev_nuvcoder.h"

// 引入预热模块头文件
#include "warmup.h"

// 声明你的 XCODER VSC 处理函数（假设这些函数在另一个文件中定义）
extern int xcoder_vsc_open_hdlr(struct spdk_nvmf_request *req);
extern int xcoder_vsc_close_hdlr(struct spdk_nvmf_request *req);
extern int xcoder_vsc_config_hdlr(struct spdk_nvmf_request *req);
extern int xcoder_vsc_query_hdlr(struct spdk_nvmf_request *req);

// 全局变量存储配置文件路径
static char *g_config_file = NULL;

static void
nvmf_usage(void)
{
    printf("\nNVMe-oF Target with Xcoder Support\n");
    printf("===================================\n");
    printf("Usage:\n");
    printf("  nvmf_tgt [SPDK_OPTIONS] [-x config_file]\n\n");
    printf("Custom options:\n");
    printf("  -x config_file        JSON configuration file for Xcoder modules\n");
    printf("  -H                    Show this usage message\n\n");
    printf("SPDK standard options:\n");
    printf("  -c config_file        SPDK JSON configuration file (SPDK standard)\n");
    printf("  -s size               Memory size in MB\n");
    printf("  ... other SPDK options ...\n");
}

static int
nvmf_parse_arg(int ch, char *arg)
{
    switch (ch) {
        case 'x':
            // Xcoder 专用配置文件
            if (g_config_file) {
                free(g_config_file);
            }
            g_config_file = strdup(arg);
            if (!g_config_file) {
                SPDK_ERRLOG("Failed to allocate memory for config file path\n");
                return -ENOMEM;
            }
            break;
        case 'H':
            nvmf_usage();
            exit(EXIT_SUCCESS);
        default:
            // 其他参数由SPDK处理
            break;
    }
    return 0;
}

static void
nvmf_tgt_started(void *arg1)
{
    int rc;
    
    printf("nvmf_tgt_started callback invoked.\n");

    // 1. 首先初始化 Xcoder Bdev 模块（在配置加载之前）
    printf("Initializing Xcoder Bdev module...\n");
    rc = bdev_xcoder_init();
    if (rc != 0) {
        SPDK_ERRLOG("Failed to initialize Xcoder Bdev module: %d\n", rc);
    } else {
        printf("Xcoder Bdev module initialized successfully.\n");
    }

    // 2. 注册 XCODER VSC 处理函数
    printf("Attempting to register XCODER VSC handlers.\n");

    spdk_nvmf_set_custom_admin_cmd_hdlr(VSC_XCODER_OPEN_OPCODE, xcoder_vsc_open_hdlr);
    printf("Successfully registered VSC handler for opcode 0x%x (VSC_XCODER_OPEN_OPCODE).\n", 
           VSC_XCODER_OPEN_OPCODE);

    spdk_nvmf_set_custom_admin_cmd_hdlr(VSC_XCODER_CLOSE_OPCODE, xcoder_vsc_close_hdlr);
    printf("Successfully registered VSC handler for opcode 0x%x (VSC_XCODER_CLOSE_OPCODE).\n", 
           VSC_XCODER_CLOSE_OPCODE);

    spdk_nvmf_set_custom_admin_cmd_hdlr(VSC_XCODER_CONFIG_OPCODE, xcoder_vsc_config_hdlr);
    printf("Successfully registered VSC handler for opcode 0x%x (VSC_XCODER_CONFIG_OPCODE).\n", 
           VSC_XCODER_CONFIG_OPCODE);

    spdk_nvmf_set_custom_admin_cmd_hdlr(VSC_XCODER_QUERY_OPCODE, xcoder_vsc_query_hdlr);
    printf("Successfully registered VSC handler for opcode 0x%x (VSC_XCODER_QUERY_OPCODE).\n", 
           VSC_XCODER_QUERY_OPCODE);

    printf("All XCODER VSC handlers registered. nvmf_tgt_started completed.\n");

    // 3. 执行预热并等待完成
    printf("\n=== Starting Xcoder Warmup ===\n");
    printf("This may take 10-20 seconds for first-time model loading...\n");
    
    rc = xcoder_start_warmup();
    if (rc == 0) {
        rc = xcoder_wait_warmup(30000);  // 最多等30秒
        if (rc == 0) {
            printf("✅ Warmup completed successfully. Target is ready.\n");
        } else {
            printf("⚠️  Warmup timed out. Target starting anyway.\n");
        }
    }
    
    printf("=== Target is now ready to accept connections ===\n\n");
}

int
main(int argc, char **argv)
{
    int rc;
    struct spdk_app_opts opts = {};

    printf("Starting SPDK application initialization for NVMe-oF Target with Xcoder support.\n");

    // 初始化 SPDK 应用选项
    spdk_app_opts_init(&opts, sizeof(opts));
    opts.name = "nvmf";
    
    // 使用 SPDK 的标准选项，不添加自定义选项到解析器中
    // 这里不设置 opts.json_config_file，让 SPDK 处理 -c 参数

    // 解析命令行参数
    // 注意：使用 "x:H" 作为自定义选项，SPDK 会自动处理标准选项
    if ((rc = spdk_app_parse_args(argc, argv, &opts, "x:H", NULL,
                                  nvmf_parse_arg, nvmf_usage)) !=
        SPDK_APP_PARSE_ARGS_SUCCESS) {
        SPDK_ERRLOG("Failed to parse application arguments: %d\n", rc);
        exit(rc);
    }

    printf("SPDK application arguments parsed. Starting application...\n");
    
    // 启动 SPDK 应用
    rc = spdk_app_start(&opts, nvmf_tgt_started, NULL);
    
    if (rc != 0) {
        SPDK_ERRLOG("spdk_app_start failed with error: %d\n", rc);
    } else {
        printf("SPDK application completed successfully.\n");
    }
    
    // 清理 Xcoder Bdev 模块
    printf("Cleaning up Xcoder Bdev module...\n");
    bdev_xcoder_fini();
    
    // 清理 SPDK 应用
    spdk_app_fini();
    
    // 清理全局变量
    if (g_config_file) {
        free(g_config_file);
    }
    
    printf("SPDK application finished with return code %d.\n", rc);
    
    return rc;
}