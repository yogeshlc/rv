//===- maskGenerator.cpp ----------------*- C++ -*-===//
//
//                     The Region Vectorizer
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
// @authors karrenberg, kloessner, simon
//

#include "rv/transforms/maskGenerator.h"

#include <stdexcept>
#include <rv/rvInfoProxyPass.h>

#include <llvm/IR/Module.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Verifier.h> // verifyFunction()

#include "utils/metadata.h"
#include "rvConfig.h"

using namespace llvm;
using namespace rv;

void
MaskGenerator::markMaskOperation(Instruction& maskOp) {
    mvecInfo.setVectorShape(maskOp, VectorShape::varying()); //isa<PHINode>(maskOp) ? VectorShape::uni() : VectorShape::varying());


  // inlined: rv::markMaskOperation(&maskOp); // TODO only for legacy backend
    assert (rv::isMetadataSetUp() && "metadata not initialized, call setUpMetadata() first!");

    // Mark new mask operation as OP_VARYING/RES_VECTOR/MASK
    // or OP_UNIFORM/RES_VECTOR/MASK in case of a phi.
    // rv::setMetadata(&maskOp, isa<PHINode>(maskOp) ?        rv::RV_METADATA_OP_UNIFORM :        rv::RV_METADATA_OP_VARYING);
    // rv::setMetadata(maskOp, rv::RV_METADATA_RES_VECTOR);
    rv::setMetadata(&maskOp, rv::RV_METADATA_MASK);
}

char MaskGeneratorWrapper::ID = 0;
// NOTE: The order of initialized dependencies is important
//       to prevent 'Unable to schedule' errors!
INITIALIZE_PASS_BEGIN(MaskGeneratorWrapper, "maskGenerator", "MaskGenerator", false, false)
INITIALIZE_PASS_DEPENDENCY(RVInfoProxyPass)
INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(MaskAnalysisWrapper)
INITIALIZE_PASS_END(MaskGeneratorWrapper, "maskGenerator", "MaskGenerator", false, false)

// Public interface to the MaskGenerator pass
FunctionPass*
llvm::createMaskGeneratorPass()
{
    return new MaskGeneratorWrapper();
}



MaskGeneratorWrapper::MaskGeneratorWrapper() : FunctionPass(ID)
{
    initializeMaskGeneratorWrapperPass(*PassRegistry::getPassRegistry());
}

MaskGenerator::MaskGenerator(RVInfo& RVinfo,
                             VectorizationInfo& Vecinfo,
                             MaskAnalysis& MaskAnalysis,
                             const LoopInfo& Loopinfo)
        : mInfo(RVinfo),
          mMaskAnalysis(MaskAnalysis),
          mvecInfo(Vecinfo),
          mLoopInfo(Loopinfo)
{
}

MaskGenerator::~MaskGenerator()
{
}

void
MaskGeneratorWrapper::releaseMemory()
{
}

void
MaskGeneratorWrapper::getAnalysisUsage(AnalysisUsage &AU) const
{
    AU.addRequired<RVInfoProxyPass>();
    AU.addPreserved<RVInfoProxyPass>();

    AU.addRequired<VectorizationInfoProxyPass>();
    AU.addPreserved<VectorizationInfoProxyPass>();

    AU.addRequired<LoopInfoWrapperPass>();
    AU.addPreserved<LoopInfoWrapperPass>();

    AU.addRequired<MaskAnalysisWrapper>();
    AU.addPreserved<MaskAnalysisWrapper>();
}

bool
MaskGeneratorWrapper::doInitialization(Module& M)
{
    // The return value presumably signals whether the module was changed or not.
    // There is no documentation on this in LLVM.
    return false;
}

bool
MaskGeneratorWrapper::doFinalization(Module& M)
{
    // The return value presumably signals whether the module was changed or not.
    // There is no documentation on this in LLVM.
    return false;
}

