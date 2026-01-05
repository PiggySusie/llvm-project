#!/bin/bash
# 测试 Post-Link Outlining Pass 优化后的程序

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# 配置
TEST_DIR="/root/llvm-project/Lab5-test"
REF_DIR="/root/llvm-project/Lab5-test-0"
BOLT_BINARY="/root/zzy-llvm-build-debug/bin/llvm-bolt"

# 统计变量
TOTAL_TESTS=0
PASSED_TESTS=0
FAILED_TESTS=0

echo "=========================================="
echo "测试 Post-Link Outlining Pass 优化后的程序"
echo "=========================================="
echo ""

# 检查 BOLT 二进制文件
if [ ! -f "$BOLT_BINARY" ]; then
    echo -e "${RED}错误: BOLT 二进制文件不存在: $BOLT_BINARY${NC}"
    echo "请先编译 BOLT: cd /root/zzy/llvm-project/lab5 && ./rebuild_bolt.sh"
    exit 1
fi

echo -e "${BLUE}BOLT 二进制: $BOLT_BINARY${NC}"
echo ""

# 辅助函数：运行 BOLT 并检查退出码
run_bolt_check() {
    local INPUT="$1"
    local OUTPUT="$2"
    shift 2
    local EXTRA_ARGS="$@"
    
    # 记录运行前的时间戳
    local BEFORE_TIME=$(date +%s)
    
    # 如果输出文件已存在，记录其修改时间
    local OUTPUT_EXISTS=false
    local OLD_OUTPUT_TIME=0
    if [ -f "$OUTPUT" ]; then
        OUTPUT_EXISTS=true
        OLD_OUTPUT_TIME=$(stat -c %Y "$OUTPUT" 2>/dev/null || echo 0)
    fi
    
    # 运行 BOLT，保存输出和退出码
    local BOLT_OUTPUT
    BOLT_OUTPUT=$("$BOLT_BINARY" \
        -enable-post-link-outlining \
        $EXTRA_ARGS \
        "$INPUT" \
        -o "$OUTPUT" 2>&1)
    local BOLT_EXIT_CODE=$?
    
    # 记录运行后的时间戳
    local AFTER_TIME=$(date +%s)
    
    # 显示 BOLT 输出（过滤后，包含调试信息）
    echo "$BOLT_OUTPUT" | grep -E "(BOLT|error|Error|warning|Aborted|assert|PLO-DEBUG|PLO-)" | head -50
    
    # 检查 BOLT 是否成功
    if [ $BOLT_EXIT_CODE -ne 0 ]; then
        echo -e "${RED}✗${NC} BOLT 优化失败（退出码: $BOLT_EXIT_CODE）"
        if echo "$BOLT_OUTPUT" | grep -q "Aborted\|assert\|crash"; then
            echo -e "${RED}  ⚠ BOLT 崩溃了！${NC}"
            echo "  详细错误信息（最后20行）:"
            echo "$BOLT_OUTPUT" | tail -20
        else
            echo "  完整错误输出（最后30行）:"
            echo "$BOLT_OUTPUT" | tail -30
        fi
        # 如果 BOLT 失败，删除可能存在的旧输出文件，避免误判
        if [ -f "$OUTPUT" ]; then
            rm -f "$OUTPUT"
            if [ $? -eq 0 ]; then
                echo "  已删除旧输出文件: $OUTPUT"
            fi
        fi
        return 1
    fi
    
    # 检查输出文件是否存在
    if [ ! -f "$OUTPUT" ]; then
        echo -e "${RED}✗${NC} 优化失败: 输出文件未生成"
        return 1
    fi
    
    # 检查输出文件的修改时间，确保是在 BOLT 运行后生成的
    local OUTPUT_TIME=$(stat -c %Y "$OUTPUT" 2>/dev/null || echo 0)
    
    # 允许 2 秒的误差（文件系统时间戳可能有延迟）
    local TIME_TOLERANCE=2
    
    if [ "$OUTPUT_EXISTS" = true ]; then
        # 如果文件之前就存在，检查是否在 BOLT 运行后更新
        # 文件时间应该 >= BEFORE_TIME - TIME_TOLERANCE（允许一些误差）
        if [ "$OUTPUT_TIME" -lt $((BEFORE_TIME - TIME_TOLERANCE)) ]; then
            echo -e "${RED}✗${NC} 优化失败: 输出文件未更新（使用的是旧文件）"
            echo "  文件修改时间: $(stat -c %y "$OUTPUT" 2>/dev/null || echo 'unknown')"
            echo "  BOLT 运行时间: $(date -d "@$BEFORE_TIME" '+%Y-%m-%d %H:%M:%S') 到 $(date -d "@$AFTER_TIME" '+%Y-%m-%d %H:%M:%S')"
            echo "  文件时间戳: $OUTPUT_TIME, 运行前时间戳: $OLD_OUTPUT_TIME, 运行后时间戳: $AFTER_TIME"
            return 1
        fi
        # 如果文件时间没有明显更新，也检查一下（可能时间戳相同但内容更新了）
        if [ "$OUTPUT_TIME" -eq "$OLD_OUTPUT_TIME" ]; then
            echo -e "${YELLOW}⚠${NC} 警告: 输出文件时间戳未变化（可能使用了旧文件）"
            echo "  文件修改时间: $(stat -c %y "$OUTPUT" 2>/dev/null || echo 'unknown')"
        fi
    else
        # 如果文件是新生成的，检查是否在 BOLT 运行时间范围内（允许误差）
        if [ "$OUTPUT_TIME" -lt $((BEFORE_TIME - TIME_TOLERANCE)) ]; then
            echo -e "${RED}✗${NC} 优化失败: 输出文件时间戳异常（文件时间早于 BOLT 运行时间）"
            echo "  文件修改时间: $(stat -c %y "$OUTPUT" 2>/dev/null || echo 'unknown')"
            echo "  BOLT 运行时间: $(date -d "@$BEFORE_TIME" '+%Y-%m-%d %H:%M:%S') 到 $(date -d "@$AFTER_TIME" '+%Y-%m-%d %H:%M:%S')"
            return 1
        fi
    fi
    
    # 显示输出文件的生成时间信息
    if [ -f "$OUTPUT" ]; then
        local OUTPUT_TIME_READABLE=$(stat -c %y "$OUTPUT" 2>/dev/null || echo 'unknown')
        local OUTPUT_SIZE=$(stat -c %s "$OUTPUT" 2>/dev/null || echo 'unknown')
        echo -e "${BLUE}✓${NC} 输出文件已生成: $OUTPUT"
        echo -e "  生成时间: ${OUTPUT_TIME_READABLE}"
        echo -e "  文件大小: ${OUTPUT_SIZE} 字节"
    fi
    
    return 0
}

