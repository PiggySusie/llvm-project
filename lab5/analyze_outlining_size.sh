#!/bin/bash
# 分析 Post-Link Outlining 的代码大小效果
# 对比 outlining 前后的代码大小变化

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# 配置
TEST_DIR="/root/llvm-project/Lab5-test"

echo "=========================================="
echo "Post-Link Outlining 代码大小分析"
echo "对比 Outlining 前后的代码大小变化"
echo "=========================================="
echo ""

# 获取 .text 段大小（只计算实际代码段，不包括 .bolt.org.text）
get_text_size() {
    local file="$1"
    if [ ! -f "$file" ]; then
        echo "0"
        return
    fi
    
    # 使用 objdump 获取 .text 段大小（不包括 .bolt.org.text）
    local objdump_size=$(objdump -h "$file" 2>/dev/null | grep -E "^[ ]+[0-9]+ \.text[^.]" | awk '{print $3}' | head -1)
    if [ -n "$objdump_size" ]; then
        # objdump 输出的是十六进制，需要转换
        echo $((0x$objdump_size))
        return
    fi
    
    # 如果 objdump 失败，尝试 readelf
    local size=$(readelf -S "$file" 2>/dev/null | grep -E "\.text[^.]" | awk '{print $6}' | head -1)
    if [ -n "$size" ]; then
        echo $((0x$size))
        return
    fi
    
    echo "0"
}

# 获取 outlined 函数段的总大小
get_outlined_size() {
    local file="$1"
    if [ ! -f "$file" ]; then
        echo "0"
        return
    fi
    
    # 获取所有 .text..outlined.* 段的大小
    local total_size=0
    while IFS= read -r line; do
        local size_hex=$(echo "$line" | awk '{print $3}')
        if [ -n "$size_hex" ]; then
            local size_dec=$((0x$size_hex))
            total_size=$((total_size + size_dec))
        fi
    done < <(objdump -h "$file" 2>/dev/null | grep "\.text\.\.outlined")
    
    echo $total_size
}

# 获取整个二进制文件大小
get_file_size() {
    local file="$1"
    if [ ! -f "$file" ]; then
        echo "0"
        return
    fi
    stat -c%s "$file" 2>/dev/null || echo "0"
}

# 格式化大小
format_size() {
    local bytes=$1
    if [ $bytes -lt 1024 ]; then
        echo "${bytes}B"
    elif [ $bytes -lt 1048576 ]; then
        echo "$((bytes/1024))KB"
    else
        echo "$((bytes/1048576))MB"
    fi
}

# 统计 outlined function 数量
count_outlined_functions() {
    local file="$1"
    if [ ! -f "$file" ]; then
        echo "0"
        return
    fi
    nm "$file" 2>/dev/null | grep -c "\.outlined\." || echo "0"
}

