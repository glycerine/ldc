#if USE_METADATA

//===-- GarbageCollect2Stack.cpp - Promote or remove GC allocations -------===//
//
//                         LDC – the LLVM D compiler
//
// This file is distributed under the BSD-style LDC license. See the LICENSE
// file for details.
//
//===----------------------------------------------------------------------===//
//
// This file attempts to turn allocations on the garbage-collected heap into
// stack allocations.
//
//===----------------------------------------------------------------------===//

#include "gen/runtime.h"
#include "gen/metadata.h"

#define DEBUG_TYPE "dgc2stack"

#include "Passes.h"

#include "llvm/Pass.h"
#if LDC_LLVM_VER >= 303
#include "llvm/IR/Module.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/DataLayout.h"
#else
#include "llvm/Module.h"
#include "llvm/Constants.h"
#include "llvm/Intrinsics.h"
#if LDC_LLVM_VER == 302
#include "llvm/IRBuilder.h"
#include "llvm/DataLayout.h"
#else
#include "llvm/Support/IRBuilder.h"
#include "llvm/Target/TargetData.h"
#endif
#endif
#include "llvm/Support/CallSite.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Analysis/CallGraph.h"
#include "llvm/Analysis/Dominators.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>

using namespace llvm;

STATISTIC(NumGcToStack, "Number of calls promoted to constant-size allocas");
STATISTIC(NumToDynSize, "Number of calls promoted to dynamically-sized allocas");
STATISTIC(NumDeleted, "Number of GC calls deleted because the return value was unused");

static cl::opt<unsigned>
SizeLimit("dgc2stack-size-limit", cl::init(1024), cl::Hidden,
  cl::desc("Require allocs to be smaller than n bytes to be promoted, 0 to ignore."));

namespace {
    struct Analysis {
#if LDC_LLVM_VER >= 302
        DataLayout& TD;
#else
        TargetData& TD;
#endif
        const Module& M;
        CallGraph* CG;
        CallGraphNode* CGNode;

        Type* getTypeFor(Value* typeinfo) const;
    };
}

//===----------------------------------------------------------------------===//
// Helper functions
//===----------------------------------------------------------------------===//

void EmitMemSet(IRBuilder<>& B, Value* Dst, Value* Val, Value* Len,
                const Analysis& A) {
    Dst = B.CreateBitCast(Dst, PointerType::getUnqual(B.getInt8Ty()));

    Module *M = B.GetInsertBlock()->getParent()->getParent();
    Type* intTy = Len->getType();
    Type *VoidPtrTy = PointerType::getUnqual(B.getInt8Ty());
    Type *Tys[2] = {VoidPtrTy, intTy};
    Function *MemSet = Intrinsic::getDeclaration(M, Intrinsic::memset,
        llvm::makeArrayRef(Tys, 2));
    Value *Align = ConstantInt::get(B.getInt32Ty(), 1);

    CallSite CS = B.CreateCall5(MemSet, Dst, Val, Len, Align, B.getFalse());
    if (A.CGNode)
        A.CGNode->addCalledFunction(CS, A.CG->getOrInsertFunction(MemSet));
}

static void EmitMemZero(IRBuilder<>& B, Value* Dst, Value* Len,
                        const Analysis& A) {
    EmitMemSet(B, Dst, ConstantInt::get(B.getInt8Ty(), 0), Len, A);
}


//===----------------------------------------------------------------------===//
// Helpers for specific types of GC calls.
//===----------------------------------------------------------------------===//

namespace {
    class FunctionInfo {
    protected:
        Type* Ty;

    public:
        unsigned TypeInfoArgNr;
        bool SafeToDelete;

        /// Whether the allocated memory is returned as a D array instead of
        /// just a plain pointer.
        bool ReturnsArray;

        // Analyze the current call, filling in some fields. Returns true if
        // this is an allocation we can stack-allocate.
        virtual bool analyze(CallSite CS, const Analysis& A) {
            Value* TypeInfo = CS.getArgument(TypeInfoArgNr);
            Ty = A.getTypeFor(TypeInfo);
            if (!Ty) return false;

            return A.TD.getTypeAllocSize(Ty) < SizeLimit;
        }

