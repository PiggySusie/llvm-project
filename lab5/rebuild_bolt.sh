#!/bin/bash

# 仅重新编译 BOLT 的脚本
# 适用于已有 zzy-llvm-build-debug 构建目录的情况
# 每次修改代码后运行此脚本即可重新编译

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# 项目目录
LLVM_SRC_DIR="/root/zzy/llvm-project"
LLVM_BUILD_DIR="/root/zzy-llvm-build-debug"

# 解析命令行参数
CLEAN=false
FORCE_RECONFIGURE=false

while [[ $# -gt 0 ]]; do
    case $1 in
        --clean|-c)
            CLEAN=true
            shift
            ;;
        --reconfigure|-r)
            FORCE_RECONFIGURE=true
            shift
            ;;
        --help|-h)
            echo "用法: $0 [选项]"
            echo "选项:"
            echo "  --clean, -c         清理 BOLT 相关的编译产物"
            echo "  --reconfigure, -r   强制重新配置 CMake"
            echo "  --help, -h          显示此帮助信息"
            exit 0
            ;;
        *)
            echo -e "${RED}未知选项: $1${NC}"
            echo "使用 --help 查看帮助"
            exit 1
            ;;
    esac
done

echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}重新编译 BOLT${NC}"
echo -e "${GREEN}========================================${NC}"
echo ""

# 检查源码目录
if [ ! -d "${LLVM_SRC_DIR}" ]; then
    echo -e "${RED}错误: 源码目录不存在: ${LLVM_SRC_DIR}${NC}"
    exit 1
fi

# 检查构建目录
if [ ! -d "${LLVM_BUILD_DIR}" ]; then
    echo -e "${RED}错误: 构建目录不存在: ${LLVM_BUILD_DIR}${NC}"
    echo "请先运行完整的编译脚本: ./build_bolt.sh"
    exit 1
fi

cd "${LLVM_BUILD_DIR}"

# 检查 CMake 配置
if [ ! -f "CMakeCache.txt" ]; then
    echo -e "${RED}错误: CMake 配置不存在，请先运行完整的编译脚本${NC}"
    echo "运行: ./build_bolt.sh"
    exit 1
fi

echo -e "${YELLOW}[1/4] 检查构建环境...${NC}"

# 检查 ninja
if ! command -v ninja &> /dev/null; then
    echo -e "${RED}错误: 未找到 ninja${NC}"
    exit 1
fi

# 清理选项
if [ "$CLEAN" = true ]; then
    echo -e "${BLUE}清理 BOLT 相关编译产物...${NC}"
    rm -f bin/llvm-bolt
    rm -rf tools/bolt/lib/Passes/CMakeFiles
    rm -f lib/libLLVMBOLTPasses.a
    echo -e "${GREEN}✓ 清理完成${NC}"
    echo ""
fi

# 重新配置 CMake
if [ "$FORCE_RECONFIGURE" = true ]; then
    echo -e "${YELLOW}[2/4] 强制重新配置 CMake...${NC}"
    rm -rf tools/bolt/lib/Passes/CMakeFiles
    cmake "${LLVM_SRC_DIR}/llvm" 2>&1 | grep -E "(Configuring|Generating|error)" | tail -10
    echo ""
elif [ "$CLEAN" = true ]; then
    echo -e "${YELLOW}[2/4] 重新配置 CMake（检测到新文件）...${NC}"
    cmake "${LLVM_SRC_DIR}/llvm" 2>&1 | grep -E "(Configuring|Generating|error)" | tail -10 || true
    echo ""
else
    echo -e "${YELLOW}[2/4] 检查 CMake 配置...${NC}"
    # 静默检查，如果有错误会显示
    cmake "${LLVM_SRC_DIR}/llvm" 2>&1 | grep -E "(error|Error)" | head -5 || true
    echo ""
fi

echo -e "${YELLOW}[3/4] 准备重新编译...${NC}"
# 清理库和可执行文件，让它们重新链接
rm -f "${LLVM_BUILD_DIR}/lib/libLLVMBOLTPasses.a"
rm -f "${LLVM_BUILD_DIR}/bin/llvm-bolt"
echo "已清理需要重新链接的文件"
echo ""

echo -e "${YELLOW}[4/4] 重新编译 BOLT...${NC}"
echo "使用并行编译: -j$(nproc)"
echo ""
echo -e "${BLUE}注意: 由于 BOLT 依赖很多 LLVM 库，ninja 可能会显示需要编译很多目标${NC}"
echo -e "${BLUE}但实际上只会重新编译已修改的文件和需要重新链接的库${NC}"
echo -e "${BLUE}ninja 的增量编译系统会自动处理，只编译真正需要的部分${NC}"
echo ""

# 编译 BOLT Passes 库（ninja 会自动只编译必要的部分）
echo -e "${BLUE}步骤 1: 编译 BOLT Passes 库...${NC}"
echo "（ninja 会自动检测哪些文件需要重新编译）"
if ninja lib/libLLVMBOLTPasses.a -j$(nproc) 2>&1 | tee /tmp/bolt_passes.log; then
    PASSES_SUCCESS=true
    echo -e "${GREEN}✓ BOLT Passes 库编译成功${NC}"
    # 检查实际编译了多少文件
    ACTUAL_BUILT=$(grep -c "Building CXX object" /tmp/bolt_passes.log 2>/dev/null || echo "0")
    if [ "$ACTUAL_BUILT" -gt 0 ]; then
        echo "   实际重新编译了 ${ACTUAL_BUILT} 个文件"
    fi
