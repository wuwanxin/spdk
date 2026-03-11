#!/bin/bash
# verify_hugepage_leak_fixed.sh
# 修复版本的巨页泄漏验证脚本

# 颜色输出
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# 日志文件
LOG_FILE="hugepage_verify_$(date +%Y%m%d_%H%M%S).log"
RESULT_FILE="hugepage_measurements.txt"

# 初始化结果文件
echo "# 时间戳 总巨页 空闲巨页 已用巨页 备注" > $RESULT_FILE

# 函数：记录巨页状态 - 修复版本
record_hugepage_state() {
    local remark="$1"
    local timestamp=$(date "+%Y-%m-%d %H:%M:%S")
    
    # 读取巨页信息
    local total=$(cat /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages 2>/dev/null || echo "0")
    local free=$(cat /sys/kernel/mm/hugepages/hugepages-2048kB/free_hugepages 2>/dev/null || echo "0")
    local reserved=$(cat /sys/kernel/mm/hugepages/hugepages-2048kB/resv_hugepages 2>/dev/null || echo "0")
    
    # 计算已用巨页
    local used=$((total - free))
    
    # 输出到控制台
    echo -e "${BLUE}[$timestamp]${NC} $remark"
    echo -e "  总巨页: ${GREEN}$total${NC}"
    echo -e "  空闲巨页: ${YELLOW}$free${NC}"
    echo -e "  已用巨页: ${RED}$used${NC}"
    echo -e "  预留巨页: $reserved"
    
    # 记录到结果文件
    echo "$timestamp $total $free $used $remark" >> $RESULT_FILE
    
    # 记录到日志
    echo "[$timestamp] $remark: Total=$total, Free=$free, Used=$used, Reserved=$reserved" >> $LOG_FILE
    
    # 只返回数字值，不返回其他文本
    echo $free
}

