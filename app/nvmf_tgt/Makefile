#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2016 Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk
include $(SPDK_ROOT_DIR)/mk/spdk.modules.mk

APP = nvmf_tgt

C_SRCS := nvmf_main.c \
		  warmup.c \
          vsc_modules/my_vsc_handlers.c \
		  vsc_modules/nuvcoder_poller.c \
          bdev_modules/nuvcoder/bdev_nuvcoder.c \
		  bdev_modules/common/nuvcoder_queue.c

# --------------------------------------------------------------------------
# NUVCoder 库的路径配置
# --------------------------------------------------------------------------

# NUVCoder 安装路径（根据您的CMake安装目录修改）
NUVCODER_INSTALL_DIR := $(CURDIR)/libnuvcoder_core
NUVCODER_LIB_DIR := $(NUVCODER_INSTALL_DIR)/lib
NUVCODER_INCLUDE_DIR := $(NUVCODER_INSTALL_DIR)/include

# --------------------------------------------------------------------------
# 添加头文件路径
# --------------------------------------------------------------------------
CFLAGS += -I$(CURDIR)
CFLAGS += -I$(CURDIR)/vsc_modules
CFLAGS += -I$(CURDIR)/bdev_modules/common
CFLAGS += -I$(CURDIR)/bdev_modules/nuvcoder

# 添加 NUVCoder 头文件路径
CFLAGS += -I$(NUVCODER_INCLUDE_DIR)
CFLAGS += -I$(NUVCODER_INCLUDE_DIR)/backend
CFLAGS += -I$(NUVCODER_INCLUDE_DIR)/ascend_core

# --------------------------------------------------------------------------
# MindX SDK 路径配置（根据您的CMakeLists.txt）
# --------------------------------------------------------------------------
MINDX_SDK_PATH := /root/MindX_SDK/mxVision
CFLAGS += -I$(MINDX_SDK_PATH)/include
CFLAGS += -I$(MINDX_SDK_PATH)/opensource/include
CFLAGS += -I$(MINDX_SDK_PATH)/opensource/include/opencv4

# --------------------------------------------------------------------------
# PyTorch/LibTorch 路径配置（根据您的CMakeLists.txt）
# --------------------------------------------------------------------------
TORCH_LIB_DIR := /usr/local/lib64/python3.9/site-packages/torch/lib
TORCH_INCLUDE_DIR := /usr/local/lib64/python3.9/site-packages/torch/include

CFLAGS += -I$(TORCH_INCLUDE_DIR)
CFLAGS += -I$(TORCH_INCLUDE_DIR)/torch/csrc/api/include

# ==================== 🎯 新增：添加 DPDK 头文件路径 ====================
# SPDK 使用 DPDK 环境，通过 pkg-config 获取 DPDK 编译参数
DPDK_CFLAGS := $(shell pkg-config --cflags libdpdk 2>/dev/null)
ifneq ($(DPDK_CFLAGS),)
    CFLAGS += $(DPDK_CFLAGS)
else
    # 如果没有 pkg-config，尝试常见的 DPDK 安装路径
    ifneq ($(wildcard /usr/local/include/dpdk/rte_malloc.h),)
        CFLAGS += -I/usr/local/include/dpdk
    else ifneq ($(wildcard /usr/include/dpdk/rte_malloc.h),)
        CFLAGS += -I/usr/include/dpdk
    else
        # DPDK 头文件可能在内核构建目录中（SPDK 构建时）
        ifneq ($(SPDK_ROOT_DIR)/dpdk/build/include,)
            CFLAGS += -I$(SPDK_ROOT_DIR)/dpdk/build/include
        else
            $(warning "DPDK headers not found. Please ensure DPDK is properly installed.")
        endif
    endif
endif

# 定义 DPDK 可用宏
CFLAGS += -DSPDK_HAVE_DPDK
# ==================== DPDK 路径添加结束 ====================