        // Returns the alloca to replace this call.
        // It will always be inserted before the call.
        virtual Value* promote(CallSite CS, IRBuilder<>& B, const Analysis& A) {
            NumGcToStack++;

            Instruction* Begin = CS.getCaller()->getEntryBlock().begin();
            return new AllocaInst(Ty, ".nongc_mem", Begin); // FIXME: align?
        }

        FunctionInfo(unsigned typeInfoArgNr, bool safeToDelete, bool returnsArray)
        : TypeInfoArgNr(typeInfoArgNr),
          SafeToDelete(safeToDelete),
          ReturnsArray(returnsArray) {}
    };

    class ArrayFI : public FunctionInfo {
        Value* arrSize;
        int ArrSizeArgNr;
        bool Initialized;

    public:
        ArrayFI(unsigned tiArgNr, bool safeToDelete, bool returnsArray,
                bool initialized, unsigned arrSizeArgNr)
        : FunctionInfo(tiArgNr, safeToDelete, returnsArray),
          ArrSizeArgNr(arrSizeArgNr),
          Initialized(initialized)
        {}

        virtual bool analyze(CallSite CS, const Analysis& A) {
            if (!FunctionInfo::analyze(CS, A))
                return false;

            arrSize = CS.getArgument(ArrSizeArgNr);

            // Extract the element type from the array type.
            const StructType* ArrTy = dyn_cast<StructType>(Ty);
            assert(ArrTy && "Dynamic array type not a struct?");
            assert(isa<IntegerType>(ArrTy->getElementType(0)));
            const PointerType* PtrTy =
                cast<PointerType>(ArrTy->getElementType(1));
            Ty = PtrTy->getElementType();

            // If the user explicitly disabled the limits, don't even check
            // whether the element count fits in 32 bits. This could cause
            // miscompilations for humongous arrays, but as the value "range"
            // (set bits) inference algorithm is rather limited, this is
            // useful for experimenting.
            if (SizeLimit > 0)
            {
                uint64_t ElemSize = A.TD.getTypeAllocSize(Ty);
                unsigned BitsLimit = Log2_64(SizeLimit / ElemSize);

                // LLVM's alloca ueses an i32 for the number of elements.
                BitsLimit = std::min(BitsLimit, 32U);

                const IntegerType* SizeType =
                    dyn_cast<IntegerType>(arrSize->getType());
                if (!SizeType)
                    return false;
                unsigned Bits = SizeType->getBitWidth();

                if (Bits > BitsLimit) {
                    APInt Mask = APInt::getLowBitsSet(Bits, BitsLimit);
                    Mask.flipAllBits();
                    APInt KnownZero(Bits, 0), KnownOne(Bits, 0);
    #if LDC_LLVM_VER >= 301
                    ComputeMaskedBits(arrSize, KnownZero, KnownOne, &A.TD);
    #else
                    ComputeMaskedBits(arrSize, Mask, KnownZero, KnownOne, &A.TD);
    #endif
                    if ((KnownZero & Mask) != Mask)
                        return false;
                }
            }

            return true;
        }

        virtual Value* promote(CallSite CS, IRBuilder<>& B, const Analysis& A) {
            IRBuilder<> Builder = B;
            // If the allocation is of constant size it's best to put it in the
            // entry block, so do so if we're not already there.
            // For dynamically-sized allocations it's best to avoid the overhead
            // of allocating them if possible, so leave those where they are.
            // While we're at it, update statistics too.
            if (isa<Constant>(arrSize)) {
                BasicBlock& Entry = CS.getCaller()->getEntryBlock();
                if (Builder.GetInsertBlock() != &Entry)
                    Builder.SetInsertPoint(&Entry, Entry.begin());
                NumGcToStack++;
            } else {
                NumToDynSize++;
            }

            // Convert array size to 32 bits if necessary
            Value* count = Builder.CreateIntCast(arrSize, Builder.getInt32Ty(), false);
            AllocaInst* alloca = Builder.CreateAlloca(Ty, count, ".nongc_mem"); // FIXME: align?

            if (Initialized) {
                // For now, only zero-init is supported.
                uint64_t size = A.TD.getTypeStoreSize(Ty);
                Value* TypeSize = ConstantInt::get(arrSize->getType(), size);
                // Use the original B to put initialization at the
                // allocation site.
                Value* Size = B.CreateMul(TypeSize, arrSize);
                EmitMemZero(B, alloca, Size, A);
            }

            if (ReturnsArray) {
                Value* arrStruct = llvm::UndefValue::get(CS.getType());
                arrStruct = Builder.CreateInsertValue(arrStruct, arrSize, 0);
                Value* memPtr = Builder.CreateBitCast(alloca,
                    PointerType::getUnqual(B.getInt8Ty()));
                arrStruct = Builder.CreateInsertValue(arrStruct, memPtr, 1);
                return arrStruct;
            }

            return alloca;
        }
    };