else
    PASSES_SUCCESS=false
    echo -e "${RED}✗ BOLT Passes 库编译失败${NC}"
    echo "查看日志: /tmp/bolt_passes.log"
fi
    echo ""
    
    # 尝试编译 llvm-bolt（即使其他依赖失败也尝试）
echo -e "${BLUE}步骤 2: 链接 llvm-bolt 可执行文件...${NC}"
BUILD_SUCCESS=false
if ninja llvm-bolt -j$(nproc) 2>&1 | tee /tmp/bolt_build.log; then
    BUILD_SUCCESS=true
    echo -e "${GREEN}✓ llvm-bolt 编译成功${NC}"
else
    # 检查是否是因为其他依赖失败，但 BOLT 本身可能已经编译成功
    if [ -f "${LLVM_BUILD_DIR}/bin/llvm-bolt" ]; then
        echo -e "${YELLOW}⚠ 警告: 编译过程中有错误，但 llvm-bolt 已存在${NC}"
        BUILD_SUCCESS=true
    else
        # 检查是否是已知的可忽略错误
        if grep -q "bolt_rt\|libLLVMCore.a" /tmp/bolt_build.log; then
            echo -e "${YELLOW}⚠ 警告: 编译失败，但可能是依赖问题（bolt_rt 或 LLVMCore）${NC}"
            echo "尝试继续检查已编译的文件..."
        else
            echo -e "${RED}✗ llvm-bolt 编译失败${NC}"
        fi
    fi
fi
echo ""

# 如果 BOLT Passes 编译成功，即使 llvm-bolt 失败也继续检查
if [ "$PASSES_SUCCESS" = true ] && [ "$BUILD_SUCCESS" = false ]; then
    echo -e "${YELLOW}提示: BOLT Passes 库已编译成功，但 llvm-bolt 链接失败${NC}"
    echo "这可能是因为其他 LLVM 库的链接问题"
    echo "如果之前已经有 llvm-bolt，可以继续使用"
    echo ""
fi

# 检查编译结果
BOLT_BINARY="${LLVM_BUILD_DIR}/bin/llvm-bolt"

# 如果 Passes 编译失败，直接退出
if [ "$PASSES_SUCCESS" = false ]; then
    echo -e "${RED}========================================${NC}"
    echo -e "${RED}编译失败！${NC}"
    echo -e "${RED}========================================${NC}"
    echo ""
    echo -e "${RED}错误: BOLT Passes 库编译失败${NC}"
    echo "查看详细日志: /tmp/bolt_passes.log"
    echo ""
    exit 1
fi

# 如果 Passes 编译成功，检查 llvm-bolt
if [ -f "${BOLT_BINARY}" ]; then
    echo ""
    echo -e "${GREEN}========================================${NC}"
    echo -e "${GREEN}编译成功!${NC}"
    echo -e "${GREEN}========================================${NC}"
    echo ""
    echo "BOLT 二进制文件: ${BOLT_BINARY}"
    echo "文件大小: $(ls -lh ${BOLT_BINARY} | awk '{print $5}')"
    echo "修改时间: $(stat -c '%y' ${BOLT_BINARY} 2>/dev/null | cut -d'.' -f1 || echo '未知')"
    echo ""
    echo -e "${GREEN}使用方法:${NC}"
    echo "  ${BOLT_BINARY} <input_binary> -o <output_binary>"
    echo ""
    echo "或者添加到 PATH:"
    echo "  export PATH=\"${LLVM_BUILD_DIR}/bin:\$PATH\""
    echo ""
    echo -e "${BLUE}提示: 每次修改代码后，运行此脚本即可重新编译${NC}"
    echo "  ./rebuild_bolt.sh              # 普通重新编译"
    echo "  ./rebuild_bolt.sh --clean      # 清理后重新编译"
    echo "  ./rebuild_bolt.sh --reconfigure # 强制重新配置 CMake"
    echo ""
else
    # llvm-bolt 不存在，但 Passes 编译成功
    echo -e "${YELLOW}========================================${NC}"
    echo -e "${YELLOW}BOLT Passes 编译成功，但 llvm-bolt 未生成${NC}"
    echo -e "${YELLOW}========================================${NC}"
    echo ""
    echo -e "${GREEN}✓ BOLT Passes 已编译到 libLLVMBOLTPasses.a${NC}"
    echo ""
    echo "可能的原因:"
    echo "  1. 其他 LLVM 库的链接问题（如 libLLVMCore.a）"
    echo "  2. 需要先修复其他依赖才能链接 llvm-bolt"
    echo ""
    echo "建议:"
    echo "  1. 检查之前的 llvm-bolt 是否仍可用"
    echo "  2. 或者修复编译错误后重新编译"
    echo ""
    echo "查看详细日志:"
    echo "  /tmp/bolt_passes.log - Passes 编译日志"
    echo "  /tmp/bolt_build.log - llvm-bolt 链接日志"
    echo ""
    exit 1
fi

