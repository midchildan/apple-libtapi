//===- CoroInternal.h - Internal Coroutine interfaces ---------*- C++ -*---===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
// Common definitions/declarations used internally by coroutine lowering passes.
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TRANSFORMS_COROUTINES_COROINTERNAL_H
#define LLVM_LIB_TRANSFORMS_COROUTINES_COROINTERNAL_H

#include "CoroInstr.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Transforms/Coroutines.h"

namespace llvm {

class CallGraph;
class CallGraphSCC;
class PassRegistry;

void initializeCoroEarlyPass(PassRegistry &);
void initializeCoroSplitPass(PassRegistry &);
void initializeCoroElidePass(PassRegistry &);
void initializeCoroCleanupPass(PassRegistry &);

// CoroEarly pass marks every function that has coro.begin with a string
// attribute "coroutine.presplit"="0". CoroSplit pass processes the coroutine
// twice. First, it lets it go through complete IPO optimization pipeline as a
// single function. It forces restart of the pipeline by inserting an indirect
// call to an empty function "coro.devirt.trigger" which is devirtualized by
// CoroElide pass that triggers a restart of the pipeline by CGPassManager.
// When CoroSplit pass sees the same coroutine the second time, it splits it up,
// adds coroutine subfunctions to the SCC to be processed by IPO pipeline.

#define CORO_PRESPLIT_ATTR "coroutine.presplit"
#define UNPREPARED_FOR_SPLIT "0"
#define PREPARED_FOR_SPLIT "1"

#define CORO_DEVIRT_TRIGGER_FN "coro.devirt.trigger"

namespace coro {

bool declaresIntrinsics(Module &M, std::initializer_list<StringRef>);
void replaceAllCoroAllocs(CoroBeginInst *CB, bool Replacement);
void replaceAllCoroFrees(CoroBeginInst *CB, Value *Replacement);
void replaceCoroFree(CoroIdInst *CoroId, bool Elide);
void updateCallGraph(Function &Caller, ArrayRef<Function *> Funcs,
                     CallGraph &CG, CallGraphSCC &SCC);

// Keeps data and helper functions for lowering coroutine intrinsics.
struct LowererBase {
  Module &TheModule;
  LLVMContext &Context;
  PointerType *const Int8Ptr;
  FunctionType *const ResumeFnType;
  ConstantPointerNull *const NullPtr;

  LowererBase(Module &M);
  Value *makeSubFnCall(Value *Arg, int Index, Instruction *InsertPt);
};

enum class ABI {
  /// The "resume-switch" lowering, where there are separate resume and
  /// destroy functions that are shared between all suspend points.  The
  /// coroutine frame implicitly stores the resume and destroy functions,
  /// the current index, and any promise value.
  Switch,

  /// The "returned-continuation" lowering, where each suspend point creates a
  /// single continuation function that is used for both resuming and
  /// destroying.  Does not support promises.
  Retcon,

  /// The "unique returned-continuation" lowering, where each suspend point
  /// creates a single continuation function that is used for both resuming
  /// and destroying.  Does not support promises.  The function is known to
  /// suspend at most once during its execution, and the return value of
  /// the continuation is void.
  RetconOnce,
};

// Holds structural Coroutine Intrinsics for a particular function and other
// values used during CoroSplit pass.
struct LLVM_LIBRARY_VISIBILITY Shape {
  CoroBeginInst *CoroBegin;
  SmallVector<CoroEndInst *, 4> CoroEnds;
  SmallVector<CoroSizeInst *, 2> CoroSizes;
  SmallVector<AnyCoroSuspendInst *, 4> CoroSuspends;
  SmallVector<CallInst*, 2> SwiftErrorOps;

  // Field indexes for special fields in the switch lowering.
  struct SwitchFieldIndex {
    enum {
      Resume,
      Destroy,
      Promise,
      Index
    };
  };

  coro::ABI ABI;

  StructType *FrameTy;
  Instruction *FramePtr;
  BasicBlock *AllocaSpillBlock;