    // FunctionInfo for _d_allocclass
    class AllocClassFI : public FunctionInfo {
        public:
        virtual bool analyze(CallSite CS, const Analysis& A) {
            // This call contains no TypeInfo parameter, so don't call the
            // base class implementation here...
            if (CS.arg_size() != 1)
                return false;
            Value* arg = CS.getArgument(0)->stripPointerCasts();
            GlobalVariable* ClassInfo = dyn_cast<GlobalVariable>(arg);
            if (!ClassInfo)
                return false;

            std::string metaname = CD_PREFIX;
            metaname += ClassInfo->getName();

            NamedMDNode* meta = A.M.getNamedMetadata(metaname);
            if (!meta)
                return false;

            MDNode* node = static_cast<MDNode*>(meta->getOperand(0));
            if (!node || node->getNumOperands() != CD_NumFields)
                return false;

            // Inserting destructor calls is not implemented yet, so classes
            // with destructors are ignored for now.
            Constant* hasDestructor = dyn_cast<Constant>(node->getOperand(CD_Finalize));
            // We can't stack-allocate if the class has a custom deallocator
            // (Custom allocators don't get turned into this runtime call, so
            // those can be ignored)
            Constant* hasCustomDelete = dyn_cast<Constant>(node->getOperand(CD_CustomDelete));
            if (hasDestructor == NULL || hasCustomDelete == NULL)
                return false;

            if (ConstantExpr::getOr(hasDestructor, hasCustomDelete)
                    != ConstantInt::getFalse(A.M.getContext()))
                return false;

            Ty =node->getOperand(CD_BodyType)->getType();
            return A.TD.getTypeAllocSize(Ty) < SizeLimit;
        }

        // The default promote() should be fine.

        AllocClassFI() : FunctionInfo(~0u, true, false) {}
    };
}


//===----------------------------------------------------------------------===//
// GarbageCollect2Stack Pass Implementation
//===----------------------------------------------------------------------===//

namespace {
    /// This pass replaces GC calls with alloca's
    ///
    class LLVM_LIBRARY_VISIBILITY GarbageCollect2Stack : public FunctionPass {
        StringMap<FunctionInfo*> KnownFunctions;
        Module* M;

        FunctionInfo AllocMemoryT;
        ArrayFI NewArrayVT;
        ArrayFI NewArrayT;
        AllocClassFI AllocClass;

    public:
        static char ID; // Pass identification
        GarbageCollect2Stack();

        bool doInitialization(Module &M) {
            this->M = &M;
            return false;
        }

        bool runOnFunction(Function &F);

        virtual void getAnalysisUsage(AnalysisUsage &AU) const {
#if LDC_LLVM_VER >= 302
          AU.addRequired<DataLayout>();
#else
          AU.addRequired<TargetData>();
#endif
          AU.addRequired<DominatorTree>();

          AU.addPreserved<CallGraph>();
          AU.addPreserved<DominatorTree>();
        }
    };
    char GarbageCollect2Stack::ID = 0;
} // end anonymous namespace.

static RegisterPass<GarbageCollect2Stack>
X("dgc2stack", "Promote (GC'ed) heap allocations to stack");

// Public interface to the pass.
FunctionPass *createGarbageCollect2Stack() {
  return new GarbageCollect2Stack();
}

