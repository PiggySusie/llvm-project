//===- bolt/Passes/PostLinkOutlining.cpp - Post-Link Outlining Pass --------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Post-Link Outlining 优化：识别重复指令序列并提取为独立函数
//
//===----------------------------------------------------------------------===//

#include "bolt/Passes/PostLinkOutlining.h"
#include "bolt/Core/BinaryFunction.h"
#include "bolt/Core/BinaryContext.h"
#include "bolt/Core/BinaryBasicBlock.h"
#include "bolt/Core/MCPlus.h"
#include "bolt/Utils/CommandLineOpts.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/Format.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCRegister.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/ADT/StringRef.h"
#include <set>
#include <functional>
#include <string>
#include <sstream>
#include <algorithm> // For std::sort
#include <limits> // For SIZE_MAX
#include <map> // For register normalization


#define DEBUG_TYPE "post-link-outlining"

using namespace llvm;
using namespace bolt;

// 获取链接寄存器 LR（X30）
static MCPhysReg getLinkRegister(BinaryContext &BC) {
  if (!BC.isAArch64())
    return 0;
  
  const MCRegisterInfo *MRI = BC.MRI.get();
  
  // 通过名称搜索 X30/LR
  for (unsigned i = 0; i < MRI->getNumRegs(); ++i) {
    StringRef Name = MRI->getName(i);
    if (Name.equals_insensitive("x30") || Name.equals_insensitive("lr")) {
      return (MCPhysReg)i;
    }
  }
  
  return 30; // AArch64 中 X30 的默认编号
}

// 辅助函数：检查函数是否为叶子函数（无调用）
static bool isLeafFunction(BinaryFunction *BF) {
  if (!BF || BF->empty())
    return true;
  
  BinaryContext &BC = BF->getBinaryContext();
  MCPlusBuilder *MIB = BC.MIB.get();
  
  for (const BinaryBasicBlock &BB : *BF) {
    for (const MCInst &Inst : BB) {
      if (MIB->isCall(Inst)) {
        return false;
      }
    }
  }
  return true;
}

// 检查在指定位置之前 LR 是否已被保存
static bool isLRSavedAtPoint(BinaryContext &BC, BinaryBasicBlock *BB, size_t StartIndex) {
  BinaryFunction *BF = BB->getFunction();
  if (!BF || BF->empty())
    return false;
  
  // 检查是否在 RET 指令之后进行外联（不安全）
  // 这很重要，因为 ret 之后 LR 可能已被破坏
  bool retEncountered = false;
  for (const BinaryBasicBlock &FuncBB : *BF) {
    for (size_t i = 0; i < FuncBB.size(); ++i) {
      const MCInst &Inst = FuncBB.getInstructionAtIndex(i);
      
      // 检查当前指令是否为 ret
      if (BC.MIA->isReturn(Inst)) {
        retEncountered = true;
        break; // 找到第一个 ret
      }
    }
    
    // 如果在当前 BB 中找到 ret，且这是我们要外联的 BB
    if (retEncountered && &FuncBB == BB) {
      // 查找 ret 的索引
      size_t retIndex = SIZE_MAX;
      for (size_t i = 0; i < BB->size(); ++i) {
        if (BC.MIA->isReturn(BB->getInstructionAtIndex(i))) {
          retIndex = i;
          break;
        }
      }
      
      // 如果外联点在 ret 之后，则不安全
      if (StartIndex > retIndex) {
        // 不安全：在 RET 指令之后进行外联
        return false; // 强制标记为不安全
      }
    }
    // 如果在之前的 BB 中找到 ret，当前 BB 不安全
    else if (retEncountered) {
      // 不安全：在包含 RET 指令的 BB 之后进行外联
      return false; // 强制标记为不安全
    }
    
    if (&FuncBB == BB) {
      // 到达当前 BB 时未找到 ret，可以继续
      break;
    }
  }
  
  const BinaryBasicBlock &EntryBB = *BF->begin();
  bool IsEntryBlock = (BB == &EntryBB);
  
  MCPlusBuilder *MIB = BC.MIB.get();
  MCPhysReg LR = getLinkRegister(BC);
  
  // 扫描入口块以查找 LR 保存
  // 如果在入口块中，扫描到 StartIndex；否则扫描整个入口块
  const BinaryBasicBlock *BlockToScan = &EntryBB;
  size_t Limit = IsEntryBlock ? StartIndex : EntryBB.size();
  
  for (size_t i = 0; i < Limit; ++i) {
    if (i >= BlockToScan->size()) break;
    
    const MCInst &Inst = BlockToScan->getInstructionAtIndex(i);
    
    // 检查是否为保存 LR 的 Push 或 Store 指令
    if (MIB->isPush(Inst) || BC.MII->get(Inst.getOpcode()).mayStore()) {
      for (const MCOperand &Op : Inst) {
        if (Op.isReg() && Op.getReg() == LR) {
          return true;
        }
      }
    }
    
    // 如果在找到 LR 保存之前遇到终止符或调用，则未保存
    if (BC.MIA->isTerminator(Inst) || BC.MIA->isCall(Inst)) {
      if (IsEntryBlock) return false;
    }
  }
  
  return false;
}

// 辅助函数：获取指令的缩放因子，用于栈偏移调整
// 根据指令类型返回缩放因子（1, 4, 8 或 16）
static int getInstructionScale(BinaryContext &BC, const MCInst &Inst) {
  unsigned Opcode = Inst.getOpcode();
  StringRef Name = BC.InstPrinter->getOpcodeName(Opcode);
  std::string NameLower = Name.lower();

  // 1. Pair Load/Store (LDP/STP)
  // LDPXi/STPXi (64-bit): Scale 8
  // LDPWi/STPWi (32-bit): Scale 4
  // LDPQi/STPQi (128-bit/Vector): Scale 16
  if (NameLower.find("ldp") == 0 || NameLower.find("stp") == 0) {
    if (NameLower.find("xi") != std::string::npos) return 8;  // 64-bit
    if (NameLower.find("wi") != std::string::npos) return 4;  // 32-bit
    if (NameLower.find("qi") != std::string::npos) return 16; // 128-bit
    if (NameLower.find("di") != std::string::npos) return 8;  // 64-bit FP
    if (NameLower.find("si") != std::string::npos) return 4;  // 32-bit FP
  }

  // 2. Single Register Load/Store (LDR/STR)
  // LDRXui/STRXui (64-bit Scaled): Scale 8
  // LDRWui/STRWui (32-bit Scaled): Scale 4
  // LDRBui/STRBui (8-bit): Scale 1
  // LDRHui/STRHui (16-bit): Scale 2
  if (NameLower.find("ldr") == 0 || NameLower.find("str") == 0) {
    if (NameLower.find("xui") != std::string::npos) return 8; // 64-bit Scaled
    if (NameLower.find("wui") != std::string::npos) return 4; // 32-bit Scaled
    if (NameLower.find("qui") != std::string::npos) return 16; // 128-bit
    if (NameLower.find("hui") != std::string::npos) return 2; // 16-bit Scaled
    if (NameLower.find("bui") != std::string::npos) return 1; // 8-bit Scaled
    
    // LDUR/STUR 是 Unscaled (Scale 1)，通常名字里没有 'ui' 后缀，或者是 'u' 前缀
    // 例如 LDURXi
    if (NameLower.find("ldur") == 0 || NameLower.find("stur") == 0) 
        return 1;
  }

  // 默认 fallback：如果识别不出缩放，假设为 1 (主要针对 LDRB, LDUR 等)
  return 1;
}

// 辅助函数：比较两条指令是否相等
// 用于重叠检测
static bool areInstructionsEqual(const MCInst &Inst1, const MCInst &Inst2) {
  // 比较操作码
  if (Inst1.getOpcode() != Inst2.getOpcode())
    return false;
  
  // 比较操作数数量
  if (Inst1.getNumOperands() != Inst2.getNumOperands())
    return false;
  
  // 比较每个操作数
  for (int i = 0, e = Inst1.getNumOperands(); i < e; ++i) {
    const MCOperand &Op1 = Inst1.getOperand(i);
    const MCOperand &Op2 = Inst2.getOperand(i);
    
    // 检查操作数类型是否匹配
    if (Op1.isReg() != Op2.isReg() ||
        Op1.isImm() != Op2.isImm() ||
        Op1.isExpr() != Op2.isExpr() ||
        Op1.isSFPImm() != Op2.isSFPImm()) {
      return false;
    }
    
    // 比较操作数值
    if (Op1.isReg() && Op1.getReg() != Op2.getReg())
      return false;
    if (Op1.isImm() && Op1.getImm() != Op2.getImm())
      return false;
    if (Op1.isSFPImm() && Op1.getSFPImm() != Op2.getSFPImm())
      return false;
    // 注意：表达式比较很复杂，暂时跳过
    // 两个表达式只有在是同一对象时才被认为相等
    if (Op1.isExpr() && Op1.getExpr() != Op2.getExpr())
      return false;
  }
  
  return true; // 指令相等
}

// 检查两个立即数是否兼容
static bool areImmediatesCompatible(BinaryContext &BC, const MCInst &Inst1, 
                                     const MCInst &Inst2, int OpIdx1, int OpIdx2) {
  if (OpIdx1 >= (int)Inst1.getNumOperands() || OpIdx2 >= (int)Inst2.getNumOperands())
    return false;
  
  const MCOperand &Op1 = Inst1.getOperand(OpIdx1);
  const MCOperand &Op2 = Inst2.getOperand(OpIdx2);
  
  if (!Op1.isImm() || !Op2.isImm())
    return Op1.isImm() == Op2.isImm();
  
  int64_t Imm1 = Op1.getImm();
  int64_t Imm2 = Op2.getImm();
  
  // 完全匹配的情况
  if (Imm1 == Imm2)
    return true;
  
  const MCInstrDesc &Desc1 = BC.MII->get(Inst1.getOpcode());
  const MCInstrDesc &Desc2 = BC.MII->get(Inst2.getOpcode());
  
  // 栈偏移必须完全匹配
  bool mayAccessStack = (Desc1.mayLoad() || Desc1.mayStore()) && 
                        (Desc2.mayLoad() || Desc2.mayStore());
  
  if (mayAccessStack) {
    // 检查是否使用SP或FP（栈访问）
    unsigned SPReg = BC.MIB->getStackPointer();
    MCPhysReg FP = BC.MIB->getFramePointer();
    bool usesSPorFP = false;
    
    // 检查所有操作数，看是否有SP/FP
    for (int i = 0; i < (int)Inst1.getNumOperands(); ++i) {
      const MCOperand &Op = Inst1.getOperand(i);
      if (Op.isReg()) {
        unsigned Reg = Op.getReg();
        if (Reg == SPReg || Reg == FP || BC.MRI->isSubRegisterEq(SPReg, Reg) ||
            BC.MRI->isSubRegisterEq(FP, Reg)) {
          usesSPorFP = true;
          break;
        }
      }
    }
    
    // 如果是栈访问的立即数偏移，必须完全匹配
    if (usesSPorFP) {
      return false;  // 栈偏移必须完全匹配
    }
  }
  
  // 对于非栈相关的立即数，根据指令类型判断
  StringRef Name1 = BC.InstPrinter->getOpcodeName(Inst1.getOpcode());
  StringRef Name2 = BC.InstPrinter->getOpcodeName(Inst2.getOpcode());
  std::string Name1Lower = Name1.lower();
  std::string Name2Lower = Name2.lower();
  
  // 移位指令的移位量允许小范围差异
  bool isShiftInst = (Name1Lower.find("lsr") != std::string::npos ||
                      Name1Lower.find("lsl") != std::string::npos ||
                      Name1Lower.find("asr") != std::string::npos ||
                      Name1Lower.find("ror") != std::string::npos) &&
                     (Name2Lower.find("lsr") != std::string::npos ||
                      Name2Lower.find("lsl") != std::string::npos ||
                      Name2Lower.find("asr") != std::string::npos ||
                      Name2Lower.find("ror") != std::string::npos);
  
  if (isShiftInst && OpIdx1 == OpIdx2) {
    // 移位量：允许小范围差异（±1），因为可能是不同的优化选择
    if (std::abs(Imm1 - Imm2) <= 1) {
      return true;
    }
  }
  
  // 小常量（0-15）允许差异为1
  if (std::abs(Imm1) <= 15 && std::abs(Imm2) <= 15) {
    if (std::abs(Imm1 - Imm2) <= 1) {
      return true;
    }
  }
  
  return false;
}