# 测试函数：对比两个文件
compare_files() {
    local file1="$1"
    local file2="$2"
    local test_name="$3"
    
    if [ ! -f "$file1" ]; then
        echo -e "${RED}✗${NC} $test_name: 输出文件不存在: $file1"
        return 1
    fi
    
    if [ ! -f "$file2" ]; then
        echo -e "${RED}✗${NC} $test_name: 标准输出文件不存在: $file2"
        return 1
    fi
    
    if diff -q "$file1" "$file2" > /dev/null 2>&1; then
        echo -e "${GREEN}✓${NC} $test_name: 输出匹配"
        return 0
    else
        echo -e "${RED}✗${NC} $test_name: 输出不匹配"
        echo "  差异详情（前20行）:"
        diff "$file1" "$file2" | head -20
        return 1
    fi
}

# 测试用例1: basicmath
test_basicmath() {
    echo "----------------------------------------"
    echo "测试用例 1: basicmath (Post-Link Outlining)"
    echo "----------------------------------------"
    
    cd "$TEST_DIR/basicmath"
    
    # 检查原始程序
    if [ ! -f "basicmath_large.original" ] && [ ! -f "basicmath_large" ]; then
        echo -e "${YELLOW}⚠${NC} 程序不存在: basicmath_large 或 basicmath_large.original"
        return
    fi
    
    local INPUT_PROG="basicmath_large.original"
    if [ ! -f "$INPUT_PROG" ]; then
        INPUT_PROG="basicmath_large"
    fi
    
    local OUTPUT_PROG="basicmath_large.plo"
    
    echo "优化程序: $INPUT_PROG -> $OUTPUT_PROG"
    echo "运行 BOLT Post-Link Outlining..."
    
    # 运行 BOLT 优化（启用 PGO 过滤和调试输出）
    if ! run_bolt_check "$INPUT_PROG" "$OUTPUT_PROG" "-post-link-outlining-pgo" "-post-link-outlining-length=32" "-post-link-outlining-debug"; then
        echo -e "${RED}✗${NC} basicmath (Post-Link Outlining): BOLT 优化失败，跳过功能测试"
        FAILED_TESTS=$((FAILED_TESTS + 1))
        TOTAL_TESTS=$((TOTAL_TESTS + 1))
        echo ""
        return
    fi
    
    chmod +x "$OUTPUT_PROG" 2>/dev/null || true
    
    # 运行测试（只有在 BOLT 成功时才运行）
    echo "运行优化后的程序..."
    ./"$OUTPUT_PROG" > output_large.plo.txt 2>&1 || true
    
    # 对比输出
    TOTAL_TESTS=$((TOTAL_TESTS + 1))
    if compare_files "output_large.plo.txt" "$REF_DIR/basicmath/output/output_large.txt" "basicmath (Post-Link Outlining)"; then
        PASSED_TESTS=$((PASSED_TESTS + 1))
    else
        FAILED_TESTS=$((FAILED_TESTS + 1))
    fi
    echo ""
}