GarbageCollect2Stack::GarbageCollect2Stack()
: FunctionPass(ID),
  AllocMemoryT(0, true, false),
  NewArrayVT(0, true, false, false, 1),
  NewArrayT(0, true, true, true, 1)
{
    KnownFunctions["_d_allocmemoryT"] = &AllocMemoryT;
    KnownFunctions["_d_newarrayvT"] = &NewArrayVT;
    KnownFunctions["_d_newarrayT"] = &NewArrayT;
    KnownFunctions[_d_allocclass] = &AllocClass;
}

static void RemoveCall(CallSite CS, const Analysis& A) {
    if (CS.isInvoke()) {
        InvokeInst* Invoke = cast<InvokeInst>(CS.getInstruction());
        // If this was an invoke instruction, we need to do some extra
        // work to preserve the control flow.

        // Create a "conditional" branch that -simplifycfg can clean up, so we
        // can keep using the DominatorTree without updating it.
        BranchInst::Create(Invoke->getNormalDest(), Invoke->getUnwindDest(),
            ConstantInt::getTrue(A.M.getContext()), Invoke->getParent());
    }
    // Remove the runtime call.
    if (A.CGNode)
        A.CGNode->removeCallEdgeFor(CS);
    CS.getInstruction()->eraseFromParent();
}

static bool isSafeToStackAllocateArray(Instruction* Alloc, DominatorTree& DT,
    SmallVector<CallInst*, 4>& RemoveTailCallInsts
);
static bool isSafeToStackAllocate(Instruction* Alloc, Value* V, DominatorTree& DT,
    SmallVector<CallInst*, 4>& RemoveTailCallInsts
);

/// runOnFunction - Top level algorithm.
///
bool GarbageCollect2Stack::runOnFunction(Function &F) {
    DEBUG(errs() << "\nRunning -dgc2stack on function " << F.getName() << '\n');

#if LDC_LLVM_VER >= 302
    DataLayout& TD = getAnalysis<DataLayout>();
#else
    TargetData& TD = getAnalysis<TargetData>();
#endif
    DominatorTree& DT = getAnalysis<DominatorTree>();
    CallGraph* CG = getAnalysisIfAvailable<CallGraph>();
    CallGraphNode* CGNode = CG ? (*CG)[&F] : NULL;

    Analysis A = { TD, *M, CG, CGNode };

    BasicBlock& Entry = F.getEntryBlock();

    IRBuilder<> AllocaBuilder(&Entry, Entry.begin());

    bool Changed = false;
    for (Function::iterator BB = F.begin(), E = F.end(); BB != E; ++BB) {
        for (BasicBlock::iterator I = BB->begin(), E = BB->end(); I != E; ) {
            // Ignore non-calls.
            Instruction* Inst = I++;
            CallSite CS(Inst);
            if (!CS.getInstruction())
                continue;

            // Ignore indirect calls and calls to non-external functions.
            Function *Callee = CS.getCalledFunction();
            if (Callee == 0 || !Callee->isDeclaration() ||
                    !(Callee->hasExternalLinkage() || Callee->hasDLLImportLinkage()))
                continue;

            // Ignore unknown calls.
            StringMap<FunctionInfo*>::iterator OMI =
                KnownFunctions.find(Callee->getName());
            if (OMI == KnownFunctions.end()) continue;

            FunctionInfo* info = OMI->getValue();

            if (Inst->use_empty() && info->SafeToDelete) {
                Changed = true;
                NumDeleted++;
                RemoveCall(CS, A);
                continue;
            }

            DEBUG(errs() << "GarbageCollect2Stack inspecting: " << *Inst);

            if (!info->analyze(CS, A))
                continue;

            SmallVector<CallInst*, 4> RemoveTailCallInsts;
            if (info->ReturnsArray) {
                if (!isSafeToStackAllocateArray(Inst, DT, RemoveTailCallInsts)) continue;
            } else {
                if (!isSafeToStackAllocate(Inst, Inst, DT, RemoveTailCallInsts)) continue;
            }

            // Let's alloca this!
            Changed = true;

            // First demote tail calls which use the value so there IR is never
            // in an invalid state.
            SmallVector<CallInst*, 4>::iterator it, end = RemoveTailCallInsts.end();
            for (it = RemoveTailCallInsts.begin(); it != end; ++it) {
                (*it)->setTailCall(false);
            }

            IRBuilder<> Builder(BB, Inst);
            Value* newVal = info->promote(CS, Builder, A);

            DEBUG(errs() << "Promoted to: " << *newVal);

            // Make sure the type is the same as it was before, and replace all
            // uses of the runtime call with the alloca.
            if (newVal->getType() != Inst->getType())
                newVal = Builder.CreateBitCast(newVal, Inst->getType());
            Inst->replaceAllUsesWith(newVal);

            RemoveCall(CS, A);
        }
    }

    return Changed;
}