// 检查两个指令是否语义等价
static bool areInstructionsSemanticallyEquivalent(BinaryContext &BC,
                                                   const MCInst &Inst1,
                                                   const MCInst &Inst2) {
  if (Inst1.getOpcode() != Inst2.getOpcode())
    return false;
  
  if (Inst1.getNumOperands() != Inst2.getNumOperands())
    return false;
  
  return true;
}

// 统一的指令过滤函数：检查指令是否应该被拒绝
// 返回拒绝原因（0=不拒绝，1=pseudo/CFI，2=branch/call，3=PC-rel，4=FP/LR，5=modifySP，6=nonLoadSP，7=complexSP）
static int shouldRejectInstruction(BinaryContext &BC, const MCInst &Inst, 
                                    int len, bool allowBranch, bool isLastInSeq,
                                    BinaryBasicBlock *BB, size_t seqStartIdx, size_t instIdx) {
  // 1. 伪指令/CFI/opcode0检查
  if (Inst.getOpcode() == 0 || BC.MIB->isPseudo(Inst) || BC.MIB->isCFI(Inst))
    return 1;
  
  // 2. 返回指令检查
  if (BC.MIB->isReturn(Inst))
    return 2;
  
  // 3. 调用指令检查
  if (BC.MIB->isCall(Inst)) {
    if (!isLastInSeq) return 2; // call必须在序列末尾
    // 检查call之前是否有栈写入（可能用于参数传递）
    if (BB && seqStartIdx < instIdx) {
      for (size_t i = seqStartIdx; i < instIdx; ++i) {
        const MCInst &PrevInst = BB->getInstructionAtIndex(i);
        const MCInstrDesc &PrevDesc = BC.MII->get(PrevInst.getOpcode());
        if (PrevDesc.mayStore()) {
          MCPhysReg SPReg = BC.MIB->getStackPointer();
          for (int opIdx = 0; opIdx < (int)PrevInst.getNumOperands(); ++opIdx) {
            const MCOperand &Op = PrevInst.getOperand(opIdx);
            if (Op.isReg() && Op.getReg() == SPReg) {
              return 2; // 有栈写入，可能参数>8
            }
          }
        }
      }
    }
    return 0; // 允许call在序列末尾
  }
  
  // 4. 分支指令检查
  if (BC.MIA->isBranch(Inst)) {
    // 条件分支：如果是在序列末尾，允许（无论是单基本块还是跨基本块）
    if (isLastInSeq) {
      // 无条件跳转：不允许（会改变控制流）
      if (BC.MIB->isUnconditionalBranch(Inst)) {
        return 2;
      }
      // 条件分支在序列末尾：允许
      return 0;
    }
    // 分支不在序列末尾：不允许
    return 2;
  }
  
  // 5. PC相对寻址检查
  if (BC.isAArch64()) {
    StringRef Name = BC.InstPrinter->getOpcodeName(Inst.getOpcode());
    if (Name.equals_insensitive("ADR") || Name.equals_insensitive("ADRP") ||
        (Name.starts_with_insensitive("LDR") && Name.contains_insensitive("_LIT")))
      return 3;
  }
  
  // 栈访问检查
  const MCInstrDesc &Desc = BC.MII->get(Inst.getOpcode());
  unsigned SPReg = BC.MIB->getStackPointer();
  MCPhysReg FP = BC.MIB->getFramePointer();
  MCPhysReg LR = getLinkRegister(BC);
  bool usesSP = false, modifiesSP = false;
  
  for (int opIdx = 0, opEnd = Inst.getNumOperands(); opIdx < opEnd; ++opIdx) {
    const MCOperand &Op = Inst.getOperand(opIdx);
    if (!Op.isReg()) continue;
    unsigned Reg = Op.getReg();
    
    // 禁止使用 FP/LR（任何使用都拒绝）
    if (Reg == FP || BC.MRI->isSubRegisterEq(FP, Reg) ||
        Reg == LR || BC.MRI->isSubRegisterEq(LR, Reg))
      return 4;
    
    if (Reg == SPReg || BC.MRI->isSubRegisterEq(SPReg, Reg)) {
      usesSP = true;
      if (opIdx < (int)Desc.getNumDefs()) modifiesSP = true;
    }
  }
  
  // 禁止修改 SP
  if (modifiesSP) {
    return 5;
  }
  
  // SP 使用检查：必须是纯读栈
  if (usesSP && !modifiesSP) {
    bool isLongSequence = (len >= 5);
    
    // 禁止 Store 指令
    if (Desc.mayStore()) {
      return 6;
    }
    
    // 短序列必须是 Load 指令
    if (!isLongSequence) {
      if (!Desc.mayLoad()) {
        return 6;
      }
      // 短序列必须有立即数偏移
      bool hasImmOffset = false;
      for (const MCOperand &Op : Inst) {
        if (Op.isImm()) {
          hasImmOffset = true;
          break;
        }
      }
      if (!hasImmOffset)
        return 7;
    }
  }
  
  return 0; // 不拒绝
}

// 跨基本块遍历辅助函数：获取下一个基本块
static BinaryBasicBlock *getNextBasicBlock(BinaryBasicBlock *CurrentBB) {
  if (!CurrentBB || CurrentBB->empty()) return nullptr;
  
  auto LastInst = std::prev(CurrentBB->end());
  BinaryContext &BC = CurrentBB->getFunction()->getBinaryContext();
  
  // 条件分支：选择最热的路径
  if (BC.MIA->isBranch(*LastInst) && !BC.MIB->isUnconditionalBranch(*LastInst)) {
    if (CurrentBB->succ_size() > 0) {
      BinaryBasicBlock *NextBB = *CurrentBB->succ_begin();
      if (CurrentBB->succ_size() > 1) {
        uint64_t maxCount = 0;
        for (BinaryBasicBlock *Succ : CurrentBB->successors()) {
          if (Succ && Succ->getKnownExecutionCount() > maxCount) {
            maxCount = Succ->getKnownExecutionCount();
            NextBB = Succ;
          }
        }
      }
      return NextBB;
    }
  } else if (!BC.MIA->isBranch(*LastInst)) {
    // 非分支：选择唯一后继或最热的路径
    if (CurrentBB->succ_size() == 1) {
      return *CurrentBB->succ_begin();
    } else if (CurrentBB->succ_size() > 1) {
      uint64_t maxCount = 0;
      BinaryBasicBlock *NextBB = nullptr;
      for (BinaryBasicBlock *Succ : CurrentBB->successors()) {
        if (Succ && Succ->getKnownExecutionCount() > maxCount) {
          maxCount = Succ->getKnownExecutionCount();
          NextBB = Succ;
        }
      }
      return NextBB ? NextBB : *CurrentBB->succ_begin();
    }
  }
  
  return nullptr;
}

namespace opts {
cl::opt<bool> EnablePostLinkOutlining(
    "enable-post-link-outlining",
    cl::desc("enable post-link outlining optimization"),
    cl::init(false),
    cl::cat(BoltOptCategory));

cl::opt<int> PostLinkOutliningLength(
    "post-link-outlining-length",
    cl::desc("maximum sequence length for post-link outlining"),
    cl::init(32),  // 保持32，允许提取较长的序列
    cl::cat(BoltOptCategory));

cl::opt<int> PostLinkOutliningMinLength(
    "post-link-outlining-min-length",
    cl::desc("minimum sequence length for post-link outlining"),
    cl::init(2),
    cl::cat(BoltOptCategory));

cl::opt<bool> PostLinkOutliningPGO(
    "post-link-outlining-pgo",
    cl::desc("enable PGO filtering for post-link outlining"),
    cl::init(false),
    cl::cat(BoltOptCategory));

cl::opt<bool> PostLinkOutliningDebug(
    "post-link-outlining-debug",
    cl::desc("enable debug output for post-link outlining"),
    cl::init(false),
    cl::Hidden,
    cl::cat(BoltOptCategory));
} // namespace opts

