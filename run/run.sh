#!/bin/bash

spdk_home="/root/spdk-git/"

set -e  # 出错时退出

echo "=== Starting NVMe-oF Target ==="

# 🎯 设置大页数量为 4096
echo "Setting HugePages to 4096..."
CURRENT_HUGEPAGES=$(cat /proc/sys/vm/nr_hugepages)
if [ "$CURRENT_HUGEPAGES" -ne "4096" ]; then
    echo "  Current: $CURRENT_HUGEPAGES, setting to 4096..."
    echo 4096 > /proc/sys/vm/nr_hugepages
    sleep 1
else
    echo "  HugePages already set to 4096"
fi

# 验证大页设置
TOTAL_HUGE=$(cat /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages)
FREE_HUGE=$(cat /sys/kernel/mm/hugepages/hugepages-2048kB/free_hugepages)
echo "  HugePages: $FREE_HUGE free / $TOTAL_HUGE total"

# 检查配置文件
if [ ! -f "target_config.json" ]; then
    echo "Error: target_config.json not found!"
    exit 1
fi

# 检查可执行文件
if [ ! -x "${spdk_home}/build/bin/nvmf_tgt" ]; then
    echo "Error: nvmf_tgt not found or not executable!"
    exit 1
fi

# 清理旧的 target 进程
if pgrep -f nvmf_tgt > /dev/null; then
    echo "Stopping existing nvmf_tgt process..."
    sudo pkill -f nvmf_tgt
    sleep 3
fi

echo "Setup spdk env..."
${spdk_home}/scripts/setup.sh
# 启动 target
echo "Starting nvmf_tgt..."
${spdk_home}/build/bin/nvmf_tgt -c target_config.json > spdk.log 2>&1 &
TGT_PID=$!

# 等待 target 启动并检查是否成功
echo "Waiting for target to initialize..."
MAX_WAIT=10
for i in $(seq 1 $MAX_WAIT); do
    if kill -0 $TGT_PID 2>/dev/null; then
        # 检查是否已经开始监听
        if grep -q "NVMf target started" spdk.log 2>/dev/null; then
            echo "Target started successfully!"
            break
        fi
    else
        echo "Error: Target failed to start!"
        cat spdk.log
        exit 1
    fi
    sleep 1
done

# 运行 Python 脚本
echo "Running manual_setup.py..."
if python manual_setup.py; then
    echo "Setup completed successfully!"
else
    echo "Error: manual_setup.py failed!"
    kill $TGT_PID
    exit 1
fi

echo ""
echo "=== All Done ==="
echo "Target PID: $TGT_PID"
echo "Log file: spdk.log"
echo ""
echo "To monitor: tail -f spdk.log"
echo "To stop: bash stop.sh"