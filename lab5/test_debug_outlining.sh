#!/bin/bash
# 测试 Post-Link Outlining 的调试输出
# 用于查看提取的序列和哈希值

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# 配置
TEST_DIR="/root/llvm-project/Lab5-test"
BOLT_BINARY="/root/zzy-llvm-build-debug/bin/llvm-bolt"

echo "=========================================="
echo "测试 Post-Link Outlining 调试输出"
echo "=========================================="
echo ""

# 检查 BOLT 二进制文件
if [ ! -f "$BOLT_BINARY" ]; then
    echo -e "${RED}错误: BOLT 二进制文件不存在: $BOLT_BINARY${NC}"
    echo "请先编译 BOLT: cd /root/zzy/llvm-project/lab5 && ./rebuild_bolt.sh"
    exit 1
fi

# 测试用例1: basicmath (小文件，输出不会太多)
echo -e "${BLUE}测试用例: basicmath${NC}"
echo "运行 BOLT Post-Link Outlining (调试模式)..."
echo ""

cd "$TEST_DIR/basicmath"

# 检查原始程序
if [ ! -f "basicmath_large.original" ] && [ ! -f "basicmath_large" ]; then
    echo -e "${YELLOW}⚠${NC} 程序不存在: basicmath_large 或 basicmath_large.original"
    exit 1
fi

INPUT_PROG="basicmath_large.original"
if [ ! -f "$INPUT_PROG" ]; then
    INPUT_PROG="basicmath_large"
fi

OUTPUT_PROG="basicmath_large.plo.debug"

echo "命令: $BOLT_BINARY -enable-post-link-outlining -post-link-outlining-debug -post-link-outlining-pgo -post-link-outlining-length=10 $INPUT_PROG -o $OUTPUT_PROG"
echo ""

# 运行 BOLT 优化（启用调试输出和 PGO）
# 注意：不使用 grep 过滤，直接显示所有输出，然后手动过滤
"$BOLT_BINARY" \
    -enable-post-link-outlining \
    -post-link-outlining-debug \
    -post-link-outlining-pgo \
    -post-link-outlining-length=10 \
    "$INPUT_PROG" \
    -o "$OUTPUT_PROG" \
    2>&1 | grep -E "(BOLT-PLO-DEBUG|error|Error|warning|Hash|Opcode|EnablePGO|HotThreshold|Precomputed|Skipping|has no profile)" | head -150

echo ""
echo -e "${GREEN}调试输出已显示（前150行）${NC}"
echo ""
echo "说明:"
echo "  - BOLT-PLO-DEBUG: 显示提取的序列信息"
echo "  - EnablePGO: 显示 PGO 是否启用"
echo "  - HotThreshold: 显示热函数阈值"
echo "  - Precomputed hot functions: 显示预计算的热函数统计"
echo "  - Skipping hot function: 显示被跳过的热函数"
echo "  - has no profile data: 显示没有 profile 数据的函数（保守处理）"
echo "  - Hash: 显示每个序列的哈希值"
echo ""
echo "注意:"
echo "  - PGO 已启用，会过滤热函数（执行次数 > 1）"
echo "  - 没有 profile 数据的函数会被保守处理（跳过）"
echo "  - 由于很多函数还是占位符，优化会提前返回，不会实际创建函数"
echo "  - 但你可以看到序列提取、哈希计算和 PGO 过滤是否正确工作"