namespace llvm {
namespace bolt {

// 获取指定长度的所有指令序列
std::vector<PostLinkOutlining::InstructionSequence> 
PostLinkOutlining::getAllseqs(BinaryContext &BC, BinaryFunction &BF, int len) {
  std::vector<InstructionSequence> sequences;
  
  // Validate length - 使用命令行参数的最小长度
  int minLength = opts::PostLinkOutliningMinLength;
  if (len < minLength || len > LargestLength) {
    return sequences;
  }
  
  // Basic Block 级别的 PGO 过滤
  
  // 过滤规则 4：跳过收缩包装函数
  // 如果函数是收缩包装的，拒绝函数中的所有段
  if (BF.hasEHRanges()) {
    // 收缩包装函数有 EH 范围，跳过外联
    if (opts::PostLinkOutliningDebug) {
      BC.outs() << "BOLT-PLO-DEBUG: Function " << BF.getPrintName() 
                << " is shrink-wrapped (has EH ranges), skipping\n";
    }
    return sequences; // Return empty sequences
  }
  
  // 遍历所有基本块
  // 支持单基本块和跨基本块序列提取
  size_t totalSequences = 0;
  size_t rejectedSequences = 0;
  
  // 统计各过滤规则拒绝的序列数量
  size_t rejectedPseudoCFI = 0;
  size_t rejectedBranchCall = 0;
  size_t rejectedPCRelative = 0;
  size_t rejectedFPLR = 0;
  size_t rejectedModifySP = 0;
  size_t rejectedStackWrite = 0;
  size_t rejectedComplexSP = 0;
  size_t rejectedNonLoadSP = 0;
  size_t rejectedCrossBlock = 0; // 跨基本块序列拒绝计数
  
  // 辅助函数：提取跨基本块序列
  // 从起始BB的startIdx开始，尝试跨越到后续BB
  auto extractCrossBlockSequence = [&](BinaryBasicBlock *StartBB, size_t startIdx, int remainingLen) -> InstructionSequence {
    InstructionSequence seq;
    seq.reserve(len);
    
    BinaryBasicBlock *CurrentBB = StartBB;
    size_t currentIdx = startIdx;
    int collected = 0;
    const int MAX_CROSS_BLOCKS = 3; // 最多跨越3个基本块
    int blockCount = 1;
    
    // 跨基本块序列的 PGO 过滤
    if (EnablePGO && CurrentBB && CurrentBB->hasProfile()) {
      uint64_t bbExecCount = CurrentBB->getKnownExecutionCount();
      const uint64_t HOT_BB_THRESHOLD = 1;
      if (bbExecCount > HOT_BB_THRESHOLD) {
        // 起始 basic block 是热的，拒绝跨基本块序列
        return InstructionSequence(); // 返回空序列
      }
    }
    
    while (collected < remainingLen && blockCount <= MAX_CROSS_BLOCKS) {
      if (!CurrentBB || CurrentBB->empty() || currentIdx >= CurrentBB->size()) {
            break;
          }
          
      // 检查当前 basic block 的执行频率
      if (EnablePGO && CurrentBB->hasProfile()) {
        uint64_t bbExecCount = CurrentBB->getKnownExecutionCount();
        const uint64_t HOT_BB_THRESHOLD = 1;
        if (bbExecCount > HOT_BB_THRESHOLD) {
          // 遇到热 basic block，停止提取
          break;
        }
      }
          
      // 从当前BB收集指令
      auto It = CurrentBB->begin();
      std::advance(It, currentIdx);
      
      while (collected < remainingLen && It != CurrentBB->end()) {
        MCInst Inst = *It;
        bool isLastInSeq = (collected == remainingLen - 1);
        
        // 使用统一的过滤函数（允许分支，因为是跨基本块序列）
        int rejectReason = shouldRejectInstruction(BC, Inst, len, true, isLastInSeq, CurrentBB, startIdx, currentIdx);
        if (rejectReason != 0) {
          // 无条件跳转特殊处理
          if (BC.MIA->isBranch(Inst) && BC.MIB->isUnconditionalBranch(Inst))
            return InstructionSequence();
          return InstructionSequence();
        }
        
        seq.push_back(Inst);
        collected++;
        ++It;
        currentIdx++;
        
        // 如果是call或条件分支且是最后一条，结束收集
        if ((BC.MIB->isCall(Inst) || (BC.MIA->isBranch(Inst) && !BC.MIB->isUnconditionalBranch(Inst))) && isLastInSeq)
          break;
      }
      
      // 移动到下一个BB
      if (collected < remainingLen && currentIdx >= CurrentBB->size()) {
        BinaryBasicBlock *NextBB = getNextBasicBlock(CurrentBB);
        if (!NextBB) break;
        CurrentBB = NextBB;
        currentIdx = 0;
        blockCount++;
      } else {
        break;
      }
    }
    
    // 如果收集的指令数不足，返回空序列
    if (collected < remainingLen) {
      return InstructionSequence();
    }
    
    return seq;
  };
  
  // 首先提取单基本块内的序列（原有逻辑）
  for (BinaryBasicBlock &BB : BF) {
    // Skip empty basic blocks
    if (BB.empty())
      continue;
    
    // Basic Block 级别的 PGO 过滤
    if (EnablePGO) {
      if (BB.hasProfile()) {
        uint64_t bbExecCount = BB.getKnownExecutionCount();
        const uint64_t HOT_BB_THRESHOLD = 1; // 与函数级别阈值一致
        if (bbExecCount > HOT_BB_THRESHOLD) {
          // 热 basic block：跳过，不提取序列
          if (opts::PostLinkOutliningDebug && totalSequences == 0) {
            BC.outs() << "BOLT-PLO-DEBUG: Skipping hot BB (execCount=" << bbExecCount 
                      << ") in function " << BF.getPrintName() << "\n";
          }
          continue; // 跳过这个热 basic block
        }
      }
      // 如果没有 profile 数据，允许提取（保守策略）
    }
    
    // 入口块可能包含可外联序列（在序言之后）
    
    // Get the number of instructions in this basic block
    size_t numInsts = BB.size();
    
    // Extract sequences of length 'len' from this basic block
    // We can extract sequences from position 0 to (numInsts - len)
    size_t len_size = static_cast<size_t>(len);
    if (len_size > numInsts)
      continue; // Skip if basic block is too small
    
    for (size_t i = 0; i + len_size <= numInsts; ++i) {
      InstructionSequence seq;
      seq.reserve(len);
      bool shouldReject = false;
      
      // Extract 'len' consecutive instructions using iterator
      auto It = BB.begin();
      std::advance(It, i);
      for (size_t j = 0; j < len_size; ++j) {
        if (It == BB.end())
          break; // Safety check
        MCInst Inst = *It;
        
        // 使用统一的过滤函数
        int rejectReason = shouldRejectInstruction(BC, Inst, len, false, (j == len_size - 1), &BB, i, i + j);
        if (rejectReason != 0) {
          shouldReject = true;
          // 更新统计计数
          if (rejectReason == 1) rejectedPseudoCFI++;
          else if (rejectReason == 2) rejectedBranchCall++;
          else if (rejectReason == 3) rejectedPCRelative++;
          else if (rejectReason == 4) rejectedFPLR++;
          else if (rejectReason == 5) rejectedModifySP++;
          else if (rejectReason == 6) rejectedNonLoadSP++;
          else if (rejectReason == 7) rejectedComplexSP++;
          break;
        }
        
        seq.push_back(Inst);
        ++It;
      }
      
      // 只有当序列达到完整长度且通过所有过滤时才添加
      if (seq.size() == len_size && !shouldReject) {
        sequences.push_back(std::move(seq));
        totalSequences++;
      } else if (shouldReject) {
        rejectedSequences++;
      }
    }
    
    // 尝试提取跨基本块序列
    if (numInsts > 0 && numInsts < len_size) {
      // 当前BB太小，无法容纳完整序列，尝试跨基本块
      for (size_t i = 0; i < numInsts; ++i) {
        InstructionSequence crossSeq = extractCrossBlockSequence(&BB, i, len);
        
        if (!crossSeq.empty() && crossSeq.size() == len_size) {
          // 验证跨基本块序列是否通过所有过滤规则
          bool crossReject = false;
          size_t instIdx = 0;
          
          for (const MCInst &Inst : crossSeq) {
            bool isLastInSeq = (instIdx == crossSeq.size() - 1);
            int rejectReason = shouldRejectInstruction(BC, Inst, len, true, isLastInSeq, &BB, i, i + instIdx);
            if (rejectReason != 0) {
              crossReject = true;
              // 更新统计
              if (rejectReason == 1) rejectedPseudoCFI++;
              else if (rejectReason == 2) rejectedBranchCall++;
              else if (rejectReason == 3) rejectedPCRelative++;
              else if (rejectReason == 4) rejectedFPLR++;
              else if (rejectReason == 5) rejectedModifySP++;
              else if (rejectReason == 6) rejectedNonLoadSP++;
              else if (rejectReason == 7) rejectedComplexSP++;
              break;
            }
            instIdx++;
          }
          
          if (!crossReject) {
            sequences.push_back(std::move(crossSeq));
            totalSequences++;
            if (opts::PostLinkOutliningDebug && totalSequences <= 5) {
              BC.outs() << "BOLT-PLO-DEBUG: Extracted cross-block sequence of length " 
                        << len << " starting at BB[" << i << "]\n";
            }
          } else {
            rejectedCrossBlock++;
            rejectedSequences++;
          }
        }
      }
    }
  }
  
  if (opts::PostLinkOutliningDebug) {
    if (totalSequences > 0) {
      BC.outs() << "BOLT-PLO-DEBUG: Function " << BF.getPrintName() 
                << ": extracted " << totalSequences << " sequences of length " 
                << len << " (rejected: " << rejectedSequences << ")\n";
    } else if (rejectedSequences > 0 || BF.size() > 0) {
      // Show when no sequences found to help debug
      BC.outs() << "BOLT-PLO-DEBUG: Function " << BF.getPrintName() 
                << ": no valid sequences of length " << len 
                << " (rejected: " << rejectedSequences 
                << ", BBs: " << BF.size() << ")\n";
      
      // 详细统计各过滤规则的拒绝数量
      if (rejectedSequences > 0) {
        BC.outs() << "BOLT-PLO-DEBUG: Rejection breakdown: "
                  << "pseudo/CFI=" << rejectedPseudoCFI << ", "
                  << "branch/call=" << rejectedBranchCall << ", "
                  << "PC-rel=" << rejectedPCRelative << ", "
                  << "FP/LR=" << rejectedFPLR << ", "
                  << "modifySP=" << rejectedModifySP << ", "
                  << "stackWrite=" << rejectedStackWrite << ", "
                  << "complexSP=" << rejectedComplexSP << ", "
                  << "nonLoadSP=" << rejectedNonLoadSP << "\n";
      }
    }
  }
  
  return sequences;
}

// PGO 热函数过滤（已在 getAllseqs 中处理）
void PostLinkOutlining::filterHotFuncs(std::vector<InstructionSequence> &seqs,
                                       BinaryFunction &BF) {
  return;
}

// 检查两个序列是否有重叠指令
bool PostLinkOutlining::hasOverlappedInstrs(const InstructionSequence &seq1,
                                             const InstructionSequence &seq2) {
  for (const MCInst &Inst1 : seq1) {
    for (const MCInst &Inst2 : seq2) {
      if (areInstructionsEqual(Inst1, Inst2)) {
        return true;
      }
    }
  }
  return false;
}

// 标记序列已处理
void PostLinkOutlining::setLabel(const InstructionSequence *seq) {
  if (seq) {
    LabeledSequences.insert(seq);
  }
}

// 检查序列是否已标记
bool PostLinkOutlining::isLabeled(const InstructionSequence *seq) {
  if (!seq) return false;
  return LabeledSequences.find(seq) != LabeledSequences.end();
}

// 将寄存器归一化到规范形式以便匹配
static uint64_t normalizeRegister(uint64_t reg, 
                                   std::map<uint64_t, uint64_t> &regMap,
                                   uint64_t &nextRegId) {
  // 特殊寄存器（SP=31, FP=29, LR=30）保持原值
  if (reg == 31 || reg == 29 || reg == 30) {
    return reg;
  }
  
  // 通用寄存器映射到规范形式
  if (regMap.find(reg) == regMap.end()) {
    regMap[reg] = nextRegId++;
  }
  return regMap[reg];
}

// 使用 FNV-1a 哈希算法计算序列哈希值
uint64_t PostLinkOutlining::getHash(const InstructionSequence &seq) {
  uint64_t hash = 14695981039346656037ULL;
  const uint64_t FNV_prime = 1099511628211ULL;
  
  std::map<uint64_t, uint64_t> regMap;
  uint64_t nextRegId = 1000;
  
  for (const MCInst &Inst : seq) {
    uint64_t opcode = Inst.getOpcode();
    hash ^= opcode;
    hash *= FNV_prime;
    
    for (int i = 0, e = Inst.getNumOperands(); i < e; ++i) {
      const MCOperand &Operand = Inst.getOperand(i);
      
      if (Operand.isReg()) {
        uint64_t normalizedReg = normalizeRegister(Operand.getReg(), regMap, nextRegId);
        hash ^= normalizedReg;
        hash *= FNV_prime;
      } else if (Operand.isImm()) {
        int64_t imm = Operand.getImm();
        hash ^= static_cast<uint64_t>(imm);
        hash *= FNV_prime;
      } else if (Operand.isExpr()) {
        hash ^= 0xDEADBEEF;
        hash *= FNV_prime;
      } else if (Operand.isSFPImm()) {
        float fpImm = Operand.getSFPImm();
        hash ^= std::hash<float>{}(fpImm);
        hash *= FNV_prime;
      }
    }
  }
  
  return hash;
}

// 从指令序列创建外联函数
BinaryFunction *PostLinkOutlining::createFunction(BinaryContext &BC,
                                                  const InstructionSequence &seq) {
  if (seq.empty()) {
    return nullptr;
  }
  
  static unsigned OutlinedFunctionCount = 0;
  std::string FuncName = "PLO_outlined_" + std::to_string(++OutlinedFunctionCount);
  
  BinaryFunction *OutlinedFunc = BC.createInjectedBinaryFunction(FuncName);
  if (!OutlinedFunc) {
    if (opts::PostLinkOutliningDebug) {
      BC.outs() << "BOLT-PLO-DEBUG: Failed to create outlined function\n";
    }
    return nullptr;
  }
  
  if (OutlinedFunc->getState() < BinaryFunction::State::CFG) {
    if (opts::PostLinkOutliningDebug) {
      BC.outs() << "BOLT-PLO-DEBUG: Warning: Outlined function state is " 
                << static_cast<int>(OutlinedFunc->getState()) 
                << " (below CFG), this may cause issues\n";
    }
  }
  
  std::string CodeSectionName = ".text." + OutlinedFunc->getOneName().str();
  OutlinedFunc->setCodeSectionName(CodeSectionName);
  
  auto TextSectionRange = BC.getSectionByName(".text");
  auto It = TextSectionRange.begin();
  if (It != TextSectionRange.end()) {
    OutlinedFunc->setOriginSection(It->second);
  } else {
    // Fallback: find first text section
    for (auto &Section : BC.sections()) {
      if (Section.isText()) {
        OutlinedFunc->setOriginSection(&Section);
        break;
      }
    }
  }
  
  MCContext &Ctx = *BC.Ctx;
  MCSymbol *BBLabel = Ctx.createNamedTempSymbol("outlined_bb");
  
  BinaryBasicBlock *BB = OutlinedFunc->addBasicBlock(BBLabel);
  if (!BB) {
    if (opts::PostLinkOutliningDebug) {
      BC.outs() << "BOLT-PLO-DEBUG: Failed to create basic block for outlined function\n";
    }
    return nullptr;
  }
  
  BB->setCFIState(0);
  
  MCPlusBuilder *MIB = BC.MIB.get();
  MCSymbol *ReturnLabel = nullptr;
  bool hasConditionalBranch = false;
  
  // 检查序列中是否有条件分支
  for (size_t i = 0; i < seq.size(); ++i) {
    const MCInst &Inst = seq[i];
    if (BC.MIA->isBranch(Inst) && !MIB->isUnconditionalBranch(Inst)) {
      hasConditionalBranch = true;
      break;
    }
  }
  
  // 如果有条件分支，创建返回标签
  if (hasConditionalBranch) {
    ReturnLabel = Ctx.createNamedTempSymbol("outlined_return");
  }
  
  for (size_t instIdx = 0; instIdx < seq.size(); ++instIdx) {
    const MCInst &OriginalInst = seq[instIdx];
    if (MIB->isCFI(OriginalInst) || MIB->isPseudo(OriginalInst))
      continue;
    
    MCInst NewInst;
    NewInst.setOpcode(OriginalInst.getOpcode());
    
    bool isConditionalBranch = (BC.MIA->isBranch(OriginalInst) && 
                                 !MIB->isUnconditionalBranch(OriginalInst));
    
    if (isConditionalBranch) {
      if (!ReturnLabel) {
        ReturnLabel = Ctx.createNamedTempSymbol("outlined_return");
        hasConditionalBranch = true;
      }
      // 条件分支目标重定向到返回标签
      for (const MCOperand &Op : MCPlus::primeOperands(OriginalInst)) {
        if (Op.isExpr()) {
          NewInst.addOperand(MCOperand::createExpr(
            MCSymbolRefExpr::create(ReturnLabel, Ctx)));
        } else {
          NewInst.addOperand(Op);
        }
      }
    } else {
      for (const MCOperand &Op : MCPlus::primeOperands(OriginalInst)) {
        NewInst.addOperand(Op);
      }
    }
    
    BB->addInstruction(std::move(NewInst));
  }
  
  // 处理条件分支的返回标签
  if (hasConditionalBranch && ReturnLabel) {
    BinaryBasicBlock *ReturnBB = OutlinedFunc->addBasicBlock(ReturnLabel);
    if (ReturnBB) {
      ReturnBB->setCFIState(0);
      MCInst RetInst;
      MIB->createReturn(RetInst);
      ReturnBB->addInstruction(std::move(RetInst));
    } else {
      MCInst RetInst;
      MIB->createReturn(RetInst);
      BB->addInstruction(std::move(RetInst));
    }
  } else {
    MCInst RetInst;
    MIB->createReturn(RetInst);
    BB->addInstruction(std::move(RetInst));
  }
  
  if (opts::PostLinkOutliningDebug) {
    BC.outs() << "BOLT-PLO-DEBUG: Created outlined function " << FuncName 
              << " with " << seq.size() << " instructions\n";
    BC.outs() << "BOLT-PLO-DEBUG: Outlined function BB size: " << BB->size() << "\n";
    BC.outs() << "BOLT-PLO-DEBUG: Outlined function state: " 
              << static_cast<int>(OutlinedFunc->getState()) << "\n";
    BC.outs() << "BOLT-PLO-DEBUG: Outlined function symbol: " 
              << (OutlinedFunc->getSymbol() ? OutlinedFunc->getSymbol()->getName() : "null") << "\n";
  }
  
  return OutlinedFunc;
}

// 栈帧管理：添加序言/结语并修正栈访问偏移
void PostLinkOutlining::stackFrameManage(BinaryFunction &outlinedFunc, bool IsCalledViaSandwich) {
  BinaryContext &BC = outlinedFunc.getBinaryContext();
  
  if (opts::PostLinkOutliningDebug) {
    BC.outs() << "BOLT-PLO-DEBUG: Starting stack frame management for " 
              << outlinedFunc.getPrintName() << "\n";
    BC.outs() << "BOLT-PLO-DEBUG: Function empty (by empty()): " << (outlinedFunc.empty() ? "yes" : "no") << "\n";
    BC.outs() << "BOLT-PLO-DEBUG: Function size (basic blocks): " << outlinedFunc.size() << "\n";
    
    // Check if any basic block has instructions
    size_t totalInsts = 0;
    for (const BinaryBasicBlock &BB : outlinedFunc) {
      totalInsts += BB.size();
      if (totalInsts == 0) {
        BC.outs() << "BOLT-PLO-DEBUG: BB size: " << BB.size() << "\n";
      }
    }
    BC.outs() << "BOLT-PLO-DEBUG: Total instructions in function: " << totalInsts << "\n";
  }
  
  // Check if function has any basic blocks with instructions
  bool hasInstructions = false;
  for (const BinaryBasicBlock &BB : outlinedFunc) {
    if (!BB.empty()) {
      hasInstructions = true;
      break;
    }
  }
  
  if (!hasInstructions) {
    if (opts::PostLinkOutliningDebug) {
      BC.outs() << "BOLT-PLO-DEBUG: Warning: Outlined function has no instructions, skipping stack frame management\n";
    }
    return;
  }
  MCPlusBuilder *MIB = BC.MIB.get();
  
  // Only support AArch64 for now
  if (!BC.isAArch64()) {
    if (opts::PostLinkOutliningDebug) {
      BC.outs() << "BOLT-PLO-DEBUG: Stack frame management not implemented for non-AArch64\n";
    }
    return;
  }
  
  // Get the first basic block (should be the only one for outlined functions)
  BinaryBasicBlock *BB = nullptr;
  for (BinaryBasicBlock &B : outlinedFunc) {
    BB = &B;
    break;
  }
  
  if (!BB || BB->empty()) {
    return;
  }
  
  const MCPhysReg LR = 30;  // AArch64::X30 (LR)
  
  if (opts::PostLinkOutliningDebug) {
    MCPhysReg MIB_SP = MIB->getStackPointer();
    MCPhysReg MIB_FP = MIB->getFramePointer();
    BC.outs() << "BOLT-PLO-DEBUG: Register constants - SP: " << MIB_SP 
              << " (AArch64::SP), FP: " << MIB_FP 
              << " (AArch64::FP/X29), LR: " << LR 
              << " (AArch64::X30)\n";
    if (MIB_SP != 31 || MIB_FP != 29) {
      BC.outs() << "BOLT-PLO-DEBUG: Warning: MIB register values differ - "
                << "MIB_SP: " << MIB_SP << " (expected 31), MIB_FP: " << MIB_FP << " (expected 29)\n";
    }
  }
  
  // 修正栈访问偏移（必须在插入序言前执行）
  // Sandwich 调用：Push(16) + Call + Pop(16) = 32字节
  // 普通调用：16字节
  const int64_t ByteFixOffset = IsCalledViaSandwich ? 32 : 16;
  MCPhysReg SP = MIB->getStackPointer();
  MCPhysReg FP = MIB->getFramePointer();
  int FixedCount = 0;

  for (BinaryBasicBlock &CurrBB : outlinedFunc) {
    for (MCInst &Inst : CurrBB) {
      bool isLoadStore = BC.MII->get(Inst.getOpcode()).mayLoad() || 
                         BC.MII->get(Inst.getOpcode()).mayStore();
      
      bool isAddSub = false;
      StringRef InstName = BC.InstPrinter->getOpcodeName(Inst.getOpcode());
      std::string InstNameLower = InstName.lower();
      if ((InstNameLower.find("add") == 0 || InstNameLower.find("sub") == 0) &&
          InstNameLower.find("sp") != std::string::npos) {
        // 排除修改 SP 的栈调整指令
        bool modifiesSP = false;
        const MCInstrDesc &Desc = BC.MII->get(Inst.getOpcode());
        for (int i = 0; i < (int)Inst.getNumOperands() && i < (int)Desc.getNumDefs(); ++i) {
          const MCOperand &Op = Inst.getOperand(i);
          if (Op.isReg() && Op.getReg() == SP) {
            modifiesSP = true;
            break;
          }
        }
        if (!modifiesSP) {
          isAddSub = true;
        }
      }
      
      if (!isLoadStore && !isAddSub) {
        continue;
      }

      int Scale = isLoadStore ? getInstructionScale(BC, Inst) : 1;
      
      if (ByteFixOffset % Scale != 0) {
          if (opts::PostLinkOutliningDebug) {
            BC.outs() << "BOLT-PLO-WARNING: Stack offset " << ByteFixOffset 
                     << " is not divisible by scale " << Scale << "\n";
          }
      }
      int64_t ImmAdjustment = ByteFixOffset / Scale;

      bool fixed = false;
      for (int i = 0; i < (int)Inst.getNumOperands() && !fixed; ++i) {
        const MCOperand &Op = Inst.getOperand(i);
        
        if (Op.isReg() && Op.getReg() == SP) {
          if (isLoadStore) {
            // Load/Store: [sp, #imm]
            if (i + 1 < (int)Inst.getNumOperands()) {
              MCOperand &NextOp = Inst.getOperand(i + 1);
              if (NextOp.isImm()) {
                NextOp.setImm(NextOp.getImm() + ImmAdjustment);
                FixedCount++;
                fixed = true;
              }
            }
          } else if (isAddSub) {
            // ADD/SUB: add x0, sp, #imm
            for (int j = i + 1; j < (int)Inst.getNumOperands(); ++j) {
              MCOperand &ImmOp = Inst.getOperand(j);
              if (ImmOp.isImm()) {
                ImmOp.setImm(ImmOp.getImm() + ImmAdjustment);
                FixedCount++;
                fixed = true;
                break;
              }
            }
          }
        }
      }
    }
  }

  if (opts::PostLinkOutliningDebug) {
    BC.outs() << "BOLT-PLO-DEBUG: Fixed " << FixedCount << " stack access instructions\n";
  }
  
  // 检测是否是纯函数（无栈访问、无call、不使用FP、无条件分支）
  bool hasStackAccess = (FixedCount > 0);
  bool needsLR = false;
  bool usesFP = false;
  bool hasConditionalBranch = false;
  
  for (const BinaryBasicBlock &CurrBB : outlinedFunc) {
    for (const MCInst &Inst : CurrBB) {
      if (BC.MIA->isBranch(Inst) && !MIB->isUnconditionalBranch(Inst)) {
        hasConditionalBranch = true;
        break;
      }
      
      if (MIB->isCall(Inst)) {
        needsLR = true;
      }
      
      // 检测FP使用
      const MCInstrDesc &Desc = BC.MII->get(Inst.getOpcode());
      for (int i = 0; i < (int)Inst.getNumOperands(); ++i) {
        const MCOperand &Op = Inst.getOperand(i);
        if (Op.isReg() && Op.getReg() == FP) {
          // 如果是源操作数（use），需要保存FP
          if (i >= (int)Desc.getNumDefs()) {
            usesFP = true;
            break;
          }
        }
      }
      
      if (hasConditionalBranch || (needsLR && usesFP)) break;
    }
    if (hasConditionalBranch || (needsLR && usesFP)) break;
  }
  
  // [优化5.1] 如果函数是纯函数，跳过prologue/epilogue
  bool isPureFunction = !hasStackAccess && !needsLR && !usesFP && !hasConditionalBranch;
  
  if (isPureFunction) {
    return;
  }
  
  // 创建并插入序言：stp x29, x30, [sp, #-16]!
  MCInst Prologue;
  MCPhysReg PrologueFP = MIB->getFramePointer();
  MCPhysReg PrologueLR = getLinkRegister(BC);
  
  MIB->createPushRegisters(Prologue, PrologueFP, PrologueLR);
  BB->insertInstruction(BB->begin(), std::move(Prologue));
  
  // 尾调用优化：将 bl 改成 b
  bool tailCallOptimized = false;
    if (BB->size() >= 2) {
    auto LastIt = BB->end();
    --LastIt;
    auto SecondLastIt = LastIt;
    --SecondLastIt;
    
    MCInst &CallInst = *SecondLastIt;
    MCInst &RetInst = *LastIt;
    
    if (MIB->isCall(CallInst) && MIB->isReturn(RetInst)) {
      if (!MIB->isIndirectCall(CallInst)) {
        MCSymbol *TargetSymbol = nullptr;
        for (const MCOperand &Op : MCPlus::primeOperands(CallInst)) {
          if (Op.isExpr()) {
            const MCSymbolRefExpr *SRE = dyn_cast<MCSymbolRefExpr>(Op.getExpr());
            if (SRE) {
              TargetSymbol = const_cast<MCSymbol*>(&SRE->getSymbol());
              break;
            }
          }
        }
        
        if (TargetSymbol) {
          MCInst BranchInst;
          MCContext &Ctx = *BC.Ctx;
          BC.MIB->createUncondBranch(BranchInst, TargetSymbol, &Ctx);
          *SecondLastIt = BranchInst;
          BB->eraseInstruction(LastIt);
          tailCallOptimized = true;
        } else {
          BB->eraseInstruction(LastIt);
          tailCallOptimized = true;
        }
      }
    }
  }
  
  if (tailCallOptimized) {
    return;
  }
  
  // 创建并插入结语：ldp x29, x30, [sp], #16
  auto InsertPoint = BB->end();
  for (auto It = BB->begin(); It != BB->end(); ++It) {
    if (MIB->isReturn(*It)) {
      InsertPoint = It;
      break;
    }
  }
  
  MCInst Epilogue;
  MCPhysReg EpilogueFP = MIB->getFramePointer();
  MCPhysReg EpilogueLR = getLinkRegister(BC);
  MIB->createPopRegisters(Epilogue, EpilogueFP, EpilogueLR);
  BB->insertInstruction(InsertPoint, std::move(Epilogue));
  
  if (opts::PostLinkOutliningDebug) {
    BC.outs() << "BOLT-PLO-DEBUG: Stack frame management applied to outlined function " 
              << outlinedFunc.getPrintName() << "\n";
    BC.outs() << "BOLT-PLO-DEBUG: Added prologue (stp x29, x30, [sp, #-16]!) and epilogue (ldp x29, x30, [sp], #16)\n";
    BC.outs() << "BOLT-PLO-DEBUG: BB size after stack frame management: " << BB->size() << "\n";
    
  }
}

// 查找序列在函数中的所有位置
std::vector<PostLinkOutlining::SequenceLocation> 
PostLinkOutlining::findSequenceLocations(BinaryContext &BC,
                                         BinaryFunction &BF,
                                         const InstructionSequence &Seq) {
  std::vector<SequenceLocation> Locations;
  
  if (Seq.empty())
    return Locations;
  
  size_t SeqLen = Seq.size();
  
  for (BinaryBasicBlock &BB : BF) {
    // 单基本块匹配
    if (BB.size() >= SeqLen) {
      for (size_t i = 0; i + SeqLen <= BB.size(); ++i) {
        bool Matches = true;
        auto It = BB.begin();
        std::advance(It, i);
        for (size_t j = 0; j < SeqLen; ++j) {
          if (It == BB.end()) {
            Matches = false;
            break;
          }
          if (!areInstructionsEqual(*It, Seq[j])) {
            Matches = false;
            break;
          }
          ++It;
        }
        
        if (Matches) {
          Locations.emplace_back(&BB, i, &Seq);
        }
      }
    }
    
    // 跨基本块序列匹配
    if (BB.size() > 0 && BB.size() < SeqLen) {
      auto checkCrossBlockMatch = [&](BinaryBasicBlock *StartBB, size_t startIdx) -> bool {
        BinaryBasicBlock *CurrentBB = StartBB;
        size_t currentIdx = startIdx;
        size_t matched = 0;
        const int MAX_CROSS_BLOCKS = 3;
        int blockCount = 1;
        
        while (matched < SeqLen && blockCount <= MAX_CROSS_BLOCKS) {
          if (!CurrentBB || CurrentBB->empty() || currentIdx >= CurrentBB->size()) {
            return false;
          }
          
          auto It = CurrentBB->begin();
          std::advance(It, currentIdx);
          
          while (matched < SeqLen && It != CurrentBB->end()) {
            // 检查指令是否匹配
            if (!areInstructionsEqual(*It, Seq[matched])) {
              return false;
            }
            
            matched++;
            ++It;
            currentIdx++;
            
            if (matched == SeqLen) {
              if (matched > 0 && BC.MIA->isBranch(Seq[matched - 1]) && 
                  !BC.MIB->isUnconditionalBranch(Seq[matched - 1])) {
                return true;
              }
            }
          }
          
          if (matched < SeqLen && currentIdx >= CurrentBB->size()) {
            BinaryBasicBlock *NextBB = nullptr;
            
            if (!CurrentBB->empty()) {
              auto LastInst = std::prev(CurrentBB->end());
              
              if (BC.MIA->isBranch(*LastInst) && !BC.MIB->isUnconditionalBranch(*LastInst)) {
                if (CurrentBB->succ_size() > 0) {
                  NextBB = *CurrentBB->succ_begin();
                  
                  if (CurrentBB->succ_size() > 1) {
                    uint64_t maxCount = 0;
                    for (BinaryBasicBlock *Succ : CurrentBB->successors()) {
                      if (Succ && Succ->getKnownExecutionCount() > maxCount) {
                        maxCount = Succ->getKnownExecutionCount();
                        NextBB = Succ;
                      }
                    }
                    if (maxCount == 0) {
                      NextBB = *CurrentBB->succ_begin();
                    }
                  }
                }
              } else if (!BC.MIA->isBranch(*LastInst)) {
                // 非分支指令：尝试获取直接后继
                if (CurrentBB->succ_size() == 1) {
                  NextBB = *CurrentBB->succ_begin();
                } else if (CurrentBB->succ_size() > 1) {
                  // [完善2] 多个后继：选择最可能的路径
                  uint64_t maxCount = 0;
                  for (BinaryBasicBlock *Succ : CurrentBB->successors()) {
                    if (Succ && Succ->getKnownExecutionCount() > maxCount) {
                      maxCount = Succ->getKnownExecutionCount();
                      NextBB = Succ;
                    }
                  }
                  if (maxCount == 0 && CurrentBB->succ_size() > 0) {
                    NextBB = *CurrentBB->succ_begin();
                  }
                }
              }
            }
            
            if (!NextBB) {
              return false;
            }
            
            CurrentBB = NextBB;
            currentIdx = 0;
            blockCount++;
          } else {
            break;
          }
        }
        
        return matched == SeqLen;
      };
      
      // 尝试从当前BB的每个位置开始匹配
      for (size_t i = 0; i < BB.size(); ++i) {
        if (checkCrossBlockMatch(&BB, i)) {
          SequenceLocation Loc(&BB, i, &Seq);
          // 注意：跨基本块序列的路径信息可以在替换时计算
          Locations.push_back(Loc);
          
          if (opts::PostLinkOutliningDebug && Locations.size() <= 3) {
            BC.outs() << "BOLT-PLO-DEBUG: Found matching cross-block sequence at BB[" << i << "]\n";
          }
        }
      }
    }
  }
  
  if (opts::PostLinkOutliningDebug) {
    BC.outs() << "BOLT-PLO-DEBUG: Total locations found: " << Locations.size() << "\n";
  }
  
  return Locations;
}

// Replace a sequence with a call to the outlined function
void PostLinkOutlining::replaceSequenceWithCall(BinaryContext &BC,
                                                  BinaryFunction &BF,
                                                  const SequenceLocation &Loc,
                                                  BinaryFunction *OutlinedFunc) {
  if (!OutlinedFunc || Loc.Seq->empty())
    return;
  
  BinaryBasicBlock *BB = Loc.BB;
  size_t StartIdx = Loc.StartIndex;
  size_t SeqLen = Loc.Seq->size();
  
  // 检查是否为跨基本块序列
  bool isCrossBlock = (StartIdx + SeqLen > BB->size());
  
  if (!isCrossBlock && (StartIdx >= BB->size() || StartIdx + SeqLen > BB->size())) {
    if (opts::PostLinkOutliningDebug) {
      BC.outs() << "BOLT-PLO-DEBUG: Warning: Invalid sequence location (StartIdx=" 
                << StartIdx << ", SeqLen=" << SeqLen << ", BB->size()=" 
                << BB->size() << ")\n";
    }
    return;
  }
  
  if (isCrossBlock && StartIdx >= BB->size()) {
    return;
  }
  
  MCInst CallInst;
  MCContext &Ctx = *BC.Ctx;
  MCPlusBuilder *MIB = BC.MIB.get();
  
  MCSymbol *OutlinedFuncLabel = OutlinedFunc->getSymbol();
  if (!OutlinedFuncLabel) {
    std::string FuncName = OutlinedFunc->getPrintName();
    OutlinedFuncLabel = Ctx.getOrCreateSymbol(FuncName);
  }
  
  if (BC.isAArch64()) {
    MIB->createCall(CallInst, OutlinedFuncLabel, &Ctx);
    
    if (opts::PostLinkOutliningDebug) {
      BC.outs() << "BOLT-PLO-DEBUG: Created call instruction to " 
                << OutlinedFunc->getPrintName() 
                << " (symbol: " << OutlinedFuncLabel->getName() << ")\n";
      BC.outs() << "BOLT-PLO-DEBUG: Call instruction opcode: " << CallInst.getOpcode() << "\n";
    }
  } else {
    // Other architectures: not implemented yet
    if (opts::PostLinkOutliningDebug) {
      BC.outs() << "BOLT-PLO-DEBUG: Warning: Cannot create call for non-AArch64 architecture\n";
    }
    return;
  }
  
  BinaryFunction *CallerFunc = BB->getFunction();
  bool IsRealLeaf = isLeafFunction(CallerFunc) && !CallerFunc->isInjected();
  bool SafeToCall = isLRSavedAtPoint(BC, BB, StartIdx);
  bool IsCallerLeaf = IsRealLeaf || !SafeToCall;
  
  if (isCrossBlock) {
    // 跨基本块序列替换
    BinaryBasicBlock *CurrentBB = BB;
    size_t currentIdx = StartIdx;
    size_t remaining = SeqLen;
    const int MAX_CROSS_BLOCKS = 3;
    int blockCount = 1;
    
    // 收集序列跨越的所有基本块和索引
    std::vector<std::pair<BinaryBasicBlock *, size_t>> blockRanges;
    
    while (remaining > 0 && blockCount <= MAX_CROSS_BLOCKS && CurrentBB) {
      size_t availableInBB = CurrentBB->size() - currentIdx;
      size_t toRemove = std::min(remaining, availableInBB);
      
      blockRanges.push_back({CurrentBB, currentIdx});
      
      remaining -= toRemove;
      
      // 移动到下一个BB
      if (remaining > 0 && currentIdx + toRemove >= CurrentBB->size()) {
        BinaryBasicBlock *NextBB = getNextBasicBlock(CurrentBB);
        if (!NextBB) {
          if (opts::PostLinkOutliningDebug) {
            BC.outs() << "BOLT-PLO-DEBUG: Warning: Cannot find next BB for cross-block sequence\n";
          }
          return;
        }
        CurrentBB = NextBB;
        currentIdx = 0;
        blockCount++;
      } else {
        break;
      }
    }
    
    if (remaining > 0) {
      if (opts::PostLinkOutliningDebug) {
        BC.outs() << "BOLT-PLO-DEBUG: Warning: Cross-block sequence replacement incomplete\n";
      }
      return;
    }
    
    // 执行替换：在起始BB插入调用，删除所有BB中的序列指令
    if (IsCallerLeaf) {
      // Leaf function: Insert Push + Call + Pop
      MCInst PushInst, PopInst;
      MCPhysReg FP = MIB->getFramePointer();
      MCPhysReg LR = getLinkRegister(BC);
      
      MIB->createPushRegisters(PushInst, FP, LR);
      MIB->createPopRegisters(PopInst, FP, LR);
      
      // 在起始BB插入 Push + Call + Pop
      BinaryBasicBlock *StartBB = blockRanges[0].first;
      size_t startIdx = blockRanges[0].second;
      
      *(StartBB->begin() + startIdx) = PushInst;
      StartBB->insertInstruction(StartBB->begin() + startIdx + 1, std::move(CallInst));
      StartBB->insertInstruction(StartBB->begin() + startIdx + 2, std::move(PopInst));
      
      // 删除起始BB中剩余的序列指令
      size_t startBBRemaining = StartBB->size() - startIdx - 3;
      size_t startBBToRemove = std::min(startBBRemaining, SeqLen - 3);
      for (size_t k = 0; k < startBBToRemove; ++k) {
        if (startIdx + 3 < StartBB->size()) {
          StartBB->eraseInstruction(StartBB->begin() + startIdx + 3);
        }
      }
      
      // 删除后续BB中的所有序列指令
      for (size_t i = 1; i < blockRanges.size(); ++i) {
        BinaryBasicBlock *BBToClean = blockRanges[i].first;
        size_t cleanStartIdx = blockRanges[i].second;
        size_t toRemove = std::min(BBToClean->size() - cleanStartIdx, SeqLen);
        
        for (size_t k = 0; k < toRemove && cleanStartIdx < BBToClean->size(); ++k) {
          BBToClean->eraseInstruction(BBToClean->begin() + cleanStartIdx);
        }
      }
      
      if (opts::PostLinkOutliningDebug) {
        BC.outs() << "BOLT-PLO-DEBUG: Replaced cross-block sequence with sandwich call (spans " 
                  << blockRanges.size() << " blocks)\n";
      }
    } else {
      // Non-leaf function: Insert Call only
      BinaryBasicBlock *StartBB = blockRanges[0].first;
      size_t startIdx = blockRanges[0].second;
      
      *(StartBB->begin() + startIdx) = CallInst;
      
      // 删除起始BB中剩余的序列指令
      size_t startBBRemaining = StartBB->size() - startIdx - 1;
      size_t startBBToRemove = std::min(startBBRemaining, SeqLen - 1);
      for (size_t k = 0; k < startBBToRemove; ++k) {
        if (startIdx + 1 < StartBB->size()) {
          StartBB->eraseInstruction(StartBB->begin() + startIdx + 1);
        }
      }
      
      // 删除后续BB中的所有序列指令
      for (size_t i = 1; i < blockRanges.size(); ++i) {
        BinaryBasicBlock *BBToClean = blockRanges[i].first;
        size_t cleanStartIdx = blockRanges[i].second;
        size_t toRemove = std::min(BBToClean->size() - cleanStartIdx, SeqLen);
        
        for (size_t k = 0; k < toRemove && cleanStartIdx < BBToClean->size(); ++k) {
          BBToClean->eraseInstruction(BBToClean->begin() + cleanStartIdx);
        }
      }
      
      if (opts::PostLinkOutliningDebug) {
        BC.outs() << "BOLT-PLO-DEBUG: Replaced cross-block sequence with call (spans " 
                  << blockRanges.size() << " blocks)\n";
      }
    }
  } else {
    // 单基本块序列替换
    if (IsCallerLeaf) {
      MCInst PushInst, PopInst;
      MCPhysReg FP = MIB->getFramePointer();
      MCPhysReg LR = getLinkRegister(BC);
      
      MIB->createPushRegisters(PushInst, FP, LR);
      MIB->createPopRegisters(PopInst, FP, LR);
      
      *(BB->begin() + StartIdx) = PushInst;
      BB->insertInstruction(BB->begin() + StartIdx + 1, std::move(CallInst));
      BB->insertInstruction(BB->begin() + StartIdx + 2, std::move(PopInst));
      
      if (SeqLen > 1) {
        size_t GarbageStartIdx = StartIdx + 3;
        for (size_t k = 1; k < SeqLen; ++k) {
          if (GarbageStartIdx < BB->size()) {
            BB->eraseInstruction(BB->begin() + GarbageStartIdx);
          }
        }
      }
    } else {
      *(BB->begin() + StartIdx) = CallInst;
      if (SeqLen > 1) {
        size_t DeleteStartIdx = StartIdx + 1;
        for (size_t k = 1; k < SeqLen; ++k) {
          if (DeleteStartIdx < BB->size()) {
            BB->eraseInstruction(BB->begin() + DeleteStartIdx);
          }
        }
      }
    }
  }
  
  BF.recomputeLandingPads();
  
  if (opts::PostLinkOutliningDebug) {
    BC.outs() << "BOLT-PLO-DEBUG: Replaced sequence at BB[" << StartIdx 
              << "] (size=" << SeqLen << ") with call to " 
              << OutlinedFunc->getPrintName() << "\n";
    BC.outs() << "BOLT-PLO-DEBUG: BB size after replacement: " << BB->size() << "\n";
  }
}

// 处理标签和跳转指令
void PostLinkOutlining::labelInstHandling(BinaryFunction &BF) {
  // 标签处理逻辑（已在 createFunction 中实现）
}

// 移除标签
void PostLinkOutlining::removeLabels(BinaryFunction &BF) {
  return;
}

// 函数收缩：移除不必要的序言/结语
void PostLinkOutlining::funcShrinking(BinaryFunction &outlinedFunc) {
  if (outlinedFunc.empty()) {
    return;
  }
  
  BinaryContext &BC = outlinedFunc.getBinaryContext();
  MCPlusBuilder *MIB = BC.MIB.get();
  
  // Only support AArch64 for now
  if (!BC.isAArch64()) {
    return;
  }
  
  bool HasCalls = false;
  bool HasOnlyTailCalls = true;
  
  for (BinaryBasicBlock &BB : outlinedFunc) {
    for (MCInst &Inst : BB) {
      if (MIB->isCall(Inst)) {
        HasCalls = true;
        if (!MIB->isTailCall(Inst)) {
          HasOnlyTailCalls = false;
          break;
        }
      }
    }
    if (HasCalls && !HasOnlyTailCalls)
      break;
  }
  
  if (!HasCalls || HasOnlyTailCalls) {
    const unsigned AArch64_STPXpre = 1696;
    const unsigned AArch64_LDPXpost = 1697;
    
    for (BinaryBasicBlock &BB : outlinedFunc) {
      auto It = BB.begin();
      if (It != BB.end() && It->getOpcode() == AArch64_STPXpre) {
        BB.eraseInstruction(It);
      }
      
      auto RetIt = BB.end();
      for (auto I = BB.begin(); I != BB.end(); ++I) {
        if (MIB->isReturn(*I)) {
          RetIt = I;
          break;
        }
      }
      
      if (RetIt != BB.end() && RetIt != BB.begin()) {
        auto PrevIt = RetIt;
        --PrevIt;
        if (PrevIt->getOpcode() == AArch64_LDPXpost) {
          BB.eraseInstruction(PrevIt);
        }
      }
    }
  }
}

// 检查函数是否为纯调用序列
bool PostLinkOutlining::isPureCallSequence(BinaryContext &BC, BinaryFunction &BF) {
  if (BF.empty()) {
    return false;
  }
  
  MCPlusBuilder *MIB = BC.MIB.get();
  
  bool hasNonCallNonStackInst = false;
  bool hasCall = false;
  
  for (BinaryBasicBlock &BB : BF) {
    for (MCInst &Inst : BB) {
      if (MIB->isPush(Inst) || MIB->isPop(Inst) || MIB->isReturn(Inst)) {
        continue;
      }
      
      if (MIB->isCall(Inst)) {
        hasCall = true;
        continue;
      }
      
      // 允许栈调整指令
      StringRef InstName = BC.InstPrinter->getOpcodeName(Inst.getOpcode());
      std::string InstNameLower = InstName.lower();
      if ((InstNameLower.find("add") == 0 || InstNameLower.find("sub") == 0) &&
          InstNameLower.find("sp") != std::string::npos) {
        const MCInstrDesc &Desc = BC.MII->get(Inst.getOpcode());
        bool modifiesSP = false;
        for (int i = 0; i < (int)Inst.getNumOperands() && i < (int)Desc.getNumDefs(); ++i) {
          const MCOperand &Op = Inst.getOperand(i);
          if (Op.isReg() && Op.getReg() == BC.MIB->getStackPointer()) {
            modifiesSP = true;
            break;
          }
        }
        if (modifiesSP) {
          continue;
        }
      }
      
      hasNonCallNonStackInst = true;
      break;
    }
    
    if (hasNonCallNonStackInst) {
      break;
    }
  }
  
  return hasCall && !hasNonCallNonStackInst;
}

// Remove redundant intermediate functions (pure call sequences)
void PostLinkOutlining::removeRedundantIntermediateFunctions(BinaryContext &BC,
                                                              std::vector<BinaryFunction *> &outlinedFunctions) {
  if (opts::PostLinkOutliningDebug) {
    BC.outs() << "BOLT-PLO-DEBUG: Starting removal of redundant intermediate functions\n";
  }
  
  MCPlusBuilder *MIB = BC.MIB.get();
  std::vector<BinaryFunction *> functionsToRemove;
  
  // Step 1: Find all pure call sequence functions
  for (BinaryFunction *outlinedFunc : outlinedFunctions) {
    if (!outlinedFunc || outlinedFunc->empty()) {
      continue;
    }
    
    // Only process outlined functions
    std::string funcName = outlinedFunc->getPrintName();
    if (funcName.find("PLO_outlined_") != 0 && !outlinedFunc->isInjected()) {
      continue;
    }
    
    if (!isPureCallSequence(BC, *outlinedFunc)) {
      continue;
    }
    
    if (opts::PostLinkOutliningDebug) {
      BC.outs() << "BOLT-PLO-DEBUG: Found pure call sequence: " << funcName << "\n";
    }
    
    std::vector<MCSymbol *> calledFunctions;
    for (BinaryBasicBlock &BB : *outlinedFunc) {
      for (MCInst &Inst : BB) {
        if (MIB->isCall(Inst) && !MIB->isIndirectCall(Inst)) {
          // Extract the call target
          for (const MCOperand &Op : MCPlus::primeOperands(Inst)) {
            if (Op.isExpr()) {
              const MCSymbolRefExpr *SRE = dyn_cast<MCSymbolRefExpr>(Op.getExpr());
              if (SRE) {
                MCSymbol *TargetSymbol = const_cast<MCSymbol*>(&SRE->getSymbol());
                calledFunctions.push_back(TargetSymbol);
                break;
              }
            }
          }
        }
      }
    }
    
    if (calledFunctions.empty()) {
      continue; // No direct calls found
    }
    
    MCSymbol *OutlinedFuncSymbol = outlinedFunc->getSymbol();
    if (!OutlinedFuncSymbol) {
      continue;
    }
    
    std::vector<std::pair<BinaryFunction *, std::pair<BinaryBasicBlock *, size_t>>> callSites;
    
    // Search all functions for calls to this outlined function
    for (auto &It : BC.getBinaryFunctions()) {
      BinaryFunction &BF = It.second;
      if (BF.empty()) {
        continue;
      }
      
      for (BinaryBasicBlock &BB : BF) {
        for (size_t i = 0; i < BB.size(); ++i) {
          MCInst &Inst = BB.getInstructionAtIndex(i);
          if (MIB->isCall(Inst) && !MIB->isIndirectCall(Inst)) {
            // Check if this call targets our outlined function
            for (const MCOperand &Op : MCPlus::primeOperands(Inst)) {
              if (Op.isExpr()) {
                const MCSymbolRefExpr *SRE = dyn_cast<MCSymbolRefExpr>(Op.getExpr());
                if (SRE && &SRE->getSymbol() == OutlinedFuncSymbol) {
                  callSites.push_back(std::make_pair(&BF, std::make_pair(&BB, i)));
                  break;
                }
              }
            }
          }
        }
      }
    }
    
    if (callSites.empty()) {
      if (opts::PostLinkOutliningDebug) {
        BC.outs() << "BOLT-PLO-DEBUG: No call sites found for " << funcName << ", skipping\n";
      }
      continue;
    }
    
    if (opts::PostLinkOutliningDebug) {
      BC.outs() << "BOLT-PLO-DEBUG: Found " << callSites.size() 
                << " call sites for " << funcName << "\n";
    }
    
    if (calledFunctions.size() == 1) {
      MCSymbol *TargetFunc = calledFunctions[0];
      
      for (auto &CallSite : callSites) {
        BinaryBasicBlock *CallerBB = CallSite.second.first;
        size_t CallIdx = CallSite.second.second;
        
        MCInst NewCallInst;
        MCContext &Ctx = *BC.Ctx;
        MIB->createCall(NewCallInst, TargetFunc, &Ctx);
        auto It = CallerBB->begin() + CallIdx;
        *It = NewCallInst;
        
        if (opts::PostLinkOutliningDebug) {
          BC.outs() << "BOLT-PLO-DEBUG: Replaced call to " << funcName 
                    << " with direct call to " << TargetFunc->getName() << "\n";
        }
      }
      
      functionsToRemove.push_back(outlinedFunc);
      
      if (opts::PostLinkOutliningDebug) {
        BC.outs() << "BOLT-PLO-DEBUG: Marked " << funcName << " for removal\n";
      }
    }
  }
  
  for (BinaryFunction *FuncToRemove : functionsToRemove) {
    FuncToRemove->setIgnored();
    outlinedFunctions.erase(
      std::remove(outlinedFunctions.begin(), outlinedFunctions.end(), FuncToRemove),
      outlinedFunctions.end()
    );
    
    if (opts::PostLinkOutliningDebug) {
      BC.outs() << "BOLT-PLO-DEBUG: Removed redundant function " 
                << FuncToRemove->getPrintName() << "\n";
    }
  }
  
  if (opts::PostLinkOutliningDebug) {
    BC.outs() << "BOLT-PLO-DEBUG: Removed " << functionsToRemove.size() 
              << " redundant intermediate functions\n";
  }
}

Error PostLinkOutlining::runOnFunctions(BinaryContext &BC) {
  
    if (opts::PostLinkOutliningDebug) {
      BC.outs() << "BOLT-PLO-DEBUG: Starting Post-Link Outlining Pass\n";
      BC.outs() << "  LargestLength: " << LargestLength << "\n";
      BC.outs() << "  EnablePGO: " << (EnablePGO ? "true" : "false") << "\n";
      if (EnablePGO) {
        BC.outs() << "  HotThreshold: execution count > 1 (paper threshold)\n";
      }
      BC.outs() << "  Total functions: " << BC.getBinaryFunctions().size() << "\n";
    }
  
  // PGO: Precompute hot functions list (InHotFuncs) for efficiency
  // This matches the paper's approach of maintaining InHotFuncs data structure
  // According to paper: execution count > 1 means hot function
  // Only functions with execution count <= 1 are considered cold and eligible for outlining
  InHotFuncs.clear();
  if (EnablePGO) {
    // Paper threshold: execution count > 1 means hot function
    const uint64_t HOT_FUNC_THRESHOLD = 1;
    size_t hotFuncCount = 0;
    size_t noProfileCount = 0;
    
    for (auto &It : BC.getBinaryFunctions()) {
      BinaryFunction &BF = It.second;
      if (BF.empty() || !shouldOptimize(BF))
        continue;
      
      // Check if function has profile data
      bool hasProfile = BF.hasProfile();
      uint64_t execCount = 0;
      
      if (hasProfile) {
        // Use actual execution count from PGO data
        execCount = BF.getKnownExecutionCount();
        
        // Hot function: execution count > 1 (conservative threshold)
        // According to user requirement: skip functions with execCount > 1
        if (execCount > HOT_FUNC_THRESHOLD) {
          InHotFuncs.insert(&BF);
          hotFuncCount++;
        }
      } else {
        // No PGO data: allow outlining (don't skip)
        // User requirement: only skip functions with execCount > 1
        // Without profile data, we can't determine if it's hot, so allow outlining
        if (opts::PostLinkOutliningDebug) {
          BC.outs() << "BOLT-PLO-DEBUG: Function " << BF.getPrintName() 
                    << " has no profile data, allowing outlining\n";
        }
        noProfileCount++;
        // Don't add to InHotFuncs - allow outlining
      }
    }
    
    if (opts::PostLinkOutliningDebug) {
      BC.outs() << "BOLT-PLO-DEBUG: Precomputed hot functions: " << hotFuncCount 
                << " out of " << BC.getBinaryFunctions().size() << " functions\n";
      if (noProfileCount > 0) {
        BC.outs() << "BOLT-PLO-DEBUG: Functions without profile data (skipped): " 
                  << noProfileCount << "\n";
      }
      BC.outs() << "BOLT-PLO-DEBUG: Hot function threshold: execution count > " 
                << HOT_FUNC_THRESHOLD << "\n";
    }
  }
  
  std::vector<BinaryFunction *> outlinedFunctions;

  int minLength = opts::PostLinkOutliningMinLength;
  for (int len = LargestLength; len >= minLength; len--) {
    size_t processedFunctions = 0;
    for (auto &It : BC.getBinaryFunctions()) {
      BinaryFunction &BF = It.second;
    
      std::string funcName = BF.getPrintName();
      bool isOutlinedFunc = (funcName.find("PLO_outlined_") == 0 || BF.isInjected());
      
      if (BF.empty() || !shouldOptimize(BF)) {
        continue;
      }
      
      processedFunctions++;

      // Clear labeled sequences for this function
      LabeledSequences.clear();
      // Get all instruction sequences of length len
      std::vector<InstructionSequence> seqs = getAllseqs(BC, BF, len);

      // If PGO is enabled, apply additional filtering (if needed)
      if (EnablePGO) {
        filterHotFuncs(seqs, BF);
      }

      // Get the size after filtering
      size_t n = seqs.size();
      
      // Debug output: show summary for all functions (not just first 5)
      if (opts::PostLinkOutliningDebug) {
        if (n > 0) {
          BC.outs() << "BOLT-PLO-DEBUG: Found " << n << " sequences of length " 
                    << len << " in function " << BF.getPrintName() << "\n";
        } else if (processedFunctions <= 10) {
          // Show when no sequences found for first 10 functions to help debug
          BC.outs() << "BOLT-PLO-DEBUG: No sequences of length " << len 
                    << " found in function " << BF.getPrintName() 
                    << " (size: " << BF.size() << " BBs)\n";
        }
        
        // Show hash values for first 3 sequences only
        size_t hashDisplayCount = std::min(n, static_cast<size_t>(3));
        if (hashDisplayCount > 0) {
          BC.outs() << "BOLT-PLO-DEBUG: Hash values (first " << hashDisplayCount << "): ";
          for (size_t idx = 0; idx < hashDisplayCount; ++idx) {
            uint64_t hash = getHash(seqs[idx]);
            BC.outs() << llvm::format_hex(hash, 16);
            if (idx < hashDisplayCount - 1) BC.outs() << ", ";
          }
          BC.outs() << "\n";
        }
        }
        
      // For each sequence i from 0 to n
      for (size_t i = 0; i < n; i++) {
        // Set label on seqs[i]
        setLabel(&seqs[i]);
        
        // Store all matching sequences (including the first one)
        std::vector<const InstructionSequence*> CurrentMatches;
        CurrentMatches.push_back(&seqs[i]);

        // Frequency counter for this sequence
        int Frequency = 1;

        // Compare with all subsequent sequences
        for (size_t j = i + 1; j < n; j++) {
          // Check for overlapping instructions with seqs[i]
          if (hasOverlappedInstrs(seqs[i], seqs[j])) {
            continue; // Skip if overlapping
          }

          uint64_t hash_i = getHash(seqs[i]);
          uint64_t hash_j = getHash(seqs[j]);
          bool sequencesMatch = (hash_i == hash_j);
          
          if (!sequencesMatch && seqs[i].size() == seqs[j].size()) {
            bool semanticallyEquivalent = true;
            for (size_t k = 0; k < seqs[i].size(); ++k) {
              if (!areInstructionsSemanticallyEquivalent(BC, seqs[i][k], seqs[j][k])) {
                semanticallyEquivalent = false;
                break;
              }
            }
            
            if (semanticallyEquivalent) {
              sequencesMatch = true;
              
              for (size_t k = 0; k < seqs[i].size(); ++k) {
                const MCInst &Inst1 = seqs[i][k];
                const MCInst &Inst2 = seqs[j][k];
                
                if (Inst1.getOpcode() != Inst2.getOpcode()) {
                  sequencesMatch = false;
                  break;
                }
                
                if (Inst1.getNumOperands() != Inst2.getNumOperands()) {
                  sequencesMatch = false;
                  break;
                }
                
                bool operandsMatch = true;
                for (int opIdx = 0; opIdx < (int)Inst1.getNumOperands(); ++opIdx) {
                  const MCOperand &Op1 = Inst1.getOperand(opIdx);
                  const MCOperand &Op2 = Inst2.getOperand(opIdx);
                  
                  if (Op1.isReg() && Op2.isReg()) {
                    MCPhysReg Reg1 = Op1.getReg();
                    MCPhysReg Reg2 = Op2.getReg();
                    const MCPhysReg AArch64_SP = 31;
                    const MCPhysReg AArch64_FP = 29;
                    const MCPhysReg AArch64_LR = 30;
                    
                    if ((Reg1 == AArch64_SP || Reg1 == AArch64_FP || Reg1 == AArch64_LR) ||
                        (Reg2 == AArch64_SP || Reg2 == AArch64_FP || Reg2 == AArch64_LR)) {
                      if (Reg1 != Reg2) {
                        operandsMatch = false;
                        break;
                      }
                    }
                  } else if (Op1.isImm() && Op2.isImm()) {
                    if (!areImmediatesCompatible(BC, Inst1, Inst2, opIdx, opIdx)) {
                      operandsMatch = false;
                      break;
                    }
                  } else if (Op1.isReg() != Op2.isReg() || 
                            Op1.isImm() != Op2.isImm() ||
                            Op1.isExpr() != Op2.isExpr()) {
                    operandsMatch = false;
                    break;
                  }
                }
                
                if (!operandsMatch) {
                  sequencesMatch = false;
                  break;
                }
              }
            }
          }
          
          if (sequencesMatch && !isLabeled(&seqs[j])) {
            bool Overlaps = false;
            for (const InstructionSequence *Accepted : CurrentMatches) {
              if (hasOverlappedInstrs(*Accepted, seqs[j])) {
                Overlaps = true;
                break;
              }
            }
            if (Overlaps) continue;
            
            setLabel(&seqs[j]); // Label seqs[j]
            Frequency++;         // Increment frequency
            CurrentMatches.push_back(&seqs[j]);
          }
        }

        const int InstSizeBytes = 4;
        const int CallInstSize = 4;
        const int SandwichCallSize = 12;
        const int PrologueSize = 4;
        const int EpilogueSize = 4;
        const int RetSize = 4;
        
        std::vector<SequenceLocation> locations = findSequenceLocations(BC, BF, seqs[i]);
        
        if (locations.empty()) {
          continue;
        }
        
        if (locations.size() < static_cast<size_t>(Frequency / 2)) {
          continue;
        }
        
        int sandwichCallCount = 0;
        int normalCallCount = 0;
        uint64_t totalExecutionFrequency = 0;
        uint64_t maxExecutionFrequency = 0;
        
        for (const SequenceLocation &Loc : locations) {
          BinaryBasicBlock *BB = Loc.BB;
          size_t StartIdx = Loc.StartIndex;
          BinaryFunction *CallerFunc = BB->getFunction();
          bool IsRealLeaf = isLeafFunction(CallerFunc) && !CallerFunc->isInjected();
          bool SafeToCall = isLRSavedAtPoint(BC, BB, StartIdx);
          if (IsRealLeaf || !SafeToCall) {
            sandwichCallCount++;
          } else {
            normalCallCount++;
          }
          
          uint64_t bbExecFreq = 1;
          if (EnablePGO && BB->hasProfile()) {
            bbExecFreq = BB->getKnownExecutionCount();
            if (bbExecFreq == 0) {
              bbExecFreq = 1;
            }
          }
          totalExecutionFrequency += bbExecFreq;
          if (bbExecFreq > maxExecutionFrequency) {
            maxExecutionFrequency = bbExecFreq;
          }
        }
        
        uint64_t weightedFrequency = EnablePGO ? totalExecutionFrequency : locations.size();
        
        // 检测是否为纯函数
        bool isOutlinedFuncPure = true;
        MCPhysReg SP = BC.MIB->getStackPointer();
        MCPhysReg FP = BC.MIB->getFramePointer();
        
        for (const MCInst &Inst : seqs[i]) {
          const MCInstrDesc &Desc = BC.MII->get(Inst.getOpcode());
          
          if (BC.MIA->isBranch(Inst) && !BC.MIB->isUnconditionalBranch(Inst)) {
            isOutlinedFuncPure = false;
            break;
          }
          
          if (Desc.mayLoad() || Desc.mayStore()) {
            for (int opIdx = 0; opIdx < (int)Inst.getNumOperands(); ++opIdx) {
              const MCOperand &Op = Inst.getOperand(opIdx);
              if (Op.isReg() && Op.getReg() == SP) {
                isOutlinedFuncPure = false;
                break;
              }
            }
          }
          
          if (BC.MIB->isCall(Inst)) {
            isOutlinedFuncPure = false;
          }
          
          for (int opIdx = 0; opIdx < (int)Inst.getNumOperands(); ++opIdx) {
            const MCOperand &Op = Inst.getOperand(opIdx);
            if (Op.isReg() && Op.getReg() == FP) {
              if (opIdx >= (int)Desc.getNumDefs()) {
                isOutlinedFuncPure = false;
                break;
              }
            }
          }
          
          if (!isOutlinedFuncPure) break;
        }
        
        int outlinedFuncSize;
        if (isOutlinedFuncPure) {
          outlinedFuncSize = (len * InstSizeBytes) + RetSize;
        } else {
          outlinedFuncSize = PrologueSize + (len * InstSizeBytes) + EpilogueSize + RetSize;
        }
        
        int totalCallCost = (sandwichCallCount * SandwichCallSize) + (normalCallCount * CallInstSize);
        int savedBytes = (len * InstSizeBytes) * static_cast<int>(weightedFrequency);
        int costBytes = outlinedFuncSize + totalCallCost;
        int netBenefit = savedBytes - costBytes;
        
        int MinBenefitThreshold;
        uint64_t freqForThreshold = EnablePGO ? weightedFrequency : static_cast<uint64_t>(locations.size());
        uint64_t avgFreq = freqForThreshold / (locations.size() > 0 ? locations.size() : 1);
        
        if (isOutlinedFuncPure) {
          if (avgFreq >= 3 || locations.size() >= 3) {
            MinBenefitThreshold = -4;
          } else if (avgFreq >= 2 || locations.size() >= 2) {
            MinBenefitThreshold = 0;
          } else {
            MinBenefitThreshold = 4;
          }
        } else {
          if (avgFreq >= 3 || locations.size() >= 3) {
            MinBenefitThreshold = 0;
          } else if (avgFreq >= 2 || locations.size() >= 2) {
            MinBenefitThreshold = 0;
          } else {
            MinBenefitThreshold = 0;
          }
        }
        
        if (opts::PostLinkOutliningDebug && (Frequency > 1 || weightedFrequency > 1)) {
          BC.outs() << "BOLT-PLO-DEBUG: Sequence benefit analysis (len=" << len 
                    << ", freq=" << Frequency << ", weightedFreq=" << weightedFrequency
                    << ", found=" << locations.size() << ", maxFreq=" << maxExecutionFrequency << "): "
                    << "saved=" << savedBytes << "B, cost=" << costBytes 
                    << "B, net=" << netBenefit << "B"
                    << " (sandwich=" << sandwichCallCount << ", normal=" << normalCallCount << ")\n";
        }
        
        // 只有当净收益大于阈值时才外联
        if (netBenefit > MinBenefitThreshold) {
          // Create outlined function
          BinaryFunction *outlinedFunc = createFunction(BC, seqs[i]);
          if (outlinedFunc) {
            labelInstHandling(BF);

            bool NeedsSandwich = false;
            for (const SequenceLocation &Loc : locations) {
              BinaryBasicBlock *BB = Loc.BB;
              size_t StartIdx = Loc.StartIndex;
              BinaryFunction *CallerFunc = BB->getFunction();
        bool IsRealLeaf = isLeafFunction(CallerFunc) && !CallerFunc->isInjected();
        bool SafeToCall = isLRSavedAtPoint(BC, BB, StartIdx);
              if (IsRealLeaf || !SafeToCall) {
                NeedsSandwich = true;
                break;
              }
            }
            
            stackFrameManage(*outlinedFunc, /*IsCalledViaSandwich=*/NeedsSandwich);
            
            // 按逆序替换以保持索引正确
            std::sort(locations.begin(), locations.end(),
              [](const SequenceLocation &a, const SequenceLocation &b) {
                if (a.BB != b.BB) return a.BB < b.BB;
                return a.StartIndex > b.StartIndex;
              });
            
            if (opts::PostLinkOutliningDebug) {
              BC.outs() << "BOLT-PLO-DEBUG: Replacing " << locations.size() 
                        << " occurrences of sequence in function " 
                        << BF.getPrintName() << "\n";
            }
            
            size_t replacementCount = 0;
            for (const SequenceLocation &Loc : locations) {
              if (opts::PostLinkOutliningDebug) {
                BC.outs() << "BOLT-PLO-DEBUG: Replacing location " << (replacementCount + 1) 
                          << "/" << locations.size() 
                          << " at BB[" << Loc.StartIndex << "]\n";
              }
              replaceSequenceWithCall(BC, BF, Loc, outlinedFunc);
              replacementCount++;
            }
            
            if (opts::PostLinkOutliningDebug) {
              BC.outs() << "BOLT-PLO-DEBUG: Successfully replaced " << replacementCount 
                        << " sequences in function " << BF.getPrintName() << "\n";
            }
            
            BF.recomputeLandingPads();
            outlinedFunctions.push_back(outlinedFunc);
          }
        } else {
          removeLabels(BF);
        }
      }
    }
  }

  for (BinaryFunction *outlinedFunc : outlinedFunctions) {
    if (outlinedFunc) {
      funcShrinking(*outlinedFunc);
    }
  }
  
  removeRedundantIntermediateFunctions(BC, outlinedFunctions);

  return Error::success();
}

} // namespace bolt
} // namespace llvm