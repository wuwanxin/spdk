#!/bin/bash

echo "=== Balancing HugePages Across NUMA Nodes ==="

# 获取总大页数
total=$(cat /proc/sys/vm/nr_hugepages)
echo "Total hugepages configured: $total"

# 获取 NUMA 节点数
nodes=$(ls -d /sys/devices/system/node/node* 2>/dev/null | wc -l)
echo "NUMA nodes: $nodes"

# 计算每个节点应分配的数量
per_node=$((total / nodes))
remainder=$((total % nodes))

echo "Per node base: $per_node, remainder: $remainder"

# 重置所有节点
for node in /sys/devices/system/node/node*/hugepages/hugepages-2048kB/nr_hugepages; do
    echo "Resetting $node to 0"
    echo 0 | sudo tee $node
done

# 重新分配
node_count=0
for node in /sys/devices/system/node/node*/hugepages/hugepages-2048kB/nr_hugepages; do
    if [ $node_count -eq 0 ]; then
        pages=$((per_node + remainder))
    else
        pages=$per_node
    fi
    echo "Setting $node to $pages"
    echo $pages | sudo tee $node
    node_count=$((node_count + 1))
done

# 验证
echo -e "\n=== Verification ==="
../spdk/scripts/setup.sh status

echo -e "\nFree hugepages per node:"
for node in /sys/devices/system/node/node*/hugepages/hugepages-2048kB/free_hugepages; do
    echo "$node: $(cat $node)"
done