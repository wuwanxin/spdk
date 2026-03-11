# 查看 node0 的内存使用
cat /sys/devices/system/node/node0/meminfo | grep -i huge

# 查看是否有进程在使用大页
sudo lsof | grep -i huge 2>/dev/null

# 如果没有 lsof，检查进程内存映射
for pid in /proc/[0-9]*; do
    if grep -q "Hugetlb" $pid/smaps 2>/dev/null; then
        echo "PID $(basename $pid) uses hugepages"
        cat $pid/cmdline | tr '\0' ' ' | head -c 100
        echo
    fi
done