bool
MaskGeneratorWrapper::runOnFunction(Function& F)
{
    RVInfo& RVinfo           = getAnalysis<RVInfoProxyPass>().getInfo();
    VectorizationInfo& Vecinfo = getAnalysis<VectorizationInfoProxyPass>().getInfo();
    MaskAnalysis* MaskAnalysis = getAnalysis<MaskAnalysisWrapper>().getMaskAnalysis();
    const LoopInfo& Loopinfo   = getAnalysis<LoopInfoWrapperPass>().getLoopInfo();

    assert (MaskAnalysis);

    MaskGenerator Generator(RVinfo, Vecinfo, *MaskAnalysis, Loopinfo);

    return Generator.generate(F);
}

void
MaskGeneratorWrapper::print(raw_ostream& O, const Module* M) const
{
}

bool
MaskGenerator::generate(Function& F)
{
    IF_DEBUG {
            outs() << "\n#########################################################\n";
            outs() << "## MASK GENERATION\n";
            outs() << "#########################################################\n";
    }

    // If an error occurred in one of the previous phases, abort.
    try {
        materializeMasks(&F);
    }
    catch (std::logic_error& error)
    {
        errs() << "\nException occurred during MaskGenerator: "
        << error.what() << "\n";
        return false;
    }
    catch (...)
    {
        errs() << "\nINTERNAL ERROR: Unexpected exception occurred during "
        << "MaskGenerator!\n";
        return false;
    }

    IF_DEBUG F.print(outs());

    fillVecInfoWithPredicates(F);

    IF_DEBUG {
      errs() << "VectorizationInfo after maskGeneration:\n";
      mvecInfo.dump();
    }

    return true;
}

