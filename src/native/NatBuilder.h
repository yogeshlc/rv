//===- NatBuilder.h -----------------------------===//
//
//                     The Region Vectorizer
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//

#ifndef NATIVE_NATBUILDER_H
#define NATIVE_NATBUILDER_H


#include <vector>

#include <rv/rvInfo.h>
#include <rv/analysis/maskAnalysis.h>
#include <rv/vectorizationInfo.h>

#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>

namespace rv {
  class Region;
}

namespace native {
    typedef std::map<const llvm::Function*, const rv::VectorMapping*> VectorMappingMap;
    typedef std::vector<llvm::Value *> LaneValueVector;

    class NatBuilder {
        llvm::IRBuilder<> builder;

        rv::RVInfo & rvInfo;
        rv::VectorizationInfo &vectorizationInfo;
        const llvm::DominatorTree &dominatorTree;

        llvm::Type *i1Ty;
        llvm::Type *i32Ty;

        rv::Region * region;

    public:
        NatBuilder(rv::RVInfo & rvInfo, VectorizationInfo &vectorizationInfo, const llvm::DominatorTree &dominatorTree);

        void vectorize();
        void vectorize(llvm::BasicBlock *const bb, llvm::BasicBlock *vecBlock);
        void vectorize(llvm::Instruction *const inst);

        void copyInstruction(llvm::Instruction *const inst, unsigned laneIdx = 0);
        void copyGEPInstruction(llvm::GetElementPtrInst *const gep, unsigned laneIdx = 0);

        void mapVectorValue(const llvm::Value *const value, llvm::Value *vecValue);

        void mapScalarValue(const llvm::Value *const value, llvm::Value *mapValue, unsigned laneIdx = 0);

        llvm::Value *getVectorValue(llvm::Value *const value);

        llvm::Value *getScalarValue(llvm::Value *const value, unsigned laneIdx = 0);

    private:
        void addValuesToPHINodes();

        void vectorizePHIInstruction(llvm::PHINode *const scalPhi);
        void vectorizeMemoryInstruction(llvm::Instruction *const inst);
        void vectorizeCallInstruction(llvm::CallInst *const scalCall);

        void mapOperandsInto(llvm::Instruction *const scalInst, llvm::Instruction *inst, bool vectorizedInst,
                             unsigned laneIdx = 0);

        llvm::DenseMap<const llvm::Value *, llvm::Value *> vectorValueMap;
        std::map<const llvm::Value *, LaneValueVector> scalarValueMap;
        llvm::DenseMap<unsigned, llvm::Function *> cascadeLoadMap;
        llvm::DenseMap<unsigned, llvm::Function *> cascadeStoreMap;
        std::vector<llvm::PHINode *> phiVector;

        llvm::Value *requestVectorValue(llvm::Value *const value);

        llvm::Value *requestScalarValue(llvm::Value *const value, unsigned laneIdx = 0,
                                        bool skipMappingWhenDone = false);
        llvm::Value *requestCascadeLoad(llvm::Value *vecPtr, unsigned int alignment, llvm::Value *mask);
        llvm::Value *requestCascadeStore(llvm::Value *vecVal, llvm::Value *vecPtr, unsigned int alignment,
                                         llvm::Value *mask);

        llvm::Function *createCascadeMemory(llvm::VectorType *pointerVectorType, unsigned alignment,
                                            llvm::VectorType *maskType, bool store);

        void mapCascadeFunction(unsigned bitWidth, llvm::Function *function, bool store);

        llvm::Function *getCascadeFunction(unsigned bitWidth, bool store);

        llvm::Value *createPTest(llvm::Value *vector);

        const rv::VectorMapping * getFunctionMapping(llvm::Function *func);

        unsigned vectorWidth();

        bool canVectorize(llvm::Instruction *inst);
        bool shouldVectorize(llvm::Instruction *inst);

        bool useMappingForCall(const rv::VectorMapping *mapping, llvm::CallInst *const scalCall);

        void vectorizeReductionCall(CallInst *wfvCall);
    };
}

#endif //NATIVE_NATBUILDER_H
