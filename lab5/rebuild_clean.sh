#!/bin/bash

# 在新复制的构建目录下重新编译（移除 PLO）

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# 项目目录
LLVM_SRC_DIR="/root/zzy/llvm-project"
LLVM_BUILD_DIR="/root/zzy-llvm-build-debug"

echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}在新构建目录下重新编译（移除 PLO）${NC}"
echo -e "${GREEN}========================================${NC}"
echo ""
echo -e "${BLUE}源码目录: ${LLVM_SRC_DIR}${NC}"
echo -e "${BLUE}构建目录: ${LLVM_BUILD_DIR}${NC}"
echo ""

# 检查源码目录
if [ ! -d "${LLVM_SRC_DIR}" ]; then
    echo -e "${RED}错误: 源码目录不存在: ${LLVM_SRC_DIR}${NC}"
    exit 1
fi

# 检查构建目录
if [ ! -d "${LLVM_BUILD_DIR}" ]; then
    echo -e "${RED}错误: 构建目录不存在: ${LLVM_BUILD_DIR}${NC}"
    exit 1
fi

cd "${LLVM_BUILD_DIR}"

# 步骤 1: 清理 PLO 和 BOLT 相关的编译产物
echo -e "${YELLOW}[1/3] 清理 PLO 和 BOLT 相关的编译产物...${NC}"

# 删除 BOLT 可执行文件
rm -f bin/llvm-bolt
echo "  已删除: bin/llvm-bolt"

# 删除 BOLT 库文件
rm -f lib/libLLVMBOLTPasses.a
rm -f lib/libLLVMBOLTCore.a
rm -f lib/libLLVMBOLTUtils.a
rm -f lib/libLLVMBOLTRewrite.a
echo "  已删除: BOLT 库文件"

# 删除 BOLT 工具目录（包含所有编译产物）
rm -rf tools/bolt
echo "  已删除: tools/bolt 目录"

# 删除 PLO 相关的目标文件（如果存在）
find . -name "*PLO*" -type f 2>/dev/null | while read file; do
    echo "  已删除: $file"
    rm -f "$file"
done

# 读取旧的 CMake 配置参数（在删除 CMakeCache.txt 之前）
if [ -f "CMakeCache.txt" ]; then
    OLD_CMAKE_BUILD_TYPE=$(grep "^CMAKE_BUILD_TYPE:STRING" CMakeCache.txt | cut -d'=' -f2)
    OLD_CMAKE_C_COMPILER=$(grep "^CMAKE_C_COMPILER:FILEPATH" CMakeCache.txt | cut -d'=' -f2)
    OLD_CMAKE_CXX_COMPILER=$(grep "^CMAKE_CXX_COMPILER:FILEPATH" CMakeCache.txt | cut -d'=' -f2)
    echo "  已读取旧的 CMake 配置参数"
else
    OLD_CMAKE_BUILD_TYPE="Debug"
    OLD_CMAKE_C_COMPILER="clang"
    OLD_CMAKE_CXX_COMPILER="clang++"
fi

