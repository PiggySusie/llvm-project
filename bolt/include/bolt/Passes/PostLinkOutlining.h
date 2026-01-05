//===- bolt/Passes/PostLinkOutlining.h - Post-Link Outlining Pass --*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Post-Link Outlining Pass: Identifies and outlines common instruction sequences
// to reduce code size while maintaining correctness.
//
//===----------------------------------------------------------------------===//

#ifndef BOLT_PASSES_POSTLINKOUTLINING_H
#define BOLT_PASSES_POSTLINKOUTLINING_H

#include "bolt/Passes/BinaryPasses.h"
#include "llvm/MC/MCInst.h"
#include <vector>

namespace llvm {
namespace bolt {

/// Post-Link Outlining Pass
/// Implements Algorithm 1: Post-link Outlining
class PostLinkOutlining : public BinaryFunctionPass {
private:
  // Configuration parameters
  int LargestLength;
  bool EnablePGO;

  // Sequence type: a sequence is a vector of MCInst
  using InstructionSequence = std::vector<MCInst>;
  
  // Track labeled sequences
  std::set<const InstructionSequence *> LabeledSequences;
  
  // PGO: Track hot functions (functions with execution count > threshold)
  // This is precomputed once at the start of the pass for efficiency
  std::set<const BinaryFunction *> InHotFuncs;

  // Helper functions (placeholders for now)
  std::vector<InstructionSequence> getAllseqs(BinaryContext &BC, BinaryFunction &BF, int len);
  void filterHotFuncs(std::vector<InstructionSequence> &seqs, BinaryFunction &BF);
  bool hasOverlappedInstrs(const InstructionSequence &seq1, 
                           const InstructionSequence &seq2);
  void setLabel(const InstructionSequence *seq);
  bool isLabeled(const InstructionSequence *seq);
  uint64_t getHash(const InstructionSequence &seq);
  BinaryFunction *createFunction(BinaryContext &BC, 
                                 const InstructionSequence &seq);
  void stackFrameManage(BinaryFunction &outlinedFunc, bool IsCalledViaSandwich = false);
  void labelInstHandling(BinaryFunction &BF);
  void removeLabels(BinaryFunction &BF);
  void funcShrinking(BinaryFunction &outlinedFunc);
  
  // Remove redundant intermediate functions (pure call sequences)
  bool isPureCallSequence(BinaryContext &BC, BinaryFunction &BF);
  void removeRedundantIntermediateFunctions(BinaryContext &BC, 
                                            std::vector<BinaryFunction *> &outlinedFunctions);
  
  // Sequence location tracking for replacement
  // 支持单基本块和跨基本块序列
  struct SequenceLocation {
    BinaryBasicBlock *BB;           // 起始基本块
    size_t StartIndex;               // 在起始基本块中的起始索引
    const InstructionSequence *Seq; // 序列指针
    
    // 跨基本块支持：记录序列跨越的所有基本块
    std::vector<std::pair<BinaryBasicBlock *, size_t>> CrossBlockPath;
    
    SequenceLocation(BinaryBasicBlock *BB, size_t StartIndex, const InstructionSequence *Seq)
      : BB(BB), StartIndex(StartIndex), Seq(Seq) {}
    
    // 检查是否为跨基本块序列
    bool isCrossBlock() const { return !CrossBlockPath.empty(); }
  };
  
  // Find all locations of a sequence in a function
  std::vector<SequenceLocation> findSequenceLocations(BinaryContext &BC,
                                                     BinaryFunction &BF,
                                                     const InstructionSequence &Seq);
  
  // Replace a sequence with a call to the outlined function
  void replaceSequenceWithCall(BinaryContext &BC,
                               BinaryFunction &BF,
                               const SequenceLocation &Loc,
                               BinaryFunction *OutlinedFunc);

public:
  explicit PostLinkOutlining(bool PrintPass = false, 
                             int LargestLength = 32,
                             bool EnablePGO = false)
    : BinaryFunctionPass(PrintPass), 
      LargestLength(LargestLength),
      EnablePGO(EnablePGO) {}

  const char *getName() const override { return "post-link-outlining"; }

  bool shouldPrint(const BinaryFunction &BF) const override {
    return BinaryFunctionPass::shouldPrint(BF);
  }

  /// Pass entry point
  Error runOnFunctions(BinaryContext &BC) override;
};

} // namespace bolt
} // namespace llvm

#endif // BOLT_PASSES_POSTLINKOUTLINING_H