// Recursively materialize masks where required:
// - OP_VARYING phis (blend operations)
// - DIVERGENT loops
// - OP_SEQUENTIAL_GUARDED operations and "native" calls with mask parameters
//   - For these, create dummy uses (calls) at the operations.
//     This ensures that dependencies are maintained during CFG linearization.
// - MANDATORY blocks with phi(s)
//   - Masks may be required if incoming edges change and cause live ranges to overlap.
void
MaskGenerator::materializeMasks(Function* f)
{
    assert (f);
    assert (!f->getParent()->getFunction("entryMaskUseFn"));

    Region * region = mvecInfo.getRegion();

    for (auto &BB : *f)
    {
#if 0
        if (region && !region->contains(&BB)) continue; // skip blocks outside of region
#endif

        if (entryMaskIsUsed(BB))
        {
            MaskPtr mask = mMaskAnalysis.getEntryMaskPtr(BB);
            Value* newEntryMask = materializeMask(mask);

            IF_DEBUG {  outs() << "  entry mask of block '"
                << BB.getName() << "': " << *newEntryMask << "\n";
            }

            // If the entry mask is not an 'or' in the same block, generate
            // dummy use for the mask at beginning of block, to be used by
            // calls that require the mask or OP_SEQUENTIAL_GUARDED operations.
            // If the entry mask is a constant or argument, we don't need the
            // dummy, either.
            if (isa<Instruction>(newEntryMask) &&
                cast<Instruction>(newEntryMask)->getParent() != &BB)
            {
                Function* entryMaskUseFn = f->getParent()->getFunction("entryMaskUseFn");
#if 0
                if (!entryMaskUseFn)
                {
                    // Create function that simply returns the argument.
                    FunctionType* fnTy = FunctionType::get(Type::getInt1Ty(*mInfo.mContext),
                                                           Type::getInt1Ty(*mInfo.mContext),
                                                           false);
                    entryMaskUseFn = Function::Create(fnTy,
                                                      Function::InternalLinkage,
                                                      "entryMaskUseFn",
                                                      f->getParent());
                    entryMaskUseFn->setDoesNotAccessMemory();
                    entryMaskUseFn->setDoesNotThrow();

                    BasicBlock* entryBB = BasicBlock::Create(*mInfo.mContext,
                                                             "entry",
                                                             entryMaskUseFn);
                    ReturnInst::Create(*mInfo.mContext,
                                       &*entryMaskUseFn->arg_begin(),
                                       entryBB);

                    // Add information about this function to functionInfoMap.
                    FunctionType* fnTySIMD = FunctionType::get(mInfo.mVectorTyBoolSIMD,
                                                               mInfo.mVectorTyBoolSIMD,
                                                               false);
                    Function* simdUseFn = Function::Create(fnTySIMD,
                                                           Function::InternalLinkage,
                                                           "entryMaskUseFnSIMD",
                                                           f->getParent());
                    simdUseFn->setDoesNotAccessMemory();
                    simdUseFn->setDoesNotThrow();
                    SmallVector<bool, 4> uniformArgs;
                    uniformArgs.push_back(false);
                    mInfo.mFunctionInfoMap.add(*entryMaskUseFn,
                                                *simdUseFn,
                                                -1,
                                                false,
                                                uniformArgs);
                }

                Instruction* insertBefore = BB.getFirstNonPHI();
                CallInst* dummyCall = CallInst::Create(entryMaskUseFn,
                                                       ArrayRef<Value*>(newEntryMask),
                                                       "entryMaskUse",
                                                       insertBefore);
                dummyCall->setDoesNotAccessMemory();
                dummyCall->setDoesNotThrow();
                rv::copyMetadata(dummyCall, *newEntryMask);

                mMaskAnalysis.updateEntryMask(BB,
                                               dummyCall,
                                               insertBefore);
#endif
            }
        }

        // exit masks
        for (BasicBlock* succBB : successors(&BB))
        {
            // assert (!rv::hasMetadata(succBB, rv::RV_METADATA_MANDATORY) == !mvecInfo.isMandatory(succBB));

            // In certain cases we will never need the exit mask.
            if (!hasNonUniformPhi(*succBB) &&
                !isHeaderOfDivergentLoop(*succBB) &&
                !mvecInfo.isMandatory(succBB))
            {
                continue;
            }

            MaskPtr mask = mMaskAnalysis.getExitMaskPtr(BB, *succBB);
            Value* newExitMask = materializeMask(mask); RV_UNUSED(newExitMask);

            IF_DEBUG{
              outs() << "  mask of edge '" << BB.getName()
                << "' -> '" << succBB->getName() << "': " << *newExitMask << "\n";
            }
        }
    }

    for (auto &L : mLoopInfo)
    {
#if 0
        if (region && !region->contains(L->getHeader())) continue; // skip loops outside of region
#endif
        materializeLoopExitMasks(L);
        materializeCombinedLoopExitMasks(L);
    }
}

bool
MaskGenerator::entryMaskIsUsed(const BasicBlock& block) const
{
    if (mvecInfo.isNotAlwaysByAll(&block)) return true; // there is divergent control at this block
    return false;

#if 0
    for (const auto &I : block)
    {
        if (rv::hasMetadata(&I, rv::RV_METADATA_OP_SEQUENTIAL_GUARDED)) //TODO refactor
        {
            return true;
        }

        if (!isa<CallInst>(I)) continue;

        // Ignore calls to metadata function.
        if (rv::isMetadataCall(&I)) continue;

        // OP_SEQUENTIAL calls never require a mask (otherwise they would be
        // OP_SEQUENTIAL_GUARDED).
        // OP_UNIFORM calls never require a mask.
        if (rv::hasMetadata(&I, rv::RV_METADATA_OP_SEQUENTIAL) || //TODO refactor
            rv::hasMetadata(&I, rv::RV_METADATA_OP_UNIFORM))
        {
            continue;
        }

        const Function* callee = cast<CallInst>(&I)->getCalledFunction();
        assert (callee);

        // Calls to functions that don't have a mapping have to be marked
        // OP_SEQUENTIAL or OP_SEQUENTIAL_GUARDED.
        assert (mInfo.mFunctionInfoMap.hasMapping(*callee) ||
                mInfo.mValueInfoMap.hasMapping(I));

        // Calls that were marked from the outside don't require a mask
        // (otherwise they would have a mapping in the function info map).
        if (!mInfo.mFunctionInfoMap.hasMapping(*callee))
        {
            assert (mInfo.mValueInfoMap.hasMapping(I));
            continue;
        }

        // Calls to functions that don't have a mask index never require a mask.
        if (mInfo.mFunctionInfoMap.getMaskIndex(*callee) == -1)
        {
            continue;
        }

        return true;
    }

    return false;
#endif
}

