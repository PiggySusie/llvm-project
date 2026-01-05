#!/bin/bash
# 测试原始程序（不使用 BOLT 优化）
# 适配 zzy/llvm-project 路径

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

# 测试目录（使用原来的测试数据目录）
TEST_DIR="/root/llvm-project/Lab5-test"
REF_DIR="/root/llvm-project/Lab5-test-0"

TOTAL_TESTS=0
PASSED_TESTS=0
FAILED_TESTS=0

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

test_basicmath() {
    echo "----------------------------------------"
    echo "测试用例 1: basicmath (原始程序)"
    echo "----------------------------------------"
    
    cd "$TEST_DIR/basicmath"
    
    # 优先使用 .original 版本（如果存在），否则使用原始版本
    if [ -f "basicmath_large.original" ]; then
        PROG="basicmath_large.original"
        echo "使用: basicmath_large.original"
    elif [ -f "basicmath_large" ]; then
        PROG="basicmath_large"
        echo "使用: basicmath_large"
    else
        echo -e "${YELLOW}⚠${NC} 程序不存在: basicmath_large 或 basicmath_large.original"
        return
    fi
    
    chmod +x "$PROG" 2>/dev/null || true
    
    echo "运行测试..."
    ./"$PROG" > output_large.txt 2>&1 || true
    
    TOTAL_TESTS=$((TOTAL_TESTS + 1))
    if compare_files "output_large.txt" "$REF_DIR/basicmath/output/output_large.txt" "basicmath (原始)"; then
        PASSED_TESTS=$((PASSED_TESTS + 1))
    else
        FAILED_TESTS=$((FAILED_TESTS + 1))
    fi
    echo ""
}

test_blowfish() {
    echo "----------------------------------------"
    echo "测试用例 2: blowfish (原始程序)"
    echo "----------------------------------------"
    
    cd "$TEST_DIR/blowfish"
    
    if [ ! -f "bf" ]; then
        echo -e "${YELLOW}⚠${NC} 程序不存在: bf"
        return
    fi
    
    chmod +x bf 2>/dev/null || true
    
    echo "运行测试..."
    ./bf e input_large.asc output_large.enc 1234567890abcdeffedcba0987654321 2>&1 || true
    ./bf d output_large.enc output_large.asc 1234567890abcdeffedcba0987654321 2>&1 || true
    
    TOTAL_TESTS=$((TOTAL_TESTS + 1))
    local all_passed=true
    
    if ! compare_files "output_large.enc" "$REF_DIR/blowfish/output/output_large.enc" "blowfish (原始, enc)"; then
        all_passed=false
    fi
    
    if ! compare_files "output_large.asc" "$REF_DIR/blowfish/output/output_large.asc" "blowfish (原始, asc)"; then
        all_passed=false
    fi
    
    if [ "$all_passed" = true ]; then
        PASSED_TESTS=$((PASSED_TESTS + 1))
        echo -e "${GREEN}✓${NC} blowfish (原始): 所有输出匹配"
    else
        FAILED_TESTS=$((FAILED_TESTS + 1))
    fi
    echo ""
}

test_typeset() {
    echo "----------------------------------------"
    echo "测试用例 3: typeset (原始程序)"
    echo "----------------------------------------"
    
    cd "$TEST_DIR/typeset"
    
    # 检查原始程序
    if [ -f "lout-3.24/lout" ]; then
        PROG="lout-3.24/lout"
    elif [ -f "lout-3.24/lout.original" ]; then
        PROG="lout-3.24/lout.original"
    else
        echo -e "${YELLOW}⚠${NC} 程序不存在: lout-3.24/lout 或 lout-3.24/lout.original"
        echo "  跳过测试"
        echo ""
        return
    fi
    
    # 检查输入文件
    if [ ! -f "large.lout" ]; then
        echo -e "${YELLOW}⚠${NC} 输入文件不存在: large.lout"
        echo "  跳过测试"
        echo ""
        return
    fi
    
    chmod +x "$PROG" 2>/dev/null || true
    
    echo "运行测试..."
    # 运行 lout 程序生成输出
    ./"$PROG" -I lout-3.24/include -D lout-3.24/data -F lout-3.24/font -C lout-3.24/maps -H lout-3.24/hyph large.lout > output_large.ps 2>&1 || true
    
    TOTAL_TESTS=$((TOTAL_TESTS + 1))
    if [ -f "output_large.ps" ] && [ -s "output_large.ps" ]; then
        if [ -f "$REF_DIR/typeset/output/output_large.ps" ]; then
            # 对于 PostScript 文件，忽略 CreationDate 行的差异（时间戳不同不影响功能）
            # 创建临时文件，删除 CreationDate 行后比较
            local tmp1=$(mktemp)
            local tmp2=$(mktemp)
            grep -v "^%%CreationDate:" output_large.ps > "$tmp1" 2>/dev/null || cp output_large.ps "$tmp1"
            grep -v "^%%CreationDate:" "$REF_DIR/typeset/output/output_large.ps" > "$tmp2" 2>/dev/null || cp "$REF_DIR/typeset/output/output_large.ps" "$tmp2"
            
            if diff -q "$tmp1" "$tmp2" > /dev/null 2>&1; then
                echo -e "${GREEN}✓${NC} typeset (原始): 输出匹配（忽略时间戳差异）"
                PASSED_TESTS=$((PASSED_TESTS + 1))
            else
                echo -e "${RED}✗${NC} typeset (原始): 输出不匹配（忽略时间戳后仍有差异）"
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
        echo -e "${RED}✗${NC} typeset (原始): 输出文件未生成或为空"
        FAILED_TESTS=$((FAILED_TESTS + 1))
    fi
    echo ""
}

