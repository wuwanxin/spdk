#!/bin/bash
# stop.sh - 强力的SPDK停止脚本

echo "=== Stopping SPDK NVMe-oF Target ==="

# 获取当前的巨页状态
total_huge=$(cat /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages)
free_before=$(cat /sys/kernel/mm/hugepages/hugepages-2048kB/free_hugepages)
echo "Before cleanup - HugePages: $free_before free / $total_huge total"

echo "1. Finding all SPDK processes..."
# 找出所有相关的进程
SPDK_PIDS=$(pgrep -f "nvmf_tgt|spdk|dpdk")
if [ -n "$SPDK_PIDS" ]; then
    echo "   Found PIDs: $SPDK_PIDS"
    
    echo "2. Sending SIGTERM to all SPDK processes..."
    for pid in $SPDK_PIDS; do
        echo "   Killing PID $pid with SIGTERM..."
        kill -15 $pid 2>/dev/null
    done
    sleep 3
    
    echo "3. Checking if processes still exist..."
    STILL_RUNNING=$(pgrep -f "nvmf_tgt|spdk|dpdk")
    if [ -n "$STILL_RUNNING" ]; then
        echo "   Processes still running: $STILL_RUNNING"
        echo "4. Sending SIGKILL to remaining processes..."
        for pid in $STILL_RUNNING; do
            echo "   Force killing PID $pid..."
            kill -9 $pid 2>/dev/null
        done
        sleep 2
    fi
else
    echo "   No SPDK processes found"
fi

echo "5. Resetting SPDK environment..."
if [ -f "../spdk/scripts/setup.sh" ]; then
    echo "   Running setup.sh reset..."
    ../spdk/scripts/setup.sh reset
fi

echo "6. Force cleaning hugepage files..."
# 更彻底的清理
echo "   Removing SPDK hugepage files..."
rm -f /dev/hugepages/spdk_* 2>/dev/null
rm -f /dev/hugepages/rtemap_* 2>/dev/null
rm -f /dev/hugepages/*map_* 2>/dev/null

# 如果挂载点在别处
if [ -d "/mnt/huge" ]; then
    echo "   Cleaning /mnt/huge..."
    rm -f /mnt/huge/spdk_* 2>/dev/null
    rm -f /mnt/huge/rtemap_* 2>/dev/null
fi

echo "7. Checking DPDK hugepage mappings..."
# 检查是否有进程还在使用巨页
for pid in /proc/[0-9]*; do
    if [ -f "$pid/smaps" ]; then
        if grep -q "Hugetlb" "$pid/smaps" 2>/dev/null; then
            pid_num=${pid#/proc/}
            cmd=$(cat $pid/cmdline 2>/dev/null | tr '\0' ' ' | cut -c 1-50)
            if [[ "$cmd" == *"nvmf"* ]] || [[ "$cmd" == *"spdk"* ]]; then
                echo "   Warning: PID $pid ($cmd) still has hugepage mappings!"
                echo "   Killing it..."
                kill -9 $pid_num 2>/dev/null
            fi
        fi
    fi
done

echo "8. Final cleanup with SPDK setup.sh..."
if [ -f "../spdk/scripts/setup.sh" ]; then
    ../spdk/scripts/setup.sh reset
    ../spdk/scripts/setup.sh cleanup
fi

sleep 2

# 检查最终状态
free_after=$(cat /sys/kernel/mm/hugepages/hugepages-2048kB/free_hugepages)
echo "After cleanup - HugePages: $free_after free / $total_huge total"

# 计算释放了多少巨页
released=$((free_after - free_before))
if [ $released -gt 0 ]; then
    echo "✅ Released $released hugepages"
fi

# 验证是否完全释放
if [ $free_after -eq $total_huge ]; then
    echo "✅ All hugepages are free!"
else
    echo "⚠️  Warning: $((total_huge - free_after)) hugepages still in use"
    echo "   Checking who's using them..."
    
    # 找出还在使用巨页的进程
    echo "   Processes using hugepages:"
    for pid in /proc/[0-9]*; do
        if [ -f "$pid/smaps" ]; then
            if grep -q "Hugetlb" "$pid/smaps" 2>/dev/null; then
                pid_num=${pid#/proc/}
                cmd=$(cat $pid/cmdline 2>/dev/null | tr '\0' ' ' | cut -c 1-100)
                echo "   - PID $pid_num: $cmd"
            fi
        fi
    done
fi

echo "=== SPDK Stopped ==="

# 最后的验证
if pgrep -f "nvmf_tgt|spdk" > /dev/null; then
    echo "❌ ERROR: Some SPDK processes are still running!"
    ps aux | grep -E "nvmf_tgt|spdk" | grep -v grep
    exit 1
else
    echo "✅ All SPDK processes stopped"
fi