bool
MaskGenerator::hasNonUniformPhi(const BasicBlock& block) const
{
    for (auto &I : block)
    {
        if (!isa<PHINode>(I)) break;
        if (!mvecInfo.getVectorShape(I).isUniform()) return true;
        // if (!rv::hasMetadata(&I, rv::RV_METADATA_OP_UNIFORM));
    }

    return false;
}

bool
MaskGenerator::isHeaderOfDivergentLoop(const BasicBlock& block) const
{
    Loop* loop = mLoopInfo.getLoopFor(&block);
    if (!loop) return false;

    // assert(rv::hasMetadata(loop, rv::RV_METADATA_LOOP_DIVERGENT_TRUE) == mvecInfo.isDivergentLoop(loop));

    return mvecInfo.isDivergentLoop(loop);
}

Value*
MaskGenerator::materializeMask(MaskPtr maskPtr)
{
    assert (maskPtr);

    Mask& mask = *maskPtr.get();

    // Return if the mask is materialized already.
    if (mask.mValue) return mask.mValue;

    // If we are materializing a loop phi, we first have to recurse into the preheader,
    // then create the phi value, then recurse into the latch (see end of this function).
    if (mask.mType == LOOPMASKPHI || mask.mType == LOOPEXITPHI)
    {
        // The preheader mask is always the first one.
        assert (mask.mOperands.size() == 2);
        MaskPtr preheaderMask = mask.mOperands[0].lock();

        // If mask is not yet materialized, do it.
        if (!preheaderMask->mValue) materializeMask(preheaderMask);
    }
    else
    {
        for (unsigned i=0, e=mask.mOperands.size(); i<e; ++i)
        {
            MaskPtr opMask = mask.mOperands[i].lock();

            // Check if mask is already materialized.
            if (opMask->mValue) continue;

            materializeMask(opMask);
        }
    }

    // Return if the mask was materialized during recursion.
    if (mask.mValue) return mask.mValue;

    Value* maskValue = nullptr;

    switch (mask.mType)
    {
        case CONSTANT: // fallthrough
        case VALUE:
        {
            assert (mask.mValue);
            return mask.mValue;
        }

        case NEGATE:
        {
            assert (mask.mOperands.size() == 1);
            Value* mask0 = mask.mOperands[0].lock()->mValue;

            maskValue = createNeg(mask0, mask.mInsertPoint);
            break;
        }

        case CONJUNCTION:
        {
            assert (mask.mOperands.size() >= 2);
            maskValue = mask.mOperands[0].lock()->mValue;
            for (unsigned i=1, e=mask.mOperands.size(); i<e; ++i)
            {
                Value* mask1 = mask.mOperands[i].lock()->mValue;
                maskValue = createAnd(maskValue, mask1, mask.mInsertPoint);
            }
            break;
        }

        case DISJUNCTION:
        {
            assert (mask.mOperands.size() >= 2);
            maskValue = mask.mOperands[0].lock()->mValue;
            for (unsigned i=1, e=mask.mOperands.size(); i<e; ++i)
            {
                Value* mask1 = mask.mOperands[i].lock()->mValue;
                maskValue = createOr(maskValue, mask1, mask.mInsertPoint);
            }

            break;
        }

        case SELECT:
        {
            assert (mask.mOperands.size() == 3);
            Value* condition = mask.mOperands[0].lock()->mValue;
            Value* trueMask  = mask.mOperands[1].lock()->mValue;
            Value* falseMask = mask.mOperands[2].lock()->mValue;
            maskValue = createSelect(condition, trueMask, falseMask, mask.mInsertPoint);
            break;
        }

        case PHI:
        {
            const unsigned numIncVals = mask.mOperands.size();
            PHINode* phi = PHINode::Create(Type::getInt1Ty(*mInfo.mContext),
                                           numIncVals,
                                           "maskPhi",
                                           mask.mInsertPoint);

            for (unsigned i=0; i<numIncVals; ++i)
            {
                Value*      incMask = mask.mOperands[i].lock()->mValue;
                BasicBlock* incBB   = mask.mIncomingDirs[i];
                phi->addIncoming(incMask, incBB);
            }

            markMaskOperation(*phi);

            maskValue = phi;
            break;
        }

        case LOOPMASKPHI:
        {
            maskValue = createPhi(mask, "loopMaskPhi");
            break;
        }

        case LOOPEXITPHI:
        {
            maskValue = createPhi(mask, "loopExitMaskPhi");
            break;
        }

        case LOOPEXITUPDATE:
        {
            // Disjunction with exactly 2 operands.
            assert (mask.mOperands.size() == 2);
            assert (isa<PHINode>(mask.mOperands[0].lock()->mValue));
            Value* mask0 = mask.mOperands[0].lock()->mValue;
            Value* mask1 = mask.mOperands[1].lock()->mValue;
            maskValue = createOr(mask0, mask1, mask.mInsertPoint);
            if (Instruction* maskValI = dyn_cast<Instruction>(maskValue))
            {
                maskValI->setName("loopMaskUpdate");
            }
            break;
        }

        case REFERENCE:
        {
            assert (mask.mOperands.empty());
            assert (mask.mIncomingDirs.size() == 2);
            BasicBlock* source = mask.mIncomingDirs[0];
            BasicBlock* target = mask.mIncomingDirs[1];
            MaskPtr maskRef = mMaskAnalysis.getExitMaskPtr(*source, *target);
            maskValue = materializeMask(maskRef);
            break;
        }

        default:
        {
            assert (false && "unknown mask type found!");
        }
    }

    assert (maskValue);

    mask.mValue = maskValue;

    return maskValue;
}