Type* Analysis::getTypeFor(Value* typeinfo) const {
    GlobalVariable* ti_global = dyn_cast<GlobalVariable>(typeinfo->stripPointerCasts());
    if (!ti_global)
        return NULL;

    std::string metaname = TD_PREFIX;
    metaname += ti_global->getName();

    NamedMDNode* meta = M.getNamedMetadata(metaname);
    if (!meta)
        return NULL;

    MDNode* node = static_cast<MDNode*>(meta->getOperand(0));
    if (!node)
        return NULL;

    if (node->getNumOperands() != TD_NumFields)
        return NULL;
    if (TD_Confirm >= 0 && (!node->getOperand(TD_Confirm) ||
           node->getOperand(TD_Confirm)->stripPointerCasts() != ti_global))
        return NULL;

    return node->getOperand(TD_Type)->getType();
}

/// Returns whether Def is used by any instruction that is reachable from Alloc
/// (without executing Def again).
static bool mayBeUsedAfterRealloc(Instruction* Def, Instruction* Alloc, DominatorTree& DT) {
    DEBUG(errs() << "### mayBeUsedAfterRealloc()\n" << *Def << *Alloc);

    // If the definition isn't used it obviously won't be used after the
    // allocation.
    // If it does not dominate the allocation, there's no way for it to be used
    // without going through Def again first, since the definition couldn't
    // dominate the user either.
    if (Def->use_empty() || !DT.dominates(Def, Alloc)) {
        DEBUG(errs() << "### No uses or does not dominate allocation\n");
        return false;
    }

    DEBUG(errs() << "### Def dominates Alloc\n");

    BasicBlock* DefBlock = Def->getParent();
    BasicBlock* AllocBlock = Alloc->getParent();

    // Create a set of users and one of blocks containing users.
    SmallSet<User*, 16> Users;
    SmallSet<BasicBlock*, 16> UserBlocks;
    for (Instruction::use_iterator UI = Def->use_begin(), UE = Def->use_end();
         UI != UE; ++UI) {
        Instruction* User = cast<Instruction>(*UI);
        DEBUG(errs() << "USER: " << *User);
        BasicBlock* UserBlock = User->getParent();

        // This dominance check is not performed if they're in the same block
        // because it will just walk the instruction list to figure it out.
        // We will instead do that ourselves in the first iteration (for all
        // users at once).
        if (AllocBlock != UserBlock && DT.dominates(AllocBlock, UserBlock)) {
            // There's definitely a path from alloc to this user that does not
            // go through Def, namely any path that ends up in that user.
            DEBUG(errs() << "### Alloc dominates user " << *User);
            return true;
        }

        // Phi nodes are checked separately, so no need to enter them here.
        if (!isa<PHINode>(User)) {
            Users.insert(User);
            UserBlocks.insert(UserBlock);
        }
    }

    // Contains first instruction of block to inspect.
    typedef std::pair<BasicBlock*, BasicBlock::iterator> StartPoint;
    SmallVector<StartPoint, 16> Worklist;
    // Keeps track of successors that have been added to the work list.
    SmallSet<BasicBlock*, 16> Visited;

    // Start just after the allocation.
    // Note that we don't insert AllocBlock into the Visited set here so the
    // start of the block will get inspected if it's reachable.
    BasicBlock::iterator Start = Alloc;
    ++Start;
    Worklist.push_back(StartPoint(AllocBlock, Start));

    while (!Worklist.empty()) {
        StartPoint sp = Worklist.pop_back_val();
        BasicBlock* B = sp.first;
        BasicBlock::iterator BBI = sp.second;
        // BBI is either just after the allocation (in the first iteration)
        // or just after the last phi node in B (in subsequent iterations) here.

        // This whole 'if' is just a way to avoid performing the inner 'for'
        // loop when it can be determined not to be necessary, avoiding
        // potentially expensive walks of the instruction list.
        // It should be equivalent to just doing the loop.
        if (UserBlocks.count(B)) {
            if (B != DefBlock && B != AllocBlock) {
                // This block does not contain the definition or the allocation,
                // so any user in this block is definitely reachable without
                // finding either the definition or the allocation.
                DEBUG(errs() << "### Block " << B->getName()
                     << " contains a reachable user\n");
                return true;
            }
            // We need to walk the instructions in the block to see whether we
            // reach a user before we reach the definition or the allocation.
            for (BasicBlock::iterator E = B->end(); BBI != E; ++BBI) {
                if (&*BBI == Alloc || &*BBI == Def)
                    break;
                if (Users.count(BBI)) {
                    DEBUG(errs() << "### Problematic user: " << *BBI);
                    return true;
                }
            }
        } else if (B == DefBlock || (B == AllocBlock && BBI != Start)) {
            // There are no users in the block so the def or the allocation
            // will be encountered before any users though this path.
            // Skip to the next item on the worklist.
            continue;
        } else {
            // No users and no definition or allocation after the start point,
            // so just keep going.
        }

        // All instructions after the starting point in this block have been
        // accounted for. Look for successors to add to the work list.
        TerminatorInst* Term = B->getTerminator();
        unsigned SuccCount = Term->getNumSuccessors();
        for (unsigned i = 0; i < SuccCount; i++) {
            BasicBlock* Succ = Term->getSuccessor(i);
            BBI = Succ->begin();
            // Check phi nodes here because we only care about the operand
            // coming in from this block.
            bool SeenDef = false;
            while (isa<PHINode>(BBI)) {
                if (Def == cast<PHINode>(BBI)->getIncomingValueForBlock(B)) {
                    DEBUG(errs() << "### Problematic phi user: " << *BBI);
                    return true;
                }
                SeenDef |= (Def == &*BBI);
                ++BBI;
            }
            // If none of the phis we just looked at were the definition, we
            // haven't seen this block yet, and it's dominated by the def
            // (meaning paths through it could lead to users), add the block and
            // the first non-phi to the worklist.
            if (!SeenDef && Visited.insert(Succ) && DT.dominates(DefBlock, Succ))
                Worklist.push_back(StartPoint(Succ, BBI));
        }
    }
    // No users found in any block reachable from Alloc
    // without going through the definition again.
    return false;
}

