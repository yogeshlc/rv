//===----------------------- vectorizationInfo.cpp
//--------------------------===//
//
//                     The Region Vectorizer
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//

#include "rv/vectorizationInfo.h"

#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>

#include "rv/region/Region.h"
#include "utils/rvTools.h"
#include "rvConfig.h"

using namespace llvm;

static bool IsLCSSA = true;

namespace rv {

bool VectorizationInfo::inRegion(const BasicBlock &block) const {
  return region.contains(&block);
}

bool VectorizationInfo::inRegion(const Instruction &inst) const {
  return region.contains(inst.getParent());
}

void VectorizationInfo::dump(const Value *val) const { print(val, errs()); }

void VectorizationInfo::print(const Value *val, llvm::raw_ostream &out) const {
  if (!val)
    return;

  auto *block = dyn_cast<const BasicBlock>(val);
  if (block && inRegion(*block)) {
    printBlockInfo(*block, out);
  }

  out << *val;

  // show shadow input (if any)
  auto * phi = dyn_cast<PHINode>(val);
  if (phi) {
    const Value * shadowIn = getShadowInput(*phi);
    if (shadowIn) {
      out << ", shadow(";
        shadowIn->printAsOperand(out, false, getScalarFunction().getParent());
      out << ")";
    }
  }

  // attach vector shape
  if (hasKnownShape(*val)) {
    out << " : " << getVectorShape(*val).str() << "\n";
  } else {
    out << " : <n/a>\n";
  }
}

void VectorizationInfo::dumpBlockInfo(const BasicBlock &block) const {
  printBlockInfo(block, errs());
}

void VectorizationInfo::printBlockInfo(const BasicBlock &block,
                                       llvm::raw_ostream &out) const {
  // block name
  out << "Block ";
  block.printAsOperand(out, false);

  // block annotations
  out << " [";
  {
    bool hasVaryingPredicate = false;
    if (getVaryingPredicateFlag(block, hasVaryingPredicate)) {
      if (hasVaryingPredicate) out << ", var-pred";
      else out << ", uni-pred";
    }

    if (hasMask(block)) {
      getMask(block).print(out);
    }

    if (isDivergentLoopExit(block)) {
      out << ", divLoopExit";
    }
  }
  out << "]";
  out << "\n";

  // instructions
  for (const Instruction &inst : block) {
    print(&inst, out);
  }
  out << "\n";
}

void VectorizationInfo::dumpArguments() const { printArguments(errs()); }

void VectorizationInfo::printArguments(llvm::raw_ostream &out) const {
  const Function *F = mapping.scalarFn;

  out << "\nArguments:\n";

  for (auto &arg : F->args()) {
    out << arg << " : "
        << (hasKnownShape(arg) ? getVectorShape(arg).str() : "missing") << "\n";
  }

  out << "\n";
}

void VectorizationInfo::dump() const { print(errs()); }

void VectorizationInfo::print(llvm::raw_ostream &out) const {
  out << "VectorizationInfo ";

  out << "for " << region.str() << "\n";

  printArguments(out);

  for (const BasicBlock &block : *mapping.scalarFn) {
    if (!inRegion(block))
      continue;
    printBlockInfo(block, out);
  }

  out << "}\n";
}

VectorizationInfo::VectorizationInfo(llvm::Function &parentFn,
                                     unsigned vectorWidth, Region &_region)
    : DL(parentFn.getParent()->getDataLayout()), region(_region),
      mapping(&parentFn, &parentFn, vectorWidth,
              CallPredicateMode::SafeWithoutPredicate) {
  mapping.resultShape = VectorShape::uni();
  for (auto &arg : parentFn.args()) {
    RV_UNUSED(arg);
    mapping.argShapes.push_back(VectorShape::uni());
  }
}

// VectorizationInfo
VectorizationInfo::VectorizationInfo(Region &_region, VectorMapping _mapping)
    : DL(_region.getFunction().getParent()->getDataLayout()), region(_region),
      mapping(_mapping) {
  assert(mapping.argShapes.size() == mapping.scalarFn->arg_size());
  auto it = mapping.scalarFn->arg_begin();
  for (auto argShape : mapping.argShapes) {
    auto &arg = *it;
    setPinned(arg);
    setVectorShape(arg, argShape);
    ++it;
  }
}

bool VectorizationInfo::hasKnownShape(const llvm::Value &val) const {

  // explicit shape annotation take precedence
  if ((bool)shapes.count(&val))
    return true;

  // in-region instruction must have an explicit shape
  auto *inst = dyn_cast<Instruction>(&val);
  if (inst && inRegion(*inst))
    return false;

  // out-of-region values default to uniform
  return true;
}

VectorShape VectorizationInfo::getObservedShape(const LoopInfo &LI,
                                                const BasicBlock &observerBlock,
                                                const llvm::Value &val) const {
  auto valShape = getVectorShape(val);
  uint alignment = valShape.getAlignmentGeneral();

  if (isTemporalDivergent(LI, observerBlock, val)) {
    return VectorShape::varying(alignment);
  }

  return valShape;
}

VectorShape VectorizationInfo::getVectorShape(const llvm::Value &val) const {
  // Undef short-cut
  if (isa<UndefValue>(val))
    return VectorShape::undef();
  auto it = shapes.find(&val);

  // give precedence to user shapes
  if (it != shapes.end()) {
    return it->second;
  }

  // return default shape for constants
  auto *constVal = dyn_cast<Constant>(&val);
  if (constVal) {
    return VectorShape::fromConstant(constVal);
  }

  // out-of-region values default to uniform
  auto *inst = dyn_cast<Instruction>(&val);
  if (!inst || (inst && !inRegion(*inst))) {
    return VectorShape::uni(); // TODO getAlignment(*inst));
  }

  // otw, the shape is undefined
  return VectorShape::undef();
}

const DataLayout &VectorizationInfo::getDataLayout() const { return DL; }

void VectorizationInfo::dropVectorShape(const Value &val) {
  auto it = shapes.find(&val);
  if (it == shapes.end())
    return;
  shapes.erase(it);
}

void VectorizationInfo::setVectorShape(const llvm::Value &val,
                                       VectorShape shape) {
  shapes[&val] = shape;
}

// tenative predicate handling
bool
VectorizationInfo::getVaryingPredicateFlag(const llvm::BasicBlock &BB, bool & oIsVarying) const {
  auto it = VaryingPredicateBlocks.find(&BB);
  if (it == VaryingPredicateBlocks.end()) return false;
  oIsVarying = it->second;
  return true;
}

void
VectorizationInfo::setVaryingPredicateFlag(const llvm::BasicBlock & BB, bool toVarying) {
  VaryingPredicateBlocks[&BB] = toVarying;
}

void
VectorizationInfo::removeVaryingPredicateFlag(const llvm::BasicBlock & BB) {
  VaryingPredicateBlocks.erase(&BB);
}

bool
VectorizationInfo::hasMask(const BasicBlock & block) const {
  auto it = masks.find(&block);
  return it != masks.end();
}

Mask&
VectorizationInfo::requestMask(const llvm::BasicBlock & block) {
  auto it = masks.find(&block);
  if (it == masks.end()) {
    auto ItInserted = masks.insert(std::pair<const BasicBlock*, Mask>(&block, Mask()));
    return ItInserted.first->second;

  } else {
    return it->second;
  }
}

const Mask&
VectorizationInfo::getMask(const llvm::BasicBlock & block) const {
  auto it = masks.find(&block);
  assert(it != masks.end());
  return it->second;
}

// predicate handling
void VectorizationInfo::dropMask(const BasicBlock &block) {
  auto it = masks.find(&block);
  if (it == masks.end())
    return;
  masks.erase(it);
}

llvm::Value *
VectorizationInfo::getActiveVectorLength(const llvm::BasicBlock &block) const {
  if (hasMask(block)) {
    return getMask(block).activeVectorLength;
  }
  return nullptr;
}


llvm::Value *
VectorizationInfo::getPredicate(const llvm::BasicBlock &block) const {
  if (hasMask(block)) {
    return getMask(block).predicate;
  }
  return nullptr;
}

void VectorizationInfo::setPredicate(const llvm::BasicBlock &block,
                                     llvm::Value &predicate) {
  requestMask(block).predicate = &predicate;
}

void VectorizationInfo::remapPredicate(Value &dest, Value &old) {
  for (auto it : masks) {
    if (it.second.predicate == &old) {
      it.second.predicate = &dest;
    }
  }
}

// loop divergence
bool VectorizationInfo::addDivergentLoop(const Loop &loop) {
  return mDivergentLoops.insert(&loop).second;
}

void VectorizationInfo::removeDivergentLoop(const Loop &loop) {
  mDivergentLoops.erase(&loop);
}

bool VectorizationInfo::isDivergentLoop(const llvm::Loop &loop) const {
  return mDivergentLoops.find(&loop) != mDivergentLoops.end();
}

bool VectorizationInfo::isDivergentLoopTopLevel(const llvm::Loop &loop) const {
  Loop *parent = loop.getParentLoop();

  return isDivergentLoop(loop) && (!parent || !isDivergentLoop(*parent));
}

// loop exit divergence
bool VectorizationInfo::isDivergentLoopExit(const BasicBlock &BB) const {
  return DivergentLoopExits.find(&BB) != DivergentLoopExits.end();
}

bool VectorizationInfo::addDivergentLoopExit(const BasicBlock &block) {
  return DivergentLoopExits.insert(&block).second;
}

void VectorizationInfo::removeDivergentLoopExit(const BasicBlock &block) {
  DivergentLoopExits.erase(&block);
}

// pinned shape handling
bool VectorizationInfo::isPinned(const Value &V) const {
  return pinned.count(&V) != 0;
}

void VectorizationInfo::setPinned(const Value &V) { pinned.insert(&V); }

LLVMContext &VectorizationInfo::getContext() const {
  return mapping.scalarFn->getContext();
}

BasicBlock &VectorizationInfo::getEntry() const {
  return region.getRegionEntry();
}

bool VectorizationInfo::isTemporalDivergent(const LoopInfo &LI,
                                            const BasicBlock &ObservingBlock,
                                            const Value &Val) const {
  const auto *Inst = dyn_cast<const Instruction>(&Val);
  if (!Inst)
    return false;

  const auto *DefLoop = LI.getLoopFor(Inst->getParent());
  if (!DefLoop || DefLoop->contains(&ObservingBlock)) {
    return false;
  }

  // FIXME this is imprecise (liveouts of uniform exits appear varying, eventhough they are uniform)
  if (!IsLCSSA) {
    // check whether any divergent loop carrying Val terminates before control
    // proceeds to ObservingBlock
    for (const auto *Loop = DefLoop;
         Loop && inRegion(*Loop->getHeader()) && !Loop->contains(&ObservingBlock);
         Loop = Loop->getParentLoop()) {
      if (isDivergentLoop(*Loop)) {
        return true;
      }
    }

  } else {
    // all loop live-outs are funneled through LCSSA phis that sit on immediate exit blocks.
    // As such, only LCSSA phi nodes can observed temporal divergence.
    return isDivergentLoopExit(ObservingBlock);
  }

  return false;
}



// struct Mask
void
Mask::print(llvm::raw_ostream & out) const {
  out << "Mask {";
    bool hasText = false;
    if (predicate) {
      out << "P: ";
      predicate->printAsOperand(out);
      hasText = true;
    }
    if (activeVectorLength) {
      if (hasText) out << ", ";
      activeVectorLength->printAsOperand(out);
    }
  out << "}";
}

void
Mask::dump() const { print(errs()); }

} // namespace rv