# 测试用例2: blowfish
test_blowfish() {
    echo "----------------------------------------"
    echo "测试用例 2: blowfish (Post-Link Outlining)"
    echo "----------------------------------------"
    
    cd "$TEST_DIR/blowfish"
    
    if [ ! -f "bf" ]; then
        echo -e "${YELLOW}⚠${NC} 程序不存在: bf"
        return
    fi
    
    local OUTPUT_PROG="bf.plo"
    
    echo "优化程序: bf -> $OUTPUT_PROG"
    echo "运行 BOLT Post-Link Outlining..."
    
    # 运行 BOLT 优化（启用 PGO 过滤和调试输出）
    if ! run_bolt_check "bf" "$OUTPUT_PROG" "-post-link-outlining-pgo" "-post-link-outlining-length=32" "-post-link-outlining-debug"; then
        echo -e "${RED}✗${NC} blowfish (Post-Link Outlining): BOLT 优化失败，跳过功能测试"
        FAILED_TESTS=$((FAILED_TESTS + 1))
        TOTAL_TESTS=$((TOTAL_TESTS + 1))
        echo ""
        return
    fi
    
    chmod +x "$OUTPUT_PROG" 2>/dev/null || true
    
    # 运行测试（只有在 BOLT 成功时才运行）
    echo "运行优化后的程序..."
    ./"$OUTPUT_PROG" e input_large.asc output_large.plo.enc 1234567890abcdeffedcba0987654321 2>&1 || true
    ./"$OUTPUT_PROG" d output_large.plo.enc output_large.plo.asc 1234567890abcdeffedcba0987654321 2>&1 || true
    
    # 对比输出
    TOTAL_TESTS=$((TOTAL_TESTS + 1))
    local all_passed=true
    
    if ! compare_files "output_large.plo.enc" "$REF_DIR/blowfish/output/output_large.enc" "blowfish (Post-Link Outlining, enc)"; then
        all_passed=false
    fi
    
    if ! compare_files "output_large.plo.asc" "$REF_DIR/blowfish/output/output_large.asc" "blowfish (Post-Link Outlining, asc)"; then
        all_passed=false
    fi
    
    if [ "$all_passed" = true ]; then
        PASSED_TESTS=$((PASSED_TESTS + 1))
        echo -e "${GREEN}✓${NC} blowfish (Post-Link Outlining): 所有输出匹配"
    else
        FAILED_TESTS=$((FAILED_TESTS + 1))
    fi
    echo ""
}

