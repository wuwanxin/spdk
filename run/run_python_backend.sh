#!/bin/bash

# ==============================================================================
# --- 1. 设置 Conda 环境路径和激活 ---
# 确保你的conda已经初始化，通常在你 ~/.bashrc 或 ~/.profile 中
# source /path/to/your/miniconda3/etc/profile.d/conda.sh

# 如果你确定conda已经安装在 /home/nvdia/miniconda3
CONDA_BASE="/home/nvdia/miniconda3"
CONDA_ENV_NAME="compressai_env" # 你的conda环境名称

# 尝试激活conda环境。注意：这只影响当前shell的环境变量，
# 使用 sudo env 启动时可能不会完全继承。
# 但是，我们仍然手动构建 PATH 和 PYTHONPATH。
source "${CONDA_BASE}/etc/profile.d/conda.sh"
conda activate "${CONDA_ENV_NAME}"

# 获取激活后的 Python 解释器路径
PYTHON_BIN="${CONDA_BASE}/envs/${CONDA_ENV_NAME}/bin/python"
# 获取激活后的 Conda 环境的库路径
CONDA_ENV_LIB_PATH="${CONDA_BASE}/envs/${CONDA_ENV_NAME}/lib"
# 获取 Conda 环境中 site-packages 的路径，特别是 torch/lib
CONDA_TORCH_LIB_PATH="${CONDA_ENV_LIB_PATH}/python3.8/site-packages/torch/lib" # 假设Python 3.8

# ==============================================================================
# --- 2. 构建 LD_LIBRARY_PATH ---
# LD_LIBRARY_PATH 是给C/C++动态链接库用的
# 重要的顺序：Conda环境的lib应优先，以确保使用Conda提供的库版本
# 其次是你的nuvcoder_core库，然后是CUDA，最后再考虑系统默认库。

# 初始化 LD_LIBRARY_PATH 为空，避免继承不必要的系统路径
export LD_LIBRARY_PATH="" 

# 添加 Conda 环境中 Python 解释器相关的库路径
LD_LIBRARY_PATH="${CONDA_ENV_LIB_PATH}"

# 添加 libnuvcoder_codec_core.so 所在的目录
LD_LIBRARY_PATH="${LD_LIBRARY_PATH}:/home/nvdia/working/wwx/spdk/app/nvmf_tgt/libnuvcoder_core/lib"

# 添加 CUDA 库所在的目录
LD_LIBRARY_PATH="${LD_LIBRARY_PATH}:/usr/local/cuda-11.4/targets/aarch64-linux/lib"

# 添加 PyTorch 库所在的目录 (如果 torch/lib 独立于 CONDA_ENV_LIB_PATH 且需要)
LD_LIBRARY_PATH="${LD_LIBRARY_PATH}:${CONDA_TORCH_LIB_PATH}"

# 如果你需要保留系统默认的 LD_LIBRARY_PATH，可以在最后追加
# 但通常为了隔离环境，不建议这样做，除非你知道必须保留某些系统库。
# LD_LIBRARY_PATH="${LD_LIBRARY_PATH}:/usr/lib/aarch64-linux-gnu" # 示例，根据需要添加

# ==============================================================================
# --- 3. 构建 PATH 和 PYTHONPATH ---
# PATH 是给可执行文件查找用的，确保找到正确的 Python 解释器
# PYTHONPATH 是给 Python 解释器查找模块用的

export PATH="${CONDA_BASE}/envs/${CONDA_ENV_NAME}/bin:${PATH}" # 将conda bin目录放在最前面

# PYTHONPATH 需要包含你的 Python 后端脚本目录，以及任何其他需要被 Python 找到的自定义模块
export PYTHONPATH="/home/nvdia/working/wwx/nuvcoder_core/backend/model/inference_jetson:${PYTHONPATH}"

# ==============================================================================
# --- 4. 打印检查最终的环境变量 ---
echo "--- Environment Variables for SPDK target ---"
echo "PATH: ${PATH}"
echo "LD_LIBRARY_PATH: ${LD_LIBRARY_PATH}"
echo "PYTHONPATH: ${PYTHONPATH}"
echo "---------------------------------------------"

# ==============================================================================
# --- 5. 使用 sudo env 启动 nvmf_tgt ---
# 使用 sudo env 传递 PATH, LD_LIBRARY_PATH 和 PYTHONPATH
# nvmf_tgt 是一个 C/C++ 程序，它会动态加载你的 libnuvcoder_codec_core.so
# 这个 .so 文件内部会调用 Python。因此，nvmf_tgt 的进程需要正确的 PATH 和 PYTHONPATH
# 来初始化 Python 解释器和查找 Python 模块。

sudo env PATH="${PATH}" \
         LD_LIBRARY_PATH="${LD_LIBRARY_PATH}" \
         PYTHONPATH="${PYTHONPATH}" \
         ../spdk/build/bin/nvmf_tgt -c target_config.json 

# 运行完后可以停用conda环境 (如果之前激活过)
conda deactivate
