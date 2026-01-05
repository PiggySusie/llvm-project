# BOLT 添加新 Pass 指南

## 代码结构

### 目录结构
```
bolt/
├── include/bolt/Passes/          # Pass 头文件目录
│   ├── YourNewPass.h             # 新 Pass 的头文件
│   └── ...
├── lib/Passes/                    # Pass 实现目录
│   ├── YourNewPass.cpp           # 新 Pass 的实现文件
│   ├── CMakeLists.txt            # 需要添加新文件到构建列表
│   └── ...
└── lib/Rewrite/
    └── BinaryPassManager.cpp     # Pass 注册位置
```

## 添加新 Pass 的步骤

### 步骤 1: 创建头文件

在 `bolt/include/bolt/Passes/` 目录下创建头文件，例如 `YourNewPass.h`:

```cpp
//===- bolt/Passes/YourNewPass.h - Your Pass Description --*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// 你的 Pass 描述
//
//===----------------------------------------------------------------------===//

#ifndef BOLT_PASSES_YOURNEWPASS_H
#define BOLT_PASSES_YOURNEWPASS_H

#include "bolt/Passes/BinaryPasses.h"

namespace llvm {
namespace bolt {

class YourNewPass : public BinaryFunctionPass {
public:
  explicit YourNewPass(bool PrintPass = false) 
    : BinaryFunctionPass(PrintPass) {}

  const char *getName() const override { return "your-new-pass"; }

  bool shouldPrint(const BinaryFunction &BF) const override {
    return BinaryFunctionPass::shouldPrint(BF);
  }

  /// Pass 入口点 - 必须实现
  Error runOnFunctions(BinaryContext &BC) override;
};

} // namespace bolt
} // namespace llvm

#endif // BOLT_PASSES_YOURNEWPASS_H
```

### 步骤 2: 创建实现文件

在 `bolt/lib/Passes/` 目录下创建实现文件，例如 `YourNewPass.cpp`:

```cpp
//===- bolt/Passes/YourNewPass.cpp - Your Pass Implementation --------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the YourNewPass class.
//
//===----------------------------------------------------------------------===//

#include "bolt/Passes/YourNewPass.h"
#include "bolt/Core/BinaryFunction.h"
#include "bolt/Core/BinaryContext.h"
#include "bolt/Utils/CommandLineOpts.h"
#include "llvm/Support/CommandLine.h"

#define DEBUG_TYPE "your-new-pass"

using namespace llvm;
using namespace bolt;

namespace opts {
// 可选：添加命令行选项
static cl::opt<bool> EnableYourNewPass(
    "enable-your-new-pass",
    cl::desc("enable your new pass optimization"),
    cl::init(false),
    cl::cat(BoltOptCategory));
} // namespace opts

namespace llvm {
namespace bolt {

Error YourNewPass::runOnFunctions(BinaryContext &BC) {
  // 遍历所有函数
  for (auto &It : BC.getBinaryFunctions()) {
    BinaryFunction &BF = It.second;

    // 跳过空函数或不可优化函数
    if (BF.empty() || !shouldOptimize(BF))
      continue;

    // 在这里实现你的 Pass 逻辑
    // 例如：
    // - 遍历基本块
    // - 修改指令
    // - 优化 CFG
    // 等等

    // 示例：遍历基本块
    for (BinaryBasicBlock *BB : BF.getLayout().blocks()) {
      // 处理基本块
      for (MCInst &Inst : *BB) {
        // 处理指令
      }
    }
  }

  return Error::success();
}

} // namespace bolt
} // namespace llvm
```

### 步骤 3: 添加到 CMakeLists.txt

在 `bolt/lib/Passes/CMakeLists.txt` 中添加新文件：

```cmake
add_llvm_library(LLVMBOLTPasses
  ...
  YourNewPass.cpp    # 添加这一行
  ...
)
```

### 步骤 4: 注册 Pass

在 `bolt/lib/Rewrite/BinaryPassManager.cpp` 中：

1. **添加头文件包含**（在文件顶部）:
```cpp
#include "bolt/Passes/YourNewPass.h"
```

2. **注册 Pass**（在 `runAllPasses` 函数中，选择合适的位置）:
```cpp
Error BinaryFunctionPassManager::runAllPasses(BinaryContext &BC) {
  BinaryFunctionPassManager Manager(BC);
  
  // ... 其他 Pass 注册 ...
  
  // 注册你的新 Pass
  Manager.registerPass(std::make_unique<YourNewPass>(PrintYourNewPass),
                       opts::EnableYourNewPass);
  
  // ... 其他 Pass 注册 ...
  
  return Manager.runPasses();
}
```

### 步骤 5: 添加命令行选项（可选）

如果需要命令行控制，在 `bolt/Utils/CommandLineOpts.h` 或相关文件中添加：

```cpp
extern cl::opt<bool> EnableYourNewPass;
```

## Pass 基类说明

### BinaryFunctionPass 基类

所有 Pass 必须继承自 `BinaryFunctionPass`，主要接口：

- **`getName()`**: 返回 Pass 名称（用于调试输出）
- **`shouldPrint()`**: 控制是否打印函数信息（用于调试）
- **`runOnFunctions()`**: Pass 的主要逻辑入口，必须实现
- **`shouldOptimize()`**: 控制是否优化某个函数（基类提供默认实现）

### 常用 API

- **`BinaryContext &BC`**: 二进制上下文，包含所有函数、符号等信息
- **`BinaryFunction &BF`**: 单个函数，包含基本块、指令等
- **`BinaryBasicBlock *BB`**: 基本块，包含指令列表
- **`MCInst &Inst`**: 机器指令

## 示例：参考现有 Pass

可以参考以下简单 Pass 作为模板：

1. **AsmDumpPass** (`AsmDump.h/cpp`) - 简单的输出 Pass
2. **DynoStatsSetPass** (`BinaryPasses.h`) - 简单的统计 Pass
3. **RemoveNops** (`BinaryPasses.h/cpp`) - 简单的优化 Pass

## 编译和测试

1. **编译**:
```bash
cd /root/zzy/llvm-project/lab5
./rebuild_bolt.sh
```

2. **测试**:
```bash
# 使用你的新 Pass
llvm-bolt -enable-your-new-pass input.bin -o output.bin
```

## 注意事项

1. **Pass 执行顺序**: Pass 按照注册顺序执行，注意依赖关系
2. **错误处理**: `runOnFunctions` 返回 `Error`，确保正确处理错误
3. **线程安全**: 如果 Pass 需要并行执行，注意线程安全
4. **调试**: 使用 `-print` 选项可以打印 Pass 执行前后的函数状态

## 完整示例文件位置

- 头文件: `bolt/include/bolt/Passes/YourNewPass.h`
- 实现: `bolt/lib/Passes/YourNewPass.cpp`
- 注册: `bolt/lib/Rewrite/BinaryPassManager.cpp`
- 构建: `bolt/lib/Passes/CMakeLists.txt`