/// Returns true if the GC call passed in is safe to turn into a stack
/// allocation.
///
/// This handles GC calls returning a D array instead of a raw pointer,
/// see isSafeToStackAllocate() for details.
bool isSafeToStackAllocateArray(Instruction* Alloc, DominatorTree& DT,
    SmallVector<CallInst*, 4>& RemoveTailCallInsts
) {
    assert(Alloc->getType()->isStructTy() && "Allocated array is not a struct?");
    Value* V = Alloc;

    for (Value::use_iterator UI = V->use_begin(), UE = V->use_end();
         UI != UE; ++UI) {
        Instruction *User = cast<Instruction>(*UI);

        switch (User->getOpcode()) {
        case Instruction::ExtractValue: {
            ExtractValueInst *EVI = cast<ExtractValueInst>(User);

            assert(EVI->getAggregateOperand() == V);
            assert(EVI->getNumIndices() == 1);

            unsigned idx = EVI->getIndices()[0];
            if (idx == 0) {
                // This extract the length argument, irrelevant for our analysis.
                assert(EVI->getType()->isIntegerTy() && "First array field not length?");
            } else {
                assert(idx == 1 && "Invalid array struct access.");
                if (!isSafeToStackAllocate(Alloc, EVI, DT, RemoveTailCallInsts))
                    return false;
            }
            break;
        }
        default:
            // We are super conservative here, the only thing we want to be able to
            // handle at this point is extracting len/ptr. More extensive analysis
            // could be added later.
            return false;
        }
    }

    // All uses examined - memory not captured.
    return true;
}


