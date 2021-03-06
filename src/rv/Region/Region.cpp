//===- Region.cpp -----------------------------===//
//
//                     The Region Vectorizer
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
// @author kloessner

#include "rv/Region/Region.h"

namespace rv {

Region::Region(RegionImpl& Impl) : mImpl(Impl)
{ }

bool
Region::contains(const BasicBlock* BB) const
{
    return mImpl.contains(BB);
}

BasicBlock&
Region::getRegionEntry() const
{
    return mImpl.getRegionEntry();
}

void
Region::getEndingBlocks(llvm::SmallPtrSet<BasicBlock*, 2>& endingBlocks) const
{
    mImpl.getEndingBlocks(endingBlocks);
}

}
