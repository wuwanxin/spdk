#!/bin/bash
# check_status.sh - 检查SPDK和巨页状态

echo "=== System Status Check ==="
echo "Time: $(date)"
echo ""

echo "=== SPDK Processes ==="
if pgrep -f nvmf_tgt > /dev/null; then
    echo "SPDK is RUNNING:"
    ps aux | head -1
    ps aux | grep nvmf_tgt | grep -v grep
else
    echo "SPDK is STOPPED"
fi
echo ""

echo "=== HugePages Status ==="
total=$(cat /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages)
free=$(cat /sys/kernel/mm/hugepages/hugepages-2048kB/free_hugepages)
used=$((total - free))
echo "Total HugePages: $total"
echo "Free HugePages:  $free"
echo "Used HugePages:  $used"
echo ""

echo "=== HugePage Files ==="
echo "/dev/hugepages:"
ls -la /dev/hugepages/ 2>/dev/null | head -10
echo ""

if [ -d "/mnt/huge" ]; then
    echo "/mnt/huge:"
    ls -la /mnt/huge/ 2>/dev/null | head -10
    echo ""
fi

echo "=== Memory Info ==="
cat /proc/meminfo | grep -E "HugePages|Hugepage" | head -5
echo ""

# 如果SPDK没运行但巨页被占用，提示
if ! pgrep -f nvmf_tgt > /dev/null && [ $used -gt 0 ]; then
    echo "⚠️  WARNING: SPDK is not running but $used hugepages are in use!"
    echo "This indicates a cleanup issue."
    
    echo ""
    echo "Processes using hugepages:"
    for pid in /proc/[0-9]*; do
        if [ -f "$pid/smaps" ]; then
            if grep -q "Hugetlb" "$pid/smaps" 2>/dev/null; then
                pid_num=${pid#/proc/}
                cmd=$(cat $pid/cmdline 2>/dev/null | tr '\0' ' ' | cut -c 1-100)
                echo "PID $pid_num: $cmd"
            fi
        fi
    done
fi