/// Returns true if the GC call passed in is safe to turn
/// into a stack allocation. This requires that the return value does not
/// escape from the function and no derived pointers are live at the call site
/// (i.e. if it's in a loop then the function can't use any pointer returned
/// from an earlier call after a new call has been made).
///
/// This is currently conservative where loops are involved: it can handle
/// simple loops, but returns false if any derived pointer is used in a
/// subsequent iteration.
///
/// Based on LLVM's PointerMayBeCaptured(), which only does escape analysis but
/// doesn't care about loops.
///
/// Alloc is the actual call to the runtime function, and V is the pointer to
/// the memory it returns (which might not be equal to Alloc in case of
/// functions returning D arrays).
///
/// If the value is used in a call instruction with the tail attribute set,
/// the attribute has to be removed before promoting the memory to the
/// stack. The affected instructions are added to RemoveTailCallInsts. If
/// the function returns false, these entries are meaningless.
bool isSafeToStackAllocate(Instruction* Alloc, Value* V, DominatorTree& DT,
    SmallVector<CallInst*, 4>& RemoveTailCallInsts
) {
  assert(isa<PointerType>(V->getType()) && "Allocated value is not a pointer?");

  SmallVector<Use*, 16> Worklist;
  SmallSet<Use*, 16> Visited;

  for (Value::use_iterator UI = V->use_begin(), UE = V->use_end();
       UI != UE; ++UI) {
    Use *U = &UI.getUse();
    Visited.insert(U);
    Worklist.push_back(U);
  }

  while (!Worklist.empty()) {
    Use *U = Worklist.pop_back_val();
    Instruction *I = cast<Instruction>(U->getUser());
    V = U->get();

    switch (I->getOpcode()) {
    case Instruction::Call:
    case Instruction::Invoke: {
      CallSite CS(I);
      // Not captured if the callee is readonly, doesn't return a copy through
      // its return value and doesn't unwind (a readonly function can leak bits
      // by throwing an exception or not depending on the input value).
      if (CS.onlyReadsMemory() && CS.doesNotThrow() &&
          I->getType() == Type::getVoidTy(I->getContext()))
        break;

      // Not captured if only passed via 'nocapture' arguments.  Note that
      // calling a function pointer does not in itself cause the pointer to
      // be captured.  This is a subtle point considering that (for example)
      // the callee might return its own address.  It is analogous to saying
      // that loading a value from a pointer does not cause the pointer to be
      // captured, even though the loaded value might be the pointer itself
      // (think of self-referential objects).
      CallSite::arg_iterator B = CS.arg_begin(), E = CS.arg_end();
      for (CallSite::arg_iterator A = B; A != E; ++A)
        if (A->get() == V) {
          if (!CS.paramHasAttr(A - B + 1,
#if LDC_LLVM_VER >= 303
              Attribute::NoCapture
#elif LDC_LLVM_VER == 302
              Attributes::NoCapture
#else
              Attribute::NoCapture
#endif
          )) {
            // The parameter is not marked 'nocapture' - captured.
            return false;
          }

          if (CS.isCall()) {
            CallInst* CI = cast<CallInst>(I);
            if (CI->isTailCall()) {
              RemoveTailCallInsts.push_back(CI);
            }
          }
        }
      // Only passed via 'nocapture' arguments, or is the called function - not
      // captured.
      break;
    }
    case Instruction::Load:
      // Loading from a pointer does not cause it to be captured.
      break;
    case Instruction::Store:
      if (V == I->getOperand(0))
        // Stored the pointer - it may be captured.
        return false;
      // Storing to the pointee does not cause the pointer to be captured.
      break;
    case Instruction::BitCast:
    case Instruction::GetElementPtr:
    case Instruction::PHI:
    case Instruction::Select:
      // It's not safe to stack-allocate if this derived pointer is live across
      // the original allocation.
      if (mayBeUsedAfterRealloc(I, Alloc, DT))
        return false;

      // The original value is not captured via this if the new value isn't.
      for (Instruction::use_iterator UI = I->use_begin(), UE = I->use_end();
           UI != UE; ++UI) {
        Use *U = &UI.getUse();
        if (Visited.insert(U))
          Worklist.push_back(U);
      }
      break;
    default:
      // Something else - be conservative and say it is captured.
      return false;
    }
  }

  // All uses examined - not captured or live across original allocation.
  return true;
}

#endif // USE_METADATA