# 删除 CMakeCache.txt 和相关配置，因为路径已改变
rm -f CMakeCache.txt
rm -rf CMakeFiles/*.cmake CMakeFiles/CMakeDirectoryInformation.cmake 2>/dev/null
echo "  已删除: CMakeCache.txt（需要重新配置）"

echo -e "${GREEN}✓ 清理完成${NC}"
echo ""

# 步骤 2: 重新配置 CMake 指向新源码
echo -e "${YELLOW}[2/3] 重新配置 CMake 指向新源码...${NC}"
echo -e "${BLUE}源码目录: ${LLVM_SRC_DIR}/llvm${NC}"
echo ""

# 使用与旧配置相同的参数重新配置
cmake "${LLVM_SRC_DIR}/llvm" \
    -G Ninja \
    -DCMAKE_BUILD_TYPE="${OLD_CMAKE_BUILD_TYPE:-Debug}" \
    -DLLVM_ENABLE_PROJECTS="bolt" \
    -DLLVM_TARGETS_TO_BUILD="AArch64" \
    -DCMAKE_C_COMPILER="${OLD_CMAKE_C_COMPILER:-clang}" \
    -DCMAKE_CXX_COMPILER="${OLD_CMAKE_CXX_COMPILER:-clang++}" \
    -DLLVM_ENABLE_ASSERTIONS=ON \
    -DLLVM_ENABLE_BACKTRACES=ON \
    -DLLVM_INCLUDE_TESTS=ON \
    -DLLVM_INCLUDE_EXAMPLES=OFF \
    -DLLVM_INCLUDE_DOCS=OFF \
    -DLLVM_ENABLE_TERMINFO=OFF \
    -DLLVM_ENABLE_ZLIB=OFF \
    -DLLVM_ENABLE_ZSTD=OFF \
    2>&1 | tee /tmp/cmake_rebuild_clean.log

if [ ${PIPESTATUS[0]} -ne 0 ]; then
    echo -e "${RED}✗ CMake 配置失败${NC}"
    echo "查看日志: /tmp/cmake_rebuild_clean.log"
    exit 1
fi

echo -e "${GREEN}✓ CMake 配置完成${NC}"
echo ""

# 步骤 3: 重新编译 BOLT（不包含 PLO）
echo -e "${YELLOW}[3/3] 重新编译 BOLT（不包含 PLO）...${NC}"
echo "使用并行编译: -j$(nproc)"
echo ""
echo -e "${BLUE}注意: 由于复用了之前的构建目录，大部分 LLVM 库不需要重新编译${NC}"
echo -e "${BLUE}只会重新编译 BOLT 相关的部分${NC}"
echo ""

# 编译 BOLT
if ninja llvm-bolt -j$(nproc) 2>&1 | tee /tmp/bolt_rebuild_clean.log; then
    echo -e "${GREEN}✓ BOLT 编译成功${NC}"
    BUILD_SUCCESS=true
else
    echo -e "${RED}✗ BOLT 编译失败${NC}"
    echo "查看日志: /tmp/bolt_rebuild_clean.log"
    BUILD_SUCCESS=false
fi
echo ""

# 检查编译结果
BOLT_BINARY="${LLVM_BUILD_DIR}/bin/llvm-bolt"
if [ -f "${BOLT_BINARY}" ]; then
    echo ""
    echo -e "${GREEN}========================================${NC}"
    echo -e "${GREEN}编译成功!${NC}"
    echo -e "${GREEN}========================================${NC}"
    echo ""
    echo "BOLT 二进制文件: ${BOLT_BINARY}"
    echo "文件大小: $(ls -lh ${BOLT_BINARY} | awk '{print $5}')"
    echo ""
    
    # 检查是否不包含 PLO
    echo -e "${BLUE}检查 PLO Pass...${NC}"
    if "${BOLT_BINARY}" --help 2>&1 | grep -q "\-enable-plo"; then
        echo -e "${YELLOW}⚠ 警告: 仍然包含 -enable-plo 选项${NC}"
        echo "可能需要检查源码中是否还有 PLO 相关代码"
    else
        echo -e "${GREEN}✓ 确认不包含 PLO Pass（干净版本）${NC}"
    fi
    echo ""
    
    echo -e "${GREEN}使用方法:${NC}"
    echo "  ${BOLT_BINARY} <input_binary> -o <output_binary>"
    echo ""
    echo "或者添加到 PATH:"
    echo "  export PATH=\"${LLVM_BUILD_DIR}/bin:\$PATH\""
    echo ""
    echo -e "${BLUE}提示: 后续修改代码后，使用以下命令重新编译${NC}"
    echo "  cd /root/zzy/llvm-project/lab5"
    echo "  ./rebuild_bolt.sh              # 普通重新编译"
    echo "  ./rebuild_bolt.sh --clean      # 清理后重新编译"
    echo ""
else
    if [ "$BUILD_SUCCESS" = false ]; then
        echo -e "${RED}错误: 编译失败，找不到二进制文件${NC}"
        echo "查看详细日志: /tmp/bolt_rebuild_clean.log"
        exit 1
    else
        echo -e "${YELLOW}警告: 编译过程完成，但找不到二进制文件${NC}"
        echo "请检查日志: /tmp/bolt_rebuild_clean.log"
        exit 1
    fi
fi