Value*
MaskGenerator::createNeg(Value*       operand,
                         Instruction* insertPoint)
{
    assert (operand && insertPoint);

    assert (operand->getType() == Type::getInt1Ty(operand->getContext()) &&
            "trying to create bit-operation of non-boolean type!");

    if (operand == mInfo.mConstBoolTrue) return mInfo.mConstBoolFalse;
    if (operand == mInfo.mConstBoolFalse) return mInfo.mConstBoolTrue;

    if (Instruction* maskInstr = dyn_cast<Instruction>(operand))
    {
        if (maskInstr->getOpcode() == Instruction::Xor)
        {
            Value* op0 = maskInstr->getOperand(0);
            Value* op1 = maskInstr->getOperand(1);
            if (op0 == mInfo.mConstBoolTrue)
            {
                return op1; //found not, return op
            }
            else if (op1 == mInfo.mConstBoolTrue)
            {
                return op0; //found not, return op
            }
            else if (op0 == op1)
            {
                return mInfo.mConstBoolTrue; //found 'zero', return 'one'
            }
        }
    }

    Instruction* notI = BinaryOperator::CreateNot(operand,
                                                  "",
                                                  insertPoint);

    markMaskOperation(*notI);

    return notI;
}

Value*
MaskGenerator::createAnd(Value*       operand0,
                         Value*       operand1,
                         Instruction* insertPoint)
{
    assert (operand0 && operand1 && insertPoint);

    assert (operand0->getType() == Type::getInt1Ty(operand0->getContext()) &&
            "trying to create bit-operation on non-boolean type!");
    assert (operand1->getType() == Type::getInt1Ty(operand1->getContext()) &&
            "trying to create bit-operation on non-boolean type!");

    if (operand0 == operand1)
    {
        return operand0;
    }

    if (operand0 == mInfo.mConstBoolFalse ||
        operand1 == mInfo.mConstBoolFalse)
    {
        return mInfo.mConstBoolFalse;
    }

    if (operand0 == mInfo.mConstBoolTrue)
    {
        return operand1;
    }

    if (operand1 == mInfo.mConstBoolTrue)
    {
        return operand0;
    }

    Instruction* andI = BinaryOperator::Create(Instruction::And,
                                               operand0,
                                               operand1,
                                               "",
                                               insertPoint);

    markMaskOperation(*andI);

    return andI;

}

