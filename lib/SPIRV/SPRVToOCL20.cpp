//===- SPRVToOCL20.cpp - Transform SPIR-V builtins to OCL20 builtins-------===//
//
//                     The LLVM/SPIRV Translator
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
// Copyright (c) 2014 Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the "Software"),
// to deal with the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:
//
// Redistributions of source code must retain the above copyright notice,
// this list of conditions and the following disclaimers.
// Redistributions in binary form must reproduce the above copyright notice,
// this list of conditions and the following disclaimers in the documentation
// and/or other materials provided with the distribution.
// Neither the names of Advanced Micro Devices, Inc., nor the names of its
// contributors may be used to endorse or promote products derived from this
// Software without specific prior written permission.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// CONTRIBUTORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS WITH
// THE SOFTWARE.
//
//===----------------------------------------------------------------------===//
//
// This file implements transform SPIR-V builtins to OCL 2.0 builtins.
//
//===----------------------------------------------------------------------===//
#define DEBUG_TYPE "spvtocl20"

#include "SPRVInternal.h"
#include "OCLUtil.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/IR/InstVisitor.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Pass.h"
#include "llvm/PassSupport.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;
using namespace SPRV;
using namespace OCLUtil;

namespace SPRV {
class SPRVToOCL20: public ModulePass,
  public InstVisitor<SPRVToOCL20> {
public:
  SPRVToOCL20():ModulePass(ID), M(nullptr), Ctx(nullptr) {
    initializeSPRVToOCL20Pass(*PassRegistry::getPassRegistry());
  }
  virtual bool runOnModule(Module &M);
  virtual void visitCallInst(CallInst &CI);

  /// Transform __spirv_MemoryBarrier to atomic_work_item_fence.
  ///   __spirv_MemoryBarrier(scope, sema) =>
  ///       atomic_work_item_fence(flag(sema), order(sema), map(scope))
  void visitCallSPRVMemoryBarrier(CallInst *CI);

  /// Transform __spirv_Atomic* to atomic_*.
  ///   __spirv_Atomic*(atomic_op, scope, sema, ops, ...) =>
  ///      atomic_*(atomic_op, ops, ..., order(sema), map(scope))
  void visitCallSPRVAtomicBuiltin(CallInst *CI, Op OC);

  /// Transform __spirv_* builtins to OCL 2.0 builtins.
  /// No change with arguments.
  void visitCallSPRVBuiltin(CallInst *CI, Op OC);

  static char ID;
private:
  Module *M;
  LLVMContext *Ctx;
};

char SPRVToOCL20::ID = 0;

bool
SPRVToOCL20::runOnModule(Module& Module) {
  M = &Module;
  Ctx = &M->getContext();
  visit(*M);

  DEBUG(dbgs() << "After SPRVToOCL20:\n" << *M);

  std::string Err;
  raw_string_ostream ErrorOS(Err);
  if (verifyModule(*M, &ErrorOS)){
    DEBUG(errs() << "Fails to verify module: " << ErrorOS.str());
  }
  return true;
}

void
SPRVToOCL20::visitCallInst(CallInst& CI) {
  DEBUG(dbgs() << "[visistCallInst] " << CI << '\n');
  auto F = CI.getCalledFunction();
  if (!F)
    return;

  auto MangledName = F->getName();
  std::string DemangledName;
  Op OC = OpNop;
  if (!oclIsBuiltin(MangledName, 20, &DemangledName) ||
      (OC = getSPRVFuncOC(DemangledName)) == OpNop)
    return;
  DEBUG(dbgs() << "DemangledName = " << DemangledName.c_str() << '\n'
               << "OpCode = " << OC << '\n');

  if (OC == OpMemoryBarrier) {
    visitCallSPRVMemoryBarrier(&CI);
    return;
  }
  if (isAtomicOpCode(OC)) {
    visitCallSPRVAtomicBuiltin(&CI, OC);
    return;
  }
  visitCallSPRVBuiltin(&CI, OC);

}

void SPRVToOCL20::visitCallSPRVMemoryBarrier(CallInst* CI) {
  AttributeSet Attrs = CI->getCalledFunction()->getAttributes();
  mutateCallInst(M, CI, [=](CallInst *, std::vector<Value *> &Args){
    auto getArg = [=](unsigned I){
      return cast<ConstantInt>(Args[I])->getZExtValue();
    };
    auto MScope = static_cast<Scope>(getArg(0));
    auto Sema = mapSPRVMemSemanticToOCL(getArg(1));
    Args.resize(3);
    Args[0] = getInt32(M, Sema.first);
    Args[1] = getInt32(M, Sema.second);
    Args[2] = getInt32(M, rmap<OCLMemScopeKind>(MScope));
    return kOCLBuiltinName::AtomicWorkItemFence;
  }, true, &Attrs);
}

void SPRVToOCL20::visitCallSPRVAtomicBuiltin(CallInst* CI, Op OC) {
  AttributeSet Attrs = CI->getCalledFunction()->getAttributes();
  mutateCallInst(M, CI, [=](CallInst *, std::vector<Value *> &Args){
    auto Ptr = findFirstPtr(Args);
    auto Name = OCLSPRVBuiltinMap::rmap(OC);
    auto NumOrder = getAtomicBuiltinNumMemoryOrderArgs(Name);
    auto OrderIdx = Ptr + 1;
    auto ScopeIdx = Ptr + 1 + NumOrder;
    if (OC == OpAtomicIIncrement ||
        OC == OpAtomicIDecrement) {
      Args.erase(Args.begin() + OrderIdx, Args.begin() + ScopeIdx + 1);
    } else {
      Args[ScopeIdx] = mapUInt(M, cast<ConstantInt>(Args[ScopeIdx]),
          [](unsigned I){
        return rmap<OCLMemScopeKind>(static_cast<Scope>(I));
      });
      for (size_t I = 0; I < NumOrder; ++I)
        Args[OrderIdx + I] = mapUInt(M, cast<ConstantInt>(Args[OrderIdx + I]),
            [](unsigned Ord) {
        return mapSPRVMemOrderToOCL(Ord);
      });
      move(Args, OrderIdx, ScopeIdx + 1, Args.size());
    }
    return Name;
  }, true, &Attrs);
}

void SPRVToOCL20::visitCallSPRVBuiltin(CallInst* CI, Op OC) {
  AttributeSet Attrs = CI->getCalledFunction()->getAttributes();
  mutateCallInst(M, CI, [=](CallInst *, std::vector<Value *> &Args){
    return OCLSPRVBuiltinMap::rmap(OC);
  }, true, &Attrs);
}

}

INITIALIZE_PASS(SPRVToOCL20, "spvtoocl20",
    "Translate SPIR-V builtins to OCL 2.0 builtins", false, false)

ModulePass *llvm::createSPRVToOCL20() {
  return new SPRVToOCL20();
}