# 函数：检查巨页文件系统
check_hugepage_fs() {
    echo -e "\n${YELLOW}检查巨页文件系统...${NC}"
    
    # 检查挂载点
    if mount | grep -q "hugetlbfs"; then
        echo "✓ hugetlbfs 已挂载:"
        mount | grep hugetlbfs | head -1
    else
        echo "✗ hugetlbfs 未挂载"
    fi
    
    # 检查巨页目录中的文件
    echo -e "\n检查 /dev/hugepages 目录:"
    if [ -d "/dev/hugepages" ]; then
        local file_count=$(ls -la /dev/hugepages/ 2>/dev/null | grep -c "spdk\|dpdk" || echo "0")
        if [ $file_count -gt 0 ]; then
            echo -e "${RED}发现 $file_count 个 SPDK/DPDK 相关文件:${NC}"
            ls -la /dev/hugepages/ | grep -E "spdk|dpdk" || true
        else
            echo -e "${GREEN}没有发现残留的巨页文件${NC}"
        fi
    else
        echo "/dev/hugepages 目录不存在"
    fi
    
    # 检查进程的巨页映射
    echo -e "\n检查进程的巨页映射:"
    local huge_found=0
    for pid in /proc/[0-9]*; do
        if [ -f "$pid/smaps" ]; then
            if grep -q "Hugetlb" "$pid/smaps" 2>/dev/null; then
                if [ $huge_found -eq 0 ]; then
                    echo -e "${YELLOW}发现巨页映射，相关进程:${NC}"
                    huge_found=1
                fi
                pid_num=${pid#/proc/}
                cmd=$(cat $pid/cmdline 2>/dev/null | tr '\0' ' ' | cut -c 1-50)
                echo "  PID $pid_num: $cmd"
            fi
        fi
    done
    if [ $huge_found -eq 0 ]; then
        echo "没有发现巨页映射"
    fi
}

# 函数：运行测试循环 - 修复版本
run_test_cycle() {
    local cycle_num=$1
    
    echo -e "\n${GREEN}========== 测试循环 #$cycle_num ==========${NC}"
    echo "开始时间: $(date)"
    
    # 记录测试前的状态 - 获取数值
    free_before=$(record_hugepage_state "测试#$cycle_num 开始前")
    # 确保 free_before 是数字
    free_before=$(echo $free_before | grep -o '^[0-9]*' | head -1)
    
    # 检查巨页文件系统
    check_hugepage_fs
    
    # 运行你的 run.sh 脚本
    echo -e "\n${YELLOW}运行 run.sh...${NC}"
    echo "run.sh 输出将被记录到 run_cycle_${cycle_num}.log"
    
    # 运行 run.sh 并捕获输出
    if bash ./run.sh > run_cycle_${cycle_num}.log 2>&1; then
        echo -e "${GREEN}✓ run.sh 执行成功${NC}"
        
        # 等待几秒让服务完全启动
        sleep 5
        
        # 获取 nvmf_tgt 的 PID
        TGT_PID=$(pgrep -f nvmf_tgt | head -1)
        if [ -n "$TGT_PID" ]; then
            echo -e "nvmf_tgt PID: $TGT_PID"
            
            # 记录运行中的状态
            free_during=$(record_hugepage_state "测试#$cycle_num 运行中 (PID=$TGT_PID)")
            
            # 等待10秒让服务稳定
            echo "等待10秒让服务稳定..."
            sleep 10
            
            # 模拟你之前的 kill 操作
            echo -e "\n${YELLOW}模拟 kill $TGT_PID 操作...${NC}"
            kill $TGT_PID
            sleep 3
            
            # 检查进程是否真的被杀
            if kill -0 $TGT_PID 2>/dev/null; then
                echo -e "${RED}警告: 进程仍然存在，使用 pkill 强制清理${NC}"
                sudo pkill -f nvmf_tgt
                sleep 2
            else
                echo -e "${GREEN}✓ 进程已终止${NC}"
            fi
        else
            echo -e "${RED}错误: 找不到 nvmf_tgt 进程${NC}"
        fi
    else
        echo -e "${RED}✗ run.sh 执行失败${NC}"
        echo "查看 run_cycle_${cycle_num}.log 了解详情"
    fi
    
    # 额外清理
    echo -e "\n${YELLOW}执行额外清理...${NC}"
    sudo pkill -f nvmf_tgt 2>/dev/null
    sudo rm -f /dev/hugepages/spdk_* 2>/dev/null
    sudo rm -f /mnt/huge/spdk_* 2>/dev/null
    sleep 2
    
    # 记录测试后的状态 - 获取数值
    free_after=$(record_hugepage_state "测试#$cycle_num 结束后")
    # 确保 free_after 是数字
    free_after=$(echo $free_after | grep -o '^[0-9]*' | head -1)
    
    # 检查是否有泄漏
    echo -e "\n${YELLOW}泄漏检查:${NC}"
    if [ -n "$free_before" ] && [ -n "$free_after" ] && [ "$free_before" -eq "$free_after" ] 2>/dev/null; then
        echo -e "${GREEN}✓ 巨页完全释放 (空闲巨页: $free_before → $free_after)${NC}"
    elif [ -n "$free_before" ] && [ -n "$free_after" ] && [ "$free_after" -lt "$free_before" ] 2>/dev/null; then
        local leaked=$((free_before - free_after))
        echo -e "${RED}✗ 发现巨页泄漏! 减少了 $leaked 个巨页 (空闲: $free_before → $free_after)${NC}"
        echo "泄漏的巨页: $leaked" >> $LOG_FILE
    else
        echo -e "${YELLOW}无法精确比较 (空闲巨页: $free_before → $free_after)${NC}"
    fi
    
    echo "测试循环 #$cycle_num 完成于: $(date)"
}

# 函数：直接测试run.sh失败的原因
test_run_sh_directly() {
    echo -e "\n${YELLOW}直接测试 run.sh 失败原因...${NC}"
    
    # 记录执行前的状态
    echo "执行前的巨页状态:"
    cat /sys/kernel/mm/hugepages/hugepages-2048kB/free_hugepages
    
    # 尝试直接运行并捕获详细输出
    echo -e "\n执行 run.sh 并捕获详细输出:"
    bash -x ./run.sh 2>&1 | tee run_debug.log
    
    echo -e "\n调试日志已保存到 run_debug.log"
}

# 函数：生成报告
generate_report() {
    echo -e "\n${GREEN}========== 测试报告 ==========${NC}"
    echo "日志文件: $LOG_FILE"
    echo "数据文件: $RESULT_FILE"
    
    echo -e "\n${YELLOW}巨页使用趋势:${NC}"
    echo "------------------------"
    cat $RESULT_FILE
    
    # 分析是否有泄漏
    echo -e "\n${YELLOW}泄漏分析:${NC}"
    # 读取数据文件，检查前后变化
    local prev_free=""
    local prev_note=""
    while read -r line; do
        if [[ $line != \#* ]]; then
            local timestamp=$(echo $line | awk '{print $1" "$2}')
            local total=$(echo $line | awk '{print $3}')
            local free=$(echo $line | awk '{print $4}')
            local note=$(echo $line | awk '{$1=$2=$3=$4=$5=""; print $0}' | sed 's/^[ \t]*//')
            
            if [ -n "$prev_free" ] && [ -n "$free" ] && [ "$free" -lt "$prev_free" ] 2>/dev/null; then
                local diff=$((prev_free - free))
                echo "⚠️  从 '$prev_note' 到 '$note' 空闲巨页减少了 $diff"
            fi
            prev_free=$free
            prev_note=$note
        fi
    done < $RESULT_FILE
}

# 主程序
main() {
    echo -e "${GREEN}================================${NC}"
    echo -e "${GREEN}   巨页泄漏验证脚本启动        ${NC}"
    echo -e "${GREEN}================================${NC}"
    
    # 检查权限
    if [ "$EUID" -eq 0 ]; then 
        echo -e "${RED}警告: 您正在以root运行此脚本${NC}"
    fi
    
    # 记录系统信息
    echo "系统信息:" > $LOG_FILE
    uname -a >> $LOG_FILE
    echo "内存信息:" >> $LOG_FILE
    cat /proc/meminfo | grep -i huge >> $LOG_FILE
    
    # 记录初始状态
    echo -e "\n${YELLOW}初始巨页状态:${NC}"
    record_hugepage_state "初始状态"
    
    # 检查巨页配置
    total_init=$(cat /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages 2>/dev/null || echo "0")
    if [ "$total_init" -eq 0 ]; then
        echo -e "${RED}警告: 系统没有配置巨页!${NC}"
        echo "尝试分配巨页..."
        echo 1024 | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
    fi
    
    # 检查 run.sh 是否存在
    if [ ! -f "./run.sh" ]; then
        echo -e "${RED}错误: 在当前目录找不到 run.sh${NC}"
        exit 1
    fi
    
    # 选择测试模式
    echo -e "\n${YELLOW}请选择测试模式:${NC}"
    echo "1) 单次测试"
    echo "2) 循环测试 (多次运行)"
    echo "3) 详细检查 (只检查当前状态)"
    echo "4) 调试 run.sh (查看失败原因)"
    read -p "选择 [1-4]: " mode
    
    case $mode in
        1)
            run_test_cycle 1
            ;;
        2)
            echo -n "输入测试循环次数: "
            read cycles
            for i in $(seq 1 $cycles); do
                run_test_cycle $i
                if [ $i -lt $cycles ]; then
                    echo -e "\n等待5秒后开始下一次测试..."
                    sleep 5
                fi
            done
            ;;
        3)
            check_hugepage_fs
            record_hugepage_state "详细检查"
            ;;
        4)
            test_run_sh_directly
            ;;
        *)
            echo "无效选择"
            exit 1
            ;;
    esac
    
    # 生成报告
    generate_report
    
    echo -e "\n${GREEN}测试完成！${NC}"
    echo "查看详细日志: cat $LOG_FILE"
    echo "查看数据记录: cat $RESULT_FILE"
    
    if [ -f "run_debug.log" ]; then
        echo "查看调试日志: cat run_debug.log"
    fi
}

# 运行主程序
main