Value*
MaskGenerator::createOr(Value*       operand0,
                        Value*       operand1,
                        Instruction* insertPoint)
{
    assert (operand0 && operand1 && insertPoint);

    assert (operand0->getType() == Type::getInt1Ty(operand0->getContext()) &&
            "trying to create bit-operation on non-boolean type!");
    assert (operand1->getType() == Type::getInt1Ty(operand1->getContext()) &&
            "trying to create bit-operation on non-boolean type!");

    if (operand0 == operand1)
    {
        return operand0;
    }

    if (operand0 == mInfo.mConstBoolTrue ||
        operand1 == mInfo.mConstBoolTrue)
    {
        return mInfo.mConstBoolTrue;
    }

    if (operand0 == mInfo.mConstBoolFalse)
    {
        return operand1;
    }

    if (operand1 == mInfo.mConstBoolFalse)
    {
        return operand0;
    }

    Instruction* orI = BinaryOperator::Create(Instruction::Or,
                                              operand0,
                                              operand1,
                                              "",
                                              insertPoint);

    markMaskOperation(*orI);

    return orI;
}

Value*
MaskGenerator::createSelect(Value*       operand0,
                            Value*       operand1,
                            Value*       operand2,
                            Instruction* insertPoint)
{
    assert (operand0 && operand1 && operand2 && insertPoint);

    assert (operand0->getType() == Type::getInt1Ty(operand0->getContext()) &&
            "trying to create mask operation on non-boolean type!");
    assert (operand1->getType() == Type::getInt1Ty(operand1->getContext()) &&
            "trying to create mask operation on non-boolean type!");
    assert (operand2->getType() == Type::getInt1Ty(operand2->getContext()) &&
            "trying to create mask operation on non-boolean type!");

    if (operand1 == operand2)
    {
        return operand1;
    }

    if (operand0 == mInfo.mConstBoolTrue)
    {
        return operand1;
    }

    if (operand0 == mInfo.mConstBoolFalse)
    {
        return operand2;
    }

    Instruction* select = SelectInst::Create(operand0,
                                             operand1,
                                             operand2,
                                             "",
                                             insertPoint);

    markMaskOperation(*select);

    return select;
}

Value*
MaskGenerator::createPhi(Mask& mask, const Twine& name)
{
    PHINode* phi = PHINode::Create(Type::getInt1Ty(*mInfo.mContext),
                                   2,
                                   name,
                                   mask.mInsertPoint);

    Value*      preheaderMask = mask.mOperands[0].lock()->mValue;
    BasicBlock* preheaderBB   = mask.mIncomingDirs[0];
    phi->addIncoming(preheaderMask, preheaderBB);

    markMaskOperation(*phi);

    // Now, recurse into operand of latch.
    // Set mask value to break the cycle.
    mask.mValue = phi;

    // The latch mask is always the second one.
    MaskPtr     latchMask = mask.mOperands[1].lock();
    BasicBlock* latchBB   = mask.mIncomingDirs[1];

    // If mask is not yet materialized, do it.
    if (!latchMask->mValue) materializeMask(latchMask);

    phi->addIncoming(latchMask->mValue, latchBB);

    return phi;
}