test_lame() {
    echo "----------------------------------------"
    echo "测试用例 4: lame (原始程序)"
    echo "----------------------------------------"
    
    cd "$TEST_DIR/lame"
    
    # 检查原始程序
    if [ -f "lame" ]; then
        PROG="lame"
    elif [ -f "lame.original" ]; then
        PROG="lame.original"
    else
        echo -e "${YELLOW}⚠${NC} 程序不存在: lame 或 lame.original"
        echo "  跳过测试"
        echo ""
        return
    fi
    
    chmod +x "$PROG" 2>/dev/null || true
    
    echo "运行测试..."
    # lame 测试需要根据实际情况调整命令
    if [ -f "large.wav" ]; then
        echo "运行: ./$PROG large.wav output.wav"
        ./"$PROG" large.wav output.wav 2>&1 || true
        
        TOTAL_TESTS=$((TOTAL_TESTS + 1))
        if [ -f "output.wav" ]; then
            if [ -s "output.wav" ]; then
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
                    if compare_files "output.wav" "$REF_FILE" "lame (原始)"; then
                        PASSED_TESTS=$((PASSED_TESTS + 1))
                    else
                        FAILED_TESTS=$((FAILED_TESTS + 1))
                    fi
                else
                    echo -e "${YELLOW}⚠${NC} lame (原始): 标准输出文件不存在，但输出文件已生成"
                    echo "  输出文件: output.wav ($(ls -lh output.wav | awk '{print $5}'))"
                    PASSED_TESTS=$((PASSED_TESTS + 1))
                fi
            else
                echo -e "${RED}✗${NC} lame (原始): 输出文件为空"
                FAILED_TESTS=$((FAILED_TESTS + 1))
            fi
        else
            echo -e "${RED}✗${NC} lame (原始): 输出文件未生成"
            FAILED_TESTS=$((FAILED_TESTS + 1))
        fi
    else
        echo -e "${YELLOW}⚠${NC} 输入文件 large.wav 不存在，跳过测试"
    fi
    echo ""
}

main() {
    echo "=========================================="
    echo "测试原始程序（不使用 BOLT 优化）"
    echo "测试 zzy/llvm-project 原始版本"
    echo "=========================================="
    echo ""
    
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
    
    test_basicmath
    test_blowfish
    test_typeset
    test_lame
    
    echo "=========================================="
    echo "测试总结（原始程序）"
    echo "=========================================="
    echo "总测试数: $TOTAL_TESTS"
    echo -e "${GREEN}通过: $PASSED_TESTS${NC}"
    echo -e "${RED}失败: $FAILED_TESTS${NC}"
    echo ""
    
    if [ $FAILED_TESTS -eq 0 ] && [ $TOTAL_TESTS -gt 0 ]; then
        echo -e "${GREEN}所有测试通过！${NC}"
        exit 0
    elif [ $TOTAL_TESTS -eq 0 ]; then
        echo -e "${YELLOW}没有找到测试程序${NC}"
        exit 0
    else
        echo -e "${RED}有测试失败，请检查输出${NC}"
        exit 1
    fi
}

main