  struct SwitchLoweringStorage {
    SwitchInst *ResumeSwitch;
    AllocaInst *PromiseAlloca;
    BasicBlock *ResumeEntryBlock;
    bool HasFinalSuspend;
  };

  struct RetconLoweringStorage {
    Function *ResumePrototype;
    Function *Alloc;
    Function *Dealloc;
    BasicBlock *ReturnBlock;
    bool IsFrameInlineInStorage;
  };

  union {
    SwitchLoweringStorage SwitchLowering;
    RetconLoweringStorage RetconLowering;
  };

  CoroIdInst *getSwitchCoroId() const {
    assert(ABI == coro::ABI::Switch);
    return cast<CoroIdInst>(CoroBegin->getId());
  }

  AnyCoroIdRetconInst *getRetconCoroId() const {
    assert(ABI == coro::ABI::Retcon ||
           ABI == coro::ABI::RetconOnce);
    return cast<AnyCoroIdRetconInst>(CoroBegin->getId());
  }

  IntegerType *getIndexType() const {
    assert(ABI == coro::ABI::Switch);
    assert(FrameTy && "frame type not assigned");
    return cast<IntegerType>(FrameTy->getElementType(SwitchFieldIndex::Index));
  }
  ConstantInt *getIndex(uint64_t Value) const {
    return ConstantInt::get(getIndexType(), Value);
  }

  FunctionType *getResumeFunctionType() const {
    switch (ABI) {
    case coro::ABI::Switch: {
      assert(FrameTy && "frame type not assigned");
      auto *FnPtrTy = FrameTy->getElementType(SwitchFieldIndex::Resume);
      return cast<FunctionType>(FnPtrTy->getPointerElementType());
    }
    case coro::ABI::Retcon:
    case coro::ABI::RetconOnce:
      return RetconLowering.ResumePrototype->getFunctionType();
    }
  }

  ArrayRef<Type*> getRetconResultTypes() const {
    assert(ABI == coro::ABI::Retcon ||
           ABI == coro::ABI::RetconOnce);
    auto FTy = CoroBegin->getFunction()->getFunctionType();

    // The safety of all this is checked by checkWFRetconPrototype.
    if (auto STy = dyn_cast<StructType>(FTy->getReturnType())) {
      return STy->elements().slice(1);
    } else {
      return ArrayRef<Type*>();
    }
  }

  ArrayRef<Type*> getRetconResumeTypes() const {
    assert(ABI == coro::ABI::Retcon ||
           ABI == coro::ABI::RetconOnce);

    // The safety of all this is checked by checkWFRetconPrototype.
    auto FTy = RetconLowering.ResumePrototype->getFunctionType();
    return FTy->params().slice(1);
  }

  CallingConv::ID getResumeFunctionCC() const {
    switch (ABI) {
    case coro::ABI::Switch:
      return CallingConv::Fast;

    case coro::ABI::Retcon:
    case coro::ABI::RetconOnce:
      return RetconLowering.ResumePrototype->getCallingConv();
    }
  }

  AllocaInst *getPromiseAlloca() const {
    if (ABI == coro::ABI::Switch)
      return SwitchLowering.PromiseAlloca;
    return nullptr;
  }

  /// Allocate memory according to the rules of the active lowering.
  ///
  /// \param CG - if non-null, will be updated for the new call
  Value *emitAlloc(IRBuilder<> &Builder, Value *Size, CallGraph *CG) const;

  /// Deallocate memory according to the rules of the active lowering.
  ///
  /// \param CG - if non-null, will be updated for the new call
  void emitDealloc(IRBuilder<> &Builder, Value *Ptr, CallGraph *CG) const;

  Shape() = default;
  explicit Shape(Function &F) { buildFrom(F); }
  void buildFrom(Function &F);
};

void buildCoroutineFrame(Function &F, Shape &Shape);

} // End namespace coro.
} // End namespace llvm

#endif