void
MaskGenerator::materializeLoopExitMasks(Loop* loop)
{
    for (auto &SL : *loop)
    {
        materializeLoopExitMasks(SL);
    }

    // assert(rv::hasMetadata(loop, rv::RV_METADATA_LOOP_DIVERGENT_FALSE) == !mvecInfo.isDivergentLoop(loop));

    if (!mvecInfo.isDivergentLoop(loop)) return;

    SmallVector<BasicBlock*, 4> exitingBlocks;
    loop->getExitingBlocks(exitingBlocks);
    SmallVector<BasicBlock*, 4> exitBlocks;
    loop->getExitBlocks(exitBlocks);

    for (unsigned i=0, e=exitBlocks.size(); i<e; ++i)
    {
        BasicBlock* exitBB    = exitBlocks[i];
        BasicBlock* exitingBB = exitingBlocks[i];

        // assert (rv::hasMetadata(exitBB, rv::RV_METADATA_OPTIONAL) == mvecInfo.getVectorShape(*exitingBB->getTerminator()).isUniform());

        // Masks of OPTIONAL exits should only be references to the loop mask phi.
        if (mvecInfo.getVectorShape(*exitingBB->getTerminator()).isUniform())
        {
            Value* mask = materializeMask(mMaskAnalysis.getExitMaskPtr(*exitingBB, *exitBB));
            RV_UNUSED(mask);

            IF_DEBUG {
              outs() << "  loop exit mask of OPTIONAL edge '"
                << exitingBB->getName() << "' -> '"
                << exitBB->getName() << "': " << *mask << "\n";
            }
            continue;
        }

        MaskPtr maskPtr = mMaskAnalysis.getLoopExitMaskPtrUpdate(*loop, *exitingBB);
        Value* mask = materializeMask(maskPtr); RV_UNUSED(mask);

        IF_DEBUG {
          outs() << "  loop exit mask of edge '"
            << exitingBB->getName() << "' -> '"
            << exitBB->getName() << "': " << *mask << "\n";
        }
    }
}

// NOTE: The "combined loop exit mask" is 'true' for all instances
//       that left the loop in its *current* iteration!
// TODO: If there are no loop live values, we don't need the combined
//       exit mask since there will be no selects that use it.
void
MaskGenerator::materializeCombinedLoopExitMasks(Loop* loop)
{
    for (auto &SL : *loop)
    {
        materializeCombinedLoopExitMasks(SL);
    }

    // assert(rv::hasMetadata(loop, rv::RV_METADATA_LOOP_DIVERGENT_FALSE) == !mvecInfo.isDivergentLoop(loop));

    if (!mvecInfo.isDivergentLoop(loop)) return;

    materializeMask(mMaskAnalysis.getCombinedLoopExitMaskPtr(*loop));

    // Set the name. This is a bit inefficient but easier than modifying more code.
    Value* maskVal = mMaskAnalysis.getCombinedLoopExitMask(*loop);
    if (!isa<Instruction>(maskVal)) return;
    cast<Instruction>(maskVal)->setName("combinedLoopExitMask");
}

void
MaskGenerator::fillVecInfoWithPredicates(Function& F)
{
    for (auto& BB : F)
    {
        Value* predicate = mMaskAnalysis.getEntryMask(BB);

        if (!predicate)
            continue;

        CallInst* call = dyn_cast<CallInst>(predicate);
        if (call && call->getCalledFunction() &&
            call->getCalledFunction()->getName() == "entryMaskUseFn")
        {
            predicate = call->getOperand(0);
        }

        mvecInfo.setPredicate(BB, *predicate);
    }
}