# --------------------------------------------------------------------------
# 添加链接目录
# --------------------------------------------------------------------------
LDFLAGS += -L$(NUVCODER_LIB_DIR)
LDFLAGS += -L$(MINDX_SDK_PATH)/lib
LDFLAGS += -L$(MINDX_SDK_PATH)/opensource/lib
LDFLAGS += -L$(TORCH_LIB_DIR)

# --------------------------------------------------------------------------
# 添加运行时路径 (rpath)
# --------------------------------------------------------------------------
# NUVCoder 库的运行时路径
LDFLAGS += -Wl,-rpath=$(NUVCODER_LIB_DIR)
# MindX SDK 库的运行时路径
LDFLAGS += -Wl,-rpath=$(MINDX_SDK_PATH)/lib
LDFLAGS += -Wl,-rpath=$(MINDX_SDK_PATH)/opensource/lib
# PyTorch 库的运行时路径
LDFLAGS += -Wl,-rpath=$(TORCH_LIB_DIR)

# --------------------------------------------------------------------------
# 链接 NUVCoder 核心库及其依赖
# --------------------------------------------------------------------------

# NUVCoder 核心库
LIBS += -lnuvcoder_codec_core

# PyTorch/LibTorch 依赖（根据您的CMakeLists.txt）
LIBS += -lc10
LIBS += -ltorch_cpu
LIBS += -ltorch

# MindX SDK 依赖
LIBS += -lmxbase
LIBS += -lopencv_world
LIBS += -lopencv_core
LIBS += -lopencv_imgproc
LIBS += -lopencv_imgcodecs

# 系统库依赖
LIBS += -lpthread
LIBS += -ldl
LIBS += -lrt

# --------------------------------------------------------------------------
# 定义编译宏（根据您的CMakeLists.txt中的平台配置）
# --------------------------------------------------------------------------
# 启用 Ascend 后端
CFLAGS += -DNUVCODER_PLATFORM_ASCEND=1
# 如果以后需要启用 Python 后端，可以添加：
# CFLAGS += -DNUVCODER_PLATFORM_ASCEND_PYTHON=1

# 设置 C++ ABI 兼容性（与您的CMakeLists.txt保持一致）
CFLAGS += -D_GLIBCXX_USE_CXX11_ABI=0

# --------------------------------------------------------------------------
# 如果 NUVCoder 使用了 OpenMP，添加 OpenMP 支持
# --------------------------------------------------------------------------
# CFLAGS += -fopenmp
# LDFLAGS += -fopenmp

# --------------------------------------------------------------------------
# 调试选项（可选）
# --------------------------------------------------------------------------
# CFLAGS += -g -O0

# --------------------------------------------------------------------------
# SPDK 库列表
# --------------------------------------------------------------------------

SPDK_LIB_LIST = $(ALL_MODULES_LIST) event event_nvmf

ifeq ($(SPDK_ROOT_DIR)/lib/env_dpdk,$(CONFIG_ENV))
SPDK_LIB_LIST += env_dpdk_rpc
endif

ifeq ($(OS),Linux)
SPDK_LIB_LIST += event_nbd
endif

include $(SPDK_ROOT_DIR)/mk/spdk.app.mk

install: $(APP)
	$(INSTALL_APP)

uninstall:
	$(UNINSTALL_APP)

# --------------------------------------------------------------------------
# 添加自定义构建后操作（可选）
# --------------------------------------------------------------------------
# 构建完成后，可以显示链接的库信息
$(APP): post_build_info

post_build_info:
	@echo "========================================="
	@echo "NUVCoder Integration Information"
	@echo "========================================="
	@echo "NUVCoder Library: $(NUVCODER_LIB_DIR)/libnuvcoder_codec_core.so"
	@echo "MindX SDK Path: $(MINDX_SDK_PATH)"
	@echo "PyTorch Path: $(TORCH_LIB_DIR)"
	@echo "DPDK CFLAGS: $(DPDK_CFLAGS)"
	@echo "========================================="

# 将 post_build_info 添加到依赖
$(APP): post_build_info