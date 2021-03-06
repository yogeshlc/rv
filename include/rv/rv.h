//===- rv.h ----------------*- C++ -*-===//
//
//                     The Region Vectorizer
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//

#ifndef RV_RV_H
#define RV_RV_H

#include "rv/vectorizationInfo.h"
#include "rv/pda/DFG.h"
#include "rv/analysis/maskAnalysis.h"
#include "rv/analysis/loopLiveValueAnalysis.h"
#include "rv/rvInfo.h"

using namespace llvm;

namespace rv {
/*
 * The new vectorizer interface.
 *
 * The functionality of this interface relies heavily on prior loop simplification
 * (see LoopExitCanonicalizer), aswell as the elimination of critical edges beforehand.
 *
 * No guarantees are made in case the Function that is handled has critical edges,
 * or loop exits with with more than one predecessor are present.
 *
 * Vectorization of the Function in question may violate the original semantics, if
 * a wrong analysis is used as the VectorizationInfo object. The user is responsible for
 * running the analysis phase prior, or specifying a valid analysis himself.
 */
class VectorizerInterface {
public:
    VectorizerInterface(RVInfo& F, Function* scalarCopy);
    //~VectorizerInterface();

    /*
     * Analyze properties of the scalar function that are needed later in transformations
     * towards its SIMD equivalent.
     *
     * This expects initial information about arguments to be set in the VectorizationInfo object
     * (see VectorizationInfo).
     */
    void analyze(VectorizationInfo& vectorizationInfo,
                 const CDG& cdg,
                 const DFG& dfg,
                 const LoopInfo& loopInfo,
                 const PostDominatorTree& postDomTree,
                 const DominatorTree& domTree);

    /*
     * Analyze mask values needed to mask certain values and preserve semantics of the function
     * after its control flow is linearized where needed.
     */
    MaskAnalysis* analyzeMasks(VectorizationInfo& vectorizationInfo, const LoopInfo& loopinfo);

    /*
     * Materialize the mask information.
     */
    bool generateMasks(VectorizationInfo& vectorizationInfo,
                       MaskAnalysis& maskAnalysis,
                       const LoopInfo& loopInfo);

    /*
     * Linearize divergent regions of the scalar function to preserve semantics for the
     * vectorized function
     */
    bool linearizeCFG(VectorizationInfo& vectorizationInfo,
                      MaskAnalysis& maskAnalysis,
                      LoopInfo& loopInfo,
                      const PostDominatorTree& postDomTree,
                      const DominatorTree& domTree);

    /*
     * Produce vectorized instructions
     */
    bool
    vectorize(VectorizationInfo& vecInfo, const DominatorTree& domTree);

    /*
     * Ends the vectorization process on this function, removes metadata and
     * writes the function to a file
     */
    void finalize();

    bool addSIMDSemantics(const Function& f,
                          const bool      isOpUniform,
                          const bool      isOpVarying,
                          const bool      isOpSequential,
                          const bool      isOpSequentialGuarded,
                          const bool      isResultUniform,
                          const bool      isResultVector,
                          const bool      isResultScalars,
                          const bool      isAligned,
                          const bool      isIndexSame,
                          const bool      isIndexConsecutive);

private:
    RVInfo&                       mInfo;
    Function*                      mScalarFn;

    bool verifyVectorizedType(Type* scalarType, Type* vecType);
    bool verifyFunctionSignaturesMatch(const Function& F1, const Function& F2);
    void addPredicateInstrinsics();
};

}

#endif // RV_RV_H