# 分析单个程序
analyze_program() {
    local name="$1"
    local orig_file="$2"
    local outlined_file="$3"
    
    echo "----------------------------------------"
    echo "分析: $name"
    echo "----------------------------------------"
    
    # 检查原始文件
    if [ ! -f "$orig_file" ]; then
        echo -e "${YELLOW}⚠${NC} 原始文件不存在: $orig_file"
        echo ""
        return
    fi
    
    # 检查优化后的文件
    if [ ! -f "$outlined_file" ]; then
        echo -e "${YELLOW}⚠${NC} 优化后文件不存在: $outlined_file"
        echo "  请先运行 BOLT 优化生成该文件"
        echo ""
        return
    fi
    
    # 获取原始大小
    local orig_text_size=$(get_text_size "$orig_file")
    local orig_file_size=$(get_file_size "$orig_file")
    
    # 获取优化后大小
    local outlined_text_size=$(get_text_size "$outlined_file")
    local outlined_func_size=$(get_outlined_size "$outlined_file")
    local outlined_file_size=$(get_file_size "$outlined_file")
    local outlined_count=$(count_outlined_functions "$outlined_file")
    
    # 计算总代码大小（.text + outlined 函数）
    local total_code_size=$((outlined_text_size + outlined_func_size))
    
    # 计算变化
    local text_diff=$((outlined_text_size - orig_text_size))
    local total_code_diff=$((total_code_size - orig_text_size))
    local file_diff=$((outlined_file_size - orig_file_size))
    
    # 计算百分比
    local text_percent=0
    if [ $orig_text_size -gt 0 ]; then
        text_percent=$((text_diff * 100 / orig_text_size))
    fi
    local total_code_percent=0
    if [ $orig_text_size -gt 0 ]; then
        total_code_percent=$((total_code_diff * 100 / orig_text_size))
    fi
    local file_percent=0
    if [ $orig_file_size -gt 0 ]; then
        file_percent=$((file_diff * 100 / orig_file_size))
    fi
    
    # 显示结果
    echo -e "${BLUE}原始二进制文件:${NC}"
    echo "  .text 段大小: $(format_size $orig_text_size)"
    echo "  文件总大小: $(format_size $orig_file_size)"
    echo ""
    
    echo -e "${BLUE}Outlining 优化后:${NC}"
    echo "  .text 段大小: $(format_size $outlined_text_size)"
    if [ $text_diff -lt 0 ]; then
        echo -e "  ${GREEN}减小: $(format_size $((-$text_diff))) ($((-$text_percent))%)${NC}"
    elif [ $text_diff -gt 0 ]; then
        echo -e "  ${RED}增加: $(format_size $text_diff) (+$text_percent%)${NC}"
    else
        echo "  无变化"
    fi
    
    echo "  Outlined 函数段大小: $(format_size $outlined_func_size)"
    echo "  Outlined 函数数量: $outlined_count"
    echo ""
    
    echo -e "${BLUE}总代码大小对比:${NC}"
    echo "  原始代码: $(format_size $orig_text_size)"
    echo "  优化后总代码 (.text + outlined): $(format_size $total_code_size)"
    if [ $total_code_diff -lt 0 ]; then
        echo -e "  ${GREEN}总代码减少: $(format_size $((-$total_code_diff))) ($((-$total_code_percent))%)${NC}"
    elif [ $total_code_diff -gt 0 ]; then
        echo -e "  ${YELLOW}总代码增加: $(format_size $total_code_diff) (+$total_code_percent%)${NC}"
        echo -e "  ${YELLOW}注意: 虽然总代码增加了，但这是正常的，因为 outlined 函数需要额外的空间${NC}"
    else
        echo "  总代码无变化"
    fi
    echo ""
    
    echo "  文件总大小: $(format_size $outlined_file_size)"
    if [ $file_diff -lt 0 ]; then
        echo -e "  ${GREEN}文件减小: $(format_size $((-$file_diff))) ($((-$file_percent))%)${NC}"
    elif [ $file_diff -gt 0 ]; then
        echo -e "  ${YELLOW}文件增加: $(format_size $file_diff) (+$file_percent%)${NC}"
        echo -e "  ${YELLOW}注意: BOLT 优化会添加额外的段（如 .bolt.org.text），导致文件变大${NC}"
    else
        echo "  文件大小无变化"
    fi
    echo ""
}

# 分析各个程序
analyze_program "basicmath" \
    "$TEST_DIR/basicmath/basicmath_large.original" \
    "$TEST_DIR/basicmath/basicmath_large.plo"

analyze_program "blowfish" \
    "$TEST_DIR/blowfish/bf" \
    "$TEST_DIR/blowfish/bf.plo"

# typeset: 尝试多个可能的路径
if [ -f "$TEST_DIR/typeset/lout-3.24/lout.original" ]; then
    analyze_program "typeset" \
        "$TEST_DIR/typeset/lout-3.24/lout.original" \
        "$TEST_DIR/typeset/lout-3.24/lout.plo"
elif [ -f "$TEST_DIR/typeset/lout-3.24/lout" ]; then
    analyze_program "typeset" \
        "$TEST_DIR/typeset/lout-3.24/lout" \
        "$TEST_DIR/typeset/lout-3.24/lout.plo"
fi

# lame: 尝试多个可能的路径
if [ -f "$TEST_DIR/lame/lame.original" ]; then
    analyze_program "lame" \
        "$TEST_DIR/lame/lame.original" \
        "$TEST_DIR/lame/lame.plo"
elif [ -f "$TEST_DIR/lame/lame" ]; then
    analyze_program "lame" \
        "$TEST_DIR/lame/lame" \
        "$TEST_DIR/lame/lame.plo"
fi

echo "=========================================="
echo "分析完成"
echo "=========================================="
echo ""
echo "说明:"
echo "  - .text 段大小: 主代码段的大小（不包括 outlined 函数）"
echo "  - Outlined 函数段: 所有 outlined 函数的总大小"
echo "  - 总代码大小: .text + outlined 函数段的总和"
echo "  - 文件总大小: 整个二进制文件的大小（包括所有段）"
echo ""