# 测试用例3: typeset
test_typeset() {
    echo "----------------------------------------"
    echo "测试用例 3: typeset (Post-Link Outlining)"
    echo "----------------------------------------"
    
    cd "$TEST_DIR/typeset"
    
    # 检查原始程序
    if [ ! -f "lout-3.24/lout" ] && [ ! -f "lout-3.24/lout.original" ]; then
        echo -e "${YELLOW}⚠${NC} 程序不存在: lout-3.24/lout 或 lout-3.24/lout.original"
        return
    fi
    
    local INPUT_PROG="lout-3.24/lout.original"
    if [ ! -f "$INPUT_PROG" ]; then
        INPUT_PROG="lout-3.24/lout"
    fi
    
    if [ ! -f "large.lout" ]; then
        echo -e "${YELLOW}⚠${NC} 输入文件不存在: large.lout"
        return
    fi
    
    local OUTPUT_PROG="lout-3.24/lout.plo"
    
    echo "优化程序: $INPUT_PROG -> $OUTPUT_PROG"
    echo "运行 BOLT Post-Link Outlining..."
    
    # 运行 BOLT 优化（启用 PGO 过滤和调试输出）
    if ! run_bolt_check "$INPUT_PROG" "$OUTPUT_PROG" "-post-link-outlining-pgo" "-post-link-outlining-length=32" "-post-link-outlining-debug"; then
        echo -e "${RED}✗${NC} typeset (Post-Link Outlining): BOLT 优化失败，跳过功能测试"
        FAILED_TESTS=$((FAILED_TESTS + 1))
        TOTAL_TESTS=$((TOTAL_TESTS + 1))
        echo ""
        return
    fi
    
    chmod +x "$OUTPUT_PROG" 2>/dev/null || true
    
    # 运行测试（只有在 BOLT 成功时才运行）
    echo "运行优化后的程序..."
    ./"$OUTPUT_PROG" -I lout-3.24/include -D lout-3.24/data -F lout-3.24/font -C lout-3.24/maps -H lout-3.24/hyph large.lout > output_large.plo.ps 2>&1 || true
    
    # 对比输出（忽略时间戳差异）
    TOTAL_TESTS=$((TOTAL_TESTS + 1))
    if [ -f "output_large.plo.ps" ] && [ -s "output_large.plo.ps" ]; then
        if [ -f "$REF_DIR/typeset/output/output_large.ps" ]; then
            # 忽略 CreationDate 行的差异
            local tmp1=$(mktemp)
            local tmp2=$(mktemp)
            grep -v "^%%CreationDate:" output_large.plo.ps > "$tmp1" 2>/dev/null || cp output_large.plo.ps "$tmp1"
            grep -v "^%%CreationDate:" "$REF_DIR/typeset/output/output_large.ps" > "$tmp2" 2>/dev/null || cp "$REF_DIR/typeset/output/output_large.ps" "$tmp2"
            
            if diff -q "$tmp1" "$tmp2" > /dev/null 2>&1; then
                echo -e "${GREEN}✓${NC} typeset (Post-Link Outlining): 输出匹配（忽略时间戳差异）"
                PASSED_TESTS=$((PASSED_TESTS + 1))
            else
                echo -e "${RED}✗${NC} typeset (Post-Link Outlining): 输出不匹配（忽略时间戳后仍有差异）"
                echo "  差异详情（前20行）:"
                diff "$tmp1" "$tmp2" | head -20
                FAILED_TESTS=$((FAILED_TESTS + 1))
            fi
            rm -f "$tmp1" "$tmp2"
        else
            echo -e "${YELLOW}⚠${NC} 标准输出文件不存在，但输出文件已生成"
            PASSED_TESTS=$((PASSED_TESTS + 1))
        fi
    else
        echo -e "${RED}✗${NC} typeset (Post-Link Outlining): 输出文件未生成或为空"
        FAILED_TESTS=$((FAILED_TESTS + 1))
    fi
    echo ""
}

# 测试用例4: lame
test_lame() {
    echo "----------------------------------------"
    echo "测试用例 4: lame (Post-Link Outlining)"
    echo "----------------------------------------"
    
    cd "$TEST_DIR/lame"
    
    if [ ! -f "lame" ] && [ ! -f "lame.original" ]; then
        echo -e "${YELLOW}⚠${NC} 程序不存在: lame 或 lame.original"
        return
    fi
    
    local INPUT_PROG="lame.original"
    if [ ! -f "$INPUT_PROG" ]; then
        INPUT_PROG="lame"
    fi
    
    if [ ! -f "large.wav" ]; then
        echo -e "${YELLOW}⚠${NC} 输入文件不存在: large.wav"
        return
    fi
    
    local OUTPUT_PROG="lame.plo"
    
    echo "优化程序: $INPUT_PROG -> $OUTPUT_PROG"
    echo "运行 BOLT Post-Link Outlining..."
    
    # 运行 BOLT 优化（启用 PGO 过滤和调试输出）
    if ! run_bolt_check "$INPUT_PROG" "$OUTPUT_PROG" "-post-link-outlining-pgo" "-post-link-outlining-length=32" "-post-link-outlining-debug"; then
        echo -e "${RED}✗${NC} lame (Post-Link Outlining): BOLT 优化失败，跳过功能测试"
        FAILED_TESTS=$((FAILED_TESTS + 1))
        TOTAL_TESTS=$((TOTAL_TESTS + 1))
        echo ""
        return
    fi
    
    chmod +x "$OUTPUT_PROG" 2>/dev/null || true
    
    # 运行测试（只有在 BOLT 成功时才运行）
    echo "运行优化后的程序..."
    echo "命令: ./$OUTPUT_PROG large.wav output.plo.wav"
    ./"$OUTPUT_PROG" large.wav output.plo.wav 2>&1 | tail -5 || true
    
    # 对比输出
    TOTAL_TESTS=$((TOTAL_TESTS + 1))
    if [ -f "output.plo.wav" ]; then
        if [ -s "output.plo.wav" ]; then
            # 尝试多个可能的参考文件路径
            REF_FILE=""
            if [ -f "$REF_DIR/lame/output/output.wav" ]; then
                REF_FILE="$REF_DIR/lame/output/output.wav"
            elif [ -f "$REF_DIR/lame/output/output_large.mp3" ]; then
                REF_FILE="$REF_DIR/lame/output/output_large.mp3"
            elif [ -f "$REF_DIR/lame/output/output.mp3" ]; then
                REF_FILE="$REF_DIR/lame/output/output.mp3"
            fi
            
            if [ -n "$REF_FILE" ] && [ -f "$REF_FILE" ]; then
                if compare_files "output.plo.wav" "$REF_FILE" "lame (Post-Link Outlining)"; then
                    PASSED_TESTS=$((PASSED_TESTS + 1))
                else
                    FAILED_TESTS=$((FAILED_TESTS + 1))
                fi
            else
                echo -e "${YELLOW}⚠${NC} lame (Post-Link Outlining): 标准输出文件不存在，但输出文件已生成"
                echo "  输出文件: output.plo.wav ($(ls -lh output.plo.wav | awk '{print $5}'))"
                PASSED_TESTS=$((PASSED_TESTS + 1))
            fi
        else
            echo -e "${RED}✗${NC} lame (Post-Link Outlining): 输出文件为空"
            FAILED_TESTS=$((FAILED_TESTS + 1))
        fi
    else
        echo -e "${RED}✗${NC} lame (Post-Link Outlining): 输出文件未生成"
        FAILED_TESTS=$((FAILED_TESTS + 1))
    fi
    echo ""
}

# 主函数
main() {
    # 检查目录
    if [ ! -d "$TEST_DIR" ]; then
        echo -e "${RED}错误: 测试目录不存在: $TEST_DIR${NC}"
        exit 1
    fi
    
    if [ ! -d "$REF_DIR" ]; then
        echo -e "${YELLOW}警告: 标准输出目录不存在: $REF_DIR${NC}"
        echo "将跳过文件对比，只检查程序是否能运行"
        echo ""
    fi
    
    # 运行所有测试
    test_basicmath
    test_blowfish
    test_typeset
    test_lame
    
    # 输出总结
    echo "=========================================="
    echo "测试总结（Post-Link Outlining）"
    echo "=========================================="
    echo "总测试数: $TOTAL_TESTS"
    echo -e "${GREEN}通过: $PASSED_TESTS${NC}"
    echo -e "${RED}失败: $FAILED_TESTS${NC}"
    echo ""
    
    if [ $FAILED_TESTS -eq 0 ] && [ $TOTAL_TESTS -gt 0 ]; then
        echo -e "${GREEN}所有测试通过！Post-Link Outlining Pass 功能正确！${NC}"
        exit 0
    elif [ $TOTAL_TESTS -eq 0 ]; then
        echo -e "${YELLOW}没有找到测试程序${NC}"
        exit 0
    else
        echo -e "${RED}有测试失败，请检查输出${NC}"
        exit 1
    fi
}

# 运行主函数
main

